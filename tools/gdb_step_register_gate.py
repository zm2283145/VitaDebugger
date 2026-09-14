#!/usr/bin/env python3
"""Run VitaDebugger's bounded live stepping/register hardware gate.

The main phase deliberately drives the target through VitaSDK GDB instead of
implementing another debugger frontend.  It validates GDB's hidden software-
breakpoint step-over, the bounded Thumb/A32 LDREX..STREX step fixtures, and
selected exception-thread ``p``/``P`` access.  A final, independent raw-RSP
phase abandons two software breakpoints and proves that reconnect cleanup
restored the exact fixture bytes.

This tool never deploys an application and never changes the kernel plugin.
The matching diagnostic application must already be running.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


# These modules own the project's already-tested MI and RSP transports.  Keep
# the live gate focused on orchestration and assertions rather than maintaining
# subtly different packet framing or GDB process code here.
import gdb_monitor_smoke as monitor
import gdb_vfp_lifecycle as vfp


SCHEMA = "vitadebugger-step-register-live-gdb-v1"
SYMBOL_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
GDB_FILE_COMMAND = re.compile(
    r'^\s*file\s+("(?:\\.|[^"\\])*"|[^\s#]+)\s*$'
)
GDB_TARGET_COMMAND = re.compile(
    r"^\s*target\s+(?:extended-)?remote\s+([A-Za-z0-9.-]+):(\d+)\s*$"
)

# Every address which receives Z0 is checked against an exact current fixture
# encoding first.  This makes a stale symbol file fail before it can patch an
# unrelated executable address.  The scanner intentionally places its internal
# temporary trap at the first instruction after STREX.
FIXTURE_SITES = (
    ("step_target", 2, bytes.fromhex("80b4")),
    ("thumb_step_exclusive_site", 4, bytes.fromhex("50e8001f")),
    ("thumb_step_exclusive_after", 2, bytes.fromhex("002a")),
    ("arm_step_exclusive_site", 4, bytes.fromhex("9f1f90e1")),
    ("arm_step_exclusive_after", 4, bytes.fromhex("000052e3")),
)
TRAP_SITES = (
    ("thumb_step_exclusive_after", 2, bytes.fromhex("002a")),
    ("arm_step_exclusive_after", 4, bytes.fromhex("000052e3")),
)
UDF_BY_SIZE = {
    2: bytes.fromhex("00de"),
    4: bytes.fromhex("f000f0e7"),
}


class GateFailure(RuntimeError):
    """A failed assertion for which the target must not be resumed."""


class UnrestoredMutation(GateFailure):
    """A register restoration could not be proven while transport was live."""


@dataclass(frozen=True)
class CurrentThread:
    gdb_id: int
    target_id: int
    name: str


@dataclass(frozen=True)
class Breakpoint:
    number: str
    symbol: str


@dataclass(frozen=True)
class LiveSymbols:
    ordinary: int
    thumb_site: int
    thumb_after: int
    thumb_word: int
    arm_site: int
    arm_after: int
    arm_word: int


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def file_identity(path: Path) -> dict[str, Any]:
    return {"sha256": sha256_file(path), "size_bytes": path.stat().st_size}


def transcript_identity(lines: Iterable[str]) -> dict[str, Any]:
    lines = list(lines)
    encoded = json.dumps(lines, ensure_ascii=False, separators=(",", ":")).encode(
        "utf-8"
    )
    return {
        "mi_record_count": len(lines),
        "mi_sha256": hashlib.sha256(encoded).hexdigest(),
    }


def write_evidence(path: Path, evidence: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def decode_gdb_file_argument(argument: str) -> str:
    if argument.startswith('"'):
        return vfp.decode_mi_string(argument)
    return argument


def main_elf_from_symbol_script(
    script: Path, host: str, port: int
) -> Path:
    """Extract the one main ELF from our generated ASLR symbol script.

    The gate needs only main-image symbols; qOffsets relocates them when GDB
    attaches.  Refusing ambiguous scripts avoids sourcing arbitrary commands
    from a file merely because it was passed as a symbol description.
    """

    try:
        lines = script.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        raise GateFailure(f"cannot read symbol script: {exc}") from exc

    file_arguments: list[str] = []
    target_endpoints: list[tuple[str, int]] = []
    for line in lines:
        file_match = GDB_FILE_COMMAND.fullmatch(line)
        if file_match:
            file_arguments.append(file_match.group(1))
            continue
        target_match = GDB_TARGET_COMMAND.fullmatch(line)
        if target_match:
            target_endpoints.append(
                (target_match.group(1), int(target_match.group(2), 10))
            )

    if len(file_arguments) != 1:
        raise GateFailure(
            "symbol script must contain exactly one complete GDB file command"
        )
    if len(target_endpoints) > 1:
        raise GateFailure("symbol script contains multiple remote targets")
    if target_endpoints and target_endpoints[0] != (host, port):
        expected = f"{host}:{port}"
        actual = f"{target_endpoints[0][0]}:{target_endpoints[0][1]}"
        raise GateFailure(
            f"symbol script targets {actual}, not requested endpoint {expected}"
        )

    path = Path(decode_gdb_file_argument(file_arguments[0]))
    if not path.is_absolute():
        path = script.parent / path
    return path.resolve()


def parse_gdb_integer(value: str) -> int:
    """Parse GDB's integer spelling, allowing its optional symbol suffix."""

    text = value.strip()
    match = re.match(r"^(0x[0-9a-fA-F]+|[0-9]+)(?:\s+<[^>]+>)?$", text)
    if not match:
        raise GateFailure(f"GDB returned a non-integer value: {value!r}")
    result = int(match.group(1), 0)
    if not 0 <= result <= 0xFFFFFFFF:
        raise GateFailure(f"GDB integer is outside the 32-bit target range: {value!r}")
    return result


def evaluate_u32(client: vfp.MiGdb, expression: str) -> int:
    return parse_gdb_integer(client.evaluate(f"(unsigned int)({expression})"))


def console_command(client: vfp.MiGdb, command: str) -> str:
    result, records = client.command(
        f"-interpreter-exec console {vfp.mi_quote(command)}"
    )
    if vfp.mi_result_class(result, client.token) != "done":
        raise GateFailure(f"GDB console command failed: {command!r}")
    output: list[str] = []
    for record in records:
        if len(record) >= 3 and record[:1] in ("~", "@", "&"):
            try:
                output.append(vfp.decode_mi_string(record[1:]))
            except vfp.GateFailure as exc:
                raise GateFailure("GDB returned malformed console output") from exc
    return "".join(output)


def flush_register_cache(client: vfp.MiGdb) -> None:
    console_command(client, "maintenance flush register-cache")


def configure_individual_register_packets(client: vfp.MiGdb) -> None:
    console_command(client, "set remote fetch-register-packet on")
    console_command(client, "set remote set-register-packet on")
    fetch = console_command(client, "show remote fetch-register-packet").lower()
    write = console_command(client, "show remote set-register-packet").lower()
    if '"on"' not in fetch or '"on"' not in write:
        raise GateFailure("GDB did not enable individual p/P register packets")


def current_thread(client: vfp.MiGdb) -> CurrentThread:
    record, _ = client.command("-thread-info")
    current_match = re.search(r'(?:^|,)current-thread-id="([0-9]+)"', record)
    if not current_match:
        raise GateFailure("GDB did not identify its current stopped thread")
    current_id = int(current_match.group(1), 10)
    selected: str | None = None
    for item in vfp.extract_mi_objects(record, "threads"):
        try:
            if int(vfp.mi_top_level_string_field(item, "id"), 10) == current_id:
                selected = item
                break
        except (ValueError, vfp.GateFailure):
            continue
    if selected is None:
        raise GateFailure("current GDB thread is absent from the thread inventory")
    target_text = vfp.mi_top_level_string_field(selected, "target-id")
    target_match = re.fullmatch(
        r"(?:Thread\s+)?(0x[0-9a-fA-F]+|[0-9]+)", target_text
    )
    if not target_match:
        raise GateFailure(f"unsupported GDB target thread ID: {target_text!r}")
    name = ""
    try:
        name = vfp.mi_top_level_string_field(selected, "name")
    except vfp.GateFailure:
        pass
    return CurrentThread(current_id, int(target_match.group(1), 0), name)


def require_exception_selection(client: vfp.MiGdb) -> CurrentThread:
    selected = current_thread(client)
    result, _ = client.command(f"-thread-select {selected.gdb_id}")
    if vfp.mi_result_class(result, client.token) != "done":
        raise GateFailure("GDB could not select the stopped exception thread")
    report = console_command(client, "monitor threads")
    line = re.search(
        rf"^\s*0x{selected.target_id:08x}\s+\[([^]]*)\]\s+([^\r\n]*)$",
        report,
        re.MULTILINE | re.IGNORECASE,
    )
    if not line:
        raise GateFailure("monitor threads omitted GDB's current stopped thread")
    flags = {flag.strip() for flag in line.group(1).split(",")}
    if not {"stopped", "Hg", "exception"}.issubset(flags):
        raise GateFailure(
            "register mutation target is not the selected stopped exception thread"
        )
    monitor_name = line.group(2).strip()
    if monitor_name != "test main":
        raise GateFailure(
            f"fixture monitor selected unexpected thread {monitor_name!r}"
        )
    if selected.name and selected.name != monitor_name:
        raise GateFailure(
            f"fixture stopped an unexpected thread named {selected.name!r}"
        )
    return selected


def parse_breakpoint(result: str, symbol: str) -> Breakpoint:
    number_match = re.search(r'(?:^|[,{{])number="([0-9]+)"', result)
    if not number_match:
        raise GateFailure(f"GDB did not report a breakpoint number for {symbol}")
    return Breakpoint(number_match.group(1), symbol)


def insert_breakpoint(client: vfp.MiGdb, symbol: str) -> Breakpoint:
    if not SYMBOL_NAME.fullmatch(symbol):
        raise GateFailure(f"unsafe fixture symbol name {symbol!r}")
    result, _ = client.command(f"-break-insert *{symbol}")
    if vfp.mi_result_class(result, client.token) != "done":
        raise GateFailure(f"GDB could not insert breakpoint at {symbol}")
    return parse_breakpoint(result, symbol)


def delete_breakpoint(client: vfp.MiGdb, breakpoint: Breakpoint) -> None:
    result, _ = client.command(f"-break-delete {breakpoint.number}")
    if vfp.mi_result_class(result, client.token) != "done":
        raise GateFailure(f"GDB could not delete breakpoint {breakpoint.number}")


def wait_for_stop(client: vfp.MiGdb, command: str) -> str:
    result, records = client.command(command)
    if vfp.mi_result_class(result, client.token) != "running":
        raise GateFailure(f"{command} did not start target execution")
    stopped = next((record for record in records if record.startswith("*stopped")), None)
    if stopped is None:
        stopped = client.wait_for("*stopped")
    if 'reason="exited' in stopped:
        raise GateFailure("diagnostic application exited during the live gate")
    return stopped


def stop_reason(stop: str) -> str:
    try:
        return vfp.mi_string_field(stop, "reason")
    except vfp.GateFailure as exc:
        raise GateFailure("GDB stop notification omitted its reason") from exc


def require_breakpoint_stop(stop: str, breakpoint: Breakpoint) -> None:
    if 'reason="breakpoint-hit"' not in stop:
        raise GateFailure(
            f"expected breakpoint {breakpoint.number}, got {stop[:200]!r}"
        )
    match = re.search(r'bkptno="([0-9]+)"', stop)
    if not match or match.group(1) != breakpoint.number:
        raise GateFailure(
            f"stop did not name expected breakpoint {breakpoint.number}"
        )


def symbol_address(client: vfp.MiGdb, symbol: str) -> int:
    if not SYMBOL_NAME.fullmatch(symbol):
        raise GateFailure(f"unsafe fixture symbol name {symbol!r}")
    return evaluate_u32(client, f"&{symbol}") & ~1


def resolve_live_symbols(client: vfp.MiGdb) -> LiveSymbols:
    return LiveSymbols(
        ordinary=symbol_address(client, "step_target"),
        thumb_site=symbol_address(client, "thumb_step_exclusive_site"),
        thumb_after=symbol_address(client, "thumb_step_exclusive_after"),
        thumb_word=symbol_address(client, "uvdb_thumb_exclusive_word"),
        arm_site=symbol_address(client, "arm_step_exclusive_site"),
        arm_after=symbol_address(client, "arm_step_exclusive_after"),
        arm_word=symbol_address(client, "uvdb_arm_exclusive_word"),
    )


def read_gdb_memory(client: vfp.MiGdb, address: int, size: int) -> bytes:
    result, _ = client.command(
        f"-data-read-memory-bytes 0x{address:08x} {size}"
    )
    if vfp.mi_result_class(result, client.token) != "done":
        raise GateFailure(f"GDB could not read fixture bytes at 0x{address:08x}")
    contents = re.findall(r'contents="([0-9a-fA-F]+)"', result)
    if len(contents) != 1 or len(contents[0]) != size * 2:
        raise GateFailure(
            f"GDB returned malformed fixture bytes at 0x{address:08x}"
        )
    return bytes.fromhex(contents[0])


def preflight_fixture_bytes(
    client: vfp.MiGdb, symbols: LiveSymbols
) -> dict[str, str]:
    addresses = {
        "step_target": symbols.ordinary,
        "thumb_step_exclusive_site": symbols.thumb_site,
        "thumb_step_exclusive_after": symbols.thumb_after,
        "arm_step_exclusive_site": symbols.arm_site,
        "arm_step_exclusive_after": symbols.arm_after,
    }
    observed: dict[str, str] = {}
    for symbol, size, expected in FIXTURE_SITES:
        actual = read_gdb_memory(client, addresses[symbol], size)
        if actual != expected:
            raise GateFailure(
                f"{symbol} target bytes were {actual.hex()}, expected "
                f"{expected.hex()}; main symbols do not match the running build"
            )
        observed[symbol] = actual.hex()
    return observed


def pc_and_cpsr(client: vfp.MiGdb) -> tuple[int, int]:
    flush_register_cache(client)
    return evaluate_u32(client, "$pc") & ~1, evaluate_u32(client, "$cpsr")


def breakpoint_count(report: str) -> int:
    matches = re.findall(
        r"^\s*software-breakpoints:\s*([0-9]+)\s*$", report, re.MULTILINE
    )
    if len(matches) != 1:
        raise GateFailure("monitor status has no unique software-breakpoint count")
    return int(matches[0], 10)


def require_no_breakpoints_gdb(client: vfp.MiGdb) -> None:
    count = breakpoint_count(console_command(client, "monitor status"))
    if count != 0:
        raise GateFailure(f"monitor status reports {count} software breakpoints")


def test_hidden_breakpoint_step_over(
    client: vfp.MiGdb, symbols: LiveSymbols
) -> dict[str, Any]:
    breakpoint = insert_breakpoint(client, "step_target")
    try:
        first_stop = wait_for_stop(client, "-exec-continue")
        require_breakpoint_stop(first_stop, breakpoint)
        first_pc, first_cpsr = pc_and_cpsr(client)
        if first_pc != symbols.ordinary or not (first_cpsr & (1 << 5)):
            raise GateFailure("ordinary Thumb breakpoint stopped at the wrong state/address")

        # Keeping the user breakpoint installed is intentional.  GDB must hide
        # it, execute the original instruction, reinsert it, and reach the next
        # loop iteration without exposing the stub's historical E16.
        second_stop = wait_for_stop(client, "-exec-continue")
        require_breakpoint_stop(second_stop, breakpoint)
        second_pc, second_cpsr = pc_and_cpsr(client)
        if second_pc != symbols.ordinary or not (second_cpsr & (1 << 5)):
            raise GateFailure("hidden breakpoint step-over returned at the wrong address")
        return {
            "result": "PASS",
            "symbol": "step_target",
            "address": f"0x{symbols.ordinary:08x}",
            "first_pc": f"0x{first_pc:08x}",
            "second_pc": f"0x{second_pc:08x}",
            "gdb_hidden_step_over": True,
            "E16_observed": False,
        }
    finally:
        delete_breakpoint(client, breakpoint)


def test_exclusive_fixture(
    client: vfp.MiGdb,
    *,
    label: str,
    site_symbol: str,
    site_address: int,
    after_address: int,
    word_symbol: str,
    thumb: bool,
) -> dict[str, Any]:
    breakpoint = insert_breakpoint(client, site_symbol)
    try:
        stop = wait_for_stop(client, "-exec-continue")
        require_breakpoint_stop(stop, breakpoint)
        before_thread = current_thread(client)
        before_pc, before_cpsr = pc_and_cpsr(client)
        if before_pc != site_address:
            raise GateFailure(f"{label} LDREX breakpoint stopped at the wrong PC")
        if bool(before_cpsr & (1 << 5)) != thumb:
            raise GateFailure(f"{label} fixture entered the wrong ARM execution state")
        word_before = evaluate_u32(client, word_symbol)

        step_stop = wait_for_stop(client, "-exec-step-instruction")
        reason = stop_reason(step_stop)
        if reason != "end-stepping-range":
            raise GateFailure(
                f"{label} exclusive step reported unexpected stop reason {reason!r}"
            )
        after_thread = current_thread(client)
        if after_thread.target_id != before_thread.target_id:
            raise GateFailure(f"{label} exclusive step changed stopped thread")
        after_pc, after_cpsr = pc_and_cpsr(client)
        word_after = evaluate_u32(client, word_symbol)
        r2 = evaluate_u32(client, "$r2")
        if after_pc != after_address:
            raise GateFailure(
                f"{label} exclusive step stopped at 0x{after_pc:08x}, "
                f"expected 0x{after_address:08x}"
            )
        if bool(after_cpsr & (1 << 5)) != thumb:
            raise GateFailure(f"{label} exclusive step changed execution state")
        if r2 != 0:
            raise GateFailure(f"{label} STREX failed with status 0x{r2:08x}")
        if word_after != ((word_before + 1) & 0xFFFFFFFF):
            raise GateFailure(
                f"{label} exclusive word did not increment exactly once"
            )
        return {
            "result": "PASS",
            "state": "Thumb-2" if thumb else "A32",
            "site": f"0x{site_address:08x}",
            "after": f"0x{after_address:08x}",
            "pc_after": f"0x{after_pc:08x}",
            "strex_status_r2": f"0x{r2:08x}",
            "stop_reason": reason,
            "stop_thread": f"0x{after_thread.target_id:08x}",
            "word_before": f"0x{word_before:08x}",
            "word_after": f"0x{word_after:08x}",
            "bounded_sequence_step": True,
        }
    finally:
        delete_breakpoint(client, breakpoint)


def write_register(client: vfp.MiGdb, register: str, value: int) -> None:
    result, _ = client.command(
        f"-data-evaluate-expression {vfp.mi_quote(f'${register} = 0x{value:08x}')}"
    )
    if vfp.mi_result_class(result, client.token) != "done":
        raise GateFailure(f"GDB rejected P write for {register}")


def read_register(client: vfp.MiGdb, register: str) -> int:
    flush_register_cache(client)
    return evaluate_u32(client, f"${register}")


def mutate_and_restore_register(
    client: vfp.MiGdb,
    register: str,
    mutated: int,
    pending: dict[str, int],
) -> dict[str, Any]:
    original = read_register(client, register)
    if original == mutated:
        mutated ^= 0x01010101
        mutated &= 0xFFFFFFFF
    pending[register] = original
    try:
        write_register(client, register, mutated)
        read_back = read_register(client, register)
        if read_back != mutated:
            raise GateFailure(
                f"{register} P write read back 0x{read_back:08x}, "
                f"expected 0x{mutated:08x}"
            )
    finally:
        # Restoration is attempted even if write validation fails.  The pending
        # entry remains set until an independent p read confirms exact recovery.
        write_register(client, register, original)
        restored = read_register(client, register)
        if restored != original:
            raise UnrestoredMutation(
                f"{register} restoration read back 0x{restored:08x}, "
                f"expected 0x{original:08x}; target remains stopped"
            )
        del pending[register]
    return {
        "original": f"0x{original:08x}",
        "mutated": f"0x{mutated:08x}",
        "read_back": True,
        "restored": True,
        "restoration_read_back": True,
        "packet_write_reply": "OK",
    }


def restore_pending_registers(
    client: vfp.MiGdb, pending: dict[str, int]
) -> list[str]:
    failures: list[str] = []
    for register, original in list(pending.items()):
        restored = False
        for _ in range(3):
            try:
                write_register(client, register, original)
                if read_register(client, register) == original:
                    del pending[register]
                    restored = True
                    break
            except (GateFailure, vfp.GateFailure, OSError):
                continue
        if not restored:
            failures.append(register)
    return failures


def test_register_transactions(client: vfp.MiGdb) -> dict[str, Any]:
    selected = require_exception_selection(client)
    configure_individual_register_packets(client)
    pending: dict[str, int] = {}
    try:
        r0_original = read_register(client, "r0")
        r0_mutated = (r0_original ^ 0x13579BDF) & 0xFFFFFFFF
        r0 = mutate_and_restore_register(client, "r0", r0_mutated, pending)

        cpsr_original = read_register(client, "cpsr")
        cpsr_mutated = cpsr_original ^ (1 << 28)
        cpsr = mutate_and_restore_register(
            client, "cpsr", cpsr_mutated, pending
        )
        if (int(cpsr["original"], 16) ^ int(cpsr["mutated"], 16)) != (1 << 28):
            raise GateFailure("CPSR transaction changed more than the V flag")
        return {
            "result": "PASS",
            "selected_exception_thread": f"0x{selected.target_id:08x}",
            "gdb_thread": selected.gdb_id,
            "individual_p_P_forced": True,
            "r0": r0,
            "cpsr": {**cpsr, "mutation_bit": "V"},
            "all_mutations_restored_before_resume_or_detach": True,
        }
    except BaseException:
        failed = restore_pending_registers(client, pending)
        if failed:
            raise UnrestoredMutation(
                "could not restore register(s) " + ", ".join(failed) +
                "; target was not explicitly resumed or detached"
            )
        raise


def read_memory(client: monitor.RspClient, address: int, size: int) -> bytes:
    response = client.request(f"m{address:x},{size:x}".encode("ascii"))
    if response.startswith(b"E"):
        raise GateFailure(
            f"memory read at 0x{address:08x} returned {response!r}"
        )
    if len(response) != size * 2 or not re.fullmatch(rb"[0-9a-fA-F]+", response):
        raise GateFailure(
            f"memory read at 0x{address:08x} returned malformed data"
        )
    return bytes.fromhex(response.decode("ascii"))


def rsp_breakpoint(
    client: monitor.RspClient, insert: bool, address: int, size: int
) -> None:
    operation = "Z" if insert else "z"
    response = client.request(
        f"{operation}0,{address:x},{size:x}".encode("ascii")
    )
    if response != b"OK":
        action = "insert" if insert else "remove"
        raise GateFailure(
            f"raw RSP could not {action} breakpoint at 0x{address:08x}: "
            f"{response!r}"
        )


def raw_monitor_count(client: monitor.RspClient) -> int:
    return breakpoint_count(monitor.monitor_request(client, "status"))


def connect_rsp(host: str, port: int, timeout: float) -> monitor.RspClient:
    deadline = time.monotonic() + timeout
    last_error: BaseException | None = None
    while time.monotonic() < deadline:
        client: monitor.RspClient | None = None
        try:
            client = monitor.RspClient(host, port, min(timeout, 5.0))
            client.negotiate()
            stop = client.request(b"?")
            if not stop.startswith((b"T", b"S")):
                client.close()
                raise GateFailure(f"raw RSP attach returned {stop[:80]!r}")
            return client
        except OSError as exc:
            if client is not None:
                client.close()
            last_error = exc
            time.sleep(0.1)
        except monitor.SmokeFailure as exc:
            if client is not None:
                client.close()
            # A just-abandoned session can accept TCP before its old transport
            # has completely unwound. Retry only that transient closure; bad
            # checksums or malformed feature replies remain immediate failures.
            if "connection closed unexpectedly" not in str(exc):
                raise
            last_error = exc
            time.sleep(0.1)
        except BaseException:
            if client is not None:
                client.close()
            raise
    raise GateFailure(f"could not reconnect to RSP endpoint: {last_error}")


def cleanup_inserted_breakpoints(
    client: monitor.RspClient, inserted: list[tuple[int, int]]
) -> None:
    for address, size in reversed(inserted):
        try:
            rsp_breakpoint(client, False, address, size)
        except (GateFailure, monitor.SmokeFailure, OSError):
            pass


def raw_sites_match(
    client: monitor.RspClient,
    addresses: dict[str, int],
    expected_by_symbol: dict[str, str],
) -> bool:
    try:
        for symbol, size, _ in TRAP_SITES:
            expected_hex = expected_by_symbol.get(symbol)
            if expected_hex is None:
                return False
            if read_memory(client, addresses[symbol], size).hex() != expected_hex:
                return False
        return True
    except (GateFailure, monitor.SmokeFailure, OSError):
        return False


def raw_safe_to_detach(
    client: monitor.RspClient,
    addresses: dict[str, int],
    expected_by_symbol: dict[str, str],
) -> bool:
    try:
        return raw_monitor_count(client) == 0 and raw_sites_match(
            client, addresses, expected_by_symbol
        )
    except (GateFailure, monitor.SmokeFailure, OSError):
        return False


def require_physical_udf(
    client: monitor.RspClient, symbol: str, address: int, size: int
) -> None:
    armed = read_memory(client, address, size)
    expected = UDF_BY_SIZE[size]
    if armed != expected:
        raise GateFailure(
            f"{symbol} Z0 returned OK but target bytes were {armed.hex()}, "
            f"expected UDF {expected.hex()}"
        )


def test_abrupt_disconnect_cleanup(
    host: str,
    port: int,
    timeout: float,
    recovery_seconds: float,
    symbols: LiveSymbols,
) -> dict[str, Any]:
    addresses = {
        "thumb_step_exclusive_after": symbols.thumb_after,
        "arm_step_exclusive_after": symbols.arm_after,
    }
    first: monitor.RspClient | None = connect_rsp(host, port, timeout)
    inserted: list[tuple[int, int]] = []
    before: dict[str, str] = {}
    try:
        if raw_monitor_count(first) != 0:
            raise GateFailure("raw cleanup subgate did not begin with zero breakpoints")
        for symbol, size, expected in TRAP_SITES:
            address = addresses[symbol]
            actual = read_memory(first, address, size)
            if actual != expected:
                raise GateFailure(
                    f"{symbol} bytes were {actual.hex()}, expected {expected.hex()}"
                )
            before[symbol] = actual.hex()
            rsp_breakpoint(first, True, address, size)
            inserted.append((address, size))
            require_physical_udf(first, symbol, address, size)
        if raw_monitor_count(first) != len(inserted):
            raise GateFailure("monitor status did not count both armed breakpoints")

        # This is the intentional failure injection.  Do not send D or z0.
        first.close()
        first = None
    except BaseException:
        if first is not None:
            cleanup_inserted_breakpoints(first, inserted)
            try:
                if raw_safe_to_detach(first, addresses, before):
                    first.request(b"D")
            except (GateFailure, monitor.SmokeFailure, OSError):
                pass
            first.close()
        raise

    time.sleep(recovery_seconds)
    second = connect_rsp(host, port, timeout)
    detach_ok = False
    try:
        count = raw_monitor_count(second)
        if count != 0:
            cleanup_inserted_breakpoints(second, inserted)
            raise GateFailure(
                f"abrupt-disconnect recovery retained {count} breakpoint(s)"
            )
        after: dict[str, str] = {}
        for symbol, size, expected in TRAP_SITES:
            actual = read_memory(second, addresses[symbol], size)
            if actual != expected or actual.hex() != before[symbol]:
                raise GateFailure(
                    f"abrupt cleanup did not restore exact bytes at {symbol}"
                )
            after[symbol] = actual.hex()
        if second.request(b"D") != b"OK":
            raise GateFailure("final raw-RSP clean detach did not return OK")
        detach_ok = True
        return {
            "result": "PASS",
            "abrupt_disconnect_without_detach": True,
            "armed_breakpoint_count": len(inserted),
            "recovered_breakpoint_count": count,
            "exact_bytes_before": before,
            "exact_bytes_after": after,
            "clean_final_detach": detach_ok,
        }
    finally:
        second.close()


def resolve_default_gdb() -> Path:
    default_sdk = r"C:\vitasdk" if os.name == "nt" else "/usr/local/vitasdk"
    sdk = Path(os.environ.get("VITASDK", default_sdk))
    suffix = ".exe" if os.name == "nt" else ""
    return sdk / "bin" / f"arm-vita-eabi-gdb{suffix}"


def gdb_version(executable: Path) -> str:
    result = subprocess.run(
        [str(executable), "--version"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=10,
    )
    if result.returncode != 0:
        raise GateFailure("could not query VitaSDK GDB version")
    return result.stdout.splitlines()[0] if result.stdout else "unknown"


def close_failed_gdb(
    client: vfp.MiGdb, unresolved_mutation: bool, timeout: float
) -> None:
    if unresolved_mutation:
        # Sending D would resume an unverified context. Terminate only the host
        # transport and delegate recovery to the abandoned-client path.
        if client.process.poll() is None:
            client.process.kill()
            client.process.wait(timeout=min(timeout, 2.0))
        client.attached = False
        return
    client.emergency_close()


def run_gdb_phases(
    args: argparse.Namespace, evidence: dict[str, Any]
) -> tuple[LiveSymbols, int, int]:
    first = vfp.MiGdb(args.gdb, args.main_elf, args.timeout)
    first_progress = 0
    try:
        first.attach(args.host, args.port)
        symbols = resolve_live_symbols(first)
        evidence["observations"]["fixture_byte_preflight"] = {
            "result": "PASS",
            "checked_before_first_breakpoint": True,
            "bytes": preflight_fixture_bytes(first, symbols),
        }
        evidence["observations"]["hidden_breakpoint_step_over"] = (
            test_hidden_breakpoint_step_over(first, symbols)
        )
        evidence["observations"]["thumb_exclusive_step"] = test_exclusive_fixture(
            first,
            label="Thumb-2",
            site_symbol="thumb_step_exclusive_site",
            site_address=symbols.thumb_site,
            after_address=symbols.thumb_after,
            word_symbol="uvdb_thumb_exclusive_word",
            thumb=True,
        )
        evidence["observations"]["arm_exclusive_step"] = test_exclusive_fixture(
            first,
            label="A32",
            site_symbol="arm_step_exclusive_site",
            site_address=symbols.arm_site,
            after_address=symbols.arm_after,
            word_symbol="uvdb_arm_exclusive_word",
            thumb=False,
        )
        evidence["observations"]["register_transactions"] = (
            test_register_transactions(first)
        )
        require_no_breakpoints_gdb(first)
        first_progress = evaluate_u32(first, "test_value")
        first.detach()
        evidence["sessions"].append(
            {"kind": "stepping_register_clean_detach", **transcript_identity(first.transcript)}
        )
        first.quit()
    except BaseException as exc:
        evidence["sessions"].append(
            {"kind": "stepping_register_failure", **transcript_identity(first.transcript)}
        )
        # Individual transaction helpers restore before ordinary failures
        # propagate. An unresolved transaction must never send D.
        close_failed_gdb(first, isinstance(exc, UnrestoredMutation), args.timeout)
        raise

    time.sleep(args.recovery_seconds)
    second = vfp.MiGdb(args.gdb, args.main_elf, args.timeout)
    try:
        second.attach(args.host, args.port)
        require_no_breakpoints_gdb(second)
        second_progress = evaluate_u32(second, "test_value")
        if second_progress == first_progress:
            raise GateFailure("application did not progress across clean detach/reconnect")
        second.detach()
        evidence["sessions"].append(
            {"kind": "clean_reconnect_and_detach", **transcript_identity(second.transcript)}
        )
        second.quit()
        evidence["observations"]["clean_reconnect"] = {
            "result": "PASS",
            "software_breakpoints": 0,
            "progress_before": f"0x{first_progress:08x}",
            "progress_after": f"0x{second_progress:08x}",
            "application_progressed": True,
            "clean_detach_count": 2,
        }
        return symbols, first_progress, second_progress
    except BaseException:
        evidence["sessions"].append(
            {"kind": "clean_reconnect_failure", **transcript_identity(second.transcript)}
        )
        second.emergency_close()
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Validate practical ARM/Thumb stepping, selected-thread p/P, and "
            "abandoned-breakpoint cleanup against a running VitaDebugger target"
        )
    )
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1234)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--main-elf", "--elf", dest="main_elf", type=Path)
    source.add_argument("--symbol-script", type=Path)
    parser.add_argument("--gdb", type=Path, default=resolve_default_gdb())
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--recovery-seconds", type=float, default=1.0)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--vpk", type=Path)
    parser.add_argument("--kernel-plugin", type=Path)
    parser.add_argument("--firmware")
    parser.add_argument("--device-class")
    parser.add_argument("--kernel-abi")
    return parser


def validate_args(args: argparse.Namespace) -> None:
    try:
        vfp.validate_endpoint(args.host, args.port)
    except vfp.GateFailure as exc:
        raise GateFailure(str(exc)) from exc
    if not 0.5 <= args.timeout <= 120.0:
        raise GateFailure("timeout must be between 0.5 and 120 seconds")
    if not 0.1 <= args.recovery_seconds <= 10.0:
        raise GateFailure("recovery seconds must be between 0.1 and 10")
    if args.symbol_script is not None:
        args.symbol_script = args.symbol_script.resolve()
        args.main_elf = main_elf_from_symbol_script(
            args.symbol_script, args.host, args.port
        )
    else:
        args.main_elf = args.main_elf.resolve()
    for label, path in (
        ("main ELF", args.main_elf),
        ("GDB", args.gdb),
        ("VPK", args.vpk),
        ("kernel plugin", args.kernel_plugin),
    ):
        if path is not None and not path.is_file():
            raise GateFailure(f"{label} is not a readable regular file: {path}")


def initial_evidence(args: argparse.Namespace) -> dict[str, Any]:
    artifacts: dict[str, Any] = {"elf": file_identity(args.main_elf)}
    if args.vpk is not None:
        artifacts["vpk"] = file_identity(args.vpk)
    if args.kernel_plugin is not None:
        artifacts["kernel_plugin"] = file_identity(args.kernel_plugin)
    hardware = {
        key: value
        for key, value in (
            ("firmware", args.firmware),
            ("device_class", args.device_class),
            ("kernel_abi", args.kernel_abi),
        )
        if value
    }
    return {
        "schema": SCHEMA,
        "result": "RUNNING",
        "started_utc": utc_now(),
        "artifacts": artifacts,
        "gdb": {
            "version": gdb_version(args.gdb),
            "individual_register_packets_forced": True,
        },
        "hardware": hardware,
        "observations": {},
        "sessions": [],
        "safety": {
            "kernel_plugin_changed": False,
            "application_deployed_by_gate": False,
            "register_mutations_restored_before_resume_or_detach": False,
            "abrupt_disconnect_cleanup_verified": False,
        },
    }


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    evidence: dict[str, Any] | None = None
    try:
        validate_args(args)
        evidence = initial_evidence(args)
        symbols, _, _ = run_gdb_phases(args, evidence)
        evidence["safety"][
            "register_mutations_restored_before_resume_or_detach"
        ] = True
        evidence["observations"]["abrupt_disconnect_cleanup"] = (
            test_abrupt_disconnect_cleanup(
                args.host,
                args.port,
                args.timeout,
                args.recovery_seconds,
                symbols,
            )
        )
        evidence["safety"]["abrupt_disconnect_cleanup_verified"] = True
        evidence["result"] = "PASS"
        evidence["finished_utc"] = utc_now()
        print("PASS: practical GDB stepping/register hardware gate")
        return 0
    except BaseException as exc:
        if evidence is not None:
            evidence["result"] = "FAIL"
            evidence["finished_utc"] = utc_now()
            evidence["failure"] = {
                "type": type(exc).__name__,
                "message": (
                    "The live stepping/register gate did not complete. "
                    "Re-run it interactively for the detailed assertion."
                ),
            }
        print(f"FAIL: {exc}", file=sys.stderr)
        return 130 if isinstance(exc, KeyboardInterrupt) else 1
    finally:
        if args.evidence is not None and evidence is not None:
            write_evidence(args.evidence.resolve(), evidence)


if __name__ == "__main__":
    raise SystemExit(main())

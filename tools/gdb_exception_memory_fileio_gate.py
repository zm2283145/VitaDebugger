#!/usr/bin/env python3
"""Bounded hardware gate for exception, live-memory, and File-I/O safety.

This tool never deploys, launches, kills, resets, or unloads a Vita title. It
operates only on an already-running, explicitly identified diagnostic build.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import gdb_symbols as symbols


ABI_VERSION = 0x00010000
BUILD_PRIOR = 0x5052494F
BUILD_NULL_BASE = 0x4E554C00
MEMORY_SIZE = 192
MAX_PACKET_COUNT = 256
A32_BREAKPOINT = bytes.fromhex("f000f0e7")
SYMBOL_NAMES = (
    "uvdb_safety_gate_abi_version",
    "uvdb_safety_gate_build_kind",
    "uvdb_safety_gate_exception_request",
    "uvdb_safety_gate_exception_completed",
    "uvdb_safety_gate_exception_observed_type",
    "uvdb_safety_gate_exception_count",
    "uvdb_safety_gate_restart_count",
    "uvdb_safety_gate_fileio_request",
    "uvdb_safety_gate_fileio_completed",
    "uvdb_safety_gate_fileio_result",
    "uvdb_safety_gate_memory",
    "uvdb_safety_gate_copy_address",
    "uvdb_safety_gate_copy_size",
    "uvdb_safety_gate_copy_call_count",
    "uvdb_safety_gate_copy_fail_first",
    "uvdb_safety_gate_copy_fail_count",
)


class GateFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class Candidate:
    elf: Path
    vpk: Path
    elf_sha256: str
    vpk_sha256: str


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_candidate(
    elf: Path, vpk: Path, elf_sha256: str, vpk_sha256: str
) -> Candidate:
    candidate = Candidate(
        elf.resolve(), vpk.resolve(), elf_sha256.lower(), vpk_sha256.lower()
    )
    for path, expected in (
        (candidate.elf, candidate.elf_sha256),
        (candidate.vpk, candidate.vpk_sha256),
    ):
        if len(expected) != 64 or any(c not in "0123456789abcdef" for c in expected):
            raise GateFailure(f"invalid expected SHA-256 for {path}")
        if not path.is_file():
            raise GateFailure(f"candidate file is missing: {path}")
        actual = sha256_file(path)
        if actual != expected:
            raise GateFailure(
                f"candidate hash mismatch for {path}: expected {expected}, got {actual}"
            )
    return candidate


def _nm_symbols(elf: Path, nm: str) -> dict[str, int]:
    completed = subprocess.run(
        [nm, "-P", "-S", "--defined-only", str(elf)],
        check=False,
        capture_output=True,
        text=True,
        shell=False,
    )
    if completed.returncode:
        raise GateFailure(f"{nm} failed: {completed.stderr.strip()}")
    wanted = set(SYMBOL_NAMES)
    result: dict[str, int] = {}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) < 3 or fields[0] not in wanted:
            continue
        try:
            result[fields[0]] = int(fields[2], 16)
        except ValueError as exc:
            raise GateFailure(f"malformed nm address in {line!r}") from exc
    missing = sorted(wanted - result.keys())
    if missing:
        raise GateFailure(f"candidate ELF lacks gate symbols: {', '.join(missing)}")
    return result


def relocate_symbols(
    image: symbols.ElfImage,
    runtime: symbols.RuntimeModule,
    link_addresses: dict[str, int],
) -> dict[str, int]:
    if len(image.segments) != len(runtime.segments):
        raise GateFailure("local/live PT_LOAD counts differ")
    relocated: dict[str, int] = {}
    for name, address in link_addresses.items():
        owners = [
            index
            for index, segment in enumerate(image.segments)
            if segment.vaddr <= address < segment.vaddr + segment.memsz
        ]
        if len(owners) != 1:
            raise GateFailure(f"symbol {name} does not map to exactly one PT_LOAD")
        index = owners[0]
        relocated[name] = runtime.segments[index] + address - image.segments[index].vaddr
    return relocated


def connect_client(host: str, port: int, timeout: float, wait: float):
    client = symbols.RspClient(host, port, timeout, wait)
    try:
        client.negotiate()
        stop = client.request(b"?")
        if not stop.startswith((b"T", b"S")):
            raise GateFailure(f"expected stopped target, got {stop[:80]!r}")
        return client, stop.decode("ascii")
    except Exception:
        client.close()
        raise


def read_memory(client, address: int, size: int) -> bytes:
    reply = client.request(f"m{address:x},{size:x}".encode("ascii"))
    if reply.startswith(b"E") or len(reply) != size * 2:
        raise GateFailure(f"memory read failed at 0x{address:08x}: {reply[:80]!r}")
    try:
        return bytes.fromhex(reply.decode("ascii"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise GateFailure("memory reply was not exact hexadecimal data") from exc


def write_memory(client, address: int, data: bytes, expected: bytes = b"OK") -> bytes:
    packet = f"M{address:x},{len(data):x}:".encode("ascii") + data.hex().encode("ascii")
    reply = client.request(packet)
    if reply != expected:
        raise GateFailure(
            f"memory write at 0x{address:08x} returned {reply!r}, expected {expected!r}"
        )
    return reply


def read_u32(client, address: int) -> int:
    return int.from_bytes(read_memory(client, address, 4), "little")


def write_u32(client, address: int, value: int) -> None:
    write_memory(client, address, value.to_bytes(4, "little"))


def send_continue(client) -> None:
    if client.ack_mode:
        raise GateFailure("gate requires negotiated no-ack mode")
    client.socket.sendall(symbols._rsp_frame(b"c"))


def wait_packet(client, deadline: float) -> bytes:
    for _ in range(MAX_PACKET_COUNT):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise GateFailure("timed out waiting for target packet")
        client.socket.settimeout(remaining)
        packet = client.read_packet()
        is_console = (
            packet.startswith(b"O")
            and len(packet) % 2 == 1
            and all(value in b"0123456789abcdefABCDEF" for value in packet[1:])
        )
        if is_console:
            continue
        return packet
    raise GateFailure("target exceeded packet-count bound")


def close_client(client) -> None:
    try:
        client.close()
    except OSError:
        pass


def resolve_live(
    client, candidate: Candidate, nm: str
) -> tuple[dict[str, int], dict[str, object]]:
    offsets = symbols.parse_qoffsets(client.request(b"qOffsets"))
    modules = symbols.parse_library_xml(client.read_library_xml())
    snapshot = symbols.TargetSnapshot(offsets, modules)
    image = symbols._read_elf(candidate.elf)
    runtime = symbols.reconcile_main(image, snapshot)
    addresses = relocate_symbols(image, runtime, _nm_symbols(candidate.elf, nm))
    return addresses, {
        "stop_offsets": {
            "style": offsets.style,
            "text": f"0x{offsets.text:08x}",
            "data": None if offsets.data is None else f"0x{offsets.data:08x}",
        },
        "main_module": runtime.name,
        "segments": [f"0x{value:08x}" for value in runtime.segments],
    }


def reconnect_verified(
    host: str,
    port: int,
    timeout: float,
    wait: float,
    candidate: Candidate,
    nm: str,
    expected_address: dict[str, int],
    expected_runtime: dict[str, object],
    expected_kind: int,
):
    client, stop = connect_client(host, port, timeout, wait)
    try:
        address, runtime = resolve_live(client, candidate, nm)
        if address != expected_address or runtime != expected_runtime:
            raise GateFailure(
                "target identity or ASLR mapping changed across reconnect"
            )
        abi = read_u32(client, address["uvdb_safety_gate_abi_version"])
        kind = read_u32(client, address["uvdb_safety_gate_build_kind"])
        if abi != ABI_VERSION or kind != expected_kind:
            raise GateFailure(
                f"fixture identity changed across reconnect: abi=0x{abi:08x}, "
                f"kind=0x{kind:08x}"
            )
        return client, stop
    except Exception:
        close_client(client)
        raise


def configure_failure(client, address: dict[str, int], first: int, count: int) -> None:
    write_u32(client, address["uvdb_safety_gate_copy_fail_first"], 0)
    write_u32(client, address["uvdb_safety_gate_copy_call_count"], 0)
    write_u32(client, address["uvdb_safety_gate_copy_fail_count"], count)
    write_u32(client, address["uvdb_safety_gate_copy_fail_first"], first)


def require_armed_breakpoint(memory: bytes, offset: int) -> None:
    if memory[offset : offset + len(A32_BREAKPOINT)] != A32_BREAKPOINT:
        raise GateFailure("software breakpoint was not re-armed after live write")


def expect_disconnect(client, timeout: float) -> str:
    try:
        packet = wait_packet(client, time.monotonic() + timeout)
    except symbols.ProtocolError as exc:
        if "connection closed unexpectedly" not in str(exc):
            raise
        return "eof"
    except ConnectionResetError:
        return "connection_reset"
    except socket.timeout as exc:
        raise GateFailure("timed out instead of observing peer disconnect") from exc
    raise GateFailure(f"expected disconnect, got packet {packet[:80]!r}")


def run_memory_gate(client, address: dict[str, int]) -> list[dict[str, object]]:
    target = address["uvdb_safety_gate_memory"]
    write_u32(client, address["uvdb_safety_gate_copy_address"], target)
    write_u32(client, address["uvdb_safety_gate_copy_size"], MEMORY_SIZE)
    configure_failure(client, address, 0, 0)
    original = read_memory(client, target, MEMORY_SIZE)
    phases: list[dict[str, object]] = []

    committed = bytes((index * 17 + 3) & 0xFF for index in range(MEMORY_SIZE))
    write_memory(client, target, committed)
    if read_memory(client, target, MEMORY_SIZE) != committed:
        raise GateFailure("multi-chunk committed bytes do not match")
    phases.append({"name": "multi_chunk_commit", "status": "pass"})

    overlap = bytes((index * 29 + 7) & 0xFF for index in range(MEMORY_SIZE))
    breakpoint = target + 80
    if client.request(f"Z0,{breakpoint:x},4".encode("ascii")) != b"OK":
        raise GateFailure("could not arm overlap software breakpoint")
    write_memory(client, target, overlap)
    armed = read_memory(client, target, MEMORY_SIZE)
    if armed[:80] != overlap[:80] or armed[84:] != overlap[84:]:
        raise GateFailure("non-breakpoint bytes differ while overlap point is armed")
    require_armed_breakpoint(armed, 80)
    if client.request(f"z0,{breakpoint:x},4".encode("ascii")) != b"OK":
        raise GateFailure("could not remove overlap software breakpoint")
    if read_memory(client, target, MEMORY_SIZE) != overlap:
        raise GateFailure("breakpoint removal did not reveal committed bytes")
    phases.append({"name": "software_breakpoint_overlap", "status": "pass"})

    write_memory(client, target, original)
    transient = bytes((index * 11 + 0x31) & 0xFF for index in range(MEMORY_SIZE))
    configure_failure(client, address, 2, 1)
    write_memory(client, target, transient, b"E0e")
    if read_memory(client, target, MEMORY_SIZE) != original:
        raise GateFailure("transient failure did not restore original bytes")
    phases.append({
        "name": "partial_failure_immediate_rollback",
        "status": "pass",
        "copy_calls": read_u32(client, address["uvdb_safety_gate_copy_call_count"]),
    })

    configure_failure(client, address, 2, 3)
    retained = bytes((index * 7 + 0x53) & 0xFF for index in range(MEMORY_SIZE))
    packet = (
        f"M{target:x},{MEMORY_SIZE:x}:".encode("ascii")
        + retained.hex().encode("ascii")
    )
    client.socket.sendall(symbols._rsp_frame(packet))
    disconnect = expect_disconnect(client, 3.0)
    close_client(client)
    phases.append({
        "name": "partial_failure_disconnect",
        "status": "observed",
        "disconnect": disconnect,
    })
    return phases


def complete_disconnect_retry(
    client, address: dict[str, int]
) -> dict[str, object]:
    calls = read_u32(client, address["uvdb_safety_gate_copy_call_count"])
    if calls < 4:
        raise GateFailure(f"disconnect cleanup did not consume expected failures: {calls}")
    if client.request(b"D") != b"OK":
        raise GateFailure("detach retry did not report OK")
    close_client(client)
    return {"name": "disconnect_detach_retry", "status": "pass", "copy_calls": calls}


def run_fileio_gate(client, address: dict[str, int], sequence: int) -> dict[str, object]:
    write_u32(client, address["uvdb_safety_gate_fileio_request"], sequence)
    send_continue(client)
    request = wait_packet(client, time.monotonic() + 5.0)
    if not request.startswith(b"Fwrite,"):
        raise GateFailure(f"expected one File-I/O request, got {request[:120]!r}")
    reply = client.request(b"F-1,4,C")
    if not reply.startswith(b"T02"):
        raise GateFailure(f"File-I/O Ctrl-C did not produce T02: {reply[:80]!r}")
    registers = client.request(b"g")
    register = client.request(b"p0")
    if registers.startswith(b"E") or len(registers) < 16:
        raise GateFailure("full register inspection failed during File-I/O stop")
    if register.startswith(b"E") or len(register) != 8:
        raise GateFailure("single register inspection failed during File-I/O stop")
    if read_u32(client, address["uvdb_safety_gate_fileio_completed"]) == sequence:
        raise GateFailure("File-I/O completed before explicit resume")
    send_continue(client)
    time.sleep(0.25)
    client.socket.sendall(b"\x03")
    stop = wait_packet(client, time.monotonic() + 5.0)
    if not stop.startswith(b"T02"):
        raise GateFailure(f"post-resume Ctrl-C did not stop coherently: {stop[:80]!r}")
    if read_u32(client, address["uvdb_safety_gate_fileio_completed"]) != sequence:
        raise GateFailure("File-I/O worker did not complete after explicit resume")
    result = int.from_bytes(
        read_memory(client, address["uvdb_safety_gate_fileio_result"], 4),
        "little", signed=True,
    )
    if result != -1:
        raise GateFailure(f"interrupted File-I/O result was {result}, expected -1")
    return {
        "name": "fileio_ctrl_c",
        "status": "pass",
        "request": request.decode("ascii", "replace"),
        "stop": stop.decode("ascii", "replace"),
        "full_register_reply_bytes": len(registers) // 2,
        "single_register_reply": register.decode("ascii"),
    }


def run_predecessor_attempt(
    client,
    address: dict[str, int],
    exception_type: int,
    sequence: int,
) -> str:
    request = (sequence << 8) | (exception_type + 1)
    write_u32(
        client,
        address["uvdb_safety_gate_exception_request"],
        request,
    )
    send_continue(client)
    disconnect = expect_disconnect(client, 3.0)
    close_client(client)
    return disconnect


def verify_predecessor_attempt(
    client,
    address: dict[str, int],
    exception_type: int,
    expected_count: int,
    sequence: int,
    disconnect: str,
) -> dict[str, object]:
    observed = read_u32(
        client, address["uvdb_safety_gate_exception_observed_type"]
    )
    count = read_u32(
        client,
        address["uvdb_safety_gate_exception_count"] + exception_type * 4,
    )
    completed = read_u32(
        client, address["uvdb_safety_gate_exception_completed"]
    )
    if observed != exception_type or count != expected_count:
        raise GateFailure(
            f"predecessor type/count mismatch: observed={observed}, count={count}"
        )
    expected_completed = (sequence << 8) | (exception_type + 1)
    if completed != expected_completed:
        raise GateFailure(f"fixture did not complete request 0x{expected_completed:08x}")
    return {
        "type": exception_type,
        "count": count,
        "observed_type": observed,
        "completed": completed,
        "disconnect": disconnect,
    }


def detach(client) -> None:
    if client.request(b"D") != b"OK":
        raise GateFailure("final detach failed")
    close_client(client)


def write_evidence(path: Path, evidence: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--vpk", type=Path, required=True)
    parser.add_argument("--elf-sha256", required=True)
    parser.add_argument("--vpk-sha256", required=True)
    parser.add_argument("--nm", default="arm-vita-eabi-nm")
    parser.add_argument(
        "--mode",
        choices=("memory-fileio", "predecessor"),
        required=True,
    )
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--connect-wait", type=float, default=10.0)
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    started = time.time()
    evidence: dict[str, object] = {
        "format": "VITADEBUGGER-EXCEPTION-MEMORY-FILEIO-GATE-1",
        "mode": args.mode,
        "started_unix": started,
        "status": "failed",
    }
    client = None
    try:
        candidate = require_candidate(
            args.elf, args.vpk, args.elf_sha256, args.vpk_sha256
        )
        evidence["candidate"] = {
            "elf": str(candidate.elf),
            "vpk": str(candidate.vpk),
            "elf_sha256": candidate.elf_sha256,
            "vpk_sha256": candidate.vpk_sha256,
        }
        client, stop = connect_client(
            args.host, args.port, args.timeout, args.connect_wait
        )
        address, runtime = resolve_live(client, candidate, args.nm)
        evidence["initial_stop"] = stop
        evidence["runtime"] = runtime
        evidence["resolved_symbols"] = {
            name: f"0x{value:08x}" for name, value in sorted(address.items())
        }
        abi = read_u32(client, address["uvdb_safety_gate_abi_version"])
        kind = read_u32(client, address["uvdb_safety_gate_build_kind"])
        if abi != ABI_VERSION:
            raise GateFailure(f"fixture ABI mismatch: 0x{abi:08x}")

        if args.mode == "memory-fileio":
            if kind != BUILD_PRIOR:
                raise GateFailure("memory/File-I/O gate requires predecessor candidate")
            original = read_memory(
                client, address["uvdb_safety_gate_memory"], MEMORY_SIZE
            )
            phases = run_memory_gate(client, address)
            client = None
            client, _ = reconnect_verified(
                args.host, args.port, args.timeout, args.connect_wait,
                candidate, args.nm, address, runtime, kind,
            )
            phases.append(complete_disconnect_retry(client, address))
            client = None
            client, _ = reconnect_verified(
                args.host, args.port, args.timeout, args.connect_wait,
                candidate, args.nm, address, runtime, kind,
            )
            if read_memory(
                client, address["uvdb_safety_gate_memory"], MEMORY_SIZE
            ) != original:
                raise GateFailure("disconnect retry did not restore original bytes")
            phases.append(run_fileio_gate(client, address, 1))
            detach(client)
            client = None
            evidence["phases"] = phases
        else:
            if kind != BUILD_PRIOR:
                raise GateFailure(
                    "NULL candidate is a manual exit-only gate; this tool will not "
                    "misclassify loss of RSP as proof of orderly process exit"
                )
            results = []
            sequence = 0
            for exception_type in range(3):
                for expected_count in (1, 2):
                    sequence += 1
                    disconnect = run_predecessor_attempt(
                        client, address, exception_type, sequence
                    )
                    client = None
                    client, _ = reconnect_verified(
                        args.host, args.port, args.timeout, args.connect_wait,
                        candidate, args.nm, address, runtime, kind,
                    )
                    results.append(
                        verify_predecessor_attempt(
                            client, address, exception_type, expected_count,
                            sequence, disconnect,
                        )
                    )
            detach(client)
            client = None
            evidence["predecessor_observations"] = results

        evidence["status"] = "pass"
        evidence["completed_unix"] = time.time()
        write_evidence(args.evidence, evidence)
        print(f"PASS: wrote {args.evidence}")
        return 0
    except Exception as exc:
        evidence["error"] = str(exc)
        evidence["completed_unix"] = time.time()
        write_evidence(args.evidence, evidence)
        print(f"HARD STOP: {exc}", file=sys.stderr)
        return 1
    finally:
        if client is not None:
            close_client(client)


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Validate VitaDebugger's foreign-thread VFP path with a real VitaSDK GDB.

The diagnostic target must be built with UVDB_GDB_VFP_FIXTURE=1.  This driver
uses GDB/MI instead of implementing a second RSP client, so a pass covers the
same target-description, thread-selection, and register-decoding path used by
an interactive GDB session.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any


FIXTURE_NAME = "GDB VFP fixture"
EXPECTED_D0 = Decimal(1)
EXPECTED_D31 = Decimal(2)
EXPECTED_FPSCR = 0x00400000
EVIDENCE_SCHEMA = "vitadebugger-vfp-live-gdb-v2"
MAX_PORTABLE_METADATA_LENGTH = 64
PORTABLE_METADATA_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+ \-]{0,63}")
SAFE_HOST = re.compile(r"[A-Za-z0-9.-]+\Z")
MAX_MI_LINE_CHARACTERS = 64 * 1024
MAX_MI_OUTPUT_BYTES = 2 * 1024 * 1024


class GateFailure(RuntimeError):
    """A fail-closed lifecycle or register assertion."""


def validate_endpoint(host: str, port: int) -> None:
    """Reject text that could change the generated GDB/MI command."""
    if not SAFE_HOST.fullmatch(host):
        raise GateFailure("host must be a plain IPv4 address or DNS hostname")
    if not 1 <= port <= 65535:
        raise GateFailure("port must be in the range 1-65535")


def mi_quote(value: str) -> str:
    """Encode a string using the C-string subset accepted by GDB/MI."""
    return json.dumps(value)


def decode_mi_string(value: str) -> str:
    """Decode one complete quoted GDB/MI C string."""
    try:
        decoded = ast.literal_eval(value)
    except (SyntaxError, ValueError) as exc:
        raise GateFailure(f"invalid GDB/MI string: {value[:80]!r}") from exc
    if not isinstance(decoded, str):
        raise GateFailure("GDB/MI value was not a string")
    return decoded


def mi_string_field(record: str, field_name: str) -> str:
    pattern = rf"(?:^|[,{{]){re.escape(field_name)}=(\"(?:\\.|[^\"\\])*\")"
    match = re.search(pattern, record)
    if not match:
        raise GateFailure(f"GDB/MI response omitted {field_name}: {record[:160]}")
    return decode_mi_string(match.group(1))


def mi_result_class(record: str, token: int) -> str:
    match = re.match(rf"{token}\^([A-Za-z-]+)(?:,|$)", record)
    if not match:
        raise GateFailure(f"malformed GDB/MI result for token {token}: {record}")
    return match.group(1)


def extract_mi_objects(record: str, list_name: str) -> list[str]:
    """Return balanced object records from a named MI list."""
    marker = f"{list_name}=["
    position = record.find(marker)
    if position < 0:
        raise GateFailure(f"GDB/MI response omitted {list_name}")
    position += len(marker)
    objects: list[str] = []
    object_start: int | None = None
    depth = 0
    in_string = False
    escaped = False
    while position < len(record):
        char = record[position]
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char == "{":
            if depth == 0:
                object_start = position
            depth += 1
        elif char == "}":
            depth -= 1
            if depth < 0:
                raise GateFailure(f"unbalanced {list_name} object list")
            if depth == 0 and object_start is not None:
                objects.append(record[object_start:position + 1])
                object_start = None
        elif char == "]" and depth == 0:
            return objects
        position += 1
    raise GateFailure(f"unterminated GDB/MI {list_name} list")


def split_mi_top_level_fields(object_record: str) -> list[str]:
    """Split an MI tuple without treating nested frame fields as thread fields."""
    if len(object_record) < 2 or object_record[0] != "{" or object_record[-1] != "}":
        raise GateFailure(f"invalid GDB/MI tuple: {object_record[:80]!r}")
    fields: list[str] = []
    start = 1
    depth = 0
    in_string = False
    escaped = False
    for position in range(1, len(object_record) - 1):
        char = object_record[position]
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
        elif char in "{[(":
            depth += 1
        elif char in "}])":
            depth -= 1
            if depth < 0:
                raise GateFailure("unbalanced nested GDB/MI tuple")
        elif char == "," and depth == 0:
            fields.append(object_record[start:position])
            start = position + 1
    if in_string or depth != 0:
        raise GateFailure("unterminated nested GDB/MI tuple")
    fields.append(object_record[start:-1])
    return fields


def mi_top_level_string_field(object_record: str, field_name: str) -> str:
    for field in split_mi_top_level_fields(object_record):
        key, separator, value = field.partition("=")
        if separator and key == field_name:
            if not re.fullmatch(r'"(?:\\.|[^"\\])*"', value):
                raise GateFailure(
                    f"GDB/MI top-level {field_name} was not a string"
                )
            return decode_mi_string(value)
    raise GateFailure(f"GDB/MI tuple omitted top-level {field_name}")


@dataclass(frozen=True)
class ThreadSelection:
    fixture_gdb_id: int
    stopped_gdb_id: int
    target_id: str


def parse_fixture_thread(record: str) -> ThreadSelection:
    current_match = re.search(r'(?:^|,)current-thread-id="([0-9]+)"', record)
    if not current_match:
        raise GateFailure("GDB did not report a current stopped thread")
    current_id = int(current_match.group(1), 10)

    candidates: list[tuple[int, str]] = []
    for item in extract_mi_objects(record, "threads"):
        name = ""
        for field_name in ("name", "details"):
            try:
                name = mi_top_level_string_field(item, field_name)
                break
            except GateFailure:
                pass
        if name != FIXTURE_NAME:
            continue
        gdb_id = int(mi_top_level_string_field(item, "id"), 10)
        target_id = mi_top_level_string_field(item, "target-id")
        candidates.append((gdb_id, target_id))
    if len(candidates) != 1:
        raise GateFailure(
            f"expected exactly one {FIXTURE_NAME!r} thread, found {len(candidates)}"
        )
    fixture_id, target_id = candidates[0]
    if fixture_id == current_id:
        raise GateFailure(
            "fixture was the exception/stopped thread; foreign-thread path was not tested"
        )
    return ThreadSelection(fixture_id, current_id, target_id)


def parse_numeric(value: str) -> Decimal:
    text = value.strip()
    try:
        return Decimal(int(text, 0))
    except ValueError:
        try:
            return Decimal(text)
        except InvalidOperation as exc:
            raise GateFailure(f"register value is not a finite number: {value!r}") from exc


def parse_integer(value: str) -> int:
    number = parse_numeric(value)
    if not number.is_finite() or number != number.to_integral_value():
        raise GateFailure(f"expected an integer, got {value!r}")
    return int(number)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def transcript_sha256(lines: list[str]) -> str:
    """Hash one MI transcript without publishing its machine-local content."""
    encoded = json.dumps(
        lines, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def record_session(
    evidence: dict[str, Any],
    kind: str,
    transcript: list[str],
    include_sensitive_transcript: bool,
) -> None:
    """Record portable session evidence, with raw MI only by explicit opt-in."""
    evidence["sessions"].append({
        "kind": kind,
        "mi_record_count": len(transcript),
        "mi_sha256": transcript_sha256(transcript),
    })
    if include_sensitive_transcript:
        evidence["sensitive"]["sessions"].append({
            "kind": kind,
            "mi": list(transcript),
        })


def record_failure(
    evidence: dict[str, Any],
    error: BaseException,
    include_sensitive_transcript: bool,
) -> None:
    """Keep default failure evidence useful without copying arbitrary GDB text."""
    evidence["failure"] = {
        "type": type(error).__name__,
        "message": (
            "The live GDB lifecycle gate did not complete. Re-run it "
            "interactively for diagnostics."
        ),
    }
    if include_sensitive_transcript:
        evidence["sensitive"]["error"] = str(error)


@dataclass
class MiGdb:
    executable: Path
    elf: Path
    timeout: float
    process: subprocess.Popen[str] = field(init=False)
    lines: queue.Queue[str | GateFailure | None] = field(
        init=False, default_factory=queue.Queue
    )
    transcript: list[str] = field(init=False, default_factory=list)
    token: int = field(init=False, default=0)
    attached: bool = field(init=False, default=False)

    def __post_init__(self) -> None:
        self.process = subprocess.Popen(
            [
                str(self.executable),
                "-nx",
                "-q",
                "--interpreter=mi2",
                str(self.elf),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        threading.Thread(target=self._reader, daemon=True).start()
        try:
            self._wait_for_prompt()
            self.command("-gdb-set confirm off")
            self.command("-gdb-set pagination off")
            self.command("-gdb-set mi-async on")
        except Exception:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=min(self.timeout, 2.0))
            raise

    def _reader(self) -> None:
        assert self.process.stdout is not None
        total_bytes = 0
        while True:
            line = self.process.stdout.readline(MAX_MI_LINE_CHARACTERS + 1)
            if not line:
                break
            if len(line) > MAX_MI_LINE_CHARACTERS:
                self.lines.put(GateFailure(
                    "GDB/MI line exceeded the configured safety limit"
                ))
                if self.process.poll() is None:
                    self.process.kill()
                return
            total_bytes += len(line.encode("utf-8", errors="replace"))
            if total_bytes > MAX_MI_OUTPUT_BYTES:
                self.lines.put(GateFailure(
                    "GDB/MI output exceeded the configured safety limit"
                ))
                if self.process.poll() is None:
                    self.process.kill()
                return
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)

    def _next_line(self, deadline: float) -> str:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise GateFailure("timed out waiting for GDB/MI")
        try:
            line = self.lines.get(timeout=remaining)
        except queue.Empty as exc:
            raise GateFailure("timed out waiting for GDB/MI") from exc
        if isinstance(line, GateFailure):
            raise line
        if line is None:
            code = self.process.poll()
            raise GateFailure(f"GDB exited unexpectedly (status {code})")
        self.transcript.append(line)
        return line

    def _wait_for_prompt(self) -> None:
        deadline = time.monotonic() + self.timeout
        while self._next_line(deadline) != "(gdb) ":
            pass

    def command(self, command: str) -> tuple[str, list[str]]:
        if self.process.poll() is not None:
            raise GateFailure("cannot send a command to an exited GDB")
        self.token += 1
        token = self.token
        assert self.process.stdin is not None
        self.transcript.append(f"> {token}{command}")
        self.process.stdin.write(f"{token}{command}\n")
        self.process.stdin.flush()
        deadline = time.monotonic() + self.timeout
        records: list[str] = []
        while True:
            line = self._next_line(deadline)
            if line.startswith(f"{token}^"):
                result_class = mi_result_class(line, token)
                if result_class == "error":
                    try:
                        message = mi_string_field(line, "msg")
                    except GateFailure:
                        message = line
                    raise GateFailure(f"GDB command {command!r} failed: {message}")
                return line, records
            records.append(line)

    def wait_for(self, prefix: str) -> str:
        deadline = time.monotonic() + self.timeout
        while True:
            line = self._next_line(deadline)
            if line.startswith(prefix):
                return line

    def drain(self, seconds: float = 0.1) -> list[str]:
        drained: list[str] = []
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return drained
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty:
                return drained
            if isinstance(line, GateFailure):
                raise line
            if line is None:
                return drained
            self.transcript.append(line)
            drained.append(line)

    def attach(self, host: str, port: int) -> None:
        result, _ = self.command(f"-target-select remote {host}:{port}")
        result_class = mi_result_class(result, self.token)
        if result_class not in ("connected", "done"):
            raise GateFailure(f"unexpected attach result: {result}")
        self.attached = True
        self.drain()

    def evaluate(self, expression: str) -> str:
        result, _ = self.command(
            f"-data-evaluate-expression {mi_quote(expression)}"
        )
        return mi_string_field(result, "value")

    def detach(self) -> None:
        result, _ = self.command("-target-detach")
        if mi_result_class(result, self.token) != "done":
            raise GateFailure(f"unexpected detach result: {result}")
        self.attached = False

    def disconnect_transport(self) -> None:
        """Close RSP without sending `D`, while allowing GDB to exit cleanly."""
        result, _ = self.command("-target-disconnect")
        if mi_result_class(result, self.token) != "done":
            raise GateFailure(f"unexpected transport-disconnect result: {result}")
        self.attached = False

    def quit(self) -> None:
        if self.process.poll() is not None:
            return
        if self.attached:
            self.detach()
        try:
            self.command("-gdb-exit")
        except GateFailure:
            self.process.kill()
        self.process.wait(timeout=self.timeout)

    def emergency_close(self) -> None:
        """Best-effort cleanup; peer loss leaves the kernel watchdog as backup."""
        if self.process.poll() is None:
            try:
                if self.attached:
                    self.detach()
                self.command("-gdb-exit")
                self.process.wait(timeout=min(self.timeout, 2.0))
                return
            except (GateFailure, OSError, subprocess.TimeoutExpired):
                if self.process.poll() is None:
                    self.process.kill()
                self.process.wait(timeout=min(self.timeout, 2.0))
        self.attached = False


def check_snapshot(client: MiGdb, phase: str) -> dict[str, Any]:
    thread_info, _ = client.command("-thread-info")
    selection = parse_fixture_thread(thread_info)
    select_result, _ = client.command(
        f"-thread-select {selection.fixture_gdb_id}"
    )
    if mi_result_class(select_result, client.token) != "done":
        raise GateFailure(f"{phase}: failed to select fixture thread")

    d0_text = client.evaluate("$d0")
    d31_text = client.evaluate("$d31")
    fpscr_text = client.evaluate("$fpscr")
    progress_text = client.evaluate("test_value")
    d0 = parse_numeric(d0_text)
    d31 = parse_numeric(d31_text)
    fpscr = parse_integer(fpscr_text)
    progress = parse_integer(progress_text)
    if not d0.is_finite() or d0 != EXPECTED_D0:
        raise GateFailure(f"{phase}: D0 was {d0_text!r}, expected 1")
    if not d31.is_finite() or d31 != EXPECTED_D31:
        raise GateFailure(f"{phase}: D31 was {d31_text!r}, expected 2")
    if fpscr != EXPECTED_FPSCR:
        raise GateFailure(
            f"{phase}: FPSCR was 0x{fpscr:08x}, expected 0x{EXPECTED_FPSCR:08x}"
        )

    snapshot = {
        "phase": phase,
        "fixture_gdb_thread": selection.fixture_gdb_id,
        "initial_stopped_gdb_thread": selection.stopped_gdb_id,
        "fixture_is_foreign": selection.fixture_gdb_id != selection.stopped_gdb_id,
        "d0": d0_text,
        "d31": d31_text,
        "fpscr": f"0x{fpscr:08x}",
        "progress": progress,
    }
    print(
        f"{phase}: foreign thread {selection.fixture_gdb_id} "
        f"({selection.target_id}), D0=1 D31=2 FPSCR=0x{fpscr:08x}, "
        f"progress={progress}"
    )
    return snapshot


def require_progress(before: dict[str, Any], after: dict[str, Any], label: str) -> None:
    if before["progress"] == after["progress"]:
        raise GateFailure(
            f"{label}: application progress did not change "
            f"({before['progress']})"
        )


def continue_and_interrupt(client: MiGdb, run_seconds: float) -> None:
    result, records = client.command("-exec-continue")
    if mi_result_class(result, client.token) != "running":
        raise GateFailure(f"unexpected continue result: {result}")
    time.sleep(run_seconds)
    result, records_after_interrupt = client.command("-exec-interrupt --all")
    if mi_result_class(result, client.token) != "done":
        raise GateFailure(f"unexpected interrupt result: {result}")
    if not any(line.startswith("*stopped") for line in
               records + records_after_interrupt):
        client.wait_for("*stopped")


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
        raise GateFailure(f"could not query GDB version: {result.stderr.strip()}")
    return result.stdout.splitlines()[0] if result.stdout else "unknown"


def write_evidence(path: Path, evidence: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def run_gate(args: argparse.Namespace, evidence: dict[str, Any]) -> None:
    snapshots: list[dict[str, Any]] = evidence["snapshots"]

    first = MiGdb(args.gdb, args.elf, args.timeout)
    try:
        first.attach(args.host, args.port)
        initial = check_snapshot(first, "initial_attach")
        snapshots.append(initial)
        continue_and_interrupt(first, args.run_seconds)
        interrupted = check_snapshot(first, "after_continue_interrupt")
        snapshots.append(interrupted)
        require_progress(initial, interrupted, "continue/interrupt")
        first.detach()
        record_session(
            evidence, "clean_detach", first.transcript,
            args.include_sensitive_transcript,
        )
        first.quit()
    except BaseException:
        record_session(
            evidence, "first_session_failure", first.transcript,
            args.include_sensitive_transcript,
        )
        first.emergency_close()
        raise

    time.sleep(args.recovery_seconds)
    second = MiGdb(args.gdb, args.elf, args.timeout)
    try:
        second.attach(args.host, args.port)
        clean_reconnect = check_snapshot(second, "after_clean_detach_reconnect")
        snapshots.append(clean_reconnect)
        require_progress(interrupted, clean_reconnect, "clean detach/reconnect")
        second.disconnect_transport()
        record_session(
            evidence, "stopped_transport_disconnect_no_detach",
            second.transcript, args.include_sensitive_transcript,
        )
        second.quit()
    except BaseException:
        record_session(
            evidence, "second_session_failure", second.transcript,
            args.include_sensitive_transcript,
        )
        second.emergency_close()
        raise

    time.sleep(args.recovery_seconds)
    third = MiGdb(args.gdb, args.elf, args.timeout)
    try:
        third.attach(args.host, args.port)
        abrupt_reconnect = check_snapshot(third, "after_abrupt_peer_reconnect")
        snapshots.append(abrupt_reconnect)
        require_progress(clean_reconnect, abrupt_reconnect,
                         "abrupt peer recovery/reconnect")
        third.detach()
        record_session(
            evidence, "final_clean_detach", third.transcript,
            args.include_sensitive_transcript,
        )
        third.quit()
    except BaseException:
        record_session(
            evidence, "third_session_failure", third.transcript,
            args.include_sensitive_transcript,
        )
        third.emergency_close()
        raise


def resolve_default_gdb() -> Path:
    default_sdk = r"C:\vitasdk" if os.name == "nt" else "/usr/local/vitasdk"
    vita_sdk = Path(os.environ.get("VITASDK", default_sdk))
    suffix = ".exe" if os.name == "nt" else ""
    return vita_sdk / "bin" / f"arm-vita-eabi-gdb{suffix}"


def validate_portable_metadata(option: str, value: str | None) -> str | None:
    """Validate a short label that cannot carry a local filesystem path."""
    if value is None:
        return None
    if (
        not isinstance(value, str)
        or value != value.strip()
        or len(value) > MAX_PORTABLE_METADATA_LENGTH
        or not PORTABLE_METADATA_RE.fullmatch(value)
    ):
        raise GateFailure(
            f"{option} must be 1-{MAX_PORTABLE_METADATA_LENGTH} ASCII "
            "letters, digits, spaces, '.', '_', '+', or '-'"
        )
    return value


def portable_file_identity(option: str, path: Path | None) -> dict[str, Any] | None:
    """Return a publishable file identity without retaining its input path."""
    if path is None:
        return None
    try:
        if not path.is_file():
            raise GateFailure(f"{option} must name an existing regular file")
        return {
            "sha256": sha256_file(path),
            "size_bytes": path.stat().st_size,
        }
    except OSError as exc:
        raise GateFailure(f"{option} must name a readable regular file") from exc


def portable_hardware_metadata(args: argparse.Namespace) -> dict[str, Any] | None:
    """Build optional portable hardware/build metadata from CLI arguments."""
    metadata: dict[str, Any] = {}
    for attribute, output_name, option in (
        ("device_class", "device_class", "--device-class"),
        ("firmware", "firmware", "--firmware"),
        ("kernel_abi", "kernel_abi", "--kernel-abi"),
    ):
        value = validate_portable_metadata(option, getattr(args, attribute, None))
        if value is not None:
            metadata[output_name] = value
    for attribute, output_name, option in (
        ("kernel_plugin", "kernel_plugin", "--kernel-plugin"),
        ("vpk", "vpk", "--vpk"),
    ):
        identity = portable_file_identity(
            option, getattr(args, attribute, None)
        )
        if identity is not None:
            metadata[output_name] = identity
    return metadata or None


def create_evidence(
    args: argparse.Namespace,
    version: str,
    started_utc: str | None = None,
) -> dict[str, Any]:
    """Create the publishable report shell before the live gate begins."""
    evidence: dict[str, Any] = {
        "schema": EVIDENCE_SCHEMA,
        "started_utc": started_utc or datetime.now(timezone.utc).isoformat(),
        "elf": {
            "sha256": sha256_file(args.elf),
            "size_bytes": args.elf.stat().st_size,
        },
        "gdb": {"version": version},
        "expected": {"d0": "1", "d31": "2", "fpscr": "0x00400000"},
        "snapshots": [],
        "sessions": [],
        "watchdog_scope": (
            "Peer-loss recovery is tested here. Abandoned kernel-lease expiry "
            "remains the separate guarded kernel-probe gate."
        ),
    }
    if args.include_sensitive_transcript:
        evidence["sensitive"] = {
            "host": args.host,
            "port": args.port,
            "elf_path": str(args.elf),
            "gdb_path": str(args.gdb),
            "sessions": [],
        }
    hardware = portable_hardware_metadata(args)
    if hardware is not None:
        evidence["hardware"] = hardware
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the VitaDebugger foreign-thread VFP lifecycle gate"
    )
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", default=1234, type=int)
    parser.add_argument("--elf", type=Path, default=Path("test.elf"))
    parser.add_argument("--gdb", type=Path, default=resolve_default_gdb())
    parser.add_argument("--timeout", default=12.0, type=float)
    parser.add_argument("--run-seconds", default=1.0, type=float)
    parser.add_argument("--recovery-seconds", default=2.5, type=float)
    parser.add_argument(
        "--device-class",
        help="portable device class label, for example 'handheld' or 'vita-tv'",
    )
    parser.add_argument("--firmware", help="portable firmware label")
    parser.add_argument("--kernel-abi", help="portable kernel ABI label")
    parser.add_argument(
        "--kernel-plugin", type=Path,
        help="kernel companion to identify by SHA-256 and size (path is omitted)",
    )
    parser.add_argument(
        "--vpk", type=Path,
        help="installed VPK to identify by SHA-256 and size (path is omitted)",
    )
    parser.add_argument(
        "--evidence",
        type=Path,
        help="write atomic, portable JSON evidence on pass or failure",
    )
    parser.add_argument(
        "--include-sensitive-transcript",
        action="store_true",
        help=(
            "include raw MI, endpoint, and absolute paths in evidence; "
            "do not publish the resulting file"
        ),
    )
    args = parser.parse_args()
    args.elf = args.elf.resolve()
    args.gdb = args.gdb.resolve()
    try:
        validate_endpoint(args.host, args.port)
    except GateFailure as exc:
        print(f"FAIL: {exc}")
        return 1
    if not args.elf.is_file():
        print(f"FAIL: matching unstripped ELF not found: {args.elf}")
        return 1
    if not args.gdb.is_file():
        print(f"FAIL: VitaSDK GDB not found: {args.gdb}")
        return 1
    if args.run_seconds <= 0 or args.recovery_seconds <= 0 or args.timeout <= 0:
        print("FAIL: timeout and run/recovery delays must be positive")
        return 1

    try:
        evidence = create_evidence(args, gdb_version(args.gdb))
    except GateFailure as exc:
        print(f"FAIL: {exc}")
        return 1
    try:
        run_gate(args, evidence)
        evidence["result"] = "PASS"
        print("PASS: foreign-thread VFP GDB lifecycle gate")
    except (GateFailure, OSError, subprocess.SubprocessError,
            KeyboardInterrupt) as exc:
        evidence["result"] = "FAIL"
        record_failure(evidence, exc, args.include_sensitive_transcript)
        print(f"FAIL: {exc}")
    finally:
        evidence["finished_utc"] = datetime.now(timezone.utc).isoformat()
        if args.evidence:
            write_evidence(args.evidence.resolve(), evidence)
            print(f"evidence: {args.evidence.resolve()}")
    return 0 if evidence.get("result") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

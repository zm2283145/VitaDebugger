#!/usr/bin/env python3
"""Run the live main-plus-user-SUPRX ASLR symbol lifecycle gate.

The diagnostic application must load the ``UVDBAslrFixture`` SUPRX before it
starts VitaDebugger.  This tool uses ``gdb_symbols.py`` for the audited RSP,
ELF matching, and relocation work, then drives a fresh batch GDB for every
phase.  Default evidence is suitable for publication: endpoints, absolute
paths, and raw GDB output are retained only with an explicit opt-in.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import nullcontext
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence

import gdb_symbols as symbols


EVIDENCE_SCHEMA = "vitadebugger-aslr-suprx-live-gdb-v2"
DEFAULT_MODULE_NAME = "UVDBAslrFixture"
MAIN_FUNCTION = "uvdb_aslr_main_breakpoint"
SUPRX_FUNCTION = "uvdb_aslr_suprx_breakpoint"
MAIN_REQUEST = "uvdb_aslr_main_request"
SUPRX_REQUEST = "uvdb_aslr_fixture_control.request"
MAX_GDB_OUTPUT = 2 * 1024 * 1024
MAX_COMPANION_REPLY = 8192
TITLE_ID = re.compile(r"[A-Z0-9]{9}\Z")
MAX_PORTABLE_METADATA_LENGTH = 64
PORTABLE_METADATA_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+ \-]{0,63}")


class GateFailure(RuntimeError):
    """A fail-closed lifecycle, symbol, or evidence assertion."""


class GdbFailure(GateFailure):
    """A GDB failure with machine-local output kept out of public evidence."""

    def __init__(self, message: str, transcript: str, returncode: int | None):
        super().__init__(message)
        self.transcript = transcript
        self.returncode = returncode


@dataclass(frozen=True)
class GdbMarkers:
    main_address: int
    suprx_address: int
    main_pc: int
    suprx_pc: int
    main_sequence: int
    suprx_sequence: int
    main_source: str
    main_line: int
    suprx_source: str
    suprx_line: int


@dataclass(frozen=True)
class SymbolContext:
    snapshot: symbols.TargetSnapshot
    main_runtime: symbols.RuntimeModule
    suprx_match: symbols.ModuleMatch
    matches: tuple[symbols.ModuleMatch, ...]
    unmatched: tuple[symbols.RuntimeModule, ...]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def artifact_record(path: Path) -> dict[str, Any]:
    """Describe an artifact without publishing its machine-local path."""
    return {"sha256": sha256_file(path), "size_bytes": path.stat().st_size}


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


def portable_kernel_identity(path: Path | None) -> dict[str, Any] | None:
    """Hash the optional kernel companion without retaining its input path."""
    if path is None:
        return None
    try:
        if not path.is_file():
            raise GateFailure(
                "--kernel-plugin must name an existing regular file"
            )
        return artifact_record(path)
    except OSError as exc:
        raise GateFailure(
            "--kernel-plugin must name a readable regular file"
        ) from exc


def portable_hardware_metadata(args: argparse.Namespace) -> dict[str, Any] | None:
    """Build optional portable hardware/build metadata."""
    metadata: dict[str, Any] = {}
    for attribute, option in (
        ("device_class", "--device-class"),
        ("firmware", "--firmware"),
        ("kernel_abi", "--kernel-abi"),
    ):
        value = validate_portable_metadata(option, getattr(args, attribute, None))
        if value is not None:
            metadata[attribute] = value
    kernel = portable_kernel_identity(getattr(args, "kernel_plugin", None))
    if kernel is not None:
        metadata["kernel_plugin"] = kernel
    return metadata or None


def _one_match(pattern: re.Pattern[str], text: str, label: str) -> re.Match[str]:
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise GateFailure(f"expected exactly one {label}, found {len(matches)}")
    return matches[0]


def _parse_hex(text: str, label: str) -> int:
    try:
        value = int(text, 16)
    except ValueError as exc:
        raise GateFailure(f"invalid {label}") from exc
    if not 0 < value <= 0xFFFFFFFF:
        raise GateFailure(f"{label} is outside ARM32")
    return value


def _source_stop(text: str, function: str, source_name: str) -> tuple[str, int]:
    pattern = re.compile(
        rf"Breakpoint\s+[0-9]+,\s+{re.escape(function)}\s*\([^\r\n]*\)"
        rf"\s+at\s+([^\r\n]+):([0-9]+)"
    )
    match = _one_match(pattern, text, f"{function} source stop")
    source = match.group(1).strip().replace("\\", "/")
    line = int(match.group(2), 10)
    if not source.endswith("/" + source_name) and source != source_name:
        raise GateFailure(f"{function} stopped in an unexpected source file")
    if line <= 0:
        raise GateFailure(f"{function} reported an invalid source line")
    return source_name, line


def parse_gdb_markers(
    transcript: str,
    expected_main_sequence: int,
    expected_suprx_sequence: int,
) -> GdbMarkers:
    """Parse the strict markers emitted by one batch-GDB phase."""
    addresses = _one_match(
        re.compile(
            r"^UVDB_ASLR_ADDRESSES main=0x([0-9a-fA-F]{1,8}) "
            r"suprx=0x([0-9a-fA-F]{1,8})\s*$",
            re.MULTILINE,
        ),
        transcript,
        "address marker",
    )
    main_hit = _one_match(
        re.compile(
            r"^UVDB_ASLR_MAIN_HIT sequence=0x([0-9a-fA-F]{1,8}) "
            r"pc=0x([0-9a-fA-F]{1,8})\s*$",
            re.MULTILINE,
        ),
        transcript,
        "main hit marker",
    )
    suprx_hit = _one_match(
        re.compile(
            r"^UVDB_ASLR_SUPRX_HIT sequence=0x([0-9a-fA-F]{1,8}) "
            r"pc=0x([0-9a-fA-F]{1,8})\s*$",
            re.MULTILINE,
        ),
        transcript,
        "SUPRX hit marker",
    )
    main_sequence = _parse_hex(main_hit.group(1), "main sequence")
    suprx_sequence = _parse_hex(suprx_hit.group(1), "SUPRX sequence")
    if main_sequence != expected_main_sequence:
        raise GateFailure("main breakpoint sequence did not match its request")
    if suprx_sequence != expected_suprx_sequence:
        raise GateFailure("SUPRX breakpoint sequence did not match its request")
    main_source, main_line = _source_stop(
        transcript, MAIN_FUNCTION, "test.c"
    )
    suprx_source, suprx_line = _source_stop(
        transcript, SUPRX_FUNCTION, "fixture.c"
    )
    return GdbMarkers(
        _parse_hex(addresses.group(1), "main function address"),
        _parse_hex(addresses.group(2), "SUPRX function address"),
        _parse_hex(main_hit.group(2), "main breakpoint PC"),
        _parse_hex(suprx_hit.group(2), "SUPRX breakpoint PC"),
        main_sequence,
        suprx_sequence,
        main_source,
        main_line,
        suprx_source,
        suprx_line,
    )


def render_phase_script(main_sequence: int, suprx_sequence: int) -> str:
    """Render one deterministic two-breakpoint batch-GDB phase."""
    for value, label in (
        (main_sequence, "main sequence"),
        (suprx_sequence, "SUPRX sequence"),
    ):
        if not 0 < value <= 0xFFFFFFFF:
            raise GateFailure(f"{label} must be a nonzero ARM32 value")
    return "\n".join((
        "set breakpoint pending off",
        "set print thread-events off",
        "delete breakpoints",
        f"break {MAIN_FUNCTION}",
        f"break {SUPRX_FUNCTION}",
        "disable 2",
        (
            "printf \"UVDB_ASLR_ADDRESSES main=0x%08x "
            f"suprx=0x%08x\\n\", (unsigned int)&{MAIN_FUNCTION}, "
            f"(unsigned int)&{SUPRX_FUNCTION}"
        ),
        f"set variable {MAIN_REQUEST} = 0x{main_sequence:08x}",
        "continue",
        (
            "printf \"UVDB_ASLR_MAIN_HIT sequence=0x%08x pc=0x%08x\\n\", "
            "(unsigned int)sequence, (unsigned int)$pc"
        ),
        "disable 1",
        "enable 2",
        f"set variable {SUPRX_REQUEST} = 0x{suprx_sequence:08x}",
        "continue",
        (
            "printf \"UVDB_ASLR_SUPRX_HIT sequence=0x%08x pc=0x%08x\\n\", "
            "(unsigned int)sequence, (unsigned int)$pc"
        ),
        "disable 2",
        "detach",
        "quit",
        "",
    ))


def _text_range(
    image: symbols.ElfImage, runtime: symbols.RuntimeModule
) -> tuple[int, int]:
    addresses = image.loaded_section_addresses(runtime.segments)
    section = next(
        (entry for entry in image.sections if entry.name == ".text" and entry.size),
        None,
    )
    if section is None:
        raise GateFailure("symbol ELF has no non-empty .text section")
    start = addresses[".text"]
    end = start + section.size
    if end > 0x100000000:
        raise GateFailure("runtime .text range wraps ARM32")
    return start, end


def _in_thumb_range(address: int, bounds: tuple[int, int]) -> bool:
    normalized = address & ~1
    return bounds[0] <= normalized < bounds[1]


def validate_marker_ranges(
    markers: GdbMarkers,
    main_image: symbols.ElfImage,
    suprx_image: symbols.ElfImage,
    context: SymbolContext,
) -> None:
    main_range = _text_range(main_image, context.main_runtime)
    suprx_range = _text_range(suprx_image, context.suprx_match.runtime)
    for address, label, bounds in (
        (markers.main_address, "main function", main_range),
        (markers.main_pc, "main breakpoint PC", main_range),
        (markers.suprx_address, "SUPRX function", suprx_range),
        (markers.suprx_pc, "SUPRX breakpoint PC", suprx_range),
    ):
        if not _in_thumb_range(address, bounds):
            raise GateFailure(f"{label} is outside its live .text range")


def snapshot_record(
    phase: str, snapshot: symbols.TargetSnapshot
) -> dict[str, Any]:
    """Record layout evidence without copying remote module names."""
    return {
        "phase": phase,
        "qoffsets": {
            "style": snapshot.offsets.style,
            "text": f"0x{snapshot.offsets.text:08x}",
            "data": (
                f"0x{snapshot.offsets.data:08x}"
                if snapshot.offsets.data is not None else None
            ),
        },
        "module_count": len(snapshot.modules),
    }


def _layout(context: SymbolContext) -> tuple[tuple[int, ...], tuple[int, ...]]:
    return context.main_runtime.segments, context.suprx_match.runtime.segments


def require_same_process_layout(
    before: SymbolContext, after: SymbolContext
) -> None:
    if _layout(before) != _layout(after):
        raise GateFailure("module layout changed without a process relaunch")


def address_change_record(
    label: str, before: SymbolContext, after: SymbolContext
) -> dict[str, Any]:
    main_before, suprx_before = _layout(before)
    main_after, suprx_after = _layout(after)
    observed = main_before != main_after or suprx_before != suprx_after
    return {
        "label": label,
        "observed": observed,
        "main_before": [f"0x{value:08x}" for value in main_before],
        "main_after": [f"0x{value:08x}" for value in main_after],
        "suprx_before": [f"0x{value:08x}" for value in suprx_before],
        "suprx_after": [f"0x{value:08x}" for value in suprx_after],
    }


def resolve_context(
    snapshot: symbols.TargetSnapshot,
    main_image: symbols.ElfImage,
    suprx_image: symbols.ElfImage,
    scanned_images: Sequence[symbols.ElfImage],
    module_name: str,
) -> SymbolContext:
    """Require an exact sidecar-derived SUPRX match in one live snapshot."""
    main_runtime = symbols.reconcile_main(main_image, snapshot)
    matches, unmatched = symbols.match_modules(
        snapshot, main_runtime, scanned_images, {}, False
    )
    candidates = [match for match in matches if match.runtime.name == module_name]
    if len(candidates) != 1:
        raise GateFailure(
            f"expected exactly one matched runtime module {module_name!r}"
        )
    suprx_match = candidates[0]
    if suprx_match.image.path != suprx_image.path:
        raise GateFailure("runtime SUPRX matched a different local ELF")
    if suprx_match.image.module_name != module_name or \
       suprx_match.reason != "embedded Vita module name":
        raise GateFailure(
            "SUPRX must match by its exact embedded Vita module name"
        )
    return SymbolContext(
        snapshot, main_runtime, suprx_match, matches, unmatched
    )


def _write_text_atomic(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    temporary.replace(path)


def render_symbol_script(
    path: Path,
    host: str,
    port: int,
    context: SymbolContext,
    main_image: symbols.ElfImage,
    mode: str,
    cache_root: Path,
) -> str:
    solib_directory = None
    if mode == "solib":
        solib_directory = symbols.materialize_solib_cache(
            context.matches, cache_root
        )
    script = symbols.render_gdb_script(
        host,
        port,
        context.snapshot,
        context.main_runtime,
        main_image,
        context.matches,
        context.unmatched,
        mode,
        solib_directory,
    )
    _write_text_atomic(path, script)
    return script


def _bounded_process(
    command: Sequence[str], timeout: float
) -> tuple[int, str]:
    """Run a subprocess while bounding both wall time and captured output."""
    process = subprocess.Popen(
        list(command),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    chunks: list[bytes] = []
    state = {"size": 0, "overflow": False}

    def reader() -> None:
        assert process.stdout is not None
        while True:
            block = process.stdout.read(4096)
            if not block:
                return
            remaining = MAX_GDB_OUTPUT - state["size"]
            if remaining > 0:
                chunks.append(block[:remaining])
                state["size"] += min(len(block), remaining)
            if len(block) > remaining:
                state["overflow"] = True
                process.kill()
                return

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    try:
        returncode = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        process.kill()
        process.wait(timeout=2.0)
        thread.join(timeout=2.0)
        transcript = b"".join(chunks).decode("utf-8", errors="replace")
        raise GdbFailure("GDB phase timed out", transcript, None) from exc
    thread.join(timeout=2.0)
    transcript = b"".join(chunks).decode("utf-8", errors="replace")
    if state["overflow"]:
        raise GdbFailure(
            "GDB output exceeded the configured safety limit",
            transcript,
            returncode,
        )
    return returncode, transcript


def run_gdb_phase(
    gdb: Path,
    symbol_script: Path,
    phase_script: Path,
    timeout: float,
    expected_main_sequence: int,
    expected_suprx_sequence: int,
    main_image: symbols.ElfImage,
    suprx_image: symbols.ElfImage,
    context: SymbolContext,
) -> tuple[GdbMarkers, str, int]:
    returncode, transcript = _bounded_process(
        (
            str(gdb), "-nx", "-q", "-batch",
            "-x", str(symbol_script), "-x", str(phase_script),
        ),
        timeout,
    )
    if returncode != 0:
        raise GdbFailure("GDB phase exited unsuccessfully", transcript, returncode)
    try:
        markers = parse_gdb_markers(
            transcript, expected_main_sequence, expected_suprx_sequence
        )
        validate_marker_ranges(markers, main_image, suprx_image, context)
    except GateFailure as exc:
        raise GdbFailure(str(exc), transcript, returncode) from exc
    return markers, transcript, returncode


def session_record(
    phase: str,
    mode: str,
    markers: GdbMarkers,
    transcript: str,
    returncode: int,
) -> dict[str, Any]:
    return {
        "phase": phase,
        "mode": mode,
        "gdb_return_code": returncode,
        "transcript_bytes": len(transcript.encode("utf-8")),
        "transcript_sha256": sha256_text(transcript),
        "addresses": {
            "main": f"0x{markers.main_address:08x}",
            "suprx": f"0x{markers.suprx_address:08x}",
        },
        "hits": [
            {
                "kind": "main",
                "function": MAIN_FUNCTION,
                "pc": f"0x{markers.main_pc:08x}",
                "sequence": f"0x{markers.main_sequence:08x}",
                "source": markers.main_source,
                "line": markers.main_line,
            },
            {
                "kind": "suprx",
                "function": SUPRX_FUNCTION,
                "pc": f"0x{markers.suprx_pc:08x}",
                "sequence": f"0x{markers.suprx_sequence:08x}",
                "source": markers.suprx_source,
                "line": markers.suprx_line,
            },
        ],
    }


class CompanionClient:
    """Bounded client for the exact Vita Companion lifecycle commands used here."""

    def __init__(self, host: str, port: int, timeout: float):
        self.host = host
        self.port = port
        self.timeout = timeout

    def command(self, command: str) -> str:
        if (
            not command or len(command) > 64 or not command.isascii() or
            any(character in command for character in "\r\n;")
        ):
            raise GateFailure("refusing unsafe Vita Companion command text")
        try:
            with socket.create_connection(
                (self.host, self.port), timeout=self.timeout
            ) as connection:
                connection.settimeout(self.timeout)
                connection.sendall(command.encode("ascii") + b"\n")
                chunks: list[bytes] = []
                size = 0
                while True:
                    block = connection.recv(2048)
                    if not block:
                        break
                    size += len(block)
                    if size > MAX_COMPANION_REPLY:
                        raise GateFailure("Vita Companion reply exceeded its limit")
                    chunks.append(block)
        except OSError as exc:
            raise GateFailure("Vita Companion command failed") from exc
        try:
            return b"".join(chunks).decode("utf-8", "strict").strip()
        except UnicodeDecodeError as exc:
            raise GateFailure("Vita Companion returned invalid UTF-8") from exc

    def version(self) -> str:
        reply = self.command("version")
        if not reply or reply.startswith("Error:"):
            raise GateFailure("Vita Companion version query failed")
        version = validate_portable_metadata("Vita Companion version", reply)
        assert version is not None
        return version

    def relaunch(self, title_id: str, recovery_seconds: float) -> dict[str, str]:
        if not TITLE_ID.fullmatch(title_id):
            raise GateFailure("title ID must be exactly nine uppercase letters/digits")
        killed = self.command(f"kill {title_id}")
        if killed != "Killed.":
            raise GateFailure("Vita Companion did not confirm the title kill")
        time.sleep(recovery_seconds)
        launched = self.command(f"launch {title_id}")
        if launched != "Launched.":
            raise GateFailure("Vita Companion did not confirm the title launch")
        return {"kill": killed, "launch": launched}


def wait_for_snapshot(
    host: str,
    port: int,
    query_timeout: float,
    launch_timeout: float,
) -> symbols.TargetSnapshot:
    deadline = time.monotonic() + launch_timeout
    while True:
        try:
            return symbols.query_target(host, port, query_timeout)
        except (OSError, symbols.SymbolError):
            if time.monotonic() >= deadline:
                raise GateFailure(
                    "the relaunched debugger did not become ready before timeout"
                )
            time.sleep(0.5)


def gdb_version(executable: Path) -> str:
    try:
        result = subprocess.run(
            (str(executable), "--version"),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise GateFailure("could not query the GDB version") from exc
    if result.returncode != 0 or not result.stdout:
        raise GateFailure("could not query the GDB version")
    return result.stdout.splitlines()[0]


def resolve_default_gdb() -> Path:
    default_sdk = r"C:\vitasdk" if os.name == "nt" else "/usr/local/vitasdk"
    vita_sdk = Path(os.environ.get("VITASDK", default_sdk))
    suffix = ".exe" if os.name == "nt" else ""
    return vita_sdk / "bin" / f"arm-vita-eabi-gdb{suffix}"


def _record_sensitive_session(
    evidence: dict[str, Any], phase: str, transcript: str
) -> None:
    if "sensitive" in evidence:
        evidence["sensitive"]["sessions"].append({
            "phase": phase,
            "gdb_output": transcript,
        })


def _execute_session(
    *,
    args: argparse.Namespace,
    evidence: dict[str, Any],
    work_dir: Path,
    phase: str,
    mode: str,
    symbol_script: Path,
    context: SymbolContext,
    main_image: symbols.ElfImage,
    suprx_image: symbols.ElfImage,
    sequence_index: int,
) -> None:
    main_sequence = 0x11000000 | sequence_index
    suprx_sequence = 0x22000000 | sequence_index
    phase_script = work_dir / f"{phase}-gate.gdb"
    _write_text_atomic(
        phase_script, render_phase_script(main_sequence, suprx_sequence)
    )
    try:
        markers, transcript, returncode = run_gdb_phase(
            args.gdb,
            symbol_script,
            phase_script,
            args.gdb_timeout,
            main_sequence,
            suprx_sequence,
            main_image,
            suprx_image,
            context,
        )
    except GdbFailure as exc:
        _record_sensitive_session(evidence, phase, exc.transcript)
        evidence["sessions"].append({
            "phase": phase,
            "mode": mode,
            "result": "FAIL",
            "gdb_return_code": exc.returncode,
            "transcript_bytes": len(exc.transcript.encode("utf-8")),
            "transcript_sha256": sha256_text(exc.transcript),
        })
        raise
    evidence["sessions"].append(
        session_record(phase, mode, markers, transcript, returncode)
    )
    _record_sensitive_session(evidence, phase, transcript)
    print(
        f"{phase}: main and {args.module_name} source breakpoints passed "
        f"({mode})"
    )


def run_gate(
    args: argparse.Namespace,
    evidence: dict[str, Any],
    work_dir: Path,
) -> None:
    main_image = symbols.read_elf(args.main_elf)
    suprx_image = symbols.read_elf(args.suprx_elf)
    scanned_images = symbols.scan_elfs((args.search_root,))

    initial_snapshot = symbols.query_target(
        args.host, args.port, args.query_timeout
    )
    initial = resolve_context(
        initial_snapshot,
        main_image,
        suprx_image,
        scanned_images,
        args.module_name,
    )
    evidence["snapshots"].append(snapshot_record("initial", initial_snapshot))
    evidence["module_match"] = {
        "runtime_name": initial.suprx_match.runtime.name,
        "reason": initial.suprx_match.reason,
        "unmatched_module_count": len(initial.unmatched),
    }

    cache_root = work_dir / "solib-cache"
    solib_script_path = work_dir / "symbols-solib.gdb"
    solib_script = render_symbol_script(
        solib_script_path,
        args.host,
        args.port,
        initial,
        main_image,
        "solib",
        cache_root,
    )
    evidence["scripts"].append({
        "phase": "initial",
        "mode": "solib",
        "sha256": sha256_text(solib_script),
    })
    _execute_session(
        args=args,
        evidence=evidence,
        work_dir=work_dir,
        phase="initial_solib",
        mode="solib",
        symbol_script=solib_script_path,
        context=initial,
        main_image=main_image,
        suprx_image=suprx_image,
        sequence_index=1,
    )

    time.sleep(args.recovery_seconds)
    reconnect_snapshot = symbols.query_target(
        args.host, args.port, args.query_timeout
    )
    reconnect = resolve_context(
        reconnect_snapshot,
        main_image,
        suprx_image,
        scanned_images,
        args.module_name,
    )
    require_same_process_layout(initial, reconnect)
    evidence["snapshots"].append(
        snapshot_record("same_process_reconnect", reconnect_snapshot)
    )
    _execute_session(
        args=args,
        evidence=evidence,
        work_dir=work_dir,
        phase="same_process_solib_reconnect",
        mode="solib",
        symbol_script=solib_script_path,
        context=reconnect,
        main_image=main_image,
        suprx_image=suprx_image,
        sequence_index=2,
    )

    companion = CompanionClient(
        args.host, args.companion_port, args.query_timeout
    )
    evidence["companion"] = {"version": companion.version(), "relaunches": []}
    first_relaunch = companion.relaunch(args.title_id, args.recovery_seconds)
    evidence["companion"]["relaunches"].append({
        "phase": "solib_relaunch", **first_relaunch,
    })
    relaunched_snapshot = wait_for_snapshot(
        args.host,
        args.port,
        args.query_timeout,
        args.launch_timeout,
    )
    relaunched = resolve_context(
        relaunched_snapshot,
        main_image,
        suprx_image,
        scanned_images,
        args.module_name,
    )
    evidence["snapshots"].append(
        snapshot_record("first_relaunch", relaunched_snapshot)
    )
    evidence["address_changes"].append(
        address_change_record("initial_to_first_relaunch", initial, relaunched)
    )
    _execute_session(
        args=args,
        evidence=evidence,
        work_dir=work_dir,
        phase="relaunched_solib_reuse",
        mode="solib",
        symbol_script=solib_script_path,
        context=relaunched,
        main_image=main_image,
        suprx_image=suprx_image,
        sequence_index=3,
    )

    time.sleep(args.recovery_seconds)
    explicit_snapshot = symbols.query_target(
        args.host, args.port, args.query_timeout
    )
    explicit_context = resolve_context(
        explicit_snapshot,
        main_image,
        suprx_image,
        scanned_images,
        args.module_name,
    )
    require_same_process_layout(relaunched, explicit_context)
    evidence["snapshots"].append(
        snapshot_record("explicit_before_second_relaunch", explicit_snapshot)
    )
    explicit_one_path = work_dir / "symbols-explicit-launch-one.gdb"
    explicit_one = render_symbol_script(
        explicit_one_path,
        args.host,
        args.port,
        explicit_context,
        main_image,
        "explicit",
        cache_root,
    )
    evidence["scripts"].append({
        "phase": "before_second_relaunch",
        "mode": "explicit",
        "sha256": sha256_text(explicit_one),
    })
    _execute_session(
        args=args,
        evidence=evidence,
        work_dir=work_dir,
        phase="explicit_current_launch",
        mode="explicit",
        symbol_script=explicit_one_path,
        context=explicit_context,
        main_image=main_image,
        suprx_image=suprx_image,
        sequence_index=4,
    )

    time.sleep(args.recovery_seconds)
    second_relaunch = companion.relaunch(args.title_id, args.recovery_seconds)
    evidence["companion"]["relaunches"].append({
        "phase": "explicit_regeneration", **second_relaunch,
    })
    second_snapshot = wait_for_snapshot(
        args.host,
        args.port,
        args.query_timeout,
        args.launch_timeout,
    )
    second = resolve_context(
        second_snapshot,
        main_image,
        suprx_image,
        scanned_images,
        args.module_name,
    )
    evidence["snapshots"].append(
        snapshot_record("second_relaunch", second_snapshot)
    )
    evidence["address_changes"].append(
        address_change_record("first_to_second_relaunch", relaunched, second)
    )
    explicit_two_path = work_dir / "symbols-explicit-launch-two.gdb"
    explicit_two = render_symbol_script(
        explicit_two_path,
        args.host,
        args.port,
        second,
        main_image,
        "explicit",
        cache_root,
    )
    evidence["scripts"].append({
        "phase": "after_second_relaunch",
        "mode": "explicit",
        "sha256": sha256_text(explicit_two),
    })
    _execute_session(
        args=args,
        evidence=evidence,
        work_dir=work_dir,
        phase="explicit_regenerated_after_relaunch",
        mode="explicit",
        symbol_script=explicit_two_path,
        context=second,
        main_image=main_image,
        suprx_image=suprx_image,
        sequence_index=5,
    )

    address_change_observed = any(
        record["observed"] for record in evidence["address_changes"]
    )
    evidence["address_change_observed"] = address_change_observed
    if args.require_address_change and not address_change_observed:
        raise GateFailure(
            "no main or SUPRX address change was observed across relaunches"
        )


def write_evidence(path: Path, evidence: dict[str, Any]) -> None:
    _write_text_atomic(
        path,
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the VitaDebugger main/user-SUPRX ASLR lifecycle gate"
    )
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", default=1234, type=int)
    parser.add_argument("--companion-port", default=1338, type=int)
    parser.add_argument("--title-id", required=True)
    parser.add_argument("--main-elf", type=Path, default=Path("test.elf"))
    parser.add_argument("--suprx-elf", required=True, type=Path)
    parser.add_argument("--suprx-velf", type=Path)
    parser.add_argument("--suprx-self", type=Path)
    parser.add_argument("--vpk", type=Path)
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
    parser.add_argument("--search-root", required=True, type=Path)
    parser.add_argument("--module-name", default=DEFAULT_MODULE_NAME)
    parser.add_argument("--gdb", type=Path, default=resolve_default_gdb())
    parser.add_argument("--query-timeout", default=8.0, type=float)
    parser.add_argument("--gdb-timeout", default=30.0, type=float)
    parser.add_argument("--launch-timeout", default=30.0, type=float)
    parser.add_argument("--recovery-seconds", default=2.5, type=float)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument(
        "--allow-relaunch",
        action="store_true",
        help="authorize two exact title-scoped Vita Companion relaunches",
    )
    parser.add_argument(
        "--require-address-change",
        action="store_true",
        help="fail if randomized bases happen to repeat across both relaunches",
    )
    parser.add_argument(
        "--include-sensitive-transcript",
        action="store_true",
        help=(
            "include endpoints, absolute paths, and raw GDB output; "
            "do not publish the resulting evidence"
        ),
    )
    return parser


def _resolve_inputs(args: argparse.Namespace) -> None:
    args.main_elf = args.main_elf.resolve()
    args.suprx_elf = args.suprx_elf.resolve()
    args.search_root = args.search_root.resolve()
    args.gdb = args.gdb.resolve()
    if args.suprx_velf is None:
        args.suprx_velf = args.suprx_elf.with_suffix(".velf")
    else:
        args.suprx_velf = args.suprx_velf.resolve()
    if args.suprx_self is not None:
        args.suprx_self = args.suprx_self.resolve()
    if args.vpk is not None:
        args.vpk = args.vpk.resolve()
    if getattr(args, "kernel_plugin", None) is not None:
        args.kernel_plugin = args.kernel_plugin.resolve()
    if args.work_dir is not None:
        args.work_dir = args.work_dir.resolve()
    if args.evidence is not None:
        args.evidence = args.evidence.resolve()


def validate_arguments(args: argparse.Namespace) -> None:
    if not args.allow_relaunch:
        raise GateFailure(
            "the complete gate requires explicit --allow-relaunch authorization"
        )
    if not symbols.SAFE_HOST.fullmatch(args.host):
        raise GateFailure("host must be a plain IPv4 address or DNS hostname")
    if not TITLE_ID.fullmatch(args.title_id):
        raise GateFailure("title ID must be exactly nine uppercase letters/digits")
    portable_hardware_metadata(args)
    if (
        len(args.module_name) > 27 or
        not symbols.SAFE_MODULE_FILENAME.fullmatch(args.module_name)
    ):
        raise GateFailure("module name is not safe for the GDB solib cache")
    if not 1 <= args.port <= 65535 or not 1 <= args.companion_port <= 65535:
        raise GateFailure("network ports must be in the range 1-65535")
    if any(
        value <= 0 for value in (
            args.query_timeout,
            args.gdb_timeout,
            args.launch_timeout,
            args.recovery_seconds,
        )
    ):
        raise GateFailure("timeouts and recovery delay must be positive")
    required_files = (args.main_elf, args.suprx_elf, args.suprx_velf, args.gdb)
    if any(not path.is_file() for path in required_files):
        raise GateFailure("one or more required local build artifacts are missing")
    if (
        args.suprx_velf.parent != args.suprx_elf.parent or
        args.suprx_velf.stem != args.suprx_elf.stem
    ):
        raise GateFailure(
            "SUPRX ELF and VELF sidecar must share one directory and stem"
        )
    for optional in (args.suprx_self, args.vpk):
        if optional is not None and not optional.is_file():
            raise GateFailure("an optional hash-recorded artifact is missing")
    if not args.search_root.is_dir():
        raise GateFailure("symbol search root is not a directory")


def create_evidence(args: argparse.Namespace) -> dict[str, Any]:
    artifacts = {
        "main_elf": artifact_record(args.main_elf),
        "suprx_elf": artifact_record(args.suprx_elf),
        "suprx_velf": artifact_record(args.suprx_velf),
    }
    if args.suprx_self is not None:
        artifacts["suprx_self"] = artifact_record(args.suprx_self)
    if args.vpk is not None:
        artifacts["vpk"] = artifact_record(args.vpk)
    evidence: dict[str, Any] = {
        "schema": EVIDENCE_SCHEMA,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "gdb": {"version": gdb_version(args.gdb)},
        "title_id": args.title_id,
        "module_name": args.module_name,
        "artifacts": artifacts,
        "snapshots": [],
        "scripts": [],
        "sessions": [],
        "address_changes": [],
    }
    hardware = portable_hardware_metadata(args)
    if hardware is not None:
        evidence["hardware"] = hardware
    if args.include_sensitive_transcript:
        evidence["sensitive"] = {
            "host": args.host,
            "port": args.port,
            "companion_port": args.companion_port,
            "paths": {
                "main_elf": str(args.main_elf),
                "suprx_elf": str(args.suprx_elf),
                "suprx_velf": str(args.suprx_velf),
                "suprx_self": (
                    str(args.suprx_self) if args.suprx_self else None
                ),
                "vpk": str(args.vpk) if args.vpk else None,
                "search_root": str(args.search_root),
                "gdb": str(args.gdb),
            },
            "sessions": [],
        }
    return evidence


def main() -> int:
    args = build_parser().parse_args()
    _resolve_inputs(args)
    evidence: dict[str, Any] | None = None
    try:
        validate_arguments(args)
        evidence = create_evidence(args)
        if args.work_dir is None:
            manager = tempfile.TemporaryDirectory(prefix="uvdb-aslr-gate-")
        else:
            args.work_dir.mkdir(parents=True, exist_ok=True)
            manager = nullcontext(str(args.work_dir))
        with manager as directory:
            run_gate(args, evidence, Path(directory).resolve())
        evidence["result"] = "PASS"
        print("PASS: main and user-SUPRX ASLR lifecycle gate")
    except (GateFailure, symbols.SymbolError, OSError,
            subprocess.SubprocessError, KeyboardInterrupt) as exc:
        if evidence is None:
            evidence = {
                "schema": EVIDENCE_SCHEMA,
                "started_utc": datetime.now(timezone.utc).isoformat(),
                "sessions": [],
            }
        evidence["result"] = "FAIL"
        evidence["failure"] = {
            "type": type(exc).__name__,
            "message": (
                "The live ASLR lifecycle gate did not complete. Re-run it "
                "interactively for diagnostics."
            ),
        }
        if args.include_sensitive_transcript:
            evidence.setdefault("sensitive", {})["error"] = str(exc)
        print(f"FAIL: {exc}")
    finally:
        if evidence is not None:
            evidence["finished_utc"] = datetime.now(timezone.utc).isoformat()
            if args.evidence is not None:
                write_evidence(args.evidence, evidence)
                print(f"evidence: {args.evidence}")
    return 0 if evidence and evidence.get("result") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

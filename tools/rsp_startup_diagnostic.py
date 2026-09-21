#!/usr/bin/env python3
"""Retrieve the bounded RSP test-title startup journal over Companion FTP."""

from __future__ import annotations

import argparse
import ftplib
import ipaddress
import json
import math
import socket
import struct
import time
from pathlib import Path
from typing import Any


MAGIC = 0x55565344
VERSION = 5
RECORD_FORMAT = "<21I"
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)
JOURNAL_SIZE = RECORD_SIZE * 2
REMOTE_PATH = "ux0:/data/vitadebugger-rsp-startup.bin"
FAILED = 1
OBS_POLL_ENTERED = 2
OBS_MALFORMED_REQUEST = 4
OBS_REQUEST_RECEIVED = 8
OBS_RESPONSE_ATTEMPTED = 16
OBS_RESPONSE_SENT = 32
OBS_SELF_PROBE_RECEIVED = 64
OBS_SELF_PROBE_FAILED = 128
KNOWN_STAGE_FLAGS = (
    FAILED
    | OBS_POLL_ENTERED
    | OBS_MALFORMED_REQUEST
    | OBS_REQUEST_RECEIVED
    | OBS_RESPONSE_ATTEMPTED
    | OBS_RESPONSE_SENT
    | OBS_SELF_PROBE_RECEIVED
    | OBS_SELF_PROBE_FAILED
)

STAGES = {
    1: "process_entry",
    2: "route_ready",
    3: "display_ready",
    4: "kernel_gate",
    5: "aslr_gate",
    6: "module_enum",
    7: "thread_fixture",
    8: "vfp_fixture",
    9: "admission_udp_begin",
    10: "admission_udp_ready",
    11: "server_begin",
    12: "server_ready",
    13: "stdio_ready",
    14: "test_ready",
    15: "main_loop",
    16: "admission_poll_begin",
    17: "admission_poll_idle",
    18: "admission_request",
    19: "admission_response",
    20: "main_loop_heartbeat",
}


class StartupDiagnosticFailure(RuntimeError):
    def __init__(self, detail: str, raw: bytes | None = None):
        super().__init__(detail)
        self.raw = raw


def numeric_ipv4(value: str) -> str:
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as exc:
        raise argparse.ArgumentTypeError(
            "host must be a numeric IPv4 literal"
        ) from exc
    if str(address) != value:
        raise argparse.ArgumentTypeError(
            "host must use canonical numeric IPv4 notation"
        )
    return value


def checksum_record(data: bytes) -> int:
    if len(data) != RECORD_SIZE:
        raise StartupDiagnosticFailure(
            f"startup record is {len(data)} bytes, expected {RECORD_SIZE}"
        )
    checksum_offset = 8 * 4
    value = 2166136261
    for index, byte in enumerate(data):
        selected = 0 if checksum_offset <= index < checksum_offset + 4 else byte
        value = ((value ^ selected) * 16777619) & 0xFFFFFFFF
    return value


def parse_record(data: bytes) -> dict[str, Any]:
    values = struct.unpack(RECORD_FORMAT, data)
    magic, version, size, sequence, stage = values[:5]
    result = values[5]
    stage_flags, build_flags, checksum = values[6:9]
    if magic != MAGIC or version != VERSION or size != RECORD_SIZE:
        raise StartupDiagnosticFailure("startup record ABI mismatch")
    if sequence == 0 or stage not in STAGES:
        raise StartupDiagnosticFailure("startup record stage is invalid")
    if checksum_record(data) != checksum:
        raise StartupDiagnosticFailure("startup record checksum mismatch")
    run_id = values[9] | (values[10] << 32)
    failed_stage_mask = values[11]
    if run_id == 0:
        raise StartupDiagnosticFailure("startup record run ID is invalid")
    details = values[12:15]
    route_address, bound_address, bound_port = values[15:18]
    endpoint_result, self_probe_result = values[18:20]
    if values[20] != 0:
        raise StartupDiagnosticFailure("startup record reserved fields are nonzero")
    if stage_flags & ~KNOWN_STAGE_FLAGS or build_flags & ~7:
        raise StartupDiagnosticFailure("startup record flags are invalid")
    valid_stage_mask = sum(1 << candidate for candidate in STAGES)
    if failed_stage_mask & ~valid_stage_mask:
        raise StartupDiagnosticFailure("startup failed-stage mask is invalid")
    signed_result = result if result < 0x80000000 else result - 0x100000000
    signed_self_probe = (
        self_probe_result
        if self_probe_result < 0x80000000
        else self_probe_result - 0x100000000
    )
    signed_endpoint = (
        endpoint_result
        if endpoint_result < 0x80000000
        else endpoint_result - 0x100000000
    )
    if bound_port > 0xFFFF:
        raise StartupDiagnosticFailure("startup bound port is invalid")
    if stage >= 10:
        if signed_endpoint == 0 and bound_port == 0:
            raise StartupDiagnosticFailure("startup bound endpoint is missing")
        if signed_endpoint != 0 and (bound_address != 0 or bound_port != 0):
            raise StartupDiagnosticFailure("startup bound endpoint is inconsistent")
        received = bool(stage_flags & OBS_SELF_PROBE_RECEIVED)
        failed_probe = bool(stage_flags & OBS_SELF_PROBE_FAILED)
        if received == failed_probe:
            raise StartupDiagnosticFailure("startup self-probe flags are invalid")
        if received != (signed_self_probe == 0):
            raise StartupDiagnosticFailure("startup self-probe result is inconsistent")
    if signed_result != 0 and not stage_flags & FAILED:
        raise StartupDiagnosticFailure("startup record failure result is inconsistent")
    if stage_flags & FAILED and not failed_stage_mask & (1 << stage):
        raise StartupDiagnosticFailure("startup record stage failure is inconsistent")
    return {
        "sequence": sequence,
        "stage": stage,
        "stage_name": STAGES[stage],
        "result": signed_result,
        "stage_flags": stage_flags,
        "failed": bool(stage_flags & FAILED),
        "poll_entered": bool(stage_flags & OBS_POLL_ENTERED),
        "malformed_request_seen": bool(stage_flags & OBS_MALFORMED_REQUEST),
        "request_received": bool(stage_flags & OBS_REQUEST_RECEIVED),
        "response_attempted": bool(stage_flags & OBS_RESPONSE_ATTEMPTED),
        "response_sent": bool(stage_flags & OBS_RESPONSE_SENT),
        "self_probe_received": bool(stage_flags & OBS_SELF_PROBE_RECEIVED),
        "self_probe_failed": bool(stage_flags & OBS_SELF_PROBE_FAILED),
        "build_flags": build_flags,
        "run_id": run_id,
        "failed_stage_mask": failed_stage_mask,
        "failed_stages": [
            STAGES[candidate]
            for candidate in STAGES
            if failed_stage_mask & (1 << candidate)
        ],
        "any_failed": failed_stage_mask != 0,
        "details": list(details),
        "details_signed": [
            value if value < 0x80000000 else value - 0x100000000
            for value in details
        ],
        "route_address": socket.inet_ntoa(struct.pack("<I", route_address)),
        "route_known": route_address != 0,
        "bound_address": socket.inet_ntoa(struct.pack("<I", bound_address)),
        "bound_port": bound_port,
        "endpoint_result": signed_endpoint,
        "self_probe_result": signed_self_probe,
        "admission_diagnostic": bool(build_flags & 1),
        "console_test": bool(build_flags & 2),
        "kernel_mode": bool(build_flags & 4),
    }


def sequence_newer(left: int, right: int) -> bool:
    delta = (left - right) & 0xFFFFFFFF
    return left != right and delta < 0x80000000


def parse_journal(data: bytes) -> dict[str, Any]:
    if len(data) != JOURNAL_SIZE:
        raise StartupDiagnosticFailure(
            f"startup journal is {len(data)} bytes, expected {JOURNAL_SIZE}"
        )
    valid = []
    errors = []
    for slot in range(2):
        raw = data[slot * RECORD_SIZE:(slot + 1) * RECORD_SIZE]
        try:
            record = parse_record(raw)
            record["slot"] = slot
            valid.append(record)
        except StartupDiagnosticFailure as exc:
            errors.append({"slot": slot, "error": str(exc)})
    if not valid:
        raise StartupDiagnosticFailure(
            f"startup journal has no valid slot: {errors}"
        )
    if len({record["run_id"] for record in valid}) != 1:
        raise StartupDiagnosticFailure("startup journal slots have different run IDs")
    selected = valid[0]
    for candidate in valid[1:]:
        if sequence_newer(candidate["sequence"], selected["sequence"]):
            selected = candidate
    return {"selected": selected, "valid_slots": valid, "slot_errors": errors}


def fetch_journal(host: str, port: int, timeout: float) -> bytes:
    chunks: list[bytes] = []
    size = 0
    with ftplib.FTP() as ftp:
        ftp.connect(host, port, timeout=timeout)
        ftp.login()

        def append(block: bytes) -> None:
            nonlocal size
            size += len(block)
            if size > JOURNAL_SIZE:
                raise StartupDiagnosticFailure(
                    "startup journal exceeds its fixed size"
                )
            chunks.append(block)

        ftp.retrbinary(f"RETR {REMOTE_PATH}", append)
    return b"".join(chunks)


def clear_journal(host: str, port: int, timeout: float) -> None:
    with ftplib.FTP() as ftp:
        ftp.connect(host, port, timeout=timeout)
        ftp.login()
        try:
            ftp.delete(REMOTE_PATH)
        except ftplib.error_perm as exc:
            if not str(exc).startswith("550"):
                raise
        try:
            ftp.retrbinary(f"RETR {REMOTE_PATH}", lambda _block: None)
        except ftplib.error_perm as exc:
            if str(exc).startswith("550"):
                return
            raise
        raise StartupDiagnosticFailure(
            "startup journal still exists after preflight deletion"
        )


def observe(
    host: str,
    port: int,
    timeout: float,
    minimum_stage: int,
    required_build_flags: int = 3,
    reject_run_id: int | None = None,
) -> tuple[dict[str, Any], bytes]:
    deadline = time.monotonic() + timeout
    last_snapshot: dict[str, Any] | None = None
    last_error: str | None = None
    last_raw: bytes | None = None
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        try:
            raw = fetch_journal(host, port, min(2.0, max(0.1, remaining)))
            last_raw = raw
            last_snapshot = parse_journal(raw)
            selected = last_snapshot["selected"]
            stale = reject_run_id is not None and selected["run_id"] == reject_run_id
            missing_flags = (
                selected["build_flags"] & required_build_flags
            ) != required_build_flags
            if stale:
                last_error = "startup journal run ID matches the rejected baseline"
            elif missing_flags:
                last_error = (
                    "startup journal is missing required build flags "
                    f"0x{required_build_flags:x}"
                )
            elif (
                selected["any_failed"]
                or selected["stage"] >= minimum_stage
                or (
                    minimum_stage >= 16
                    and selected["stage"] in (17, 18, 19, 20)
                )
            ):
                return last_snapshot, raw
        except (
            OSError,
            EOFError,
            ftplib.Error,
            StartupDiagnosticFailure,
        ) as exc:
            last_error = f"{type(exc).__name__}: {exc}"
        time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
    if last_snapshot is not None and last_raw is not None:
        last_snapshot["deadline_expired"] = True
        last_snapshot["last_error"] = last_error
        return last_snapshot, last_raw
    raise StartupDiagnosticFailure(
        "startup stage deadline expired: "
        f"last_snapshot={last_snapshot}; last_error={last_error}",
        last_raw,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True, type=numeric_ipv4)
    parser.add_argument("--ftp-port", type=int, default=1337)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--minimum-stage", type=int, default=20)
    parser.add_argument("--required-build-flags", type=lambda value: int(value, 0),
                        default=3)
    parser.add_argument("--reject-run-id", type=lambda value: int(value, 0))
    parser.add_argument("--action", choices=("observe", "clear"), default="observe")
    parser.add_argument("--raw-output", type=Path)
    parser.add_argument("--summary", required=True, type=Path)
    args = parser.parse_args()
    if not 1 <= args.ftp_port <= 65535:
        parser.error("--ftp-port must be between 1 and 65535")
    if not math.isfinite(args.timeout) or not 0 < args.timeout <= 30:
        parser.error("--timeout must be within (0, 30]")
    if args.minimum_stage not in STAGES:
        parser.error("--minimum-stage is not a defined startup stage")
    if args.required_build_flags & ~7:
        parser.error("--required-build-flags contains an unknown flag")
    if args.action == "observe" and args.raw_output is None:
        parser.error("--raw-output is required for observe")

    started = time.monotonic()
    try:
        if args.action == "clear":
            clear_journal(args.host, args.ftp_port, args.timeout)
            result = {
                "format": "VITADEBUGGER-RSP-STARTUP-DIAGNOSTIC-1",
                "status": "PASS",
                "action": "clear",
                "duration_seconds": time.monotonic() - started,
                "remote_path": REMOTE_PATH,
                "verified_absent": True,
            }
            args.summary.write_text(
                json.dumps(result, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(json.dumps(result, indent=2, sort_keys=True))
            return 0
        snapshot, raw = observe(
            args.host, args.ftp_port, args.timeout, args.minimum_stage,
            args.required_build_flags, args.reject_run_id)
        args.raw_output.write_bytes(raw)
        result = {
            "format": "VITADEBUGGER-RSP-STARTUP-DIAGNOSTIC-1",
            "status": (
                "FAIL"
                if snapshot["selected"]["any_failed"]
                or snapshot.get("deadline_expired")
                else "PASS"
            ),
            "duration_seconds": time.monotonic() - started,
            "snapshot": snapshot,
        }
        args.summary.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(json.dumps(result, indent=2, sort_keys=True))
        return 1 if result["status"] == "FAIL" else 0
    except Exception as exc:
        raw = exc.raw if isinstance(exc, StartupDiagnosticFailure) else None
        if raw is not None and args.raw_output is not None:
            args.raw_output.write_bytes(raw)
        result = {
            "format": "VITADEBUGGER-RSP-STARTUP-DIAGNOSTIC-1",
            "status": "FAIL",
            "duration_seconds": time.monotonic() - started,
            "error": type(exc).__name__,
            "detail": str(exc),
        }
        args.summary.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(json.dumps(result, indent=2, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

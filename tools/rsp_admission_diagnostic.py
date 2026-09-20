#!/usr/bin/env python3
"""Bounded out-of-band diagnosis of the first RSP admission transition."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any

from rsp_admission_snapshot import (
    AdmissionDiagnosticFailure,
    EVENT_COUNT,
    EVENT_NAMES,
    SNAPSHOT_FORMAT,
    SNAPSHOT_SIZE,
    STATE_NETWORK_CLOSING,
    STATE_PROTOCOL_OWNED,
    STATE_TARGET_STOPPED,
    STATE_TEST_TITLE_READY,
    SnapshotUnavailable,
    VERSION,
    numeric_ipv4,
    parse_snapshot,
    query_snapshot,
)
from rsp_network_retail_gate import Rsp, Transcript, parse_offsets, utc_now


def wait_debugger_ready(
    host: str, port: int, timeout: float, transcript: Transcript
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_snapshot: dict[str, Any] | None = None
    last_miss: str | None = None
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        try:
            last_snapshot = query_snapshot(
                host, port, min(1.0, remaining), transcript
            )
        except SnapshotUnavailable as exc:
            last_miss = str(exc)
            continue
        events = last_snapshot["events"]
        current = last_snapshot["current"]
        if (
            events["listener_ready"]["occurrences"] >= 1
            and events["test_title_ready"]["occurrences"] >= 1
            and current["listener"] >= 0
            and current["socket"] < 0
            and current["candidate"] < 0
            and current["generation"] == 0
            and current["owner"] == 0
            and current["owner_epoch"] == 0
            and current["connection_epoch"] == 0
            and current["dropped_event_writes"] == 0
            and current["test_title_ready"]
            and not current["target_stopped"]
            and not current["network_closing"]
        ):
            transcript.event("debugger_ready", snapshot=last_snapshot)
            return last_snapshot
        time.sleep(0.1)
    raise AdmissionDiagnosticFailure(
        "debugger-ready marker not observed before deadline: "
        f"snapshot={last_snapshot}; last_miss={last_miss}"
    )


def require_order(snapshot: dict[str, Any]) -> None:
    epoch = snapshot["current"]["connection_epoch"]
    if snapshot["current"]["dropped_event_writes"] != 0:
        raise AdmissionDiagnosticFailure(
            "diagnostic event writer contention dropped telemetry"
        )
    required = (
        "listener_ready",
        "candidate_accepted",
        "valid_frame",
        "promotion_begin",
        "socket_published",
        "protocol_acquired",
        "target_stopped",
        "main_loop_entered",
        "first_packet",
    )
    events = snapshot["events"]
    missing = [
        name for name in required
        if events[name]["occurrences"] != 1
        or events[name]["sequence"] == 0
        or events[name]["epoch"] != epoch
    ]
    if (
        events["test_title_ready"]["occurrences"] != 1
        or events["test_title_ready"]["sequence"] == 0
    ):
        missing.append("test_title_ready")
    if missing:
        raise AdmissionDiagnosticFailure(
            f"missing or repeated admission events: {missing}"
        )
    transition_order = (
        "candidate_accepted",
        "valid_frame",
        "promotion_begin",
        "socket_published",
        "protocol_acquired",
        "target_stopped",
        "main_loop_entered",
        "first_packet",
    )
    ordered = sorted(
        transition_order, key=lambda name: events[name]["sequence"]
    )
    if (
        tuple(ordered) != transition_order
        or events["listener_ready"]["sequence"]
        >= events["candidate_accepted"]["sequence"]
        or events["test_title_ready"]["sequence"]
        >= events["candidate_accepted"]["sequence"]
    ):
        raise AdmissionDiagnosticFailure(
            f"admission event order differs: {ordered}"
        )
    published = events["socket_published"]
    acquired = events["protocol_acquired"]
    if (
        published["descriptor"] < 0
        or published["generation"] == 0
        or acquired["descriptor"] != published["descriptor"]
        or acquired["generation"] != published["generation"]
        or acquired["owner"] == 0
    ):
        raise AdmissionDiagnosticFailure(
            "published descriptor/generation and protocol ownership differ"
        )


def wait_detached(
    host: str, port: int, timeout: float, transcript: Transcript
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_snapshot: dict[str, Any] | None = None
    last_miss: str | None = None
    while time.monotonic() < deadline:
        try:
            last_snapshot = query_snapshot(
                host, port,
                min(1.0, deadline - time.monotonic()), transcript
            )
        except SnapshotUnavailable as exc:
            last_miss = str(exc)
            continue
        current = last_snapshot["current"]
        epoch = current["connection_epoch"]
        events = last_snapshot["events"]
        if (
            current["socket"] < 0
            and current["owner"] == 0
            and current["owner_epoch"] == 0
            and not current["target_stopped"]
            and current["listener"] >= 0
            and all(
                events[name]["epoch"] == epoch
                and events[name]["occurrences"] == 1
                for name in (
                    "target_running",
                    "protocol_released",
                    "session_normalized",
                    "listener_reopened",
                )
            )
        ):
            return last_snapshot
        time.sleep(0.05)
    raise AdmissionDiagnosticFailure(
        "detach state did not quiesce before deadline: "
        f"snapshot={last_snapshot}; last_miss={last_miss}"
    )


def run(args: argparse.Namespace, transcript: Transcript) -> dict[str, Any]:
    ready = wait_debugger_ready(
        args.host, args.diagnostic_port, args.ready_timeout, transcript
    )
    client = Rsp(
        args.host, args.port, args.timeout, transcript,
        "pre-negotiation-diagnostic"
    )
    try:
        packet_size = client.negotiate()
        offsets_payload, _ = client.request(b"qOffsets")
        offsets = parse_offsets(offsets_payload)
        snapshot = query_snapshot(
            args.host, args.diagnostic_port, args.timeout, transcript
        )
        require_order(snapshot)
        if snapshot["current"]["connection_epoch"] != (
            ready["current"]["connection_epoch"] + 1
        ):
            raise AdmissionDiagnosticFailure(
                "connection epoch changed outside the diagnostic session"
            )
        if snapshot["current"]["first_packet_size"] == 0:
            raise AdmissionDiagnosticFailure(
                "first packet size was not recorded"
            )
        client.detach()
        final_snapshot = wait_detached(
            args.host, args.diagnostic_port, args.timeout, transcript
        )
        return {
            "format": "VITADEBUGGER-RSP-ADMISSION-DIAGNOSTIC-1",
            "status": "PASS",
            "tested_at": utc_now(),
            "packet_size": packet_size,
            "offsets": offsets,
            "ready_snapshot": ready,
            "negotiated_snapshot": snapshot,
            "final_snapshot": final_snapshot,
        }
    except BaseException as exc:
        try:
            failure_snapshot = query_snapshot(
                args.host, args.diagnostic_port, args.timeout, transcript
            )
        except BaseException as telemetry_exc:
            failure_snapshot = {
                "telemetry_error": type(telemetry_exc).__name__,
                "detail": str(telemetry_exc),
            }
        transcript.event(
            "pre_negotiation_failure",
            error=type(exc).__name__,
            detail=str(exc),
            snapshot=failure_snapshot,
        )
        raise AdmissionDiagnosticFailure(
            f"{type(exc).__name__}: {exc}; telemetry={failure_snapshot}"
        ) from exc
    finally:
        client.close(reset=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True, type=numeric_ipv4)
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--diagnostic-port", type=int, default=1235)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--ready-timeout", type=float, default=20.0)
    parser.add_argument(
        "--ready-only",
        action="store_true",
        help="verify pristine UDP readiness without opening TCP 1234",
    )
    parser.add_argument("--transcript", required=True, type=Path)
    parser.add_argument("--summary", required=True, type=Path)
    args = parser.parse_args()
    if not 0 < args.port <= 65535 or not 0 < args.diagnostic_port <= 65535:
        parser.error("ports must be between 1 and 65535")
    if args.timeout <= 0 or args.ready_timeout <= 0:
        parser.error("timeouts must be positive")

    transcript = Transcript(args.transcript)
    started = time.monotonic()
    try:
        if args.ready_only:
            result = {
                "format": "VITADEBUGGER-RSP-ADMISSION-READY-1",
                "status": "PASS",
                "tested_at": utc_now(),
                "ready_snapshot": wait_debugger_ready(
                    args.host,
                    args.diagnostic_port,
                    args.ready_timeout,
                    transcript,
                ),
            }
        else:
            result = run(args, transcript)
        result["duration_seconds"] = time.monotonic() - started
        args.summary.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except BaseException as exc:
        result = {
            "format": "VITADEBUGGER-RSP-ADMISSION-DIAGNOSTIC-1",
            "status": "FAIL",
            "tested_at": utc_now(),
            "error": type(exc).__name__,
            "detail": str(exc),
            "duration_seconds": time.monotonic() - started,
        }
        args.summary.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(json.dumps(result, indent=2, sort_keys=True))
        return 1
    finally:
        transcript.close()


if __name__ == "__main__":
    raise SystemExit(main())

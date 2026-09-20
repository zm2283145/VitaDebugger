#!/usr/bin/env python3
"""Bounded out-of-band diagnosis of the first RSP admission transition."""

from __future__ import annotations

import argparse
import json
import socket
import struct
import time
from pathlib import Path
from typing import Any

from rsp_network_retail_gate import Rsp, Transcript, parse_offsets, utc_now


class AdmissionDiagnosticFailure(RuntimeError):
    pass


REQUEST = b"UVDB-ADMISSION-1"
VERSION = 1
EVENT_NAMES = (
    "listener_ready",
    "test_title_ready",
    "candidate_accepted",
    "valid_frame",
    "promotion_begin",
    "socket_published",
    "protocol_acquired",
    "target_stopped",
    "main_loop_entered",
    "first_packet",
    "target_running",
    "network_closing",
)
EVENT_COUNT = len(EVENT_NAMES)
STATE_TARGET_STOPPED = 1 << 0
STATE_NETWORK_CLOSING = 1 << 1
STATE_TEST_TITLE_READY = 1 << 2
STATE_PROTOCOL_OWNED = 1 << 3
SNAPSHOT_FORMAT = (
    "<4I"
    + f"{EVENT_COUNT}I"
    + f"{EVENT_COUNT}I"
    + f"{EVENT_COUNT}i"
    + f"{EVENT_COUNT}I"
    + f"{EVENT_COUNT}I"
    + f"{EVENT_COUNT}I"
    + "3i5I"
)
SNAPSHOT_SIZE = struct.calcsize(SNAPSHOT_FORMAT)


def parse_snapshot(data: bytes) -> dict[str, Any]:
    if len(data) != SNAPSHOT_SIZE:
        raise AdmissionDiagnosticFailure(
            f"diagnostic response size {len(data)} != {SNAPSHOT_SIZE}"
        )
    values = iter(struct.unpack(SNAPSHOT_FORMAT, data))
    version = next(values)
    size = next(values)
    revision = next(values)
    event_count = next(values)
    if version != VERSION or size != SNAPSHOT_SIZE or event_count != EVENT_COUNT:
        raise AdmissionDiagnosticFailure(
            "diagnostic ABI mismatch: "
            f"version={version} size={size} events={event_count}"
        )

    sequences = list(next(values) for _ in EVENT_NAMES)
    occurrences = list(next(values) for _ in EVENT_NAMES)
    descriptors = list(next(values) for _ in EVENT_NAMES)
    generations = list(next(values) for _ in EVENT_NAMES)
    owners = list(next(values) for _ in EVENT_NAMES)
    states = list(next(values) for _ in EVENT_NAMES)
    current_socket = next(values)
    current_candidate = next(values)
    current_listener = next(values)
    current_generation = next(values)
    current_owner = next(values)
    current_state = next(values)
    first_packet_size = next(values)
    connection_epoch = next(values)
    return {
        "version": version,
        "size": size,
        "revision": revision,
        "events": {
            name: {
                "sequence": sequences[index],
                "occurrences": occurrences[index],
                "descriptor": descriptors[index],
                "generation": generations[index],
                "owner": owners[index],
                "state": states[index],
            }
            for index, name in enumerate(EVENT_NAMES)
        },
        "current": {
            "socket": current_socket,
            "candidate": current_candidate,
            "listener": current_listener,
            "generation": current_generation,
            "owner": current_owner,
            "state": current_state,
            "target_stopped": bool(current_state & STATE_TARGET_STOPPED),
            "network_closing": bool(current_state & STATE_NETWORK_CLOSING),
            "test_title_ready": bool(current_state & STATE_TEST_TITLE_READY),
            "protocol_owned": bool(current_state & STATE_PROTOCOL_OWNED),
            "first_packet_size": first_packet_size,
            "connection_epoch": connection_epoch,
        },
    }


def query_snapshot(
    host: str, port: int, timeout: float, transcript: Transcript
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    expected_host = socket.gethostbyname(host)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        while time.monotonic() < deadline:
            client.settimeout(min(0.5, max(0.01, deadline - time.monotonic())))
            try:
                transcript.event(
                    "diagnostic_query", peer=f"{host}:{port}",
                    request=REQUEST.decode("ascii"),
                )
                client.sendto(REQUEST, (host, port))
                data, peer = client.recvfrom(SNAPSHOT_SIZE + 1)
                if peer[0] != expected_host:
                    continue
                snapshot = parse_snapshot(data)
                transcript.event(
                    "diagnostic_snapshot",
                    peer=f"{peer[0]}:{peer[1]}",
                    snapshot=snapshot,
                )
                return snapshot
            except socket.timeout:
                continue
            except OSError as exc:
                last_error = exc
                time.sleep(0.05)
    raise AdmissionDiagnosticFailure(
        f"no diagnostic snapshot within {timeout}s: {last_error}"
    )


def wait_debugger_ready(
    host: str, port: int, timeout: float, transcript: Transcript
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_snapshot: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        last_snapshot = query_snapshot(
            host, port, min(1.0, remaining), transcript
        )
        events = last_snapshot["events"]
        current = last_snapshot["current"]
        if (
            events["listener_ready"]["occurrences"] >= 1
            and events["test_title_ready"]["occurrences"] >= 1
            and current["listener"] >= 0
            and current["socket"] < 0
            and current["candidate"] < 0
            and current["test_title_ready"]
            and not current["network_closing"]
        ):
            transcript.event("debugger_ready", snapshot=last_snapshot)
            return last_snapshot
        time.sleep(0.1)
    raise AdmissionDiagnosticFailure(
        f"debugger-ready marker not observed: {last_snapshot}"
    )


def require_order(snapshot: dict[str, Any]) -> None:
    required = (
        "listener_ready",
        "test_title_ready",
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
    ]
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
    while time.monotonic() < deadline:
        last_snapshot = query_snapshot(
            host, port, min(1.0, deadline - time.monotonic()), transcript
        )
        current = last_snapshot["current"]
        if (
            current["socket"] < 0
            and current["owner"] == 0
            and not current["target_stopped"]
        ):
            return last_snapshot
        time.sleep(0.05)
    raise AdmissionDiagnosticFailure(
        f"detach state did not quiesce: {last_snapshot}"
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
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--diagnostic-port", type=int, default=1235)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--ready-timeout", type=float, default=20.0)
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

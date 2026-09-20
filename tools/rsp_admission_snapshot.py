#!/usr/bin/env python3
"""Strict parser and bounded UDP transport for RSP admission snapshots."""

from __future__ import annotations

import argparse
import ipaddress
import socket
import struct
import time
from typing import Any


class AdmissionDiagnosticFailure(RuntimeError):
    pass


class SnapshotUnavailable(AdmissionDiagnosticFailure):
    pass


REQUEST = b"UVDB-ADMISSION-1"
VERSION = 2
EVENT_NAMES = (
    "listener_ready",
    "test_title_ready",
    "candidate_accepted",
    "valid_frame",
    "promotion_begin",
    "socket_published",
    "exception_rejected",
    "protocol_acquired",
    "protocol_rejected",
    "target_stopped",
    "main_loop_entered",
    "first_packet",
    "target_running",
    "protocol_released",
    "session_normalized",
    "listener_reopened",
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
    + f"{EVENT_COUNT}I"
    + f"{EVENT_COUNT}i"
    + "3i7I"
)
SNAPSHOT_SIZE = struct.calcsize(SNAPSHOT_FORMAT)


def numeric_ipv4(value: str) -> str:
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as exc:
        raise argparse.ArgumentTypeError(
            "--host must be a numeric IPv4 literal"
        ) from exc
    if str(address) != value:
        raise argparse.ArgumentTypeError(
            "--host must use canonical dotted-decimal IPv4"
        )
    return value


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
    epochs = list(next(values) for _ in EVENT_NAMES)
    results = list(next(values) for _ in EVENT_NAMES)
    current_socket = next(values)
    current_candidate = next(values)
    current_listener = next(values)
    current_generation = next(values)
    current_owner = next(values)
    current_owner_epoch = next(values)
    current_state = next(values)
    first_packet_size = next(values)
    connection_epoch = next(values)
    dropped_event_writes = next(values)
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
                "epoch": epochs[index],
                "result": results[index],
            }
            for index, name in enumerate(EVENT_NAMES)
        },
        "current": {
            "socket": current_socket,
            "candidate": current_candidate,
            "listener": current_listener,
            "generation": current_generation,
            "owner": current_owner,
            "owner_epoch": current_owner_epoch,
            "state": current_state,
            "target_stopped": bool(current_state & STATE_TARGET_STOPPED),
            "network_closing": bool(current_state & STATE_NETWORK_CLOSING),
            "test_title_ready": bool(current_state & STATE_TEST_TITLE_READY),
            "protocol_owned": bool(current_state & STATE_PROTOCOL_OWNED),
            "first_packet_size": first_packet_size,
            "connection_epoch": connection_epoch,
            "dropped_event_writes": dropped_event_writes,
        },
    }


def query_snapshot(
    host: str, port: int, timeout: float, transcript: Any
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        while time.monotonic() < deadline:
            client.settimeout(min(0.5, max(0.01, deadline - time.monotonic())))
            transcript.event(
                "diagnostic_query", peer=f"{host}:{port}",
                request=REQUEST.decode("ascii"),
            )
            try:
                client.sendto(REQUEST, (host, port))
                data, peer = client.recvfrom(SNAPSHOT_SIZE + 1)
            except socket.timeout:
                transcript.event(
                    "diagnostic_miss", peer=f"{host}:{port}",
                    reason="timeout",
                )
                continue
            except OSError as exc:
                last_error = exc
                transcript.event(
                    "diagnostic_miss", peer=f"{host}:{port}",
                    reason="transport_error",
                    error=type(exc).__name__,
                    detail=str(exc),
                )
                time.sleep(0.05)
                continue
            if peer[0] != host:
                transcript.event(
                    "diagnostic_miss",
                    peer=f"{peer[0]}:{peer[1]}",
                    reason="unexpected_peer",
                )
                continue
            snapshot = parse_snapshot(data)
            transcript.event(
                "diagnostic_snapshot",
                peer=f"{peer[0]}:{peer[1]}",
                snapshot=snapshot,
            )
            return snapshot
    raise SnapshotUnavailable(
        f"no diagnostic snapshot within {timeout}s: {last_error}"
    )

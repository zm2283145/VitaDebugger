#!/usr/bin/env python3
"""Capture one delayed-request raw-receive diagnostic from VitaDebugger."""

from __future__ import annotations

import argparse
import base64
import json
import math
import socket
import string
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class DiagnosticFailure(RuntimeError):
    pass


MAX_FRAME_BYTES = 0x40000
MAX_RESPONSE_BYTES = 0x100000
HEX_BYTES = frozenset(string.hexdigits.encode("ascii"))


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def frame(payload: bytes) -> bytes:
    return b"$" + payload + f"#{sum(payload) & 0xFF:02x}".encode("ascii")


class Transcript:
    def __init__(self, path: Path):
        self.handle = path.open("x", encoding="utf-8", newline="\n")

    def event(self, event: str, **fields: Any) -> None:
        record = {"time": utc_now(), "monotonic": time.monotonic(), "event": event}
        record.update(fields)
        self.handle.write(json.dumps(record, sort_keys=True) + "\n")
        self.handle.flush()

    def wire(self, direction: str, session: str, data: bytes) -> None:
        self.event(
            "wire",
            direction=direction,
            session=session,
            size=len(data),
            base64=base64.b64encode(data).decode("ascii"),
        )

    def close(self) -> None:
        self.handle.close()


class Rsp:
    def __init__(
        self,
        host: str,
        port: int,
        connect_timeout: float,
        io_timeout: float,
        transcript: Transcript,
        session: str,
    ):
        self.transcript = transcript
        self.session = session
        self.buffer = bytearray()
        self.io_timeout = io_timeout
        self.closed = False
        deadline = time.monotonic() + connect_timeout
        last_error: OSError | None = None
        while time.monotonic() < deadline:
            try:
                self.sock = socket.create_connection(
                    (host, port), timeout=min(1.0, connect_timeout)
                )
                self.sock.settimeout(io_timeout)
                transcript.event("connect", session=session, peer=f"{host}:{port}")
                return
            except OSError as exc:
                last_error = exc
                time.sleep(0.1)
        raise DiagnosticFailure(
            f"{session}: listener did not accept within {connect_timeout}s: {last_error}"
        )

    def send(self, data: bytes) -> None:
        self.transcript.wire("send_attempt", self.session, data)
        self.sock.sendall(data)
        self.transcript.event("send_complete", session=self.session, size=len(data))

    def read_byte(self, deadline: float) -> int:
        if not self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise DiagnosticFailure(f"{self.session}: response deadline expired")
            self.sock.settimeout(min(self.io_timeout, remaining))
            data = self.sock.recv(65536)
            self.transcript.wire("recv", self.session, data)
            if not data:
                raise EOFError(f"{self.session}: peer closed")
            self.buffer.extend(data)
        value = self.buffer[0]
        del self.buffer[0]
        return value

    def expect_ack(self) -> None:
        value = self.read_byte(time.monotonic() + self.io_timeout)
        if value != ord("+"):
            raise DiagnosticFailure(
                f"{self.session}: expected '+', received 0x{value:02x}"
            )

    def read_packet(self, deadline: float) -> bytes:
        first = self.read_byte(deadline)
        if first != ord("$"):
            raise DiagnosticFailure(
                f"{self.session}: expected '$', received 0x{first:02x}"
            )
        payload = bytearray()
        while True:
            value = self.read_byte(deadline)
            if value == ord("#"):
                break
            payload.append(value)
            if len(payload) > MAX_FRAME_BYTES:
                raise DiagnosticFailure(f"{self.session}: response frame is oversized")
        checksum = bytes((self.read_byte(deadline), self.read_byte(deadline)))
        if any(value not in HEX_BYTES for value in checksum):
            raise DiagnosticFailure(
                f"{self.session}: response checksum is not two hexadecimal digits"
            )
        received = int(checksum, 16)
        expected = sum(payload) & 0xFF
        if received != expected:
            raise DiagnosticFailure(
                f"{self.session}: checksum {received:02x} != {expected:02x}"
            )
        return bytes(payload)

    def read_response(self, *, acknowledge_final: bool) -> bytes:
        deadline = time.monotonic() + self.io_timeout
        console_bytes = 0
        while True:
            payload = self.read_packet(deadline)
            if payload.startswith(b"O") and payload != b"OK":
                encoded = payload[1:]
                if len(encoded) % 2 or any(value not in HEX_BYTES for value in encoded):
                    raise DiagnosticFailure(
                        f"{self.session}: malformed console packet"
                    )
                try:
                    decoded = bytes.fromhex(encoded.decode("ascii"))
                except (UnicodeDecodeError, ValueError) as exc:
                    raise DiagnosticFailure(
                        f"{self.session}: malformed console packet"
                    ) from exc
                console_bytes += len(decoded)
                if console_bytes > MAX_RESPONSE_BYTES:
                    raise DiagnosticFailure(
                        f"{self.session}: console response exceeds bound"
                    )
                self.send(b"+")
                continue
            if acknowledge_final:
                self.send(b"+")
            return payload

    def send_request(self, payload: bytes) -> None:
        self.send(frame(payload))
        self.expect_ack()

    def acknowledge_and_request(self, payload: bytes) -> None:
        self.send(b"+" + frame(payload))
        self.expect_ack()

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        self.sock.close()
        self.transcript.event("close", session=self.session)


def require_packet_size(payload: bytes) -> str:
    packet_size = next(
        (item for item in payload.split(b";") if item.startswith(b"PacketSize=")),
        None,
    )
    if packet_size is None:
        raise DiagnosticFailure("qSupported response omitted PacketSize")
    value = packet_size.split(b"=", 1)[1]
    int(value, 16)
    return value.decode("ascii")


def parse_diagnostic(payload: bytes) -> dict[str, Any]:
    try:
        fields = dict(item.split("=", 1) for item in payload.decode("ascii").split(";"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise DiagnosticFailure(f"invalid diagnostic response {payload!r}") from exc
    expected = {
        "captured",
        "result",
        "descriptor",
        "generation",
        "closing",
        "phase",
        "packet_io",
        "target_stopped",
        "protocol_owned",
        "errno_available",
    }
    if set(fields) != expected:
        raise DiagnosticFailure(
            f"diagnostic fields differ: missing={sorted(expected - set(fields))}, "
            f"extra={sorted(set(fields) - expected)}"
        )
    decoded: dict[str, Any] = {}
    for name, value in fields.items():
        if len(value) != 8:
            raise DiagnosticFailure(f"diagnostic {name} is not fixed-width hex")
        decoded[name] = int(value, 16)
    if decoded["captured"] != 1:
        raise DiagnosticFailure("diagnostic did not capture a raw receive result")
    raw = decoded["result"]
    decoded["result_signed"] = raw - 0x100000000 if raw & 0x80000000 else raw
    decoded["raw"] = payload.decode("ascii")
    return decoded


def retrieve_after_qsupported(client: Rsp) -> tuple[str, dict[str, Any]]:
    client.send_request(b"qSupported:multiprocess+;swbreak+;vContSupported+")
    supported = client.read_response(acknowledge_final=False)
    packet_size = require_packet_size(supported)
    client.acknowledge_and_request(b"qUvdbRawRecvDiagnostic")
    diagnostic = parse_diagnostic(client.read_response(acknowledge_final=False))
    return packet_size, diagnostic


def run(args: argparse.Namespace, transcript: Transcript) -> dict[str, Any]:
    started = time.monotonic()
    primary = Rsp(
        args.host,
        args.port,
        args.listener_timeout,
        args.timeout,
        transcript,
        "delayed-qOffsets",
    )
    qoffsets_status = "not-sent"
    qoffsets_response: str | None = None
    retrieval = "same-connection"
    retrieval_ack = "pipelined-with-detach"
    try:
        primary.send_request(b"qSupported:multiprocess+;swbreak+;vContSupported+")
        supported = primary.read_response(acknowledge_final=False)
        packet_size = require_packet_size(supported)
        primary.send(b"+")
        transcript.event("bounded_gap_start", seconds=args.gap)
        time.sleep(args.gap)
        transcript.event("bounded_gap_end", seconds=args.gap)
        qoffsets_status = "send-attempt"
        primary.send(frame(b"qOffsets"))
        qoffsets_status = "sent"
        primary.expect_ack()
        response = primary.read_response(acknowledge_final=False)
        qoffsets_response = response.decode("ascii", "replace")
        qoffsets_status = "response"
        primary.acknowledge_and_request(b"qUvdbRawRecvDiagnostic")
        diagnostic = parse_diagnostic(primary.read_response(acknowledge_final=False))
    except (EOFError, ConnectionResetError, ConnectionAbortedError, BrokenPipeError) as exc:
        if qoffsets_status not in {"send-attempt", "sent", "response"}:
            raise DiagnosticFailure(
                f"connection failed before delayed qOffsets was sent: {exc}"
            ) from exc
        qoffsets_status = (
            "peer-closed-after-send"
            if qoffsets_status in {"sent", "response"}
            else "peer-closed-before-send-complete"
        )
        transcript.event(
            "expected_failure_boundary",
            error=type(exc).__name__,
            detail=str(exc),
        )
        try:
            primary.close()
        except OSError:
            pass
        retrieval = "reconnect"
        recovery = Rsp(
            args.host,
            args.port,
            args.listener_timeout,
            args.timeout,
            transcript,
            "diagnostic-retrieval",
        )
        try:
            recovery.send_request(b"qUvdbRawRecvDiagnostic")
            diagnostic = parse_diagnostic(
                recovery.read_response(acknowledge_final=False)
            )
            try:
                recovery.send(b"+")
                retrieval_ack = "sent"
            except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
                retrieval_ack = "peer-closed"
        finally:
            recovery.close()
    else:
        primary.acknowledge_and_request(b"D")
        detach = primary.read_response(acknowledge_final=True)
        if detach != b"OK":
            raise DiagnosticFailure(f"detach returned {detach!r}")
    finally:
        primary.close()
    phase = diagnostic["phase"]
    if phase not in {1, 2}:
        raise DiagnosticFailure(f"diagnostic captured unexpected packet phase {phase}")
    capture_boundary = (
        "deliberate-request-gap" if phase == 1 else "response-ack-poll"
    )
    expected_context = {
        "result_signed": -35,
        "closing": 0,
        "packet_io": 1,
        "target_stopped": 1,
        "protocol_owned": 1,
        "errno_available": 0,
    }
    mismatches = {
        name: {"expected": expected, "actual": diagnostic[name]}
        for name, expected in expected_context.items()
        if diagnostic[name] != expected
    }
    if diagnostic["descriptor"] > 0x7FFFFFFF or diagnostic["generation"] == 0:
        mismatches["connection_identity"] = {
            "expected": "nonnegative descriptor and nonzero generation",
            "actual": {
                "descriptor": diagnostic["descriptor"],
                "generation": diagnostic["generation"],
            },
        }
    result = {
        "format": "VITADEBUGGER-RSP-RAW-RECV-DIAGNOSTIC-1",
        "status": "PASS" if not mismatches else "CAPTURED_UNEXPECTED_STATE",
        "started_at": utc_now(),
        "duration_seconds": time.monotonic() - started,
        "gap_seconds": args.gap,
        "packet_size": packet_size,
        "qoffsets_status": qoffsets_status,
        "qoffsets_response": qoffsets_response,
        "diagnostic_retrieval": retrieval,
        "diagnostic_response_ack": retrieval_ack,
        "capture_boundary": capture_boundary,
        "diagnostic": diagnostic,
        "context_mismatches": mismatches,
    }
    transcript.event("diagnostic_complete", result=result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--listener-timeout", type=float, default=15.0)
    parser.add_argument("--gap", type=float, default=0.5)
    parser.add_argument("--transcript", required=True, type=Path)
    parser.add_argument("--summary", required=True, type=Path)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if not math.isfinite(args.gap) or not 0.1 <= args.gap <= 5.0:
        parser.error("--gap must be between 0.1 and 5 seconds")
    if not math.isfinite(args.timeout) or not 0.1 <= args.timeout <= 30.0:
        parser.error("--timeout must be between 0.1 and 30 seconds")
    if (
        not math.isfinite(args.listener_timeout)
        or not 0.1 <= args.listener_timeout <= 30.0
    ):
        parser.error("--listener-timeout must be between 0.1 and 30 seconds")
    transcript = Transcript(args.transcript)
    started = time.monotonic()
    try:
        result = run(args, transcript)
        args.summary.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result["status"] == "PASS" else 1
    except BaseException as exc:
        transcript.event("diagnostic_failure", error=type(exc).__name__, detail=str(exc))
        result = {
            "format": "VITADEBUGGER-RSP-RAW-RECV-DIAGNOSTIC-1",
            "status": "FAIL",
            "error": type(exc).__name__,
            "detail": str(exc),
            "duration_seconds": time.monotonic() - started,
        }
        args.summary.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(json.dumps(result, indent=2, sort_keys=True))
        return 1
    finally:
        transcript.close()


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Deterministic raw-RSP smoke test for VitaDebugger console output."""

from __future__ import annotations

import argparse
import socket
import time
from dataclasses import dataclass, field


class SmokeFailure(RuntimeError):
    pass


def frame(payload: bytes) -> bytes:
    checksum = sum(payload) & 0xFF
    return b"$" + payload + f"#{checksum:02x}".encode("ascii")


def decode_console(payload: bytes) -> bytes:
    if not payload.startswith(b"O") or len(payload) % 2 != 1:
        raise SmokeFailure(f"malformed console packet: {payload[:40]!r}")
    try:
        return bytes.fromhex(payload[1:].decode("ascii"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise SmokeFailure("console packet contains invalid hexadecimal data") from exc


@dataclass
class RspClient:
    host: str
    port: int
    timeout: float
    connection: socket.socket = field(init=False)
    buffered: bytearray = field(default_factory=bytearray, init=False)
    ack_mode: bool = field(default=True, init=False)

    def __post_init__(self) -> None:
        self.connection = socket.create_connection(
            (self.host, self.port), timeout=self.timeout
        )
        self.connection.settimeout(self.timeout)

    def close(self) -> None:
        self.connection.close()

    def _read_byte(self) -> int:
        if not self.buffered:
            block = self.connection.recv(4096)
            if not block:
                raise SmokeFailure("debugger connection closed unexpectedly")
            self.buffered.extend(block)
        value = self.buffered[0]
        del self.buffered[0]
        return value

    def _expect_ack(self) -> None:
        while True:
            value = self._read_byte()
            if value == ord("+"):
                return
            if value == ord("-"):
                raise SmokeFailure("stub rejected the request checksum")
            if value not in b"\r\n":
                raise SmokeFailure(f"expected RSP acknowledgement, got 0x{value:02x}")

    def read_packet(self, timeout: float | None = None) -> bytes:
        previous_timeout = self.connection.gettimeout()
        if timeout is not None:
            self.connection.settimeout(timeout)
        try:
            while self._read_byte() != ord("$"):
                pass
            payload = bytearray()
            while True:
                value = self._read_byte()
                if value == ord("#"):
                    break
                payload.append(value)
            try:
                received_checksum = int(
                    bytes((self._read_byte(), self._read_byte())), 16
                )
            except ValueError as exc:
                raise SmokeFailure("stub returned a malformed packet checksum") from exc
            expected_checksum = sum(payload) & 0xFF
            if received_checksum != expected_checksum:
                if self.ack_mode:
                    self.connection.sendall(b"-")
                raise SmokeFailure(
                    f"packet checksum mismatch: {received_checksum:02x} != "
                    f"{expected_checksum:02x}"
                )
            if self.ack_mode:
                self.connection.sendall(b"+")
            return bytes(payload)
        finally:
            if timeout is not None:
                self.connection.settimeout(previous_timeout)

    def request(self, payload: bytes) -> bytes:
        self.connection.sendall(frame(payload))
        if self.ack_mode:
            self._expect_ack()
        return self.read_packet()

    def negotiate(self) -> bytes:
        supported = self.request(
            b"qSupported:multiprocess+;swbreak+;hwbreak+;vContSupported+"
        )
        features = supported.split(b";")
        if b"QStartNoAckMode+" not in features:
            raise SmokeFailure("stub did not advertise QStartNoAckMode+")
        packet_size = next(
            (item for item in features if item.startswith(b"PacketSize=")), None
        )
        if packet_size is None or int(packet_size.split(b"=", 1)[1], 16) < 261:
            raise SmokeFailure("stub advertised a missing or unusable PacketSize")

        # request() sends the final '+' acknowledging the framed OK response.
        if self.request(b"QStartNoAckMode") != b"OK":
            raise SmokeFailure("stub rejected QStartNoAckMode")
        self.ack_mode = False
        return supported


def expect_stop(client: RspClient, first_request: bool) -> bytes:
    packet = client.request(b"?") if first_request else client.read_packet()
    while not (packet.startswith(b"T") or packet.startswith(b"S")):
        # A complete O frame may already have been in flight when Ctrl-C won
        # ownership. It is valid only before, never after, the stop reply.
        if first_request or not packet.startswith(b"O"):
            raise SmokeFailure(f"expected stop reply, got {packet[:80]!r}")
        packet = client.read_packet()
    return packet


def collect_console(
    client: RspClient, seconds: float, require_markers: bool = True
) -> tuple[bytes, list[bytes]]:
    deadline = time.monotonic() + seconds
    output = bytearray()
    packets: list[bytes] = []
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        try:
            payload = client.read_packet(timeout=min(0.5, max(0.01, remaining)))
        except socket.timeout:
            continue
        packets.append(payload)
        if payload.startswith(b"O"):
            output.extend(decode_console(payload))
        elif payload.startswith((b"T", b"S")):
            raise SmokeFailure(
                "target stopped unexpectedly while collecting output: "
                f"{payload[:80]!r}"
            )
        else:
            raise SmokeFailure(f"unexpected running-state packet: {payload[:80]!r}")
    if require_markers:
        if b"[uvdb stdout]" not in output:
            raise SmokeFailure("stdout marker did not reach the GDB console stream")
        if b"[uvdb stderr]" not in output:
            raise SmokeFailure("stderr marker did not reach the GDB console stream")
    return bytes(output), packets


def assert_quiet_while_stopped(client: RspClient, seconds: float = 0.35) -> None:
    try:
        payload = client.read_packet(timeout=seconds)
    except socket.timeout:
        return
    if payload.startswith(b"O"):
        raise SmokeFailure("console packet arrived after the stop reply")
    raise SmokeFailure(f"unexpected packet while stopped: {payload[:80]!r}")


def run_once(host: str, port: int, timeout: float, collect_seconds: float) -> None:
    client = RspClient(host, port, timeout)
    try:
        supported = client.negotiate()
        initial_stop = expect_stop(client, first_request=True)
        print(f"qSupported: {supported.decode('ascii', 'replace')}")
        print(f"initial stop: {initial_stop.decode('ascii', 'replace')}")

        client.connection.sendall(frame(b"c"))
        output, packets = collect_console(client, collect_seconds)
        print(f"console packets: {len(packets)}, decoded bytes: {len(output)}")
        print(output.decode("utf-8", "backslashreplace"), end="")

        client.connection.sendall(b"\x03")
        stop = expect_stop(client, first_request=False)
        print(f"Ctrl-C stop: {stop.decode('ascii', 'replace')}")
        assert_quiet_while_stopped(client)

        if client.request(b"D") != b"OK":
            raise SmokeFailure("clean detach did not return OK")
    finally:
        client.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--collect-seconds", type=float, default=3.0)
    parser.add_argument(
        "--reconnect",
        action="store_true",
        help="repeat the complete handshake/run/stop/detach cycle",
    )
    args = parser.parse_args()

    run_once(args.host, args.port, args.timeout, args.collect_seconds)
    if args.reconnect:
        time.sleep(0.5)
        run_once(args.host, args.port, args.timeout, args.collect_seconds)
    print("PASS: GDB no-ack stdout/stderr console smoke test")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, SmokeFailure) as exc:
        print(f"FAIL: {exc}")
        raise SystemExit(1) from exc

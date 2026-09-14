#!/usr/bin/env python3
"""Live raw-RSP validation for VitaDebugger's read-only monitor commands."""

from __future__ import annotations

import argparse
import re
import socket
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field


class SmokeFailure(RuntimeError):
    pass


def frame(payload: bytes) -> bytes:
    checksum = sum(payload) & 0xFF
    return b"$" + payload + f"#{checksum:02x}".encode("ascii")


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
                raise SmokeFailure("stub rejected a request checksum")
            if value not in b"\r\n":
                raise SmokeFailure(
                    f"expected RSP acknowledgement, got 0x{value:02x}"
                )

    def read_packet(self) -> bytes:
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
            raise SmokeFailure("stub returned a malformed checksum") from exc
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

    def request(self, payload: bytes) -> bytes:
        self.connection.sendall(frame(payload))
        if self.ack_mode:
            self._expect_ack()
        return self.read_packet()

    def negotiate(self) -> bytes:
        supported = self.request(
            b"qSupported:multiprocess+;swbreak+;vContSupported+"
        )
        features = supported.split(b";")
        if b"QStartNoAckMode+" not in features:
            raise SmokeFailure("stub did not advertise QStartNoAckMode+")
        packet_size = next(
            (item for item in features if item.startswith(b"PacketSize=")), None
        )
        if packet_size is None or int(packet_size.split(b"=", 1)[1], 16) < 4092:
            raise SmokeFailure("stub advertised an unusable PacketSize")
        if self.request(b"QStartNoAckMode") != b"OK":
            raise SmokeFailure("stub rejected QStartNoAckMode")
        self.ack_mode = False
        return supported


def decode_hex_output(payload: bytes) -> bytes:
    if len(payload) & 1:
        raise SmokeFailure(f"monitor output has odd hex length: {payload[:80]!r}")
    try:
        return bytes.fromhex(payload.decode("ascii"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise SmokeFailure(f"monitor output is not valid hexadecimal: {payload[:80]!r}") from exc


def monitor_request(client: RspClient, command: str) -> str:
    request = b"qRcmd," + command.encode("ascii").hex().encode("ascii")
    response = client.request(request)
    output = bytearray()
    while response.startswith(b"O"):
        output.extend(decode_hex_output(response[1:]))
        response = client.read_packet()
    if response == b"OK":
        pass
    elif response.startswith(b"E"):
        raise SmokeFailure(f"monitor {command!r} returned {response!r}")
    else:
        output.extend(decode_hex_output(response))
    try:
        return output.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise SmokeFailure("monitor output is not UTF-8 text") from exc


def collect_threads(client: RspClient) -> list[int]:
    response = client.request(b"qfThreadInfo")
    threads: list[int] = []
    while response != b"l":
        if not response.startswith(b"m"):
            raise SmokeFailure(f"malformed thread inventory: {response[:80]!r}")
        try:
            threads.extend(int(value, 16) for value in response[1:].split(b","))
        except ValueError as exc:
            raise SmokeFailure("thread inventory contains a malformed ID") from exc
        response = client.request(b"qsThreadInfo")
    if not threads or len(threads) != len(set(threads)):
        raise SmokeFailure("thread inventory is empty or contains duplicates")
    return threads


def collect_libraries(client: RspClient, chunk_size: int = 0x400) -> list[str]:
    output = bytearray()
    while True:
        request = (
            f"qXfer:libraries:read::{len(output):x},{chunk_size:x}".encode("ascii")
        )
        response = client.request(request)
        if not response or response[:1] not in (b"m", b"l"):
            raise SmokeFailure(f"malformed library-list chunk: {response[:80]!r}")
        output.extend(response[1:])
        if response[:1] == b"l":
            break
        if len(output) > 1024 * 1024:
            raise SmokeFailure("library list exceeded its validation bound")
    try:
        root = ET.fromstring(bytes(output))
    except ET.ParseError as exc:
        raise SmokeFailure("library list is malformed XML") from exc
    return [entry.attrib["name"] for entry in root.findall("library")]


def require(text: str, *values: str) -> None:
    missing = [value for value in values if value not in text]
    if missing:
        raise SmokeFailure(f"monitor output is missing {missing!r}: {text[:240]!r}")
    if "... output truncated" in text:
        raise SmokeFailure("monitor output unexpectedly truncated on the live target")


def run_once(host: str, port: int, timeout: float) -> None:
    client = RspClient(host, port, timeout)
    try:
        supported = client.negotiate()
        stop = client.request(b"?")
        if not stop.startswith((b"T", b"S")):
            raise SmokeFailure(f"expected initial stop reply, got {stop[:80]!r}")

        rsp_threads = collect_threads(client)
        rsp_modules = collect_libraries(client)
        pc_before = client.request(b"pF")
        if not re.fullmatch(rb"[0-9a-fA-F]{8}", pc_before):
            raise SmokeFailure(f"invalid initial PC register reply: {pc_before!r}")

        help_text = monitor_request(client, "help")
        require(help_text, "help", "status", "threads", "modules", "read-only")

        status_text = monitor_request(client, "status")
        require(
            status_text,
            "state: connected",
            "target: stopped",
            "kernel: compatible",
            "stop-session: active, healthy",
            "VFP reads: enabled",
        )
        count_match = re.search(r"^  threads: (\d+)$", status_text, re.MULTILINE)
        if not count_match or int(count_match.group(1)) != len(rsp_threads):
            raise SmokeFailure("monitor status thread count differs from qfThreadInfo")

        thread_text = monitor_request(client, "threads")
        monitor_threads = {
            int(value, 16)
            for value in re.findall(r"^  (0x[0-9a-f]{8}) ", thread_text, re.MULTILINE)
        }
        if monitor_threads != set(rsp_threads):
            raise SmokeFailure(
                "monitor threads differs from qfThreadInfo: "
                f"monitor={sorted(monitor_threads)!r} rsp={sorted(rsp_threads)!r}"
            )
        require(thread_text, "[stopped", "Hg")

        module_text = monitor_request(client, "modules")
        module_names = re.findall(
            r"^  0x[0-9a-f]{8} ([^\r\n]+)$", module_text, re.MULTILINE
        )
        missing_modules = sorted(set(rsp_modules) - set(module_names))
        if missing_modules:
            raise SmokeFailure(
                f"monitor modules omitted qXfer modules: {missing_modules[:8]!r}"
            )
        require(module_text, "segment[0]: address=", "perms=")

        unknown = monitor_request(client, "not-a-command")
        require(unknown, "unknown monitor command", "monitor help")
        malformed_response = client.request(b"qRcmd,0")
        malformed = decode_hex_output(malformed_response).decode("utf-8")
        require(malformed, "malformed monitor command", "monitor help")

        if collect_threads(client) != rsp_threads:
            raise SmokeFailure("read-only monitor commands changed thread inventory")
        status_after = monitor_request(client, "status")
        if status_after != status_text:
            raise SmokeFailure("read-only monitor commands changed debugger status")
        if client.request(b"pF") != pc_before:
            raise SmokeFailure("read-only monitor commands changed the selected PC")
        if client.request(b"D") != b"OK":
            raise SmokeFailure("clean detach did not return OK")

        print(f"qSupported: {supported.decode('ascii', 'replace')}")
        print(f"initial stop: {stop.decode('ascii', 'replace')}")
        print(f"threads: {len(rsp_threads)}; modules: {len(module_names)}")
        print(help_text, end="")
        print(status_text, end="")
    finally:
        client.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument(
        "--reconnect",
        action="store_true",
        help="repeat the complete read-only command and detach gate",
    )
    args = parser.parse_args()

    run_once(args.host, args.port, args.timeout)
    if args.reconnect:
        time.sleep(0.5)
        run_once(args.host, args.port, args.timeout)
    print("PASS: read-only GDB qRcmd monitor commands")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, SmokeFailure) as exc:
        print(f"FAIL: {exc}")
        raise SystemExit(1) from exc

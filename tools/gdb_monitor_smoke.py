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
    console_output: bytearray = field(default_factory=bytearray, init=False)
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

    def request(self, payload: bytes, *, demux_console: bool = True) -> bytes:
        self.connection.sendall(frame(payload))
        if self.ack_mode:
            self._expect_ack()
        return self.read_response() if demux_console else self.read_packet()

    def read_response(self) -> bytes:
        """Read one solicited reply while preserving intervening O packets."""
        response = self.read_packet()
        while is_console_output_packet(response):
            self.console_output.extend(decode_hex_output(response[1:]))
            response = self.read_packet()
        return response

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


def is_console_output_packet(payload: bytes) -> bool:
    """Distinguish an RSP O packet from the terminal success reply ``OK``."""
    return payload.startswith(b"O") and payload != b"OK"


def monitor_request(client: RspClient, command: str) -> str:
    request = b"qRcmd," + command.encode("ascii").hex().encode("ascii")
    # qRcmd deliberately returns its command text through O packets. Read this
    # request without the normal asynchronous-console demultiplexer so the
    # monitor output remains associated with the command being validated.
    response = client.request(request, demux_console=False)
    output = bytearray()
    while is_console_output_packet(response):
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


def parse_uint(text: str, pattern: str, label: str) -> int:
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        raise SmokeFailure(f"monitor output has no valid {label}: {text[:240]!r}")
    return int(match.group(1), 10)


def validate_console(text: str) -> None:
    require(
        text,
        "VitaDebugger console",
        "session: open",
        "no-ack=yes",
        "transport-failed=no",
        "queued:",
        "accepted:",
        "sent:",
        "dropped-total:",
        "transport: frames=",
        "transport-errors:",
    )
    dropped = re.search(
        r"^  dropped-total: (\d+) records / (\d+) bytes$", text, re.MULTILINE
    )
    errors = re.search(
        r"^  transport-errors: partial=(\d+) hard=(\d+) session=(\d+) "
        r"last-native=(0x[0-9a-fA-F]{8})$",
        text,
        re.MULTILINE,
    )
    if not dropped or tuple(map(int, dropped.groups())) != (0, 0):
        raise SmokeFailure("monitor console reports captured-output loss")
    if not errors or tuple(map(int, errors.groups()[:3])) != (0, 0, 0):
        raise SmokeFailure("monitor console reports transport errors")
    if errors.group(4).lower() != "0x00000000":
        raise SmokeFailure("monitor console reports a native transport error")


def validate_framebuffer(text: str, label: str) -> tuple[int, int, int, int]:
    match = re.search(
        rf"^  framebuffer-{label}: address=(0x[0-9a-fA-F]{{8}}) "
        r"size=(\d+)x(\d+) pitch=(\d+) "
        r"format=A8B8G8R8\(0x00000000\)$",
        text,
        re.MULTILINE,
    )
    if not match:
        raise SmokeFailure(f"monitor display has no valid {label} framebuffer")
    address = int(match.group(1), 16)
    width, height, pitch = map(int, match.groups()[1:])
    if not address or (width, height) != (960, 544) or pitch < width:
        raise SmokeFailure(f"monitor display reports incoherent {label} geometry")
    return address, width, height, pitch


def validate_display(text: str) -> int:
    require(
        text,
        "VitaDebugger display",
        "primary-head: 0",
        "maximum-framebuffer: 960x544",
    )
    if "unavailable" in text:
        raise SmokeFailure("monitor display reports an unavailable live query")
    vcount = parse_uint(text, r"^  vcount: (\d+)$", "vcount")
    refresh_match = re.search(
        r"^  refresh-rate: (\d+)\.(\d{3}) Hz$", text, re.MULTILINE
    )
    if not refresh_match:
        raise SmokeFailure("monitor display has no valid refresh rate")
    refresh_millihz = (
        int(refresh_match.group(1)) * 1000 + int(refresh_match.group(2))
    )
    if not 1_000 <= refresh_millihz <= 240_000:
        raise SmokeFailure("monitor display refresh rate is implausible")
    validate_framebuffer(text, "immediate")
    validate_framebuffer(text, "next")
    return vcount


def run_once(host: str, port: int, timeout: float) -> int:
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
        require(
            help_text,
            "help",
            "status",
            "threads",
            "modules",
            "console",
            "display",
            "read-only",
        )

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

        console_text = monitor_request(client, "console")
        validate_console(console_text)

        display_text = monitor_request(client, "display")
        display_vcount = validate_display(display_text)

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
        print(console_text, end="")
        print(display_text, end="")
        return display_vcount
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

    first_vcount = run_once(args.host, args.port, args.timeout)
    if args.reconnect:
        time.sleep(0.5)
        second_vcount = run_once(args.host, args.port, args.timeout)
        if first_vcount == second_vcount:
            raise SmokeFailure("monitor display vcount did not advance across reconnect")
    print("PASS: read-only GDB qRcmd monitor commands")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, SmokeFailure) as exc:
        print(f"FAIL: {exc}")
        raise SystemExit(1) from exc

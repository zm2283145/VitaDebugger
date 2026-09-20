#!/usr/bin/env python3
"""Bounded retail RSP/network lifecycle gate for VitaDebugger."""

from __future__ import annotations

import argparse
import base64
import ctypes
import json
import math
import socket
import string
import struct
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from rsp_admission_snapshot import (
    AdmissionDiagnosticFailure,
    SnapshotUnavailable,
    numeric_ipv4,
    query_snapshot,
)


class GateFailure(RuntimeError):
    pass


MAX_RSP_PAYLOAD = 0x3FFFC
MAX_CONSOLE_BYTES = 1024 * 1024
CORE_REGISTER_BYTES = 16 * 4
LEGACY_FPA_BYTES = (8 * 12) + 4
CPSR_BYTES = 4
LEGACY_G_REPLY_BYTES = CORE_REGISTER_BYTES + LEGACY_FPA_BYTES + CPSR_BYTES
HEX_BYTES = frozenset(string.hexdigits.encode("ascii"))
WINDOWS_FIONREAD = 0x4004667F


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def frame(payload: bytes) -> bytes:
    return b"$" + payload + f"#{sum(payload) & 0xff:02x}".encode("ascii")


def decode_console(payload: bytes) -> bytes:
    if not payload.startswith(b"O") or payload == b"OK":
        return b""
    encoded = payload[1:]
    if len(encoded) % 2 or any(value not in HEX_BYTES for value in encoded):
        raise GateFailure(f"invalid console packet {payload[:80]!r}")
    try:
        return bytes.fromhex(encoded.decode("ascii"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise GateFailure(f"invalid console packet {payload[:80]!r}") from exc


class Transcript:
    def __init__(self, path: Path):
        self.path = path
        self.handle = path.open("x", encoding="utf-8", newline="\n")

    def event(self, event: str, **fields: Any) -> None:
        record = {"time": utc_now(), "monotonic": time.monotonic(), "event": event}
        record.update(fields)
        self.handle.write(json.dumps(record, sort_keys=True) + "\n")
        self.handle.flush()

    def wire(self, direction: str, case: str, data: bytes) -> None:
        self.event(
            "wire",
            direction=direction,
            case=case,
            size=len(data),
            base64=base64.b64encode(data).decode("ascii"),
        )

    def close(self) -> None:
        self.handle.close()


class FailureTelemetryCapture:
    def __init__(
        self,
        host: str,
        port: int,
        timeout: float,
        expected_epoch: int | None,
        log: Transcript,
    ):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.expected_epoch = expected_epoch
        self.log = log
        self.result: dict[str, Any] | None = None

    def _capture(
        self,
        case: str,
        socket_open: bool,
        error: BaseException,
    ) -> None:
        if self.result is not None:
            return
        try:
            snapshot = query_snapshot(
                self.host, self.port, self.timeout, self.log)
            epoch = snapshot["current"]["connection_epoch"]
            if snapshot["current"]["dropped_event_writes"]:
                status = "dropped_event_writes"
            elif self.expected_epoch is None or epoch == self.expected_epoch:
                status = "captured"
            elif epoch < self.expected_epoch:
                status = "stale_epoch"
            else:
                status = "unexpected_epoch"
            self.result = {
                "status": status,
                "expected_epoch": self.expected_epoch,
                "observed_epoch": epoch,
                "socket_open_during_capture": socket_open,
                "case": case,
                "protocol_error": type(error).__name__,
                "protocol_detail": str(error),
                "snapshot": snapshot,
            }
        except SnapshotUnavailable as exc:
            self.result = {
                "status": "udp_unavailable",
                "expected_epoch": self.expected_epoch,
                "socket_open_during_capture": socket_open,
                "case": case,
                "telemetry_error": type(exc).__name__,
                "telemetry_detail": str(exc),
            }
        except AdmissionDiagnosticFailure as exc:
            self.result = {
                "status": "malformed_snapshot",
                "expected_epoch": self.expected_epoch,
                "socket_open_during_capture": socket_open,
                "case": case,
                "telemetry_error": type(exc).__name__,
                "telemetry_detail": str(exc),
            }
        except BaseException as exc:
            self.result = {
                "status": "telemetry_local_error",
                "expected_epoch": self.expected_epoch,
                "socket_open_during_capture": socket_open,
                "case": case,
                "telemetry_error": type(exc).__name__,
                "telemetry_detail": str(exc),
            }
        self.log.event("failure_telemetry", **self.result)

    def capture(self, client: "Rsp", error: BaseException) -> None:
        self._capture(
            client.case,
            not client.closed and client.sock.fileno() >= 0,
            error,
        )

    def capture_connect(self, case: str, error: BaseException) -> None:
        self._capture(case, False, error)


failure_telemetry: FailureTelemetryCapture | None = None


def capture_failure(client: "Rsp", error: BaseException) -> None:
    if failure_telemetry is not None and failure_telemetry.result is None:
        failure_telemetry.capture(client, error)


class Rsp:
    def __init__(self, host: str, port: int, timeout: float, log: Transcript, case: str):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.log = log
        self.case = case
        self.buffer = bytearray()
        self.ack_mode = True
        self.closed = False
        deadline = time.monotonic() + timeout
        last_error: OSError | None = None
        while time.monotonic() < deadline:
            try:
                self.sock = socket.create_connection((host, port), timeout=min(1.0, timeout))
                self.sock.settimeout(timeout)
                self.log.event("connect", case=case, peer=f"{host}:{port}")
                return
            except OSError as exc:
                last_error = exc
                time.sleep(0.1)
        error = GateFailure(
            f"{case}: could not connect within {timeout}s: {last_error}")
        if failure_telemetry is not None:
            failure_telemetry.capture_connect(case, error)
        raise error

    def set_timeout(self, timeout: float) -> None:
        self.sock.settimeout(timeout)

    def send(self, data: bytes) -> None:
        self.log.wire("send_attempt", self.case, data)
        self.sock.sendall(data)
        self.log.event("send_complete", case=self.case, size=len(data))

    def recv_chunk(self, deadline: float) -> bytes:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise GateFailure(f"{self.case}: response deadline expired")
        self.sock.settimeout(min(self.timeout, remaining))
        data = self.sock.recv(65536)
        self.log.wire("recv", self.case, data)
        if not data:
            raise EOFError("peer closed")
        return data

    def read_byte(self, deadline: float | None = None) -> int:
        if deadline is None:
            deadline = time.monotonic() + self.timeout
        if not self.buffer:
            self.buffer.extend(self.recv_chunk(deadline))
        value = self.buffer[0]
        del self.buffer[0]
        return value

    def expect_ack(self, expected: int = ord("+")) -> None:
        value = self.read_byte(time.monotonic() + self.timeout)
        if value != expected:
            raise GateFailure(
                f"{self.case}: expected ack {chr(expected)!r}, got 0x{value:02x}"
            )

    def read_packet(self) -> bytes:
        deadline = time.monotonic() + self.timeout
        first = self.read_byte(deadline)
        if first != ord("$"):
            raise GateFailure(
                f"{self.case}: expected packet start, got 0x{first:02x}"
            )
        payload = bytearray()
        while True:
            value = self.read_byte(deadline)
            if value == ord("#"):
                break
            payload.append(value)
            if len(payload) > MAX_RSP_PAYLOAD:
                raise GateFailure(f"{self.case}: response exceeds PacketSize")
        checksum = bytes((self.read_byte(deadline), self.read_byte(deadline)))
        if any(value not in HEX_BYTES for value in checksum):
            raise GateFailure(f"{self.case}: response has nonhex checksum")
        received = int(checksum, 16)
        expected = sum(payload) & 0xFF
        if received != expected:
            raise GateFailure(
                f"{self.case}: response checksum {received:02x} != {expected:02x}"
            )
        return bytes(payload)

    def read_response(self, *, acknowledge: bool = True) -> tuple[bytes, bytes]:
        console = bytearray()
        while True:
            payload = self.read_packet()
            if self.ack_mode and acknowledge:
                self.send(b"+")
            decoded = decode_console(payload)
            if decoded:
                console.extend(decoded)
                if len(console) > MAX_CONSOLE_BYTES:
                    raise GateFailure(f"{self.case}: console response exceeds bound")
                continue
            return payload, bytes(console)

    def request(
        self, payload: bytes, *, acknowledge_response: bool = True
    ) -> tuple[bytes, bytes]:
        try:
            self.send(frame(payload))
            if self.ack_mode:
                self.expect_ack()
            return self.read_response(acknowledge=acknowledge_response)
        except BaseException as exc:
            capture_failure(self, exc)
            raise

    def negotiate(self, *, no_ack: bool = False) -> int:
        response, _ = self.request(
            b"qSupported:multiprocess+;swbreak+;vContSupported+"
        )
        packet = next(
            (item for item in response.split(b";") if item.startswith(b"PacketSize=")),
            None,
        )
        if packet is None:
            raise GateFailure(f"{self.case}: qSupported omitted PacketSize")
        packet_value = packet.split(b"=", 1)[1]
        if (
            not packet_value
            or len(packet_value) > 8
            or any(value not in HEX_BYTES for value in packet_value)
        ):
            raise GateFailure(f"{self.case}: invalid PacketSize")
        try:
            packet_size = int(packet_value, 16)
        except ValueError as exc:
            raise GateFailure(f"{self.case}: invalid PacketSize") from exc
        if not 0 < packet_size <= MAX_RSP_PAYLOAD:
            raise GateFailure(
                f"{self.case}: PacketSize is outside the supported range")
        if no_ack:
            result, _ = self.request(b"QStartNoAckMode")
            if result != b"OK":
                raise GateFailure(f"{self.case}: no-ack negotiation returned {result!r}")
            self.ack_mode = False
        return packet_size

    def detach(self) -> None:
        response, _ = self.request(b"D")
        if response != b"OK":
            raise GateFailure(f"{self.case}: detach returned {response!r}")
        self.close()
        time.sleep(0.2)

    def close(self, *, reset: bool = False) -> None:
        if self.closed:
            return
        self.closed = True
        if reset:
            self.sock.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("hh", 1, 0)
            )
        try:
            self.sock.close()
        finally:
            self.log.event("close", case=self.case, reset=reset)


def parse_offsets(payload: bytes) -> dict[str, int]:
    try:
        items = [
            item.split("=", 1)
            for item in payload.decode("ascii").split(";")
        ]
        if (
            any(len(item) != 2 for item in items)
            or len({item[0] for item in items}) != len(items)
        ):
            raise ValueError("duplicate or malformed qOffsets field")
        fields = dict(items)
        if set(fields) != {"TextSeg", "DataSeg"}:
            raise ValueError("unexpected qOffsets fields")
        if any(
            not value
            or len(value) > 8
            or any(byte not in HEX_BYTES for byte in value.encode("ascii"))
            for value in fields.values()
        ):
            raise ValueError("qOffsets values are not ARM32 hexadecimal")
        return {
            "text_segment": int(fields["TextSeg"], 16),
            "data_segment": int(fields["DataSeg"], 16),
        }
    except (UnicodeDecodeError, ValueError, KeyError) as exc:
        raise GateFailure(f"invalid qOffsets response: {payload!r}") from exc


def symbol_values(nm: Path, elf: Path, timeout: float) -> dict[str, int]:
    try:
        result = subprocess.run(
            [str(nm), "-n", str(elf)],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise GateFailure("ELF symbol resolution exceeded its deadline") from exc
    wanted = {"trigger_fault"}
    values: dict[str, int] = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] in wanted:
            values[parts[-1]] = int(parts[0], 16)
    if values.keys() != wanted:
        raise GateFailure(f"ELF symbols missing: {sorted(wanted - values.keys())}")
    return values


def writable_fixture_address(
    repo: Path,
    elf: Path,
    nm: Path,
    offsets: dict[str, int],
    timeout: float,
) -> tuple[int, dict[str, Any]]:
    import sys

    sys.path.insert(0, str(repo / "tools"))
    import gdb_symbols

    image = gdb_symbols.read_elf(elf)
    live_segments = gdb_symbols.expected_main_segments(
        image,
        gdb_symbols.OffsetReply(
            "segments",
            offsets["text_segment"],
            offsets["data_segment"],
        ),
    )
    linked = symbol_values(nm, elf, timeout)["trigger_fault"]
    owners = [
        (index, segment)
        for index, segment in enumerate(image.segments)
        if segment.vaddr <= linked and linked + 4 <= segment.vaddr + segment.memsz
    ]
    if len(owners) != 1:
        raise GateFailure(
            "trigger_fault is not contained by exactly one PT_LOAD"
        )
    index, segment = owners[0]
    if not (segment.flags & 2):
        raise GateFailure("trigger_fault PT_LOAD is not writable")
    address = live_segments[index] + linked - segment.vaddr
    if not 0 < address <= 0xFFFFFFFF - 3:
        raise GateFailure("relocated writable fixture is outside ARM32")
    return address, {
        "symbol": "trigger_fault",
        "linked_address": f"0x{linked:08x}",
        "segment_index": index,
        "linked_segment": f"0x{segment.vaddr:08x}",
        "live_segment": f"0x{live_segments[index]:08x}",
        "runtime_address": f"0x{address:08x}",
        "size": 4,
        "writable": True,
    }


def readable_text_range(
    repo: Path,
    elf: Path,
    offsets: dict[str, int],
) -> tuple[int, int, dict[str, Any]]:
    import sys

    sys.path.insert(0, str(repo / "tools"))
    import gdb_symbols

    image = gdb_symbols.read_elf(elf)
    live_segments = gdb_symbols.expected_main_segments(
        image,
        gdb_symbols.OffsetReply(
            "segments",
            offsets["text_segment"],
            offsets["data_segment"],
        ),
    )
    candidates = [
        (index, segment)
        for index, segment in enumerate(image.segments)
        if live_segments[index] == offsets["text_segment"]
        and segment.flags & 4
        and segment.filesz > 0
    ]
    if len(candidates) != 1:
        raise GateFailure("main text mapping is not one readable PT_LOAD")
    index, segment = candidates[0]
    size = min(0x1F000, segment.filesz)
    if size < 0x10000:
        raise GateFailure("main text mapping is too small for send backpressure")
    return live_segments[index], size, {
        "segment_index": index,
        "runtime_address": f"0x{live_segments[index]:08x}",
        "linked_address": f"0x{segment.vaddr:08x}",
        "readable_file_bytes": segment.filesz,
        "requested_bytes": size,
    }


def require_hex_bytes(value: bytes, size: int, label: str) -> None:
    if (
        len(value) != size * 2
        or any(byte not in HEX_BYTES for byte in value)
    ):
        raise GateFailure(f"{label}: expected {size * 2} hex digits, got {value!r}")


def require_hex_payload(
    value: bytes, label: str, *, minimum_bytes: int = 1
) -> None:
    if (
        len(value) < minimum_bytes * 2
        or len(value) % 2
        or any(byte not in HEX_BYTES for byte in value)
    ):
        raise GateFailure(f"{label}: response is not a bounded hex payload")


def require_legacy_register_bank_payload(value: bytes, label: str) -> None:
    expected_size = LEGACY_G_REPLY_BYTES * 2
    if len(value) != expected_size:
        raise GateFailure(
            f"{label}: expected {expected_size} register characters, "
            f"got {len(value)}"
        )

    unavailable_start = CORE_REGISTER_BYTES * 2
    unavailable_end = unavailable_start + LEGACY_FPA_BYTES * 2
    for offset in range(0, expected_size, 2):
        pair = value[offset:offset + 2]
        if pair[0] in HEX_BYTES and pair[1] in HEX_BYTES:
            continue
        if unavailable_start <= offset < unavailable_end and pair == b"xx":
            continue
        raise GateFailure(
            f"{label}: invalid register byte at character {offset}"
        )


def expect_quiet(client: Rsp, seconds: float) -> None:
    client.set_timeout(seconds)
    try:
        data = client.sock.recv(1)
    except socket.timeout:
        return
    client.log.wire("recv", client.case, data)
    raise GateFailure(f"{client.case}: expected bounded silence, received {data!r}")


def bounded_pause(log: Transcript, case: str, phase: str, seconds: float) -> None:
    log.event("bounded_pause_start", case=case, phase=phase, seconds=seconds)
    time.sleep(seconds)
    log.event("bounded_pause_end", case=case, phase=phase, seconds=seconds)


def pending_socket_bytes(connection: socket.socket) -> int:
    pending = ctypes.c_ulong()
    result = ctypes.windll.ws2_32.ioctlsocket(
        connection.fileno(), WINDOWS_FIONREAD, ctypes.byref(pending))
    if result != 0:
        error = ctypes.windll.ws2_32.WSAGetLastError()
        raise GateFailure(f"FIONREAD failed with Winsock error {error}")
    return int(pending.value)


def malformed_case(
    host: str,
    port: int,
    timeout: float,
    log: Transcript,
    name: str,
    wire: bytes,
    *,
    expect_nack: bool,
) -> None:
    start = time.monotonic()
    client = Rsp(host, port, timeout, log, name)
    try:
        client.send(wire)
        if expect_nack:
            try:
                value = client.read_byte()
            except EOFError:
                log.event("malformed_rejected", case=name, mode="closed")
            else:
                if value != ord("-"):
                    raise GateFailure(
                        f"{name}: expected fail-closed EOF or NACK, got 0x{value:02x}"
                    )
                log.event("malformed_rejected", case=name, mode="nack")
        else:
            try:
                expect_quiet(client, 0.5)
            except EOFError:
                log.event("malformed_rejected", case=name, mode="closed")
    finally:
        client.close(reset=True)
    log.event("case_pass", case=name, duration=time.monotonic() - start)
    time.sleep(1.25)


def stopped_session(
    host: str, port: int, timeout: float, log: Transcript, name: str
) -> tuple[Rsp, int, dict[str, int]]:
    client = Rsp(host, port, timeout, log, name)
    packet_size = client.negotiate()
    offsets, _ = client.request(b"qOffsets")
    return client, packet_size, parse_offsets(offsets)


def verify_running_reconnect(
    host: str,
    port: int,
    timeout: float,
    log: Transcript,
    name: str,
    command: bytes,
    expected: bytes,
) -> None:
    client, _, _ = stopped_session(host, port, timeout, log, name)
    try:
        actual, _ = client.request(command)
        if actual != expected:
            raise GateFailure(
                f"{name}: invariant mismatch, expected {expected[:80]!r}, "
                f"got {actual[:80]!r}"
            )
        client.detach()
    except BaseException:
        client.close(reset=True)
        raise


def disconnect_command(
    host: str,
    port: int,
    timeout: float,
    log: Transcript,
    name: str,
    command: bytes,
    verify_command: bytes,
    response_expected: bytes,
    verify_expected: bytes,
) -> None:
    start = time.monotonic()
    client, _, _ = stopped_session(host, port, timeout, log, name)
    try:
        response, _ = client.request(command, acknowledge_response=False)
        if response != response_expected:
            raise GateFailure(
                f"{name}: command response mismatch {response[:80]!r} != "
                f"{response_expected[:80]!r}"
            )
        client.close(reset=True)
    except BaseException:
        try:
            client.close(reset=True)
        except OSError:
            pass
        raise
    time.sleep(0.25)
    verify_running_reconnect(
        host, port, timeout, log, name + "-verify", verify_command, verify_expected
    )
    log.event("case_pass", case=name, duration=time.monotonic() - start)


def run_matrix(args: argparse.Namespace, log: Transcript) -> dict[str, Any]:
    import sys

    sys.path.insert(0, str(args.repo / "deploy"))
    from host.vitadevdeploy.companion import VitaCompanionClient

    summary: dict[str, Any] = {"cases": [], "console_bytes": 0}
    companion = VitaCompanionClient(
        args.host, args.command_port, args.timeout)
    resume_from_g = args.phase == "matrix-from-g"
    memory_command: bytes | None = None
    if resume_from_g:
        summary["accepted_prior_cases"] = ["disconnect-m", "disconnect-M"]
    else:
        seed, _, offsets = stopped_session(
            args.host, args.port, args.timeout, log, "invariant-baseline"
        )
        memory_address, fixture = writable_fixture_address(
            args.repo, args.elf, args.nm, offsets, args.timeout
        )
        memory_command = f"m{memory_address:x},4".encode("ascii")
        memory_value, _ = seed.request(memory_command)
        if memory_value != b"00000000":
            seed.close(reset=True)
            raise GateFailure("stable trigger_fault fixture is not zero")
        seed.detach()
        summary["memory_address"] = f"0x{memory_address:08x}"
        summary["text_segment"] = f"0x{offsets['text_segment']:08x}"
        summary["data_segment"] = f"0x{offsets['data_segment']:08x}"
        summary["fixture"] = fixture

    def reset_after_response(
        case: str, command: bytes, expected: bytes | None
    ) -> bytes:
        client, _, _ = stopped_session(
            args.host, args.port, args.timeout, log, case)
        try:
            response, _ = client.request(
                command, acknowledge_response=False)
            if expected is not None and response != expected:
                raise GateFailure(
                    f"{case}: response {response[:80]!r} != {expected!r}")
            client.close(reset=True)
            return response
        except BaseException:
            client.close(reset=True)
            raise

    def verify_memory_zero(case: str) -> None:
        if memory_command is None:
            raise GateFailure(f"{case}: memory prerequisite was not initialized")
        client, _, _ = stopped_session(
            args.host, args.port, args.timeout, log, case)
        try:
            value, _ = client.request(memory_command)
            if value != b"00000000":
                raise GateFailure(
                    f"{case}: trigger_fault changed to {value!r}")
            client.detach()
        except BaseException:
            client.close(reset=True)
            raise

    def verify_register_read(case: str, command: bytes) -> bytes:
        client, _, _ = stopped_session(
            args.host, args.port, args.timeout, log, case)
        try:
            value, _ = client.request(command)
            if command == b"g":
                require_legacy_register_bank_payload(value, case)
            else:
                require_hex_payload(value, case, minimum_bytes=4)
            client.detach()
            return value
        except BaseException:
            client.close(reset=True)
            raise

    if not resume_from_g:
        assert memory_command is not None
        memory_read = reset_after_response(
            "disconnect-m", memory_command, b"00000000")
        require_hex_bytes(memory_read, 4, "disconnect-m")
        verify_memory_zero("disconnect-m-verify")
        log.event("case_pass", case="disconnect-m")

        write_command = (
            f"M{memory_address:x},4:00000000".encode("ascii"))
        reset_after_response("disconnect-M", write_command, b"OK")
        verify_memory_zero("disconnect-M-verify")
        log.event("case_pass", case="disconnect-M")

    registers = reset_after_response("disconnect-g", b"g", None)
    require_legacy_register_bank_payload(registers, "disconnect-g")
    verify_register_read("disconnect-g-verify", b"g")
    log.event("case_pass", case="disconnect-g")

    r0 = reset_after_response("disconnect-p", b"p0", None)
    require_hex_bytes(r0, 4, "disconnect-p")
    verify_register_read("disconnect-p-verify", b"p0")
    log.event("case_pass", case="disconnect-p")

    current, _, _ = stopped_session(
        args.host, args.port, args.timeout, log, "disconnect-G")
    try:
        current_registers, _ = current.request(b"g")
        require_legacy_register_bank_payload(
            current_registers, "disconnect-G current")
        response, _ = current.request(
            b"G" + current_registers, acknowledge_response=False)
        if response != b"OK":
            raise GateFailure(f"disconnect-G returned {response!r}")
        current.close(reset=True)
    except BaseException:
        current.close(reset=True)
        raise
    verify_register_read("disconnect-G-verify", b"g")
    log.event("case_pass", case="disconnect-G")

    current, _, _ = stopped_session(
        args.host, args.port, args.timeout, log, "disconnect-P")
    try:
        current_r0, _ = current.request(b"p0")
        require_hex_bytes(current_r0, 4, "disconnect-P current")
        response, _ = current.request(
            b"P0=" + current_r0, acknowledge_response=False)
        if response != b"OK":
            raise GateFailure(f"disconnect-P returned {response!r}")
        current.close(reset=True)
    except BaseException:
        current.close(reset=True)
        raise
    verify_register_read("disconnect-P-verify", b"p0")
    log.event("case_pass", case="disconnect-P")
    if not resume_from_g:
        summary["cases"].extend(["disconnect-m", "disconnect-M"])
    summary["cases"].extend(
        ["disconnect-g", "disconnect-p", "disconnect-G", "disconnect-P"])

    first, _, _ = stopped_session(
        args.host, args.port, args.timeout, log, "second-owner-primary"
    )
    second: Rsp | None = None
    try:
        second = Rsp(
            args.host, args.port, args.timeout, log,
            "second-owner-contender")
        second.send(frame(b"qSupported"))
        expect_quiet(second, 0.5)
        first.detach()
        second.set_timeout(args.timeout)
        second.expect_ack()
        response, _ = second.read_response()
        if b"PacketSize=" not in response:
            raise GateFailure("second owner did not acquire after primary detach")
        second.detach()
    except BaseException as exc:
        if second is not None:
            capture_failure(second, exc)
        first.close(reset=True)
        if second is not None:
            second.close(reset=True)
        raise
    log.event("case_pass", case="second-owner-exclusion")
    summary["cases"].append("second-owner-exclusion")

    first, first_packet_size, _ = stopped_session(
        args.host, args.port, args.timeout, log, "detach-reconnect-primary")
    first.detach()
    second, second_packet_size, _ = stopped_session(
        args.host, args.port, args.timeout, log, "detach-reconnect-secondary")
    if second_packet_size != first_packet_size:
        second.close(reset=True)
        raise GateFailure("PacketSize changed across explicit detach/reconnect")
    second.detach()
    log.event("case_pass", case="detach-reconnect")
    summary["cases"].append("detach-reconnect")

    console_bytes = 0
    shutdown_cycles = 0
    for cycle in range(args.cycles):
        if cycle and cycle % 5 == 0:
            bad = Rsp(
                args.host,
                args.port,
                args.timeout,
                log,
                f"stress-{cycle:03d}-fault",
            )
            bad.send(b"$qSupported#00")
            try:
                value = bad.read_byte()
            except EOFError:
                value = ord("-")
            if value != ord("-"):
                bad.close(reset=True)
                raise GateFailure(
                    f"stress-{cycle:03d}: malformed candidate was not rejected"
                )
            bad.close(reset=True)
            time.sleep(0.05)
        client = Rsp(
            args.host, args.port, args.timeout, log, f"stress-{cycle:03d}"
        )
        try:
            client.negotiate(no_ack=True)
            client.send(frame(b"c"))
            time.sleep(args.run_seconds)
            client.send(b"\x03")
            deadline = time.monotonic() + args.timeout
            stop = None
            while time.monotonic() < deadline:
                payload = client.read_packet()
                decoded = decode_console(payload)
                if decoded:
                    console_bytes += len(decoded)
                    continue
                if payload.startswith((b"T", b"S")):
                    stop = payload
                    break
                raise GateFailure(
                    f"stress-{cycle:03d}: unexpected running response {payload[:80]!r}"
                )
            if stop is None:
                raise GateFailure(f"stress-{cycle:03d}: no bounded Ctrl-C stop")
            client.detach()
        except BaseException:
            client.close(reset=True)
            raise
        log.event("stress_cycle_pass", case=client.case, cycle=cycle)
        if (cycle + 1) % args.shutdown_interval == 0:
            shutdown_start = time.monotonic()
            destroy = companion.destroy()
            bounded_pause(
                log, client.case, "shutdown-livearea", args.shutdown_pause)
            launch = companion.launch(args.title_id)
            bounded_pause(
                log, client.case, "shutdown-relaunch", args.launch_delay)
            shutdown_cycles += 1
            log.event(
                "stress_shutdown_pass",
                case=client.case,
                cycle=cycle,
                destroy=destroy,
                launch=launch,
                duration_seconds=time.monotonic() - shutdown_start,
            )
    if console_bytes == 0:
        raise GateFailure("stress observed no redirected console output")
    if shutdown_cycles == 0:
        raise GateFailure("stress observed no title shutdown/relaunch cycle")
    summary["console_bytes"] = console_bytes
    summary["stress_cycles"] = args.cycles
    summary["shutdown_cycles"] = shutdown_cycles
    summary["cases"].append("bounded-reconnect-fault-console-soak")
    return summary


def run_second_admission(
    args: argparse.Namespace, log: Transcript
) -> dict[str, Any]:
    first, first_packet_size, first_offsets = stopped_session(
        args.host, args.port, args.timeout, log,
        "second-admission-primary",
    )
    try:
        first.detach()
    except BaseException:
        first.close(reset=True)
        raise

    second, second_packet_size, second_offsets = stopped_session(
        args.host, args.port, args.timeout, log,
        "second-admission-secondary",
    )
    try:
        if second_packet_size != first_packet_size:
            raise GateFailure(
                "PacketSize changed across immediate detach/reconnect")
        if second_offsets != first_offsets:
            raise GateFailure(
                "qOffsets changed across immediate detach/reconnect")
        second.detach()
    except BaseException:
        second.close(reset=True)
        raise

    snapshot = query_snapshot(
        args.host, args.diagnostic_port, min(args.timeout, 3.0), log)
    observed_epoch = snapshot["current"]["connection_epoch"]
    if args.expected_epoch is not None and observed_epoch != args.expected_epoch:
        raise GateFailure(
            f"second admission epoch {observed_epoch} != "
            f"{args.expected_epoch}")
    if snapshot["current"]["dropped_event_writes"]:
        raise GateFailure("second admission telemetry dropped an event write")
    log.event(
        "case_pass",
        case="second-admission",
        connection_epoch=observed_epoch,
        packet_size=second_packet_size,
        offsets=second_offsets,
    )
    return {
        "cases": ["second-admission"],
        "packet_size": second_packet_size,
        "offsets": second_offsets,
        "final_snapshot": snapshot,
    }


def run_rst_sentinel(args: argparse.Namespace, log: Transcript) -> dict[str, Any]:
    result: dict[str, Any] = {"cases": []}
    for no_ack in (False, True):
        mode = "no-ack" if no_ack else "ack"
        case = f"stopped-rst-{mode}"
        start = time.monotonic()
        client = Rsp(args.host, args.port, args.timeout, log, case)
        try:
            packet_size = client.negotiate(no_ack=no_ack)
            bounded_pause(
                log, case, "after-negotiation-before-qOffsets",
                args.idle_gap)
            offsets_payload, _ = client.request(b"qOffsets")
            offsets = parse_offsets(offsets_payload)
            address, fixture = writable_fixture_address(
                args.repo, args.elf, args.nm, offsets, args.timeout
            )
            read = f"m{address:x},4".encode("ascii")
            before, _ = client.request(read)
            require_hex_bytes(before, 4, case + " preflight read")
            if before != b"00000000":
                raise GateFailure(
                    f"{case}: trigger_fault fixture is not zero")
            write = f"M{address:x},4:{before.decode('ascii')}".encode("ascii")
            write_reply, _ = client.request(write)
            if write_reply != b"OK":
                raise GateFailure(f"{case}: idempotent M returned {write_reply!r}")
            after, _ = client.request(read)
            if after != before:
                raise GateFailure(
                    f"{case}: fixture changed across idempotent m/M/m preflight"
                )
            registers, _ = client.request(b"g")
            register_zero, _ = client.request(b"p0")
            if not registers:
                raise GateFailure(f"{case}: g returned an empty register packet")
            require_hex_bytes(register_zero, 4, case + " p0")
            bounded_pause(
                log, case, "stopped-wait-before-rst", args.reset_gap)
            client.close(reset=True)
        except BaseException:
            try:
                client.close(reset=True)
            except OSError:
                pass
            raise

        reconnect_start = time.monotonic()
        reconnect, reopened_packet_size, _ = stopped_session(
            args.host,
            args.port,
            min(args.timeout, 15.0),
            log,
            case + "-reopen",
        )
        if reopened_packet_size != packet_size:
            reconnect.close(reset=True)
            raise GateFailure(f"{case}: PacketSize changed after RST")
        reconnect.detach()
        reopen_seconds = time.monotonic() - reconnect_start
        if reopen_seconds > 15.0:
            raise GateFailure(
                f"{case}: listener reopen exceeded 15 seconds ({reopen_seconds:.3f})"
            )
        observation = {
            "case": case,
            "mode": mode,
            "status": "PASS",
            "duration_seconds": time.monotonic() - start,
            "reopen_seconds": reopen_seconds,
            "packet_size": packet_size,
            "fixture": fixture,
            "fixture_bytes": before.decode("ascii"),
            "g_payload_bytes": len(registers),
            "p0": register_zero.decode("ascii"),
        }
        log.event("case_pass", **observation)
        result["cases"].append(observation)
    return result


def run_shutdown_case(args: argparse.Namespace, log: Transcript) -> dict[str, Any]:
    sys_path = str(args.repo / "deploy")
    import sys

    sys.path.insert(0, sys_path)
    from host.vitadevdeploy.companion import VitaCompanionClient

    client, _, _ = stopped_session(
        args.host, args.port, args.timeout, log, "connected-receive-shutdown"
    )
    client.set_timeout(args.timeout)
    companion = VitaCompanionClient(args.host, args.command_port, args.timeout)
    start = time.monotonic()
    reply = companion.destroy()
    log.event("companion", command="destroy", response=reply)
    closed = False
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        try:
            data = client.sock.recv(65536)
            log.wire("recv", client.case, data)
            if not data:
                closed = True
                break
        except (ConnectionResetError, ConnectionAbortedError):
            closed = True
            break
        except socket.timeout:
            break
    client.close()
    if not closed:
        raise GateFailure("connected blocked receive was not cancelled by title shutdown")
    duration = time.monotonic() - start
    bounded_pause(
        log, client.case, "post-destroy-livearea", args.shutdown_pause)
    log.event(
        "case_pass",
        case="connected-receive-cancellation-and-hup-shutdown",
        duration=duration,
    )
    return {
        "cases": ["connected-receive-cancellation", "connected-HUP-shutdown"],
        "shutdown_seconds": duration,
        "companion_response": reply,
    }


def run_send_shutdown_case(args: argparse.Namespace, log: Transcript) -> dict[str, Any]:
    import sys

    sys.path.insert(0, str(args.repo / "deploy"))
    from host.vitadevdeploy.companion import VitaCompanionClient

    companion = VitaCompanionClient(args.host, args.command_port, args.timeout)
    client: Rsp | None = None
    destroyed = False
    try:
        launch = companion.launch(args.title_id)
        log.event(
            "companion", command=f"launch {args.title_id}", response=launch)
        bounded_pause(
            log, "connected-send-shutdown", "post-launch", args.launch_delay)

        client, packet_size, offsets = stopped_session(
            args.host, args.port, args.timeout, log, "connected-send-shutdown"
        )
        client.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        text_address, available, text_fixture = readable_text_range(
            args.repo, args.elf, offsets)
        read_size = min(available, (packet_size - 4) // 2)
        request = f"m{text_address:x},{read_size:x}".encode("ascii")
        client.send(frame(request))
        client.expect_ack()
        bounded_pause(
            log, client.case, "partial-send-observation", 0.5)
        pending = pending_socket_bytes(client.sock)
        peek = (
            client.sock.recv(min(pending, MAX_RSP_PAYLOAD), socket.MSG_PEEK)
            if pending else b""
        )
        observed = bytes(client.buffer) + peek
        expected_wire = read_size * 2 + 4
        if (
            not 0 < len(observed) < expected_wire
            or b"#" in observed
            or observed.startswith(b"$E")
        ):
            raise GateFailure(
                "could not prove a partial connected send: "
                f"observed={len(observed)}, pending={pending}, "
                f"expected_wire={expected_wire}, prefix={observed[:16]!r}"
            )
        log.event(
            "partial_send_observed",
            case=client.case,
            buffered_bytes=len(client.buffer),
            pending_bytes=pending,
            observed_bytes=len(observed),
            expected_wire=expected_wire,
            text_fixture=text_fixture,
        )
        start = time.monotonic()
        reply = companion.destroy()
        destroyed = True
        log.event("companion", command="destroy", response=reply)
        closed = False
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            try:
                data = client.sock.recv(65536)
                log.wire("recv", client.case, data)
                if not data:
                    closed = True
                    break
            except (ConnectionResetError, ConnectionAbortedError):
                closed = True
                break
            except socket.timeout:
                break
        if not closed:
            raise GateFailure(
                "connected partial send was not cancelled by title shutdown")
        duration = time.monotonic() - start
    finally:
        if client is not None:
            client.close()
        if not destroyed:
            reply = companion.destroy()
            destroyed = True
            log.event(
                "companion", command="destroy-after-failure", response=reply)
        bounded_pause(
            log, "connected-send-shutdown",
            "post-destroy-livearea", args.shutdown_pause)
    log.event(
        "case_pass",
        case="connected-partial-send-cancellation",
        duration=duration,
        pending_before_shutdown=pending,
        expected_wire=expected_wire,
    )
    return {
        "cases": ["connected-partial-send-cancellation"],
        "pending_before_shutdown": pending,
        "buffered_before_shutdown": len(observed) - pending,
        "expected_wire": expected_wire,
        "text_fixture": text_fixture,
        "shutdown_seconds": duration,
        "companion_response": reply,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "phase",
        choices=(
            "second-admission",
            "rst-sentinel",
            "matrix",
            "matrix-from-g",
            "receive-shutdown",
            "send-shutdown",
        ),
    )
    parser.add_argument("--host", required=True, type=numeric_ipv4)
    parser.add_argument("--port", type=int, default=1234)
    parser.add_argument("--diagnostic-port", type=int, default=1235)
    parser.add_argument("--expected-epoch", type=int)
    parser.add_argument("--command-port", type=int, default=1338)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--cycles", type=int, default=50)
    parser.add_argument("--run-seconds", type=float, default=0.15)
    parser.add_argument("--launch-delay", type=float, default=3.0)
    parser.add_argument("--idle-gap", type=float, default=0.5)
    parser.add_argument("--reset-gap", type=float, default=0.25)
    parser.add_argument("--shutdown-interval", type=int, default=10)
    parser.add_argument("--shutdown-pause", type=float, default=2.0)
    parser.add_argument("--title-id", default="SLRS00001")
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--nm", required=True, type=Path)
    parser.add_argument("--transcript", required=True, type=Path)
    parser.add_argument("--summary", required=True, type=Path)
    args = parser.parse_args()
    if (
        not 1 <= args.port <= 65535
        or not 1 <= args.command_port <= 65535
        or not 1 <= args.diagnostic_port <= 65535
    ):
        parser.error("ports must be between 1 and 65535")
    if args.expected_epoch is not None and args.expected_epoch < 1:
        parser.error("--expected-epoch must be positive")
    for name in (
        "timeout",
        "run_seconds",
        "launch_delay",
        "idle_gap",
        "reset_gap",
        "shutdown_pause",
    ):
        value = getattr(args, name)
        if not math.isfinite(value) or value <= 0 or value > 30.0:
            parser.error(f"--{name.replace('_', '-')} must be within (0, 30]")
    if args.phase in ("matrix", "matrix-from-g") and args.cycles != 50:
        parser.error("the authorized matrix requires exactly --cycles 50")
    if args.shutdown_pause < 2.0:
        parser.error("--shutdown-pause must be at least 2 seconds")
    if not 1 <= args.shutdown_interval <= args.cycles:
        parser.error("--shutdown-interval must be between 1 and --cycles")
    if not args.repo.is_dir() or not args.elf.is_file() or not args.nm.is_file():
        parser.error("--repo, --elf, and --nm must exist")

    log = Transcript(args.transcript)
    started = time.monotonic()
    started_at = utc_now()
    global failure_telemetry
    failure_telemetry = FailureTelemetryCapture(
        args.host,
        args.diagnostic_port,
        min(args.timeout, 3.0),
        args.expected_epoch,
        log,
    )
    try:
        if args.phase == "second-admission":
            result = run_second_admission(args, log)
        elif args.phase == "rst-sentinel":
            result = run_rst_sentinel(args, log)
        elif args.phase in ("matrix", "matrix-from-g"):
            result = run_matrix(args, log)
        elif args.phase == "receive-shutdown":
            result = run_shutdown_case(args, log)
        else:
            result = run_send_shutdown_case(args, log)
        result.update(
            {
                "phase": args.phase,
                "status": "PASS",
                "started_at": started_at,
                "duration_seconds": time.monotonic() - started,
            }
        )
        args.summary.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except BaseException as exc:
        log.event("gate_failure", error=type(exc).__name__, detail=str(exc))
        result = {
            "phase": args.phase,
            "status": "FAIL",
            "error": type(exc).__name__,
            "detail": str(exc),
            "duration_seconds": time.monotonic() - started,
            "failure_telemetry": failure_telemetry.result,
        }
        args.summary.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(json.dumps(result, indent=2, sort_keys=True))
        return 1
    finally:
        failure_telemetry = None
        log.close()


if __name__ == "__main__":
    raise SystemExit(main())

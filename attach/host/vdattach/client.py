"""Bounded host client for protocol-v1 read-only attach discovery."""

from __future__ import annotations

import ipaddress
import socket
import time
from dataclasses import dataclass

from .errors import CapabilityError, ProtocolError, RemoteError, TransportError
from .protocol import (
    CURRENT_KERNEL_ABI,
    MAX_FRAME_SIZE,
    READ_ONLY_CAPABILITIES,
    DiscoverResult,
    HelloResult,
    ReleaseResult,
    frame_record,
    make_discover_request,
    make_hello_request,
    make_release_request,
    parse_discover_result,
    parse_frame_prefix,
    parse_hello_result,
    parse_release_result,
    validate_title_id,
)


def canonical_ipv4(value: str) -> str:
    if not isinstance(value, str) or value != value.strip() or not value:
        raise ProtocolError("host must be a canonical dotted-decimal IPv4 address")
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as exc:
        raise ProtocolError("host must be a canonical dotted-decimal IPv4 address") from exc
    if str(address) != value:
        raise ProtocolError("host must use canonical dotted-decimal notation")
    return value


def checked_port(value: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 65535:
        raise ProtocolError("port must be an integer from 1 through 65535")
    return value


def checked_timeout(value: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ProtocolError("timeout must be a number")
    result = float(value)
    if not 0.1 <= result <= 30.0:
        raise ProtocolError("timeout must be from 0.1 through 30 seconds")
    return result


def _set_deadline_timeout(stream: socket.socket,
                          deadline: float | None) -> None:
    if deadline is None:
        return
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TransportError("attach broker exchange exceeded its time limit")
    stream.settimeout(remaining)


def _recv_exact(stream: socket.socket, size: int,
                deadline: float | None = None) -> bytes:
    output = bytearray()
    while len(output) < size:
        _set_deadline_timeout(stream, deadline)
        try:
            chunk = stream.recv(size - len(output))
        except socket.timeout as exc:
            raise TransportError(
                "attach broker exchange exceeded its time limit") from exc
        except OSError as exc:
            raise TransportError(f"attach broker receive failed: {exc}") from exc
        if not chunk:
            raise TransportError("attach broker closed the connection mid-frame")
        output.extend(chunk)
    return bytes(output)


def receive_record(stream: socket.socket,
                   deadline: float | None = None) -> bytes:
    prefix = _recv_exact(stream, 4, deadline)
    try:
        size = parse_frame_prefix(prefix)
    except ProtocolError:
        # Never allocate or drain a peer-selected oversized frame.
        raise
    return _recv_exact(stream, size, deadline)


def send_record(stream: socket.socket, data: bytes,
                deadline: float | None = None) -> None:
    framed = frame_record(data)
    _set_deadline_timeout(stream, deadline)
    try:
        stream.sendall(framed)
    except socket.timeout as exc:
        raise TransportError(
            "attach broker exchange exceeded its time limit") from exc
    except OSError as exc:
        raise TransportError(f"attach broker send failed: {exc}") from exc


@dataclass(frozen=True)
class DiscoverySnapshot:
    title_id: str
    pid: int
    main_modid: int
    main_fingerprint: int
    target_generation: int
    ticket_lease_ms: int


class AttachDiscoveryClient:
    """One-session state machine with no target mutation operation."""

    def __init__(
        self,
        stream: socket.socket,
        *,
        expected_kernel_abi: int = CURRENT_KERNEL_ABI,
        required_caps: int = READ_ONLY_CAPABILITIES,
        operation_timeout: float = 3.0,
    ) -> None:
        self._stream = stream
        self._expected_kernel_abi = expected_kernel_abi
        self._required_caps = required_caps
        self._operation_timeout = checked_timeout(operation_timeout)
        self._hello: HelloResult | None = None
        self._target: DiscoverResult | None = None

    @classmethod
    def connect(
        cls,
        host: str,
        port: int,
        *,
        timeout: float = 3.0,
        expected_kernel_abi: int = CURRENT_KERNEL_ABI,
        required_caps: int = READ_ONLY_CAPABILITIES,
    ) -> "AttachDiscoveryClient":
        address = canonical_ipv4(host)
        checked_port(port)
        bounded_timeout = checked_timeout(timeout)
        stream: socket.socket | None = None
        try:
            stream = socket.create_connection((address, port), timeout=bounded_timeout)
            stream.settimeout(bounded_timeout)
        except OSError as exc:
            if stream is not None:
                stream.close()
            raise TransportError(
                f"could not connect to attach discovery broker at {address}:{port}: {exc}"
            ) from exc
        return cls(
            stream,
            expected_kernel_abi=expected_kernel_abi,
            required_caps=required_caps,
            operation_timeout=bounded_timeout,
        )

    def close(self) -> None:
        try:
            self._stream.close()
        except OSError:
            pass

    def __enter__(self) -> "AttachDiscoveryClient":
        return self

    def __exit__(self, _exc_type, _exc, _traceback) -> None:
        self.close()

    def hello(self) -> HelloResult:
        if self._hello is not None:
            raise ProtocolError("hello may be sent only once per connection")
        request = make_hello_request(
            expected_kernel_abi=self._expected_kernel_abi,
            required_caps=self._required_caps,
        )
        deadline = time.monotonic() + self._operation_timeout
        send_record(self._stream, request.data, deadline)
        result = parse_hello_result(receive_record(self._stream, deadline))
        if result.request_id != request.request_id or result.client_nonce != request.client_nonce:
            raise ProtocolError("hello result is not bound to this request")
        if result.state != "ready":
            raise RemoteError(f"attach discovery broker is unavailable: {result.message}")
        if result.kernel_abi != self._expected_kernel_abi:
            raise CapabilityError(
                "kernel companion ABI mismatch: "
                f"expected 0x{self._expected_kernel_abi:08x}, got 0x{result.kernel_abi:08x}"
            )
        missing = self._required_caps & ~result.attach_caps
        if missing:
            raise CapabilityError(
                f"attach discovery broker lacks required capability bits 0x{missing:08x}"
            )
        self._hello = result
        return result

    def discover(self, title_id: str) -> DiscoverySnapshot:
        validate_title_id(title_id)
        if self._hello is None:
            raise ProtocolError("hello must succeed before discovery")
        if self._target is not None:
            raise ProtocolError("release the existing discovery ticket first")
        request = make_discover_request(
            server_nonce=self._hello.server_nonce,
            service_generation=self._hello.service_generation,
            target_title_id=title_id,
        )
        deadline = time.monotonic() + self._operation_timeout
        send_record(self._stream, request.data, deadline)
        result = parse_discover_result(receive_record(self._stream, deadline))
        if (
            result.request_id != request.request_id
            or result.server_nonce != self._hello.server_nonce
            or result.service_generation != self._hello.service_generation
            or result.target_title_id != title_id
        ):
            raise ProtocolError("discover result is not bound to this session and title")
        if result.state != "found":
            raise RemoteError(
                f"title {title_id} was not safely discovered ({result.state}): {result.message}"
            )
        self._target = result
        return DiscoverySnapshot(
            title_id=result.target_title_id,
            pid=result.pid,
            main_modid=result.main_modid,
            main_fingerprint=result.main_fingerprint,
            target_generation=result.target_generation,
            ticket_lease_ms=self._hello.ticket_lease_ms,
        )

    def release(self) -> ReleaseResult:
        if self._hello is None or self._target is None:
            raise ProtocolError("there is no discovery ticket to release")
        target = self._target
        request = make_release_request(
            server_nonce=self._hello.server_nonce,
            service_generation=self._hello.service_generation,
            target_ticket=target.target_ticket,
        )
        deadline = time.monotonic() + self._operation_timeout
        send_record(self._stream, request.data, deadline)
        result = parse_release_result(receive_record(self._stream, deadline))
        if (
            result.request_id != request.request_id
            or result.server_nonce != self._hello.server_nonce
            or result.service_generation != self._hello.service_generation
            or result.target_ticket != target.target_ticket
        ):
            raise ProtocolError("release result is not bound to this session and ticket")
        if result.state not in {"released", "missing"}:
            raise RemoteError(f"discovery ticket release failed ({result.state}): {result.message}")
        self._target = None
        return result

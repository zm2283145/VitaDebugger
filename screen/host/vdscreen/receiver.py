"""Strict, bounded receiver and atomic latest-frame store."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import os
import socket
import time
import zlib
from pathlib import Path
from typing import Callable

from .protocol import (
    AUTH_SIZE,
    AUTH_TOKEN_SIZE,
    FRAME_HEADER_SIZE,
    PIXEL_BGRA8888,
    PIXEL_RGB565_LE,
    PIXEL_RGBA8888,
    FrameHeader,
    ProtocolError,
    StreamIdentity,
    decode_auth,
    decode_header,
)

DEFAULT_LISTENER_PORT = 18197
MIN_LISTENER_PORT = 18000
MAX_LISTENER_PORT = 18999
RESERVED_LISTENER_PORTS = frozenset({
    18194,  # VitaDebugger DebugNet gate
    18195,  # VitaProfiler TCP gate
    18196,  # VitaDevDeploy direct TCP
    18198,  # VitaDebugger companion control
})


@dataclasses.dataclass(frozen=True)
class ReceiverLimits:
    max_width: int = 960
    max_height: int = 544
    max_payload_bytes: int = 4 * 1024 * 1024
    max_frames_per_second: float = 10.0
    rate_burst_frames: int = 2
    idle_timeout_seconds: float = 3.0
    max_session_seconds: float = 3600.0

    def validate(self) -> None:
        if not 1 <= self.max_width <= 4096:
            raise ValueError("max_width must be between 1 and 4096")
        if not 1 <= self.max_height <= 4096:
            raise ValueError("max_height must be between 1 and 4096")
        if not 1 <= self.max_payload_bytes <= 64 * 1024 * 1024:
            raise ValueError("max_payload_bytes must be between 1 and 64 MiB")
        if not 0.1 <= self.max_frames_per_second <= 60.0:
            raise ValueError("max_frames_per_second must be between 0.1 and 60")
        if not 1 <= self.rate_burst_frames <= 120:
            raise ValueError("rate_burst_frames must be between 1 and 120")
        if not 0.1 <= self.idle_timeout_seconds <= 60.0:
            raise ValueError("idle_timeout_seconds must be between 0.1 and 60")
        if not 1.0 <= self.max_session_seconds <= 86400.0:
            raise ValueError("max_session_seconds must be between 1 and 86400")


@dataclasses.dataclass
class ReceiveStats:
    frames_received: int = 0
    frames_published: int = 0
    duplicates_dropped: int = 0
    sequence_gaps: int = 0
    bytes_received: int = 0


def validate_listener_port(port: int) -> None:
    if not MIN_LISTENER_PORT <= port <= MAX_LISTENER_PORT:
        raise ValueError(
            f"listener port must be between {MIN_LISTENER_PORT} and "
            f"{MAX_LISTENER_PORT}")
    if port in RESERVED_LISTENER_PORTS:
        raise ValueError("listener port is reserved by another VitaDebugger service")


def _validate_auth_token(token: bytes) -> None:
    if len(token) != AUTH_TOKEN_SIZE or not any(token):
        raise ValueError("receiver token must be exactly 32 nonzero bytes")


def _is_loopback(bind: str) -> bool:
    import ipaddress

    try:
        return ipaddress.ip_address(bind).is_loopback
    except ValueError:
        return bind.lower() == "localhost"


class _RateGate:
    def __init__(self, rate: float, burst: int, now: float) -> None:
        self._rate = rate
        self._burst = float(burst)
        self._tokens = float(burst)
        self._updated = now

    def accept(self, now: float) -> bool:
        elapsed = now - self._updated
        if elapsed < 0:
            return False
        self._updated = now
        self._tokens = min(self._burst, self._tokens + elapsed * self._rate)
        if self._tokens < 1.0:
            return False
        self._tokens -= 1.0
        return True


class LatestFrameStore:
    """Two-slot store with an atomic manifest naming the complete slot."""

    def __init__(self, output: Path) -> None:
        self.output = output
        self.output.mkdir(parents=True, exist_ok=True)

    def cleanup_temps(self) -> None:
        for name in ("latest.json.tmp", "frame-0.ppm.tmp", "frame-1.ppm.tmp"):
            try:
                (self.output / name).unlink()
            except FileNotFoundError:
                pass

    def publish(self, header: FrameHeader, payload: bytes,
                peer: tuple[str, int] | None,
                identity: StreamIdentity) -> None:
        ppm = _to_ppm(header, payload)
        slot = header.sequence % 2
        image_name = f"frame-{slot}.ppm"
        image_path = self.output / image_name
        image_temp = self.output / f"{image_name}.tmp"
        manifest_path = self.output / "latest.json"
        manifest_temp = self.output / "latest.json.tmp"
        image_digest = hashlib.sha256(ppm).hexdigest()
        metadata = {
            "complete": True,
            "protocol_version": 1,
            "sequence": header.sequence,
            "timestamp_us": header.timestamp_us,
            "width": header.width,
            "height": header.height,
            "stride_bytes": header.stride_bytes,
            "pixel_format": header.pixel_format,
            "payload_length": header.payload_length,
            "payload_crc32": f"{header.payload_crc32:08x}",
            "title_id": identity.title_id,
            "process_id": identity.process_id,
            "process_generation": identity.process_generation,
            "session_id": identity.session_id,
            "image": image_name,
            "image_format": "ppm-p6",
            "image_sha256": image_digest,
            "peer": f"{peer[0]}:{peer[1]}" if peer else None,
        }
        try:
            _write_and_replace(image_temp, image_path, ppm)
            encoded = (json.dumps(metadata, sort_keys=True, indent=2) +
                       "\n").encode("utf-8")
            _write_and_replace(manifest_temp, manifest_path, encoded)
        finally:
            self.cleanup_temps()


def _write_and_replace(temp: Path, destination: Path, data: bytes) -> None:
    with temp.open("wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temp, destination)


def _to_ppm(header: FrameHeader, payload: bytes) -> bytes:
    output = bytearray(f"P6\n{header.width} {header.height}\n255\n".encode("ascii"))
    for row_index in range(header.height):
        row = payload[
            row_index * header.stride_bytes:
            (row_index + 1) * header.stride_bytes
        ]
        if header.pixel_format == PIXEL_RGBA8888:
            for offset in range(0, header.width * 4, 4):
                output.extend(row[offset:offset + 3])
        elif header.pixel_format == PIXEL_BGRA8888:
            for offset in range(0, header.width * 4, 4):
                output.extend((row[offset + 2], row[offset + 1], row[offset]))
        elif header.pixel_format == PIXEL_RGB565_LE:
            for offset in range(0, header.width * 2, 2):
                value = row[offset] | (row[offset + 1] << 8)
                red = ((value >> 11) & 0x1F) * 255 // 31
                green = ((value >> 5) & 0x3F) * 255 // 63
                blue = (value & 0x1F) * 255 // 31
                output.extend((red, green, blue))
        else:
            raise ProtocolError("unsupported pixel format")
    return bytes(output)


def _read_exact(connection: socket.socket, size: int,
                deadline: float, now_fn: Callable[[], float]) -> bytes | None:
    data = bytearray(size)
    view = memoryview(data)
    offset = 0
    while offset < size:
        remaining = deadline - now_fn()
        if remaining <= 0:
            raise ProtocolError("session time limit exceeded")
        connection.settimeout(remaining)
        try:
            received = connection.recv_into(view[offset:])
        except TimeoutError as error:
            raise ProtocolError("peer idle timeout") from error
        if received == 0:
            if offset == 0:
                return None
            raise ProtocolError("truncated record")
        offset += received
    return bytes(data)


def _finish_connection(connection: socket.socket,
                       store: LatestFrameStore) -> BaseException | None:
    first_error: BaseException | None = None
    operations = (
        store.cleanup_temps,
        lambda: connection.shutdown(socket.SHUT_RDWR),
        connection.close,
    )
    for operation in operations:
        try:
            operation()
        except BaseException as error:
            if first_error is None:
                first_error = error
    return first_error


def receive_connection(
    connection: socket.socket,
    *,
    token: bytes,
    store: LatestFrameStore,
    limits: ReceiverLimits = ReceiverLimits(),
    peer: tuple[str, int] | None = None,
    now_fn: Callable[[], float] = time.monotonic,
) -> ReceiveStats:
    stats = ReceiveStats()
    primary_error: BaseException | None = None
    primary_traceback = None
    try:
        limits.validate()
        _validate_auth_token(token)
        started = now_fn()
        deadline = started + limits.max_session_seconds
        rate = _RateGate(
            limits.max_frames_per_second, limits.rate_burst_frames, started)
        last_sequence: int | None = None
        last_timestamp: int | None = None
        store.cleanup_temps()
        connection.settimeout(limits.idle_timeout_seconds)
        auth = _read_exact(
            connection, AUTH_SIZE,
            min(deadline, now_fn() + limits.idle_timeout_seconds), now_fn)
        if auth is None:
            raise ProtocolError("connection closed before authentication")
        identity = decode_auth(auth, token)
        while True:
            header_data = _read_exact(
                connection, FRAME_HEADER_SIZE,
                min(deadline, now_fn() + limits.idle_timeout_seconds), now_fn)
            if header_data is None:
                break
            header = decode_header(
                header_data,
                max_width=limits.max_width,
                max_height=limits.max_height,
                max_payload=limits.max_payload_bytes,
            )
            payload = _read_exact(
                connection, header.payload_length,
                min(deadline, now_fn() + limits.idle_timeout_seconds), now_fn)
            if payload is None:
                raise ProtocolError("truncated frame payload")
            stats.frames_received += 1
            stats.bytes_received += FRAME_HEADER_SIZE + len(payload)
            if zlib.crc32(payload) != header.payload_crc32:
                raise ProtocolError("frame checksum mismatch")
            if header.session_id != identity.session_id:
                raise ProtocolError("frame session identity mismatch")
            current_time = now_fn()
            if not rate.accept(current_time):
                raise ProtocolError("frame rate limit exceeded")
            if last_sequence is not None:
                if header.sequence < last_sequence:
                    raise ProtocolError("frame sequence regressed")
                if header.sequence == last_sequence:
                    stats.duplicates_dropped += 1
                    continue
                if header.sequence > last_sequence + 1:
                    stats.sequence_gaps += header.sequence - last_sequence - 1
            if last_timestamp is not None and header.timestamp_us < last_timestamp:
                raise ProtocolError("frame timestamp regressed")
            store.publish(header, payload, peer, identity)
            stats.frames_published += 1
            last_sequence = header.sequence
            last_timestamp = header.timestamp_us
    except BaseException as error:
        primary_error = error
        primary_traceback = error.__traceback__

    cleanup_error = _finish_connection(connection, store)
    if primary_error is not None:
        raise primary_error.with_traceback(primary_traceback)
    if cleanup_error is not None:
        raise cleanup_error
    return stats


def listen_once(
    *,
    bind: str,
    port: int,
    allow_lan: bool,
    accept_timeout_seconds: float,
    token: bytes,
    store: LatestFrameStore,
    limits: ReceiverLimits = ReceiverLimits(),
) -> ReceiveStats:
    validate_listener_port(port)
    if not _is_loopback(bind) and not allow_lan:
        raise ValueError("non-loopback bind requires explicit LAN authorization")
    if not 0 < accept_timeout_seconds <= 3600:
        raise ValueError("accept timeout must be between 0 and 3600 seconds")
    limits.validate()
    _validate_auth_token(token)
    connection = None
    ownership_transferred = False
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind((bind, port))
            listener.listen(1)
            listener.settimeout(accept_timeout_seconds)
            connection, peer = listener.accept()
        ownership_transferred = True
        return receive_connection(
            connection, token=token, store=store, limits=limits, peer=peer)
    finally:
        if connection is not None and not ownership_transferred:
            try:
                connection.close()
            except OSError:
                pass


def read_latest(output: Path, attempts: int = 3) -> tuple[dict, bytes]:
    manifest_path = output / "latest.json"
    for _ in range(attempts):
        first = manifest_path.read_bytes()
        metadata = json.loads(first)
        if metadata.get("complete") is not True:
            raise ProtocolError("latest frame is not marked complete")
        image_name = metadata.get("image")
        if image_name not in {"frame-0.ppm", "frame-1.ppm"}:
            raise ProtocolError("latest frame names an invalid image slot")
        image = (output / image_name).read_bytes()
        if hashlib.sha256(image).hexdigest() != metadata["image_sha256"]:
            continue
        if manifest_path.read_bytes() == first:
            return metadata, image
    raise ProtocolError("latest frame changed while being read")

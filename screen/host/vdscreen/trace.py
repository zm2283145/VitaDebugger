"""Bounded parser, storage, and cooperative replay for input trace v1."""

from __future__ import annotations

import dataclasses
import errno
import json
import os
import secrets
import struct
import time
import zlib
from collections.abc import Callable
from pathlib import Path

TRACE_MAGIC = b"VDTR"
TRACE_VERSION = 1
TRACE_HEADER_SIZE = 96
TRACE_EVENT_SIZE = 64
TRACE_MAX_EVENTS = 4096
TRACE_MAX_DURATION_US = 3_600_000_000
TRACE_MAX_FILE_SIZE = TRACE_HEADER_SIZE + TRACE_MAX_EVENTS * TRACE_EVENT_SIZE
TRACE_NO_FRAME = (1 << 64) - 1
TRACE_END_COMPLETE = 1
TRACE_EVENT_INPUT = 1
TRACE_EVENT_CHECKPOINT = 2
TRACE_ALLOWED_BUTTONS = 0x0000F3F9
TRACE_DEFAULT_MAX_SLEEP_SECONDS = 0.05
TRACE_DEFAULT_MAX_DRIFT_SECONDS = 0.1

_HEADER = struct.Struct(">4sHHHHI9s3xIQQQIIQI20x")
_EVENT = struct.Struct(">IHHQQIhhhhBB2xHHHH4xHHHH4x")
_NEUTRAL_VALUES = (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)


class TraceError(ValueError):
    """The trace is malformed, unsafe, stale, or cannot be replayed."""


class TraceReplayCancelled(TraceError):
    """Cooperative replay was explicitly cancelled."""


class TraceCleanupError(TraceError):
    """Replay failed and the required neutral input release also failed."""

    def __init__(
        self,
        primary_error: BaseException,
        cleanup_error: BaseException,
    ) -> None:
        super().__init__("neutral input release failed after replay failure")
        self.primary_error = primary_error
        self.cleanup_error = cleanup_error


@dataclasses.dataclass(frozen=True)
class TraceIdentity:
    title_id: str
    process_id: int
    process_generation: int
    session_id: int


@dataclasses.dataclass(frozen=True)
class InputState:
    buttons: int
    left_x: int
    left_y: int
    right_x: int
    right_y: int
    touches: tuple[tuple[int, int, int, int], ...] = ()


NEUTRAL_INPUT = InputState(0, 0, 0, 0, 0)


@dataclasses.dataclass(frozen=True)
class TraceEvent:
    sequence: int
    kind: int
    relative_us: int
    frame_index: int | None
    marker: int
    input: InputState | None


@dataclasses.dataclass(frozen=True)
class InputTrace:
    identity: TraceIdentity
    trace_id: int
    end_reason: int
    duration_us: int
    events: tuple[TraceEvent, ...]
    data: bytes


@dataclasses.dataclass(frozen=True)
class ReplayStats:
    events_dispatched: int
    checkpoints_seen: int
    max_drift_us: int


def _checksum(data: bytes) -> int:
    crc = zlib.crc32(data[:72])
    return zlib.crc32(data[76:], crc)


def _valid_title_id(raw: bytes) -> bool:
    return len(raw) == 9 and all(
        ord("A") <= value <= ord("Z") or ord("0") <= value <= ord("9")
        for value in raw
    )


def _fsync_parent_directory(directory: Path) -> bool:
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    unsupported = {errno.EINVAL, errno.ENOTSUP}
    if os.name == "nt":
        unsupported.update((errno.EACCES, errno.EISDIR, errno.EPERM))
    descriptor: int | None = None
    try:
        descriptor = os.open(directory, flags)
        os.fsync(descriptor)
    except OSError as error:
        if error.errno in unsupported:
            return False
        raise
    finally:
        if descriptor is not None:
            os.close(descriptor)
    return True


def verify_trace(data: bytes, *,
                 expected_identity: TraceIdentity | None = None) -> InputTrace:
    if not TRACE_HEADER_SIZE <= len(data) <= TRACE_MAX_FILE_SIZE:
        raise TraceError("trace size is outside fixed bounds")
    if any(data[25:28]) or any(data[76:96]):
        raise TraceError("trace header reserved bytes must be zero")
    values = _HEADER.unpack_from(data)
    (magic, version, header_size, event_size, end_reason, flags,
     title_raw, process_id, generation, session_id, trace_id, event_count,
     total_size, duration_us, checksum) = values
    if (magic != TRACE_MAGIC or version != TRACE_VERSION or
            header_size != TRACE_HEADER_SIZE or event_size != TRACE_EVENT_SIZE):
        raise TraceError("trace header version or size is invalid")
    if end_reason not in range(1, 10) or flags != 1:
        raise TraceError("trace terminal state is invalid")
    if not _valid_title_id(title_raw):
        raise TraceError("trace title ID is invalid")
    if not all((process_id, generation, session_id, trace_id)):
        raise TraceError("trace identity values must be nonzero")
    if event_count > TRACE_MAX_EVENTS:
        raise TraceError("trace event count exceeds the limit")
    expected_size = TRACE_HEADER_SIZE + event_count * TRACE_EVENT_SIZE
    if total_size != len(data) or expected_size != len(data):
        raise TraceError("trace total size is inconsistent")
    if duration_us > TRACE_MAX_DURATION_US:
        raise TraceError("trace duration exceeds the limit")
    if checksum != _checksum(data):
        raise TraceError("trace checksum mismatch")

    identity = TraceIdentity(
        title_raw.decode("ascii"), process_id, generation, session_id)
    if expected_identity is not None and identity != expected_identity:
        raise TraceError("trace target identity does not match")

    events: list[TraceEvent] = []
    last_time = 0
    last_frame: int | None = None
    for index in range(event_count):
        offset = TRACE_HEADER_SIZE + index * TRACE_EVENT_SIZE
        event_raw = data[offset:offset + TRACE_EVENT_SIZE]
        if (any(event_raw[38:40]) or any(event_raw[48:52]) or
                any(event_raw[60:64])):
            raise TraceError("trace event reserved bytes must be zero")
        unpacked = _EVENT.unpack(event_raw)
        (sequence, kind, size, relative_us, frame_raw, buttons,
         left_x, left_y, right_x, right_y, touch_count, marker,
         touch0_id, touch0_x, touch0_y, touch0_force,
         touch1_id, touch1_x, touch1_y, touch1_force) = unpacked
        if sequence != index + 1 or size != TRACE_EVENT_SIZE:
            raise TraceError("trace event framing is invalid")
        if index and relative_us < last_time:
            raise TraceError("trace event time regressed")
        if relative_us > duration_us:
            raise TraceError("trace event exceeds declared duration")
        frame_index = None if frame_raw == TRACE_NO_FRAME else frame_raw
        if (frame_index is not None and last_frame is not None and
                frame_index < last_frame):
            raise TraceError("trace frame index regressed")
        if frame_index is not None:
            last_frame = frame_index
        touches_raw = (
            (touch0_id, touch0_x, touch0_y, touch0_force),
            (touch1_id, touch1_x, touch1_y, touch1_force),
        )
        if kind == TRACE_EVENT_INPUT:
            if marker != 0 or touch_count > 2:
                raise TraceError("input event marker or touch count is invalid")
            if buttons & ~TRACE_ALLOWED_BUTTONS:
                raise TraceError("input event contains a system button")
            if any(any(touch) for touch in touches_raw[touch_count:]):
                raise TraceError("unused touch slots must be zero")
            input_state = InputState(
                buttons, left_x, left_y, right_x, right_y,
                tuple(touches_raw[:touch_count]))
        elif kind == TRACE_EVENT_CHECKPOINT:
            if marker not in (1, 2, 3):
                raise TraceError("checkpoint marker is invalid")
            if ((buttons, left_x, left_y, right_x, right_y, touch_count,
                 *touches_raw[0], *touches_raw[1]) != _NEUTRAL_VALUES):
                raise TraceError("checkpoint carries input state")
            input_state = None
        else:
            raise TraceError("trace event kind is unsupported")
        events.append(TraceEvent(
            sequence, kind, relative_us, frame_index, marker, input_state))
        last_time = relative_us
    if (events and last_time != duration_us) or (not events and duration_us):
        raise TraceError("trace duration does not match its events")
    return InputTrace(
        identity, trace_id, end_reason, duration_us, tuple(events), bytes(data))


def save_trace(data: bytes, destination: Path, *,
               expected_identity: TraceIdentity | None = None) -> InputTrace:
    trace = verify_trace(data, expected_identity=expected_identity)
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    descriptor: int | None = None
    try:
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        flags |= getattr(os, "O_BINARY", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        for _ in range(32):
            candidate = destination.with_name(
                f".{destination.name}.{secrets.token_hex(16)}.tmp")
            try:
                descriptor = os.open(candidate, flags, 0o600)
            except FileExistsError:
                continue
            temporary = candidate
            break
        if descriptor is None or temporary is None:
            raise FileExistsError(
                "could not allocate a unique trace temporary file")
        with os.fdopen(descriptor, "wb") as output:
            descriptor = None
            output.write(trace.data)
            output.flush()
            os.fsync(output.fileno())
        for attempt in range(20):
            try:
                os.replace(temporary, destination)
                break
            except PermissionError:
                if attempt == 19:
                    raise
                time.sleep(0.005)
        temporary = None
        _fsync_parent_directory(destination.parent)
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
    return trace


def trace_listing(trace: InputTrace) -> dict[str, object]:
    return {
        "version": TRACE_VERSION,
        "title_id": trace.identity.title_id,
        "process_id": trace.identity.process_id,
        "process_generation": trace.identity.process_generation,
        "session_id": trace.identity.session_id,
        "trace_id": trace.trace_id,
        "end_reason": trace.end_reason,
        "duration_us": trace.duration_us,
        "event_count": len(trace.events),
        "events": [
            {
                "sequence": event.sequence,
                "kind": "input" if event.input is not None else "checkpoint",
                "relative_us": event.relative_us,
                "frame_index": event.frame_index,
                "marker": event.marker or None,
                "input": dataclasses.asdict(event.input)
                if event.input is not None else None,
            }
            for event in trace.events
        ],
    }


def replay_trace(
    trace: InputTrace,
    apply: Callable[[InputState], object],
    *,
    expected_identity: TraceIdentity,
    now: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
    cancelled: Callable[[], bool] = lambda: False,
    max_sleep_seconds: float = TRACE_DEFAULT_MAX_SLEEP_SECONDS,
    max_drift_seconds: float = TRACE_DEFAULT_MAX_DRIFT_SECONDS,
) -> ReplayStats:
    trace = verify_trace(
        trace.data, expected_identity=expected_identity)
    if trace.end_reason != TRACE_END_COMPLETE:
        raise TraceError("only explicitly complete traces may be replayed")
    if not 0 < max_sleep_seconds <= TRACE_DEFAULT_MAX_SLEEP_SECONDS:
        raise ValueError("max_sleep_seconds must be between 0 and 0.05")
    if not 0 < max_drift_seconds <= 1.0:
        raise ValueError("max_drift_seconds must be between 0 and 1")
    started = now()
    dispatched = 0
    checkpoints = 0
    max_drift = 0.0
    primary: BaseException | None = None
    try:
        for event in trace.events:
            target = started + event.relative_us / 1_000_000.0
            while True:
                if cancelled():
                    raise TraceReplayCancelled("trace replay cancelled")
                remaining = target - now()
                if remaining <= 0:
                    break
                sleep(min(remaining, max_sleep_seconds))
            drift = now() - target
            if drift < 0 or drift > max_drift_seconds:
                raise TraceError("trace replay scheduling drift exceeded")
            max_drift = max(max_drift, drift)
            if event.input is None:
                checkpoints += 1
            else:
                result = apply(event.input)
                if result not in (None, 0, True):
                    raise TraceError("application input callback failed")
                dispatched += 1
        return ReplayStats(
            dispatched, checkpoints, round(max_drift * 1_000_000))
    except BaseException as error:
        primary = error
        raise
    finally:
        try:
            result = apply(NEUTRAL_INPUT)
            if result not in (None, 0, True):
                raise TraceError("neutral input release failed")
        except BaseException as cleanup_error:
            if primary is None:
                raise
            raise TraceCleanupError(primary, cleanup_error) from cleanup_error


def listing_json(trace: InputTrace) -> str:
    return json.dumps(trace_listing(trace), indent=2, sort_keys=True) + "\n"

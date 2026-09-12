"""Decode the agent's fixed binary startup diagnostic records."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
import struct


STARTUP_MAGIC = 0x31444456  # "VDD1" in little-endian file byte order.
STARTUP_VERSION = 1
STARTUP_STRUCT = struct.Struct("<IIIi")

STARTUP_IO_MAGIC = 0x314F4956  # "VIO1" in little-endian file byte order.
STARTUP_IO_VERSION = 1
STARTUP_IO_HEADER = struct.Struct("<IIII")
STARTUP_IO_EVENT = struct.Struct("<IIIi")


class StartupTraceError(ValueError):
    """A startup diagnostic file has an unsupported or malformed format."""


class StartupStage(IntEnum):
    MAIN = 1
    ROOT = 2
    INBOX = 3
    RESULTS = 4
    POWER_TICK = 5
    RNG = 6
    CHALLENGE_WRITE = 7
    WAITING = 8
    JOB_COMPLETE = 9
    SAFE_EXIT = 10


class AtomicWriteStep(IntEnum):
    OPEN = 1
    WRITE = 2
    FILE_SYNC = 3
    CLOSE = 4
    RENAME = 5
    POSTSTAT = 6
    PARENT_DOPEN = 7
    PARENT_SYNC = 8
    PARENT_DCLOSE = 9
    DEVICE_SYNC = 10


class AtomicTraceEvent(IntEnum):
    ENTER = 2
    RESULT = 3


@dataclass(frozen=True)
class StartupRecord:
    version: int
    stage: int
    code: int


@dataclass(frozen=True)
class StartupIoEvent:
    sequence: int
    step: int
    event: int
    code: int

    @property
    def step_name(self) -> str:
        try:
            return AtomicWriteStep(self.step).name.lower()
        except ValueError:
            return f"unknown-step-{self.step}"

    @property
    def event_name(self) -> str:
        try:
            return AtomicTraceEvent(self.event).name.lower()
        except ValueError:
            return f"unknown-event-{self.event}"


@dataclass(frozen=True)
class StartupIoTrace:
    version: int
    events: tuple[StartupIoEvent, ...]
    partial_tail: bytes = b""


def parse_startup_record(data: bytes) -> StartupRecord:
    """Parse the stable 16-byte VDD1 prefix, tolerating future appended data."""

    if len(data) < STARTUP_STRUCT.size:
        raise StartupTraceError("startup record is shorter than 16 bytes")
    magic, version, stage, code = STARTUP_STRUCT.unpack_from(data)
    if magic != STARTUP_MAGIC:
        raise StartupTraceError("startup record magic is not VDD1")
    if version != STARTUP_VERSION:
        raise StartupTraceError(f"unsupported startup record version: {version}")
    return StartupRecord(version=version, stage=stage, code=code)


def parse_startup_io_trace(data: bytes) -> StartupIoTrace:
    """Parse VIO1 events and retain, rather than reject, a torn final event."""

    if len(data) < STARTUP_IO_HEADER.size:
        raise StartupTraceError("startup I/O trace is shorter than its header")
    magic, version, header_size, event_size = STARTUP_IO_HEADER.unpack_from(data)
    if magic != STARTUP_IO_MAGIC:
        raise StartupTraceError("startup I/O trace magic is not VIO1")
    if version != STARTUP_IO_VERSION:
        raise StartupTraceError(f"unsupported startup I/O trace version: {version}")
    if header_size != STARTUP_IO_HEADER.size or event_size != STARTUP_IO_EVENT.size:
        raise StartupTraceError("startup I/O trace has unexpected record sizes")

    body = data[header_size:]
    complete_size = len(body) - (len(body) % event_size)
    events = []
    expected_sequence = 1
    for offset in range(0, complete_size, event_size):
        sequence, step, event, code = STARTUP_IO_EVENT.unpack_from(body, offset)
        if sequence != expected_sequence:
            raise StartupTraceError(
                f"startup I/O event sequence is {sequence}, expected {expected_sequence}"
            )
        events.append(StartupIoEvent(sequence, step, event, code))
        expected_sequence += 1
    return StartupIoTrace(version, tuple(events), body[complete_size:])


def format_result_code(code: int) -> str:
    """Show signed decimal and the exact 32-bit value returned by Vita APIs."""

    return f"{code} (0x{code & 0xFFFFFFFF:08X})"


def format_startup_diagnostics(startup: StartupRecord,
                               io_trace: StartupIoTrace | None = None) -> str:
    try:
        stage_name = StartupStage(startup.stage).name.lower()
    except ValueError:
        stage_name = f"unknown-stage-{startup.stage}"
    lines = [
        f"startup: stage={stage_name} ({startup.stage}) "
        f"code={format_result_code(startup.code)}"
    ]
    if io_trace is not None:
        lines.append(f"challenge atomic-write events: {len(io_trace.events)}")
        for event in io_trace.events:
            code = "" if event.event == AtomicTraceEvent.ENTER else (
                f" code={format_result_code(event.code)}"
            )
            lines.append(
                f"  {event.sequence:02d} {event.event_name:6s} "
                f"{event.step_name}{code}"
            )
        if io_trace.partial_tail:
            lines.append(
                f"  warning: ignored {len(io_trace.partial_tail)} byte(s) "
                "from a torn final event"
            )
    return "\n".join(lines)

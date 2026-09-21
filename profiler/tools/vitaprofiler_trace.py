#!/usr/bin/env python3
"""Receive, validate, inspect, and export VitaProfiler binary captures."""

from __future__ import annotations

import argparse
import collections
import dataclasses
import datetime
import hashlib
import ipaddress
import json
import math
import os
import socket
import struct
import sys
import tempfile
import time
import unicodedata
import zlib
from pathlib import Path
from typing import Callable, Iterable, Sequence


WIRE_MAGIC = 0x46525056
WIRE_VERSION = 1
WIRE_HEADER_SIZE = 32
WIRE_EVENT_SIZE = 32
WIRE_FLAGS = 1
WIRE_CLOCK_HZ = 1_000_000

STREAM_V2_CHUNK_MAGIC = 0x32435056
STREAM_V2_VERSION = 2
STREAM_V2_CHUNK_HEADER_SIZE = 32
STREAM_V2_SESSION_PAYLOAD_SIZE = 96
STREAM_V2_THREAD_PAYLOAD_SIZE = 32
STREAM_V2_MODULE_PAYLOAD_SIZE = 32
STREAM_V2_STATS_PAYLOAD_SIZE = 32
STREAM_V2_TIMER_SOURCE_MAX = 40
STREAM_V2_TIMER_UNIT_MAX = 16
STREAM_V2_MAX_CHUNK_PAYLOAD = 2 * 1024 * 1024
STREAM_V2_MAX_THREAD_METADATA = 64
STREAM_V2_MAX_MODULE_METADATA = 64

STREAM_V2_CHUNK_SESSION = 1
STREAM_V2_CHUNK_DICTIONARY = 2
STREAM_V2_CHUNK_EVENTS = 3
STREAM_V2_CHUNK_THREAD = 4
STREAM_V2_CHUNK_MODULE = 5
STREAM_V2_CHUNK_STATS = 6
STREAM_V2_CHUNK_END = 7

STREAM_V2_SESSION_PROCESS_ID = 1 << 0
STREAM_V2_SESSION_PROCESS_IDENTITY = 1 << 1
STREAM_V2_SESSION_TIMER_SOURCE = 1 << 2
STREAM_V2_SESSION_TIMER_UNIT = 1 << 3
STREAM_V2_SESSION_FLAGS_ALL = (
    STREAM_V2_SESSION_PROCESS_ID |
    STREAM_V2_SESSION_PROCESS_IDENTITY |
    STREAM_V2_SESSION_TIMER_SOURCE |
    STREAM_V2_SESSION_TIMER_UNIT
)
STREAM_V2_THREAD_IDENTITY = 1 << 0
STREAM_V2_MODULE_EXECUTABLE = 1 << 0
STREAM_V2_MODULE_ARM = 1 << 1
STREAM_V2_MODULE_THUMB = 1 << 2
STREAM_V2_MODULE_FLAGS_ALL = (
    STREAM_V2_MODULE_EXECUTABLE |
    STREAM_V2_MODULE_ARM |
    STREAM_V2_MODULE_THUMB
)

NAME_MAGIC = 0x4D4E5056
NAME_VERSION = 1
NAME_HEADER_SIZE = 24
NAME_ENTRY_HEADER_SIZE = 8
NAME_FLAGS = 1
NAME_FLAG_BUILTIN = 1
NAME_MAX_LENGTH = 255
NAME_MAX_USER_ENTRIES = 4096

EVENT_ZONE_BEGIN = 1
EVENT_ZONE_END = 2
EVENT_COUNTER = 3
EVENT_FRAME = 4
EVENT_MEMORY_SAMPLE = 5
EVENT_THREAD_SAMPLE = 6
EVENT_PROCESS_SAMPLE = 7
EVENT_CUSTOM = 0x8000

EVENT_FLAG_FIRST = 1 << 0
EVENT_FLAG_THREAD_MISMATCH = 1 << 1
EVENT_FLAG_CLOCK_REGRESSION = 1 << 2
EVENT_FLAG_RAW_VALUE = 1 << 3

WIRE_HEADER = struct.Struct("<IHHHHIQQ")
WIRE_EVENT = struct.Struct("<QqIIIHH")
NAME_HEADER = struct.Struct("<IHHHHIII")
NAME_ENTRY_HEADER = struct.Struct("<IHH")
STREAM_V2_CHUNK_HEADER = struct.Struct("<IHHHHIIIQ")
STREAM_V2_SESSION = struct.Struct("<QQQIIHHI40s16s")
STREAM_V2_THREAD = struct.Struct("<QIIIIQ")
STREAM_V2_MODULE = struct.Struct("<IIIIIIQ")
STREAM_V2_STATS = struct.Struct("<IIIIQQ")

DEFAULT_PORT = 18195
DEFAULT_MAX_BYTES = 16 * 1024 * 1024
MAX_DECODED_EVENTS = 262_144
DEFAULT_ACCEPT_TIMEOUT = 30.0
DEFAULT_IDLE_TIMEOUT = 5.0
DEFAULT_CAPTURE_TIMEOUT = 300.0
RECEIVER_CANCEL_POLL_SECONDS = 0.1
RUN_CLOCKS_METRIC_ID = 0xFFF00005
RUN_CLOCKS_SOURCE = "SceKernelThreadInfo.runClocks"
RUN_CLOCKS_UNIT = "unknown"
RUN_CLOCKS_EXPERIMENT_FORMAT = "vitaprofiler-runclocks-experiment-v1"
RUN_CLOCKS_REPORT_FORMAT = "vitaprofiler-runclocks-characterization-v1"
MAX_EXPERIMENT_METADATA_BYTES = 64 * 1024
MAX_EXPERIMENT_THREADS = 64
MAX_METADATA_TEXT = 1024

BUILTIN_NAMES = {
    0xFFF00001: "vita.memory.free_user_bytes",
    0xFFF00002: "vita.memory.free_cdram_bytes",
    0xFFF00003: "vita.memory.free_phycont_bytes",
    0xFFF00004: "vita.process.time_us",
    0xFFF00005: "vita.thread.run_clocks",
    0xFFF00006: "vita.thread.stack_free_bytes",
    0xFFF00007: "vita.thread.preemptions",
    0xFFF00008: "vita.thread.interrupt_preemptions",
}

EVENT_TYPE_NAMES = {
    EVENT_ZONE_BEGIN: "zone_begin",
    EVENT_ZONE_END: "zone_end",
    EVENT_COUNTER: "counter",
    EVENT_FRAME: "frame",
    EVENT_MEMORY_SAMPLE: "memory_sample",
    EVENT_THREAD_SAMPLE: "thread_sample",
    EVENT_PROCESS_SAMPLE: "process_sample",
}

EVENT_FLAG_NAMES = {
    EVENT_FLAG_FIRST: "first",
    EVENT_FLAG_THREAD_MISMATCH: "thread_mismatch",
    EVENT_FLAG_CLOCK_REGRESSION: "clock_regression",
    EVENT_FLAG_RAW_VALUE: "raw_value",
}


class TraceFormatError(ValueError):
    """The input is not one complete supported VitaProfiler capture."""


class TraceReceiveError(RuntimeError):
    """A bounded TCP capture could not be received completely."""


class TraceReceiveCancelled(TraceReceiveError):
    """A TCP capture was cancelled by its local controller."""


@dataclasses.dataclass(frozen=True)
class NameEntry:
    name_id: int
    name: str
    flags: int

    @property
    def builtin(self) -> bool:
        return bool(self.flags & NAME_FLAG_BUILTIN)


@dataclasses.dataclass(frozen=True)
class TraceHeader:
    version: int
    flags: int
    clock_hz: int
    stream_start_us: int


@dataclasses.dataclass(frozen=True)
class SessionMetadata:
    session_id: int
    process_id: int | None
    process_identity: int | None
    timer_source: str | None
    timer_unit: str | None


@dataclasses.dataclass(frozen=True)
class ThreadIdentity:
    thread_id: int
    generation: int
    identity: int | None
    name_id: int
    first_event_index: int


@dataclasses.dataclass(frozen=True)
class ModuleRange:
    module_id: int
    generation: int
    address_start: int
    address_end: int
    name_id: int
    executable: bool
    arm: bool
    thumb: bool


@dataclasses.dataclass(frozen=True)
class LossCounters:
    producer_accepted: int
    producer_dropped: int
    transport_lost: int
    sink_lost: int
    events_written: int
    bytes_written: int


@dataclasses.dataclass(frozen=True)
class Event:
    index: int
    timestamp_us: int
    value: int
    name_id: int
    thread_id: int
    correlation_id: int
    event_type: int
    flags: int

    @property
    def type_name(self) -> str:
        if self.event_type >= EVENT_CUSTOM:
            return f"custom_0x{self.event_type:04x}"
        return EVENT_TYPE_NAMES.get(self.event_type,
                                    f"unknown_{self.event_type}")

    @property
    def flag_names(self) -> tuple[str, ...]:
        names = [name for bit, name in EVENT_FLAG_NAMES.items()
                 if self.flags & bit]
        unknown = self.flags & ~sum(EVENT_FLAG_NAMES)
        if unknown:
            names.append(f"unknown_0x{unknown:04x}")
        return tuple(names)


@dataclasses.dataclass(frozen=True)
class TraceCapture:
    header: TraceHeader
    names: dict[int, NameEntry]
    events: tuple[Event, ...]
    dictionary_size: int
    raw_size: int
    session: SessionMetadata | None = None
    thread_identities: tuple[ThreadIdentity, ...] = ()
    module_ranges: tuple[ModuleRange, ...] = ()
    loss: LossCounters | None = None
    complete: bool = True

    def resolve_name(self, name_id: int) -> str:
        if name_id == 0:
            return "(unnamed)"
        entry = self.names.get(name_id)
        return entry.name if entry else f"name_0x{name_id:08x}"


@dataclasses.dataclass(frozen=True)
class ZoneSpan:
    name_id: int
    name: str
    thread_id: int
    correlation_id: int
    begin_us: int
    end_us: int
    duration_us: int
    flags: int
    begin_event_index: int
    end_event_index: int


@dataclasses.dataclass(frozen=True)
class ZoneAnalysis:
    spans: tuple[ZoneSpan, ...]
    unmatched_begins: tuple[Event, ...]
    unmatched_ends: tuple[Event, ...]
    duplicate_begins: tuple[Event, ...]
    negative_durations: tuple[Event, ...]


@dataclasses.dataclass(frozen=True)
class ThreadGeneration:
    thread_id: int
    generation: int
    label: str
    first_event_index: int
    last_event_index: int


@dataclasses.dataclass(frozen=True)
class RunClocksExperiment:
    metadata: dict[str, object]
    counter_bits: int | None
    max_wrap_delta_raw: int | None
    threads: tuple[ThreadGeneration, ...]


_UNSAFE_BIDI_CLASSES = frozenset({
    "BN", "LRE", "RLE", "LRO", "RLO", "PDF", "LRI", "RLI", "FSI",
    "PDI",
})


def _decode_safe_name(raw_name: bytes, index: int) -> str:
    return _decode_safe_text(raw_name, f"VPNM entry {index}")


def _decode_safe_text(raw_text: bytes, label: str) -> str:
    try:
        name = raw_text.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise TraceFormatError(
            f"{label} is not valid UTF-8") from error
    for character in name:
        if (unicodedata.category(character) in {"Cc", "Cf", "Cs", "Zl", "Zp"} or
                unicodedata.bidirectional(character) in _UNSAFE_BIDI_CLASSES):
            raise TraceFormatError(
                f"{label} contains unsafe display controls")
    return name


def fnv1a_name_id(raw_name: bytes) -> int:
    if not raw_name:
        return 0
    value = 2166136261
    for byte in raw_name:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value or 1


def _decode_dictionary(data: bytes) -> tuple[dict[int, NameEntry], int]:
    if len(data) < NAME_HEADER_SIZE:
        raise TraceFormatError("truncated VPNM dictionary header")
    (magic, version, header_size, entry_header_size, flags, entry_count,
     total_size, reserved) = NAME_HEADER.unpack_from(data)
    if magic != NAME_MAGIC:
        raise TraceFormatError("bad VPNM dictionary magic")
    if version != NAME_VERSION:
        raise TraceFormatError(f"unsupported VPNM version {version}")
    if (header_size != NAME_HEADER_SIZE or
            entry_header_size != NAME_ENTRY_HEADER_SIZE or
            flags != NAME_FLAGS):
        raise TraceFormatError("unsupported VPNM header or flags")
    if reserved:
        raise TraceFormatError("VPNM reserved field is not zero")
    builtin_count = len(BUILTIN_NAMES)
    if not builtin_count <= entry_count <= NAME_MAX_USER_ENTRIES + builtin_count:
        raise TraceFormatError("VPNM entry count is outside the supported bound")
    if not NAME_HEADER_SIZE <= total_size <= len(data):
        raise TraceFormatError("VPNM total size exceeds available bytes")
    if entry_count > (total_size - NAME_HEADER_SIZE) // 12:
        raise TraceFormatError("VPNM entry count cannot fit in its block")

    result: dict[int, NameEntry] = {}
    seen_builtins: set[int] = set()
    previous_id = 0
    offset = NAME_HEADER_SIZE
    for index in range(entry_count):
        if total_size - offset < NAME_ENTRY_HEADER_SIZE:
            raise TraceFormatError(f"truncated VPNM entry {index}")
        name_id, name_length, entry_flags = NAME_ENTRY_HEADER.unpack_from(
            data, offset)
        padded_length = (name_length + 3) & ~3
        end = offset + NAME_ENTRY_HEADER_SIZE + padded_length
        if name_id == 0 or not 1 <= name_length <= NAME_MAX_LENGTH:
            raise TraceFormatError(f"invalid VPNM entry {index}")
        if index and name_id <= previous_id:
            raise TraceFormatError("VPNM IDs are not strictly increasing")
        if end > total_size:
            raise TraceFormatError(f"truncated VPNM name at entry {index}")
        raw_name = data[offset + NAME_ENTRY_HEADER_SIZE:
                        offset + NAME_ENTRY_HEADER_SIZE + name_length]
        padding = data[offset + NAME_ENTRY_HEADER_SIZE + name_length:end]
        if b"\0" in raw_name:
            raise TraceFormatError(f"VPNM entry {index} contains NUL")
        if any(padding):
            raise TraceFormatError(f"VPNM entry {index} has nonzero padding")

        builtin_name = BUILTIN_NAMES.get(name_id)
        if builtin_name is not None:
            if (entry_flags != NAME_FLAG_BUILTIN or
                    raw_name != builtin_name.encode("utf-8")):
                raise TraceFormatError(
                    f"VPNM built-in 0x{name_id:08x} was redefined")
            seen_builtins.add(name_id)
        else:
            if entry_flags != 0 or fnv1a_name_id(raw_name) != name_id:
                raise TraceFormatError(
                    f"VPNM application name 0x{name_id:08x} failed validation")
        name = _decode_safe_name(raw_name, index)
        result[name_id] = NameEntry(name_id, name, entry_flags)
        previous_id = name_id
        offset = end

    if offset != total_size:
        raise TraceFormatError("VPNM block has unaccounted bytes")
    if seen_builtins != set(BUILTIN_NAMES):
        missing = sorted(set(BUILTIN_NAMES) - seen_builtins)
        raise TraceFormatError(
            "VPNM block is missing built-ins: " +
            ", ".join(f"0x{item:08x}" for item in missing))
    return result, total_size


def _decode_header(data: bytes, offset: int) -> TraceHeader:
    if len(data) - offset < WIRE_HEADER_SIZE:
        raise TraceFormatError("truncated VPRF stream header")
    (magic, version, header_size, event_size, flags, clock_hz,
     stream_start_us, reserved) = WIRE_HEADER.unpack_from(data, offset)
    if magic != WIRE_MAGIC:
        raise TraceFormatError("bad VPRF stream magic")
    if version != WIRE_VERSION:
        raise TraceFormatError(f"unsupported VPRF version {version}")
    if (header_size != WIRE_HEADER_SIZE or event_size != WIRE_EVENT_SIZE or
            flags != WIRE_FLAGS or clock_hz != WIRE_CLOCK_HZ):
        raise TraceFormatError("unsupported VPRF sizes, flags, or clock")
    if reserved:
        raise TraceFormatError("VPRF reserved field is not zero")
    return TraceHeader(version, flags, clock_hz, stream_start_us)


def _decode_v1_capture(data: bytes) -> TraceCapture:
    if len(data) < 4:
        raise TraceFormatError("capture is too short to contain a magic")
    first_magic = struct.unpack_from("<I", data)[0]
    names: dict[int, NameEntry] = {}
    dictionary_size = 0
    if first_magic == NAME_MAGIC:
        names, dictionary_size = _decode_dictionary(data)
    elif first_magic != WIRE_MAGIC:
        raise TraceFormatError("capture must begin with VPNM or VPRF")

    header = _decode_header(data, dictionary_size)
    event_offset = dictionary_size + WIRE_HEADER_SIZE
    payload_size = len(data) - event_offset
    if payload_size % WIRE_EVENT_SIZE:
        raise TraceFormatError("VPRF stream ends with a partial event")
    event_count = payload_size // WIRE_EVENT_SIZE
    if event_count > MAX_DECODED_EVENTS:
        raise TraceFormatError(
            f"VPRF stream contains {event_count} events; decoded limit is "
            f"{MAX_DECODED_EVENTS}")

    events: list[Event] = []
    for index, offset in enumerate(
            range(event_offset, len(data), WIRE_EVENT_SIZE)):
        timestamp, value, name_id, thread_id, correlation_id, event_type, flags = (
            WIRE_EVENT.unpack_from(data, offset))
        events.append(Event(index, timestamp, value, name_id, thread_id,
                            correlation_id, event_type, flags))
    return TraceCapture(header, names, tuple(events), dictionary_size,
                        len(data))


class IncrementalTraceDecoder:
    """Bounded incremental decoder for v2, with strict v1 EOF fallback."""

    def __init__(self, max_bytes: int = DEFAULT_MAX_BYTES,
                 max_events: int = MAX_DECODED_EVENTS) -> None:
        if max_bytes < WIRE_HEADER_SIZE or max_events < 0:
            raise ValueError("invalid incremental decoder bounds")
        self.max_bytes = max_bytes
        self.max_events = max_events
        self._buffer = bytearray()
        self._v1_data = bytearray()
        self._mode: str | None = None
        self._raw_size = 0
        self._processed_size = 0
        self._current_chunk_end = 0
        self._next_sequence = 0
        self._session: SessionMetadata | None = None
        self._header: TraceHeader | None = None
        self._names: dict[int, NameEntry] = {}
        self._dictionary_size = 0
        self._events: list[Event] = []
        self._threads: list[ThreadIdentity] = []
        self._modules: list[ModuleRange] = []
        self._thread_keys: set[tuple[int, int]] = set()
        self._module_keys: set[tuple[int, int]] = set()
        self._loss: LossCounters | None = None
        self._end_seen = False
        self._complete = False
        self._finished = False

    @property
    def is_v2(self) -> bool:
        return self._mode == "v2"

    @property
    def complete(self) -> bool:
        return self._complete

    @property
    def end_seen(self) -> bool:
        return self._end_seen

    @property
    def event_count(self) -> int:
        return len(self._events)

    def feed(self, data: bytes) -> bool:
        if self._finished:
            raise TraceFormatError("capture decoder is already finished")
        if not data:
            return False
        self._raw_size += len(data)
        if self._raw_size > self.max_bytes:
            raise TraceFormatError(
                f"capture exceeds the {self.max_bytes}-byte safety limit")
        if self._mode == "v1":
            self._v1_data.extend(data)
            return False
        self._buffer.extend(data)
        if self._mode is None:
            if len(self._buffer) < 4:
                return False
            magic = struct.unpack_from("<I", self._buffer)[0]
            if magic == STREAM_V2_CHUNK_MAGIC:
                self._mode = "v2"
            elif magic in (NAME_MAGIC, WIRE_MAGIC):
                self._mode = "v1"
                self._v1_data.extend(self._buffer)
                self._buffer.clear()
                return False
            else:
                raise TraceFormatError(
                    "capture must begin with VPNM, VPRF, or v2 chunk")

        changed = False
        while len(self._buffer) >= STREAM_V2_CHUNK_HEADER_SIZE:
            fields = STREAM_V2_CHUNK_HEADER.unpack_from(self._buffer)
            (magic, version, header_size, chunk_type, flags, payload_size,
             sequence, checksum, reserved) = fields
            if magic != STREAM_V2_CHUNK_MAGIC:
                raise TraceFormatError("bad v2 chunk magic")
            if version != STREAM_V2_VERSION:
                raise TraceFormatError(
                    f"unsupported profiler stream version {version}")
            if header_size != STREAM_V2_CHUNK_HEADER_SIZE:
                raise TraceFormatError("unsupported v2 chunk header size")
            if chunk_type not in range(STREAM_V2_CHUNK_SESSION,
                                       STREAM_V2_CHUNK_END + 1):
                raise TraceFormatError(f"unknown v2 chunk type {chunk_type}")
            if flags or reserved:
                raise TraceFormatError("v2 chunk reserved fields are not zero")
            if payload_size > STREAM_V2_MAX_CHUNK_PAYLOAD:
                raise TraceFormatError("v2 chunk payload exceeds safety limit")
            total_size = STREAM_V2_CHUNK_HEADER_SIZE + payload_size
            if len(self._buffer) < total_size:
                return changed
            payload = bytes(self._buffer[STREAM_V2_CHUNK_HEADER_SIZE:
                                         total_size])
            if zlib.crc32(payload) & 0xFFFFFFFF != checksum:
                raise TraceFormatError("v2 chunk payload CRC mismatch")
            if sequence != self._next_sequence:
                raise TraceFormatError(
                    f"v2 chunk sequence {sequence} does not match "
                    f"{self._next_sequence}")
            if self._end_seen:
                raise TraceFormatError("v2 data follows the END chunk")
            self._current_chunk_end = self._processed_size + total_size
            self._decode_v2_chunk(chunk_type, payload)
            del self._buffer[:total_size]
            self._processed_size = self._current_chunk_end
            self._next_sequence += 1
            changed = True
        return changed

    def _decode_v2_chunk(self, chunk_type: int, payload: bytes) -> None:
        if self._next_sequence == 0:
            if chunk_type != STREAM_V2_CHUNK_SESSION:
                raise TraceFormatError("v2 stream must begin with SESSION")
            self._decode_v2_session(payload)
            return
        if self._next_sequence == 1:
            if chunk_type != STREAM_V2_CHUNK_DICTIONARY:
                raise TraceFormatError(
                    "v2 SESSION must be followed by DICTIONARY")
            self._names, self._dictionary_size = _decode_dictionary(payload)
            if self._dictionary_size != len(payload):
                raise TraceFormatError("v2 dictionary has trailing bytes")
            return
        if chunk_type in (STREAM_V2_CHUNK_SESSION,
                          STREAM_V2_CHUNK_DICTIONARY):
            raise TraceFormatError("duplicate v2 lifecycle chunk")
        if chunk_type == STREAM_V2_CHUNK_EVENTS:
            self._decode_v2_events(payload)
        elif chunk_type == STREAM_V2_CHUNK_THREAD:
            self._decode_v2_thread(payload)
        elif chunk_type == STREAM_V2_CHUNK_MODULE:
            self._decode_v2_module(payload)
        elif chunk_type in (STREAM_V2_CHUNK_STATS, STREAM_V2_CHUNK_END):
            loss = self._decode_v2_stats(payload)
            if self._loss is not None and (
                    loss.events_written < self._loss.events_written or
                    loss.bytes_written < self._loss.bytes_written):
                raise TraceFormatError("v2 loss counters regressed")
            if loss.events_written > len(self._events):
                raise TraceFormatError(
                    "v2 stats claim events not present in the stream")
            self._loss = loss
            if chunk_type == STREAM_V2_CHUNK_END:
                if loss.events_written != len(self._events):
                    raise TraceFormatError(
                        "v2 END event count does not match the stream")
                if loss.sink_lost:
                    raise TraceFormatError(
                        "v2 END cannot claim a clean sink after sink loss")
                if loss.bytes_written != self._current_chunk_end:
                    raise TraceFormatError(
                        "v2 END byte count does not match the stream")
                self._end_seen = True

    def _decode_v2_session(self, payload: bytes) -> None:
        if len(payload) != STREAM_V2_SESSION_PAYLOAD_SIZE:
            raise TraceFormatError("invalid v2 SESSION payload size")
        (session_id, process_identity, stream_start_us, process_id, flags,
         source_length, unit_length, reserved, source, unit) = (
            STREAM_V2_SESSION.unpack(payload))
        if not session_id or flags & ~STREAM_V2_SESSION_FLAGS_ALL or reserved:
            raise TraceFormatError("invalid v2 SESSION metadata")
        if (source_length > STREAM_V2_TIMER_SOURCE_MAX or
                unit_length > STREAM_V2_TIMER_UNIT_MAX):
            raise TraceFormatError("v2 timer metadata exceeds its bound")
        if any(source[source_length:]) or any(unit[unit_length:]):
            raise TraceFormatError("v2 timer metadata has nonzero padding")
        has_source = bool(flags & STREAM_V2_SESSION_TIMER_SOURCE)
        has_unit = bool(flags & STREAM_V2_SESSION_TIMER_UNIT)
        if has_source != bool(source_length) or has_unit != bool(unit_length):
            raise TraceFormatError("v2 timer metadata flags disagree")
        if (not flags & STREAM_V2_SESSION_PROCESS_ID and process_id) or (
                not flags & STREAM_V2_SESSION_PROCESS_IDENTITY and
                process_identity):
            raise TraceFormatError("v2 process metadata flags disagree")
        timer_source = (_decode_safe_text(
            source[:source_length], "v2 timer source") if has_source else None)
        timer_unit = (_decode_safe_text(
            unit[:unit_length], "v2 timer unit") if has_unit else None)
        self._session = SessionMetadata(
            session_id=session_id,
            process_id=(process_id if flags &
                        STREAM_V2_SESSION_PROCESS_ID else None),
            process_identity=(process_identity if flags &
                              STREAM_V2_SESSION_PROCESS_IDENTITY else None),
            timer_source=timer_source,
            timer_unit=timer_unit,
        )
        self._header = TraceHeader(
            STREAM_V2_VERSION, WIRE_FLAGS, WIRE_CLOCK_HZ, stream_start_us)

    def _decode_v2_events(self, payload: bytes) -> None:
        if not payload or len(payload) % WIRE_EVENT_SIZE:
            raise TraceFormatError("v2 EVENTS payload has a partial event")
        event_count = len(payload) // WIRE_EVENT_SIZE
        if len(self._events) + event_count > self.max_events:
            raise TraceFormatError(
                f"v2 stream exceeds decoded event limit {self.max_events}")
        for offset in range(0, len(payload), WIRE_EVENT_SIZE):
            values = WIRE_EVENT.unpack_from(payload, offset)
            self._events.append(Event(len(self._events), *values))

    def _decode_v2_thread(self, payload: bytes) -> None:
        if len(payload) != STREAM_V2_THREAD_PAYLOAD_SIZE:
            raise TraceFormatError("invalid v2 THREAD payload size")
        identity, thread_id, generation, name_id, flags, reserved = (
            STREAM_V2_THREAD.unpack(payload))
        if (not thread_id or flags & ~STREAM_V2_THREAD_IDENTITY or reserved or
                bool(flags & STREAM_V2_THREAD_IDENTITY) != bool(identity)):
            raise TraceFormatError("invalid v2 THREAD metadata")
        item = ThreadIdentity(
            thread_id, generation,
            identity if flags & STREAM_V2_THREAD_IDENTITY else None, name_id,
            len(self._events))
        key = (thread_id, generation)
        if key in self._thread_keys:
            raise TraceFormatError("duplicate v2 thread generation")
        if len(self._thread_keys) == STREAM_V2_MAX_THREAD_METADATA:
            raise TraceFormatError("v2 thread metadata exceeds safety limit")
        self._thread_keys.add(key)
        self._threads.append(item)

    def _decode_v2_module(self, payload: bytes) -> None:
        if len(payload) != STREAM_V2_MODULE_PAYLOAD_SIZE:
            raise TraceFormatError("invalid v2 MODULE payload size")
        (module_id, generation, start, end, name_id, flags, reserved) = (
            STREAM_V2_MODULE.unpack(payload))
        if (not module_id or start >= end or
                flags & ~STREAM_V2_MODULE_FLAGS_ALL or
                not flags & (STREAM_V2_MODULE_ARM |
                             STREAM_V2_MODULE_THUMB) or reserved):
            raise TraceFormatError("invalid v2 MODULE metadata")
        item = ModuleRange(
            module_id, generation, start, end, name_id,
            bool(flags & STREAM_V2_MODULE_EXECUTABLE),
            bool(flags & STREAM_V2_MODULE_ARM),
            bool(flags & STREAM_V2_MODULE_THUMB))
        key = (module_id, generation)
        if key in self._module_keys:
            raise TraceFormatError("duplicate v2 module generation")
        if len(self._module_keys) == STREAM_V2_MAX_MODULE_METADATA:
            raise TraceFormatError("v2 module metadata exceeds safety limit")
        self._module_keys.add(key)
        self._modules.append(item)

    @staticmethod
    def _decode_v2_stats(payload: bytes) -> LossCounters:
        if len(payload) != STREAM_V2_STATS_PAYLOAD_SIZE:
            raise TraceFormatError("invalid v2 STATS payload size")
        return LossCounters(*STREAM_V2_STATS.unpack(payload))

    def snapshot(self, allow_incomplete: bool = True) -> TraceCapture:
        if self._mode != "v2" or self._header is None or self._session is None:
            raise TraceFormatError("v2 session metadata is not complete")
        if self._next_sequence < 2:
            raise TraceFormatError("v2 dictionary is not complete")
        if not allow_incomplete and not self._complete:
            raise TraceFormatError("v2 session ended without an END chunk")
        return TraceCapture(
            self._header, dict(self._names), tuple(self._events),
            self._dictionary_size, self._raw_size, self._session,
            tuple(self._threads), tuple(self._modules), self._loss,
            self._complete)

    def finish(self) -> TraceCapture:
        if self._finished:
            raise TraceFormatError("capture decoder is already finished")
        self._finished = True
        if self._mode == "v1":
            return _decode_v1_capture(bytes(self._v1_data))
        if self._mode != "v2":
            raise TraceFormatError("capture is too short to contain a magic")
        if self._buffer:
            raise TraceFormatError("v2 stream ends with a truncated chunk")
        if not self._end_seen:
            raise TraceFormatError("v2 session ended without an END chunk")
        self._complete = True
        return self.snapshot(allow_incomplete=False)


def decode_capture(data: bytes) -> TraceCapture:
    """Decode one complete v1 capture or CRC-framed v2 session."""
    decoder = IncrementalTraceDecoder(max_bytes=max(DEFAULT_MAX_BYTES,
                                                    len(data)))
    decoder.feed(data)
    return decoder.finish()


def analyze_zones(capture: TraceCapture) -> ZoneAnalysis:
    active: dict[tuple[int, int], list[Event]] = collections.defaultdict(list)
    active_correlations: collections.Counter[int] = collections.Counter()
    spans: list[ZoneSpan] = []
    unmatched_ends: list[Event] = []
    duplicate_begins: list[Event] = []
    negative_durations: list[Event] = []

    for event in capture.events:
        if event.event_type == EVENT_ZONE_BEGIN:
            if active_correlations[event.correlation_id]:
                duplicate_begins.append(event)
            active[(event.correlation_id, event.name_id)].append(event)
            active_correlations[event.correlation_id] += 1
            continue
        if event.event_type != EVENT_ZONE_END:
            continue
        key = (event.correlation_id, event.name_id)
        candidates = active.get(key, [])
        if not candidates:
            unmatched_ends.append(event)
            continue
        begin = candidates.pop()
        active_correlations[event.correlation_id] -= 1
        if active_correlations[event.correlation_id] == 0:
            del active_correlations[event.correlation_id]
        if not candidates:
            active.pop(key, None)
        if event.value < 0:
            negative_durations.append(event)
        spans.append(ZoneSpan(
            name_id=begin.name_id,
            name=capture.resolve_name(begin.name_id),
            thread_id=begin.thread_id,
            correlation_id=begin.correlation_id,
            begin_us=begin.timestamp_us,
            end_us=event.timestamp_us,
            duration_us=max(0, event.value),
            flags=begin.flags | event.flags,
            begin_event_index=begin.index,
            end_event_index=event.index,
        ))

    unmatched_begins = tuple(
        event for candidates in active.values() for event in candidates)
    return ZoneAnalysis(tuple(spans), unmatched_begins,
                        tuple(unmatched_ends), tuple(duplicate_begins),
                        tuple(negative_durations))


def _run_clocks_raw_value(event: Event) -> int:
    return event.value & 0xFFFFFFFFFFFFFFFF


def _thread_generation_for_event(
        event: Event,
        threads: tuple[ThreadGeneration, ...],
        ) -> ThreadGeneration | None:
    matches = [
        item for item in threads
        if (item.thread_id == event.thread_id and
            item.first_event_index <= event.index <= item.last_event_index)
    ]
    if len(matches) > 1:
        raise TraceFormatError(
            f"event {event.index} matches overlapping thread generations")
    return matches[0] if matches else None


def analyze_run_clocks(
        capture: TraceCapture,
        experiment: RunClocksExperiment | None = None,
        ) -> dict[str, object]:
    """Preserve runClocks as an unknown-unit raw counter and derive safe deltas."""
    counter_bits = experiment.counter_bits if experiment else None
    max_wrap_delta = experiment.max_wrap_delta_raw if experiment else None
    threads = experiment.threads if experiment else ()
    explicit_state: dict[
        tuple[int, int], tuple[int, int, int, int, int]] = {}
    inferred_state: dict[int, tuple[int, int, int, int, int]] = {}
    samples: list[dict[str, object]] = []
    status_counts: collections.Counter[str] = collections.Counter()
    missing_raw_flags = 0

    for event in capture.events:
        if event.name_id != RUN_CLOCKS_METRIC_ID:
            continue
        if event.event_type != EVENT_THREAD_SAMPLE:
            raise TraceFormatError(
                f"runClocks event {event.index} is not a thread sample")
        if not event.flags & EVENT_FLAG_RAW_VALUE:
            missing_raw_flags += 1

        raw_value = _run_clocks_raw_value(event)
        if counter_bits is not None and raw_value >= (1 << counter_bits):
            raise TraceFormatError(
                f"runClocks event {event.index} does not fit the declared "
                f"{counter_bits}-bit counter")
        declared = _thread_generation_for_event(event, threads)
        if declared is not None:
            generation = declared.generation
            label = declared.label
            identity_source = "experiment_metadata"
            state_key = (event.thread_id, generation)
            previous = explicit_state.get(state_key)
        else:
            label = f"tid-0x{event.thread_id:08x}"
            identity_source = "inferred_tid_continuity"
            previous = inferred_state.get(event.thread_id)
            generation = previous[2] if previous else 0

        status = "first_observation"
        delta_raw: int | None = None
        elapsed_us: int | None = None
        counter_epoch = previous[1] if previous else 0
        previous_raw: int | None = None
        previous_event_index: int | None = None
        if previous is not None:
            (previous_raw, counter_epoch, previous_generation,
             previous_timestamp, previous_event_index) = previous
            if declared is None:
                generation = previous_generation
            elapsed_us = event.timestamp_us - previous_timestamp
            if elapsed_us < 0:
                elapsed_us = None
                status = "timestamp_regression"
            elif raw_value >= previous_raw:
                delta_raw = raw_value - previous_raw
                status = "delta"
            else:
                wrapped_delta = None
                if counter_bits is not None:
                    modulus = 1 << counter_bits
                    if raw_value < modulus and previous_raw < modulus:
                        wrapped_delta = modulus - previous_raw + raw_value
                if (wrapped_delta is not None and
                        max_wrap_delta is not None and
                        wrapped_delta <= max_wrap_delta):
                    delta_raw = wrapped_delta
                    status = "wrap"
                else:
                    counter_epoch += 1
                    status = "reset_or_reuse"
                    if declared is None:
                        generation += 1

        sample = {
            "event_index": event.index,
            "timestamp_us": event.timestamp_us,
            "thread_id": event.thread_id,
            "thread_id_hex": f"0x{event.thread_id:08x}",
            "thread_generation": generation,
            "thread_label": label,
            "identity_source": identity_source,
            "counter_epoch": counter_epoch,
            "raw_value_u64": raw_value,
            "raw_value_u64_decimal": str(raw_value),
            "raw_value_u64_hex": f"0x{raw_value:016x}",
            "previous_event_index": previous_event_index,
            "previous_raw_value_u64": previous_raw,
            "previous_raw_value_u64_decimal": (
                str(previous_raw) if previous_raw is not None else None),
            "elapsed_us": elapsed_us,
            "delta_raw": delta_raw,
            "delta_raw_decimal": (
                str(delta_raw) if delta_raw is not None else None),
            "delta_raw_hex": (
                f"0x{delta_raw:x}" if delta_raw is not None else None),
            "delta_status": status,
            "unit": RUN_CLOCKS_UNIT,
        }
        samples.append(sample)
        status_counts[status] += 1
        state = (raw_value, counter_epoch, generation, event.timestamp_us,
                 event.index)
        if declared is not None:
            explicit_state[state_key] = state
        else:
            inferred_state[event.thread_id] = state

    return {
        "semantics": {
            "source": RUN_CLOCKS_SOURCE,
            "source_storage_type": "SceKernelSysClock (uint64_t)",
            "source_storage_bits": 64,
            "capture_metric": BUILTIN_NAMES[RUN_CLOCKS_METRIC_ID],
            "kind": "cumulative_raw_counter",
            "unit": RUN_CLOCKS_UNIT,
            "effective_counter_bits": counter_bits,
            "cpu_utilization": False,
            "conversion_applied": False,
        },
        "policy": {
            "counter_bits": counter_bits,
            "max_wrap_delta_raw": max_wrap_delta,
            "wraps_require_explicit_counter_bits_and_delta_bound": True,
            "ambiguous_regressions": "reset_or_reuse",
            "unmapped_identity": "thread ID plus observed counter continuity",
        },
        "summary": {
            "samples": len(samples),
            "derived_deltas": sum(
                1 for sample in samples if sample["delta_raw"] is not None),
            "statuses": dict(sorted(status_counts.items())),
            "samples_missing_raw_flag": missing_raw_flags,
        },
        "samples": samples,
    }


def _format_table(rows: Sequence[Sequence[str]], headers: Sequence[str]) -> list[str]:
    all_rows = [tuple(headers), *(tuple(row) for row in rows)]
    widths = [max(len(row[index]) for row in all_rows)
              for index in range(len(headers))]
    result = ["  " + "  ".join(value.ljust(widths[index])
                                for index, value in enumerate(headers))]
    result.append("  " + "  ".join("-" * width for width in widths))
    result.extend("  " + "  ".join(value.ljust(widths[index])
                                    for index, value in enumerate(row))
                  for row in rows)
    return result


def render_summary(capture: TraceCapture) -> str:
    events = capture.events
    type_counts = collections.Counter(event.type_name for event in events)
    referenced_ids = {event.name_id for event in events if event.name_id}
    unresolved = sorted(referenced_ids - set(capture.names))
    threads = sorted({event.thread_id for event in events})
    if events:
        first_us = min(event.timestamp_us for event in events)
        last_us = max(event.timestamp_us for event in events)
        span_us = last_us - first_us
    else:
        first_us = last_us = span_us = 0
    zones = analyze_zones(capture)

    lines = [
        "VitaProfiler capture",
        f"  bytes: {capture.raw_size}",
        f"  stream start: {capture.header.stream_start_us} us",
        f"  events: {len(events)} over {span_us} us",
        f"  session complete: {'yes' if capture.complete else 'no'}",
        f"  dictionary: {len(capture.names)} names "
        f"({len(unresolved)} referenced IDs unresolved)",
        "  threads: " + (", ".join(f"0x{thread:08x}" for thread in threads)
                           if threads else "none"),
    ]

    if type_counts:
        lines.extend(["", "Event types:"])
        rows = [(name, str(count)) for name, count in sorted(type_counts.items())]
        lines.extend(_format_table(rows, ("type", "count")))

    if capture.loss is not None:
        lines.extend([
            "",
            "Loss counters:",
            f"  producer accepted: {capture.loss.producer_accepted}",
            f"  producer dropped: {capture.loss.producer_dropped}",
            f"  transport lost: {capture.loss.transport_lost}",
            f"  sink lost: {capture.loss.sink_lost}",
        ])

    if zones.spans:
        aggregate: dict[str, list[int]] = {}
        for span in zones.spans:
            values = aggregate.get(span.name)
            if values is None:
                aggregate[span.name] = [1, span.duration_us,
                                        span.duration_us, span.duration_us]
            else:
                values[0] += 1
                values[1] += span.duration_us
                values[2] = min(values[2], span.duration_us)
                values[3] = max(values[3], span.duration_us)
        rows = []
        for name, values in sorted(aggregate.items()):
            count, total, minimum, maximum = values
            rows.append((name, str(count), str(total),
                         f"{total / count:.2f}", str(minimum), str(maximum)))
        lines.extend(["", "Complete CPU zones (microseconds):"])
        lines.extend(_format_table(rows, ("name", "count", "total", "avg",
                                          "min", "max")))

    counters: dict[str, list[int]] = {}
    for event in events:
        if event.event_type == EVENT_COUNTER:
            name = capture.resolve_name(event.name_id)
            values = counters.get(name)
            if values is None:
                counters[name] = [1, event.value, event.value, event.value]
            else:
                values[0] += 1
                values[1] = event.value
                values[2] = min(values[2], event.value)
                values[3] = max(values[3], event.value)
    if counters:
        rows = []
        for name, values in sorted(counters.items()):
            count, latest, minimum, maximum = values
            rows.append((name, str(count), str(latest), str(minimum),
                         str(maximum)))
        lines.extend(["", "Counters:"])
        lines.extend(_format_table(rows, ("name", "samples", "last", "min",
                                          "max")))

    run_clocks = analyze_run_clocks(capture)
    run_clocks_summary = run_clocks["summary"]
    if run_clocks_summary["samples"]:
        statuses = run_clocks_summary["statuses"]
        lines.extend([
            "",
            "SceKernelThreadInfo.runClocks (raw counter; unit unknown):",
            f"  samples: {run_clocks_summary['samples']}",
            f"  safe raw deltas: {run_clocks_summary['derived_deltas']}",
            "  delta status: " + ", ".join(
                f"{name}={count}" for name, count in statuses.items()),
            "  CPU utilization: not derived",
        ])

    frame_count = 0
    frame_total = 0
    frame_minimum: int | None = None
    frame_maximum: int | None = None
    for event in events:
        if (event.event_type == EVENT_FRAME and
                not event.flags & EVENT_FLAG_FIRST and event.value > 0):
            frame_count += 1
            frame_total += event.value
            frame_minimum = (event.value if frame_minimum is None else
                             min(frame_minimum, event.value))
            frame_maximum = (event.value if frame_maximum is None else
                             max(frame_maximum, event.value))
    if frame_count:
        average = frame_total / frame_count
        fps = 1_000_000.0 / average if average else math.inf
        lines.extend(["", "Frames:",
                      f"  samples: {frame_count}",
                      f"  average: {average:.2f} us ({fps:.2f} FPS)",
                      f"  range: {frame_minimum}..{frame_maximum} us"])

    diagnostics = {
        "unmatched zone begins": len(zones.unmatched_begins),
        "unmatched zone ends": len(zones.unmatched_ends),
        "duplicate active correlation IDs": len(zones.duplicate_begins),
        "negative zone durations": len(zones.negative_durations),
        "unresolved referenced name IDs": len(unresolved),
    }
    if any(diagnostics.values()):
        lines.extend(["", "Diagnostics:"])
        lines.extend(f"  {name}: {value}" for name, value in diagnostics.items()
                     if value)
        if unresolved:
            lines.append("  unresolved IDs: " +
                         ", ".join(f"0x{value:08x}" for value in unresolved))
    return "\n".join(lines)


def render_events(capture: TraceCapture, limit: int | None = None) -> str:
    selected: Iterable[Event] = capture.events
    if limit is not None:
        selected = capture.events[:limit]
    lines = []
    for event in selected:
        flags = ",".join(event.flag_names) or "-"
        value = f"value={event.value}"
        if event.name_id == RUN_CLOCKS_METRIC_ID:
            value = (
                f"raw_u64={_run_clocks_raw_value(event)} "
                f"unit={RUN_CLOCKS_UNIT} source={RUN_CLOCKS_SOURCE}")
        lines.append(
            f"{event.index:6d} {event.timestamp_us:14d} us "
            f"tid=0x{event.thread_id:08x} {event.type_name:16s} "
            f"{capture.resolve_name(event.name_id)!r} {value} "
            f"corr={event.correlation_id} flags={flags}")
    if limit is not None and len(capture.events) > limit:
        lines.append(f"... {len(capture.events) - limit} more events")
    return "\n".join(lines)


def capture_to_json(capture: TraceCapture) -> dict[str, object]:
    if not capture.complete:
        raise TraceFormatError("cannot export an incomplete capture")
    thread_metadata = [
        {
            **dataclasses.asdict(item),
            "thread_id_hex": f"0x{item.thread_id:08x}",
            "identity_hex": (
                f"0x{item.identity:016x}" if item.identity is not None else
                None),
            "name": capture.resolve_name(item.name_id),
        }
        for item in capture.thread_identities
    ]
    module_metadata = [
        {
            **dataclasses.asdict(item),
            "address_start_hex": f"0x{item.address_start:08x}",
            "address_end_hex": f"0x{item.address_end:08x}",
            "name": capture.resolve_name(item.name_id),
        }
        for item in capture.module_ranges
    ]
    return {
        "format": ("vitaprofiler-decoded-v2"
                   if capture.header.version == STREAM_V2_VERSION else
                   "vitaprofiler-decoded-v1"),
        "header": dataclasses.asdict(capture.header),
        "complete": capture.complete,
        "session": (dataclasses.asdict(capture.session)
                    if capture.session is not None else None),
        "loss": (dataclasses.asdict(capture.loss)
                 if capture.loss is not None else None),
        "thread_identities": thread_metadata,
        "module_ranges": module_metadata,
        "dictionary_size": capture.dictionary_size,
        "raw_size": capture.raw_size,
        "names": [
            {
                "name_id": entry.name_id,
                "name_id_hex": f"0x{entry.name_id:08x}",
                "name": entry.name,
                "builtin": entry.builtin,
            }
            for entry in capture.names.values()
        ],
        "run_clocks_analysis": analyze_run_clocks(capture),
        "events": [
            {
                "index": event.index,
                "timestamp_us": event.timestamp_us,
                "value": event.value,
                "name_id": event.name_id,
                "name_id_hex": f"0x{event.name_id:08x}",
                "name": capture.resolve_name(event.name_id),
                "thread_id": event.thread_id,
                "correlation_id": event.correlation_id,
                "type": event.event_type,
                "type_name": event.type_name,
                "flags": event.flags,
                "flag_names": list(event.flag_names),
                **({
                    "raw_value_u64": _run_clocks_raw_value(event),
                    "raw_value_u64_decimal": str(
                        _run_clocks_raw_value(event)),
                    "raw_value_u64_hex": (
                        f"0x{_run_clocks_raw_value(event):016x}"),
                    "value_unit": RUN_CLOCKS_UNIT,
                    "value_source": RUN_CLOCKS_SOURCE,
                } if event.name_id == RUN_CLOCKS_METRIC_ID else {}),
            }
            for event in capture.events
        ],
    }


def capture_to_chrome_trace(capture: TraceCapture) -> dict[str, object]:
    """Return Chrome Trace Event JSON, also loadable by Perfetto."""
    if not capture.complete:
        raise TraceFormatError("cannot export an incomplete capture")
    process_id = (capture.session.process_id
                  if capture.session is not None and
                  capture.session.process_id is not None else 1)
    trace_events: list[dict[str, object]] = [{
        "name": "process_name", "ph": "M", "pid": process_id, "tid": 0,
        "args": {"name": "PS Vita application"},
    }]
    for identity in capture.thread_identities:
        trace_events.append({
            "name": "thread_name", "ph": "M", "pid": process_id,
            "tid": identity.thread_id,
            "args": {
                "name": capture.resolve_name(identity.name_id),
                "generation": identity.generation,
                "identity": identity.identity,
                "first_event_index": identity.first_event_index,
            },
        })
    zones = analyze_zones(capture)
    for span in zones.spans:
        trace_events.append({
            "name": span.name,
            "cat": "cpu.zone",
            "ph": "X",
            "pid": process_id,
            "tid": span.thread_id,
            "ts": span.begin_us,
            "dur": span.duration_us,
            "args": {
                "correlation_id": span.correlation_id,
                "end_timestamp_us": span.end_us,
                "flags": [name for bit, name in EVENT_FLAG_NAMES.items()
                          if span.flags & bit],
            },
        })

    unmatched = {event.index for event in zones.unmatched_begins}
    unmatched.update(event.index for event in zones.unmatched_ends)
    for event in capture.events:
        name = capture.resolve_name(event.name_id)
        base = {"name": name, "pid": process_id, "tid": event.thread_id,
                "ts": event.timestamp_us}
        if event.event_type in (EVENT_ZONE_BEGIN, EVENT_ZONE_END):
            if event.index not in unmatched:
                continue
            trace_events.append({
                **base, "cat": "diagnostic.unmatched_zone", "ph": "i",
                "s": "t", "args": {"record": event.type_name,
                                      "correlation_id": event.correlation_id},
            })
        elif event.event_type == EVENT_COUNTER:
            trace_events.append({
                **base, "cat": "counter", "ph": "C",
                "args": {"value": event.value},
            })
        elif event.event_type == EVENT_FRAME:
            trace_events.append({
                **base, "cat": "frame", "ph": "i", "s": "g",
                "args": {"duration_us": event.value,
                          "frame_sequence": event.correlation_id,
                          "first": bool(event.flags & EVENT_FLAG_FIRST)},
            })
        elif event.event_type in (EVENT_MEMORY_SAMPLE, EVENT_THREAD_SAMPLE,
                                  EVENT_PROCESS_SAMPLE):
            args: dict[str, object] = {
                "value": event.value,
                "raw": bool(event.flags & EVENT_FLAG_RAW_VALUE),
            }
            category = event.type_name
            if event.name_id == RUN_CLOCKS_METRIC_ID:
                category = "thread_sample.raw_unknown_unit"
                args = {
                    "raw_value_u64": _run_clocks_raw_value(event),
                    "raw_value_u64_decimal": str(
                        _run_clocks_raw_value(event)),
                    "raw_value_u64_hex": (
                        f"0x{_run_clocks_raw_value(event):016x}"),
                    "unit": RUN_CLOCKS_UNIT,
                    "source": RUN_CLOCKS_SOURCE,
                    "cpu_utilization": False,
                }
            trace_events.append({
                **base, "cat": category, "ph": "C", "args": args,
            })
        else:
            trace_events.append({
                **base, "cat": event.type_name, "ph": "i", "s": "t",
                "args": {"value": event.value,
                          "correlation_id": event.correlation_id,
                          "flags": list(event.flag_names)},
            })

    run_clocks = analyze_run_clocks(capture)
    for sample in run_clocks["samples"]:
        if sample["delta_raw"] is not None:
            trace_events.append({
                "name": "vita.thread.run_clocks.delta_raw",
                "cat": "thread_sample.raw_unknown_unit",
                "ph": "C",
                "pid": process_id,
                "tid": sample["thread_id"],
                "ts": sample["timestamp_us"],
                "args": {
                    "delta_raw": sample["delta_raw"],
                    "delta_raw_decimal": sample["delta_raw_decimal"],
                    "delta_raw_hex": sample["delta_raw_hex"],
                    "unit": RUN_CLOCKS_UNIT,
                    "source": RUN_CLOCKS_SOURCE,
                    "delta_status": sample["delta_status"],
                    "thread_generation": sample["thread_generation"],
                    "counter_epoch": sample["counter_epoch"],
                    "cpu_utilization": False,
                },
            })
        elif sample["delta_status"] == "reset_or_reuse":
            trace_events.append({
                "name": "vita.thread.run_clocks discontinuity",
                "cat": "diagnostic.run_clocks",
                "ph": "i",
                "s": "t",
                "pid": process_id,
                "tid": sample["thread_id"],
                "ts": sample["timestamp_us"],
                "args": {
                    "status": sample["delta_status"],
                    "unit": RUN_CLOCKS_UNIT,
                    "thread_generation": sample["thread_generation"],
                    "counter_epoch": sample["counter_epoch"],
                },
            })

    trace_events.sort(key=lambda item: (int(item.get("ts", -1)),
                                        str(item.get("ph", ""))))
    return {
        "traceEvents": trace_events,
        "displayTimeUnit": "ms",
        "metadata": {
            "source": "VitaProfiler",
            "wire_version": capture.header.version,
            "clock_hz": capture.header.clock_hz,
            "stream_start_us": capture.header.stream_start_us,
            "event_count": len(capture.events),
            "dictionary_entries": len(capture.names),
            "complete": capture.complete,
            "session": (dataclasses.asdict(capture.session)
                        if capture.session is not None else None),
            "loss": (dataclasses.asdict(capture.loss)
                     if capture.loss is not None else None),
            "thread_identities": [
                dataclasses.asdict(item)
                for item in capture.thread_identities
            ],
            "module_ranges": [
                dataclasses.asdict(item) for item in capture.module_ranges
            ],
            "unmatched_zone_begins": len(zones.unmatched_begins),
            "unmatched_zone_ends": len(zones.unmatched_ends),
            "run_clocks": run_clocks["semantics"],
            "run_clocks_summary": run_clocks["summary"],
        },
    }


def receive_socket(connection: socket.socket, max_bytes: int,
                   idle_timeout: float | None = None,
                   total_timeout: float | None = DEFAULT_CAPTURE_TIMEOUT,
                   cancelled: Callable[[], bool] | None = None,
                   on_progress: Callable[[int], None] | None = None,
                   on_chunk: Callable[[bytes], None] | None = None,
                   ) -> bytes:
    """Read one capture through EOF with hard memory and time bounds."""
    if max_bytes < WIRE_HEADER_SIZE:
        raise ValueError("max_bytes must fit at least one VPRF header")
    if idle_timeout is not None:
        if not math.isfinite(idle_timeout) or idle_timeout <= 0:
            raise ValueError("idle_timeout must be positive or None")
    if total_timeout is not None:
        if not math.isfinite(total_timeout) or total_timeout <= 0:
            raise ValueError("total_timeout must be positive or None")
    started = time.monotonic()
    deadline = started + total_timeout if total_timeout is not None else None
    idle_deadline = started + idle_timeout if idle_timeout is not None else None
    chunks: list[bytes] = []
    received = 0
    while True:
        if cancelled is not None and cancelled():
            raise TraceReceiveCancelled("capture cancelled")
        now = time.monotonic()
        remaining = None if deadline is None else deadline - now
        if remaining is not None and remaining <= 0:
            raise TraceReceiveError("capture exceeded its total time limit")
        idle_remaining = (None if idle_deadline is None else
                          idle_deadline - now)
        if idle_remaining is not None and idle_remaining <= 0:
            raise TraceReceiveError("capture connection became idle")
        timeout = idle_remaining
        if remaining is not None:
            timeout = remaining if timeout is None else min(timeout, remaining)
        if cancelled is not None:
            timeout = (RECEIVER_CANCEL_POLL_SECONDS if timeout is None else
                       min(timeout, RECEIVER_CANCEL_POLL_SECONDS))
        connection.settimeout(timeout)
        try:
            chunk = connection.recv(min(64 * 1024, max_bytes - received + 1))
        except socket.timeout as error:
            if cancelled is not None and cancelled():
                raise TraceReceiveCancelled("capture cancelled") from error
            if deadline is not None and time.monotonic() >= deadline:
                raise TraceReceiveError(
                    "capture exceeded its total time limit") from error
            if (idle_deadline is not None and
                    time.monotonic() >= idle_deadline):
                raise TraceReceiveError(
                    "capture connection became idle") from error
            continue
        except OSError as error:
            raise TraceReceiveError(f"capture receive failed: {error}") from error
        if not chunk:
            break
        received += len(chunk)
        if received > max_bytes:
            raise TraceReceiveError(
                f"capture exceeded the {max_bytes}-byte safety limit")
        chunks.append(chunk)
        if on_chunk is not None:
            on_chunk(chunk)
        if idle_timeout is not None:
            idle_deadline = time.monotonic() + idle_timeout
        if on_progress is not None:
            on_progress(received)
    return b"".join(chunks)


def receive_tcp_once(bind: str, port: int, source: str | None,
                     max_bytes: int, accept_timeout: float | None,
                     idle_timeout: float | None,
                     on_listening: Callable[[tuple[str, int]], None] | None = None,
                     capture_timeout: float | None = DEFAULT_CAPTURE_TIMEOUT,
                     cancelled: Callable[[], bool] | None = None,
                     on_progress: Callable[[int], None] | None = None,
                     on_chunk: Callable[[bytes], None] | None = None,
                     ) -> tuple[bytes, tuple[str, int]]:
    """Accept one allowlisted IPv4 sender and read until its clean EOF."""
    if accept_timeout is not None:
        if not math.isfinite(accept_timeout) or accept_timeout <= 0:
            raise ValueError("accept_timeout must be positive or None")
    allowed = str(ipaddress.IPv4Address(source)) if source else None
    deadline = (time.monotonic() + accept_timeout
                if accept_timeout is not None else None)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((bind, port))
        listener.listen(1)
        if on_listening:
            host, actual_port = listener.getsockname()[:2]
            on_listening((str(host), int(actual_port)))
        while True:
            if cancelled is not None and cancelled():
                raise TraceReceiveCancelled("capture cancelled")
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TraceReceiveError("timed out waiting for a sender")
                timeout = remaining
            else:
                timeout = None
            if cancelled is not None:
                timeout = (RECEIVER_CANCEL_POLL_SECONDS if timeout is None else
                           min(timeout, RECEIVER_CANCEL_POLL_SECONDS))
            listener.settimeout(timeout)
            try:
                connection, address = listener.accept()
            except socket.timeout as error:
                if cancelled is not None and cancelled():
                    raise TraceReceiveCancelled(
                        "capture cancelled") from error
                if deadline is not None and time.monotonic() >= deadline:
                    raise TraceReceiveError(
                        "timed out waiting for a sender") from error
                continue
            peer = (str(address[0]), int(address[1]))
            if allowed is not None and peer[0] != allowed:
                connection.close()
                continue
            with connection:
                return receive_socket(connection, max_bytes, idle_timeout,
                                      capture_timeout, cancelled,
                                      on_progress, on_chunk), peer


def _metadata_text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value or len(value) > MAX_METADATA_TEXT:
        raise TraceFormatError(
            f"runClocks metadata {field} must be 1..{MAX_METADATA_TEXT} chars")
    if any(
            unicodedata.category(character) in {"Cc", "Cf", "Cs", "Zl", "Zp"} or
            unicodedata.bidirectional(character) in _UNSAFE_BIDI_CLASSES
            for character in value):
        raise TraceFormatError(
            f"runClocks metadata {field} contains unsafe controls")
    return value


def _metadata_positive_int(value: object, field: str) -> int:
    if (isinstance(value, bool) or not isinstance(value, int) or value <= 0 or
            value > 0xFFFFFFFFFFFFFFFF):
        raise TraceFormatError(
            f"runClocks metadata {field} must be a positive uint64")
    return value


def _metadata_nonnegative_int(value: object, field: str) -> int:
    if (isinstance(value, bool) or not isinstance(value, int) or value < 0 or
            value > 0xFFFFFFFFFFFFFFFF):
        raise TraceFormatError(
            f"runClocks metadata {field} must be a nonnegative uint64")
    return value


def _metadata_thread_id(value: object, field: str) -> int:
    try:
        parsed = int(value, 0) if isinstance(value, str) else value
    except ValueError as error:
        raise TraceFormatError(
            f"runClocks metadata {field} is not a thread ID") from error
    if (isinstance(parsed, bool) or not isinstance(parsed, int) or
            not 0 <= parsed <= 0xFFFFFFFF):
        raise TraceFormatError(
            f"runClocks metadata {field} must fit uint32")
    return parsed


def decode_run_clocks_experiment(data: bytes) -> RunClocksExperiment:
    if len(data) > MAX_EXPERIMENT_METADATA_BYTES:
        raise TraceFormatError(
            "runClocks experiment metadata exceeds the 65536-byte limit")
    try:
        decoded = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise TraceFormatError(
            "runClocks experiment metadata is not valid UTF-8 JSON") from error
    if not isinstance(decoded, dict):
        raise TraceFormatError("runClocks experiment metadata must be an object")

    required = {
        "format", "experiment_id", "captured_at_utc", "device_model",
        "firmware", "title_id", "build_id", "workload", "clock_profile",
        "power_state", "sample_interval_us", "sample_count_limit",
        "capture_duration_limit_us", "producer_dropped_events",
        "sink_lost_events", "threads", "counter_bits", "max_wrap_delta_raw",
    }
    allowed = required | {"notes"}
    missing = required - set(decoded)
    unknown = set(decoded) - allowed
    if missing:
        raise TraceFormatError(
            "runClocks metadata is missing: " + ", ".join(sorted(missing)))
    if unknown:
        raise TraceFormatError(
            "runClocks metadata has unknown fields: " +
            ", ".join(sorted(unknown)))
    if decoded["format"] != RUN_CLOCKS_EXPERIMENT_FORMAT:
        raise TraceFormatError("unsupported runClocks experiment format")

    normalized: dict[str, object] = {
        "format": RUN_CLOCKS_EXPERIMENT_FORMAT,
    }
    for field in (
            "experiment_id", "captured_at_utc", "device_model", "firmware",
            "title_id", "build_id", "workload", "clock_profile",
            "power_state"):
        normalized[field] = _metadata_text(decoded[field], field)
    captured_at = str(normalized["captured_at_utc"])
    if not captured_at.endswith("Z"):
        raise TraceFormatError(
            "runClocks metadata captured_at_utc must end in Z")
    try:
        datetime.datetime.fromisoformat(captured_at[:-1] + "+00:00")
    except ValueError as error:
        raise TraceFormatError(
            "runClocks metadata captured_at_utc is not ISO 8601") from error
    normalized["sample_interval_us"] = _metadata_positive_int(
        decoded["sample_interval_us"], "sample_interval_us")
    sample_count_limit = _metadata_positive_int(
        decoded["sample_count_limit"], "sample_count_limit")
    if not 2 <= sample_count_limit <= MAX_DECODED_EVENTS:
        raise TraceFormatError(
            "runClocks metadata sample_count_limit must be 2..262144")
    normalized["sample_count_limit"] = sample_count_limit
    normalized["capture_duration_limit_us"] = _metadata_positive_int(
        decoded["capture_duration_limit_us"], "capture_duration_limit_us")
    if (normalized["sample_interval_us"] >
            normalized["capture_duration_limit_us"]):
        raise TraceFormatError(
            "runClocks metadata sample interval exceeds duration limit")
    normalized["producer_dropped_events"] = _metadata_nonnegative_int(
        decoded["producer_dropped_events"], "producer_dropped_events")
    normalized["sink_lost_events"] = _metadata_nonnegative_int(
        decoded["sink_lost_events"], "sink_lost_events")

    counter_bits = decoded["counter_bits"]
    if counter_bits not in (None, 32, 64):
        raise TraceFormatError(
            "runClocks metadata counter_bits must be null, 32, or 64")
    max_wrap_delta = decoded["max_wrap_delta_raw"]
    if max_wrap_delta is not None:
        max_wrap_delta = _metadata_positive_int(
            max_wrap_delta, "max_wrap_delta_raw")
        if counter_bits is None or max_wrap_delta >= (1 << counter_bits):
            raise TraceFormatError(
                "max_wrap_delta_raw requires counter_bits and must be "
                "smaller than its modulus")
    normalized["counter_bits"] = counter_bits
    normalized["max_wrap_delta_raw"] = max_wrap_delta

    raw_threads = decoded["threads"]
    if (not isinstance(raw_threads, list) or
            not 1 <= len(raw_threads) <= MAX_EXPERIMENT_THREADS):
        raise TraceFormatError(
            "runClocks metadata threads must contain 1..64 entries")
    threads: list[ThreadGeneration] = []
    normalized_threads: list[dict[str, object]] = []
    for index, raw_thread in enumerate(raw_threads):
        if not isinstance(raw_thread, dict):
            raise TraceFormatError(
                f"runClocks metadata threads[{index}] must be an object")
        expected = {
            "thread_id", "generation", "label", "first_event_index",
            "last_event_index",
        }
        if set(raw_thread) != expected:
            raise TraceFormatError(
                f"runClocks metadata threads[{index}] fields must be: " +
                ", ".join(sorted(expected)))
        thread_id = _metadata_thread_id(
            raw_thread["thread_id"], f"threads[{index}].thread_id")
        generation = raw_thread["generation"]
        first = raw_thread["first_event_index"]
        last = raw_thread["last_event_index"]
        if (isinstance(generation, bool) or not isinstance(generation, int) or
                generation < 0):
            raise TraceFormatError(
                f"runClocks metadata threads[{index}].generation must be "
                "nonnegative")
        if (isinstance(first, bool) or not isinstance(first, int) or first < 0 or
                isinstance(last, bool) or not isinstance(last, int) or
                last < first):
            raise TraceFormatError(
                f"runClocks metadata threads[{index}] has an invalid "
                "event range")
        label = _metadata_text(
            raw_thread["label"], f"threads[{index}].label")
        thread = ThreadGeneration(thread_id, generation, label, first, last)
        threads.append(thread)
        normalized_threads.append({
            "thread_id": thread_id,
            "thread_id_hex": f"0x{thread_id:08x}",
            "generation": generation,
            "label": label,
            "first_event_index": first,
            "last_event_index": last,
        })
    for index, thread in enumerate(threads):
        for other in threads[index + 1:]:
            if thread.thread_id != other.thread_id:
                continue
            if thread.generation == other.generation:
                raise TraceFormatError(
                    "runClocks metadata repeats generation "
                    f"{thread.generation} for thread 0x{thread.thread_id:08x}")
            if (thread.first_event_index <= other.last_event_index and
                    other.first_event_index <= thread.last_event_index):
                raise TraceFormatError(
                    "runClocks metadata has overlapping ranges for thread "
                    f"0x{thread.thread_id:08x}")
    normalized["threads"] = normalized_threads
    if "notes" in decoded:
        normalized["notes"] = _metadata_text(decoded["notes"], "notes")
    return RunClocksExperiment(
        normalized, counter_bits, max_wrap_delta, tuple(threads))


def run_clocks_characterization_report(
        capture: TraceCapture,
        capture_data: bytes,
        experiment: RunClocksExperiment,
        ) -> dict[str, object]:
    analysis = analyze_run_clocks(capture, experiment)
    samples = analysis["samples"]
    summary = analysis["summary"]
    if summary["samples_missing_raw_flag"]:
        raise TraceFormatError(
            "runClocks characterization requires raw_value flags")
    if (experiment.metadata["producer_dropped_events"] or
            experiment.metadata["sink_lost_events"]):
        raise TraceFormatError(
            "runClocks characterization requires zero producer and sink loss")
    if len(samples) < 2:
        raise TraceFormatError(
            "runClocks characterization requires at least two samples")
    limit = int(experiment.metadata["sample_count_limit"])
    if len(samples) > limit:
        raise TraceFormatError(
            f"capture has {len(samples)} runClocks samples; metadata limit "
            f"is {limit}")
    unmapped = [
        sample["event_index"] for sample in samples
        if sample["identity_source"] != "experiment_metadata"
    ]
    if unmapped:
        raise TraceFormatError(
            "runClocks metadata does not identify sample events: " +
            ", ".join(str(index) for index in unmapped))
    run_clock_events = {
        int(sample["event_index"]): int(sample["thread_id"])
        for sample in samples
    }
    for thread in experiment.threads:
        if thread.last_event_index >= len(capture.events):
            raise TraceFormatError(
                f"thread generation {thread.label!r} extends beyond capture")
        if not any(
                thread.first_event_index <= index <= thread.last_event_index and
                thread_id == thread.thread_id
                for index, thread_id in run_clock_events.items()):
            raise TraceFormatError(
                f"thread generation {thread.label!r} contains no runClocks "
                "sample")
    if summary["statuses"].get("timestamp_regression", 0):
        raise TraceFormatError(
            "runClocks characterization has a timestamp regression")
    timestamps = [int(sample["timestamp_us"]) for sample in samples]
    duration_us = max(timestamps) - min(timestamps)
    duration_limit = int(
        experiment.metadata["capture_duration_limit_us"])
    if duration_us < 0 or duration_us > duration_limit:
        raise TraceFormatError(
            f"runClocks sample span {duration_us} us exceeds metadata limit "
            f"{duration_limit} us")
    intervals = [
        int(sample["elapsed_us"]) for sample in samples
        if sample["elapsed_us"] is not None
    ]
    return {
        "format": RUN_CLOCKS_REPORT_FORMAT,
        "provenance": {
            "capture_sha256": hashlib.sha256(capture_data).hexdigest(),
            "capture_bytes": len(capture_data),
            "wire_version": capture.header.version,
            "source_api": "sceKernelGetThreadInfo",
            "source_field": RUN_CLOCKS_SOURCE,
            "source_storage_type": "SceKernelSysClock (uint64_t)",
            "capture_metric": BUILTIN_NAMES[RUN_CLOCKS_METRIC_ID],
        },
        "experiment": experiment.metadata,
        "observed": {
            "first_timestamp_us": min(timestamps),
            "last_timestamp_us": max(timestamps),
            "sample_span_us": duration_us,
            "sample_count": len(samples),
            "interval_min_us": min(intervals) if intervals else None,
            "interval_max_us": max(intervals) if intervals else None,
        },
        "run_clocks_analysis": analysis,
    }


def _read_bounded_bytes(path: Path, max_bytes: int) -> bytes:
    if max_bytes < 1:
        raise ValueError("max_bytes must be positive")
    with path.open("rb") as capture_file:
        data = capture_file.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise TraceFormatError(
            f"capture exceeds the {max_bytes}-byte safety limit")
    return data


def _read_capture(path: Path, max_bytes: int = DEFAULT_MAX_BYTES) -> TraceCapture:
    return decode_capture(_read_bounded_bytes(path, max_bytes))


def _write_atomic(path: Path, data: bytes, force: bool = False) -> None:
    if path.exists() and not force:
        raise FileExistsError(f"refusing to overwrite {path}; pass --force")
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".part", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        if force:
            os.replace(temporary, path)
        else:
            try:
                # The temporary file is in the destination directory, so this
                # publishes a complete file atomically and fails if another
                # process wins the destination-name race.
                os.link(temporary, path)
            except FileExistsError as error:
                raise FileExistsError(
                    f"refusing to overwrite {path}; pass --force") from error
            temporary.unlink()
    except BaseException:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def _json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=False, ensure_ascii=False) +
            "\n").encode("utf-8")


def read_capture(path: Path, max_bytes: int = DEFAULT_MAX_BYTES) -> TraceCapture:
    """Read and validate one bounded capture file."""
    return _read_capture(path, max_bytes)


def write_capture(path: Path, data: bytes, force: bool = False) -> None:
    """Atomically save an already validated raw capture."""
    _write_atomic(path, data, force)


def export_decoded_json(capture: TraceCapture, path: Path,
                        force: bool = False) -> None:
    """Atomically export the complete decoded representation."""
    _write_atomic(path, _json_bytes(capture_to_json(capture)), force)


def export_chrome_trace(capture: TraceCapture, path: Path,
                        force: bool = False) -> None:
    """Atomically export Chrome Trace Event JSON accepted by Perfetto."""
    _write_atomic(path, _json_bytes(capture_to_chrome_trace(capture)), force)


def _positive_int(value: str) -> int:
    parsed = int(value, 10)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def _port(value: str) -> int:
    parsed = int(value, 10)
    if not 1 <= parsed <= 65535:
        raise argparse.ArgumentTypeError("must be between 1 and 65535")
    return parsed


def _timeout(value: str) -> float | None:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed < 0:
        raise argparse.ArgumentTypeError("must be zero or positive")
    return parsed or None


def _positive_timeout(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    view = subparsers.add_parser("view", help="print a capture summary")
    view.add_argument("capture", type=Path)
    view.add_argument("--events", type=int, default=0,
                      help="also print this many decoded events (-1 for all)")
    view.add_argument("--max-bytes", type=_positive_int,
                      default=DEFAULT_MAX_BYTES)

    decoded = subparsers.add_parser("json", help="export complete decoded JSON")
    decoded.add_argument("capture", type=Path)
    decoded.add_argument("output", type=Path)
    decoded.add_argument("--force", action="store_true")
    decoded.add_argument("--max-bytes", type=_positive_int,
                         default=DEFAULT_MAX_BYTES)

    chrome = subparsers.add_parser(
        "chrome", help="export Chrome Trace/Perfetto JSON")
    chrome.add_argument("capture", type=Path)
    chrome.add_argument("output", type=Path)
    chrome.add_argument("--force", action="store_true")
    chrome.add_argument("--max-bytes", type=_positive_int,
                        default=DEFAULT_MAX_BYTES)

    run_clocks = subparsers.add_parser(
        "runclocks",
        help="export a bounded raw runClocks characterization report")
    run_clocks.add_argument("capture", type=Path)
    run_clocks.add_argument("metadata", type=Path)
    run_clocks.add_argument("output", type=Path)
    run_clocks.add_argument("--force", action="store_true")
    run_clocks.add_argument("--max-bytes", type=_positive_int,
                            default=DEFAULT_MAX_BYTES)

    receive = subparsers.add_parser(
        "receive", help="receive one TCP capture through clean sender EOF")
    receive.add_argument("output", type=Path)
    receive.add_argument("--bind", default="0.0.0.0")
    receive.add_argument("--port", type=_port, default=DEFAULT_PORT)
    receive.add_argument("--source",
                         help="accept only this Vita IPv4 address")
    receive.add_argument("--max-bytes", type=_positive_int,
                         default=DEFAULT_MAX_BYTES)
    receive.add_argument("--accept-timeout", type=_timeout,
                         default=DEFAULT_ACCEPT_TIMEOUT,
                         help="seconds; zero waits indefinitely")
    receive.add_argument("--idle-timeout", type=_timeout,
                         default=DEFAULT_IDLE_TIMEOUT,
                         help="seconds between bytes; zero waits indefinitely")
    receive.add_argument("--capture-timeout", type=_positive_timeout,
                         default=DEFAULT_CAPTURE_TIMEOUT,
                         help="absolute seconds allowed after accepting a sender")
    receive.add_argument("--force", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "view":
            capture = _read_capture(args.capture, args.max_bytes)
            print(render_summary(capture))
            if args.events:
                if args.events < -1:
                    parser.error("--events must be -1 or nonnegative")
                print("\nEvents:")
                print(render_events(capture,
                                    None if args.events == -1 else args.events))
            return 0

        if args.command in ("json", "chrome"):
            capture = _read_capture(args.capture, args.max_bytes)
            if args.command == "json":
                export_decoded_json(capture, args.output, args.force)
            else:
                export_chrome_trace(capture, args.output, args.force)
            print(f"Wrote {args.output} ({len(capture.events)} events)")
            return 0

        if args.command == "runclocks":
            capture_data = _read_bounded_bytes(args.capture, args.max_bytes)
            capture = decode_capture(capture_data)
            metadata_data = _read_bounded_bytes(
                args.metadata, MAX_EXPERIMENT_METADATA_BYTES)
            experiment = decode_run_clocks_experiment(metadata_data)
            report = run_clocks_characterization_report(
                capture, capture_data, experiment)
            _write_atomic(args.output, _json_bytes(report), args.force)
            sample_count = report["run_clocks_analysis"]["summary"]["samples"]
            print(
                f"Wrote {args.output} ({sample_count} raw runClocks samples; "
                "unit unknown)")
            return 0

        if args.command == "receive":
            if args.output.exists() and not args.force:
                raise FileExistsError(
                    f"refusing to overwrite {args.output}; pass --force")

            def announce(address: tuple[str, int]) -> None:
                print(f"Listening for one VitaProfiler TCP capture on "
                      f"{address[0]}:{address[1]}", flush=True)

            raw, peer = receive_tcp_once(
                args.bind, args.port, args.source, args.max_bytes,
                args.accept_timeout, args.idle_timeout, announce,
                args.capture_timeout)
            capture = decode_capture(raw)
            _write_atomic(args.output, raw, args.force)
            print(f"Accepted {len(raw)} bytes from {peer[0]}:{peer[1]}")
            print(f"Saved validated capture to {args.output}\n")
            print(render_summary(capture))
            return 0
        parser.error("unknown command")
    except (FileNotFoundError, FileExistsError, OSError, TraceFormatError,
            TraceReceiveError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

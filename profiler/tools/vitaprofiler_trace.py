#!/usr/bin/env python3
"""Receive, validate, inspect, and export VitaProfiler binary captures."""

from __future__ import annotations

import argparse
import collections
import dataclasses
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
from pathlib import Path
from typing import Callable, Iterable, Sequence


WIRE_MAGIC = 0x46525056
WIRE_VERSION = 1
WIRE_HEADER_SIZE = 32
WIRE_EVENT_SIZE = 32
WIRE_FLAGS = 1
WIRE_CLOCK_HZ = 1_000_000

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

DEFAULT_PORT = 18195
DEFAULT_MAX_BYTES = 16 * 1024 * 1024
MAX_DECODED_EVENTS = 262_144
DEFAULT_ACCEPT_TIMEOUT = 30.0
DEFAULT_IDLE_TIMEOUT = 5.0
DEFAULT_CAPTURE_TIMEOUT = 300.0
RECEIVER_CANCEL_POLL_SECONDS = 0.1

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


@dataclasses.dataclass(frozen=True)
class ZoneAnalysis:
    spans: tuple[ZoneSpan, ...]
    unmatched_begins: tuple[Event, ...]
    unmatched_ends: tuple[Event, ...]
    duplicate_begins: tuple[Event, ...]
    negative_durations: tuple[Event, ...]


_UNSAFE_BIDI_CLASSES = frozenset({
    "BN", "LRE", "RLE", "LRO", "RLO", "PDF", "LRI", "RLI", "FSI",
    "PDI",
})


def _decode_safe_name(raw_name: bytes, index: int) -> str:
    try:
        name = raw_name.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise TraceFormatError(
            f"VPNM entry {index} is not valid UTF-8") from error
    for character in name:
        if (unicodedata.category(character) in {"Cc", "Cf", "Cs", "Zl", "Zp"} or
                unicodedata.bidirectional(character) in _UNSAFE_BIDI_CLASSES):
            raise TraceFormatError(
                f"VPNM entry {index} contains unsafe display controls")
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


def decode_capture(data: bytes) -> TraceCapture:
    """Decode one optional VPNM block plus one complete VPRF-to-EOF block."""
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
        ))

    unmatched_begins = tuple(
        event for candidates in active.values() for event in candidates)
    return ZoneAnalysis(tuple(spans), unmatched_begins,
                        tuple(unmatched_ends), tuple(duplicate_begins),
                        tuple(negative_durations))


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
        f"  dictionary: {len(capture.names)} names "
        f"({len(unresolved)} referenced IDs unresolved)",
        "  threads: " + (", ".join(f"0x{thread:08x}" for thread in threads)
                           if threads else "none"),
    ]

    if type_counts:
        lines.extend(["", "Event types:"])
        rows = [(name, str(count)) for name, count in sorted(type_counts.items())]
        lines.extend(_format_table(rows, ("type", "count")))

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
        lines.append(
            f"{event.index:6d} {event.timestamp_us:14d} us "
            f"tid=0x{event.thread_id:08x} {event.type_name:16s} "
            f"{capture.resolve_name(event.name_id)!r} value={event.value} "
            f"corr={event.correlation_id} flags={flags}")
    if limit is not None and len(capture.events) > limit:
        lines.append(f"... {len(capture.events) - limit} more events")
    return "\n".join(lines)


def capture_to_json(capture: TraceCapture) -> dict[str, object]:
    return {
        "format": "vitaprofiler-decoded-v1",
        "header": dataclasses.asdict(capture.header),
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
            }
            for event in capture.events
        ],
    }


def capture_to_chrome_trace(capture: TraceCapture) -> dict[str, object]:
    """Return Chrome Trace Event JSON, also loadable by Perfetto."""
    trace_events: list[dict[str, object]] = [{
        "name": "process_name", "ph": "M", "pid": 1, "tid": 0,
        "args": {"name": "PS Vita application"},
    }]
    zones = analyze_zones(capture)
    for span in zones.spans:
        trace_events.append({
            "name": span.name,
            "cat": "cpu.zone",
            "ph": "X",
            "pid": 1,
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
        base = {"name": name, "pid": 1, "tid": event.thread_id,
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
            trace_events.append({
                **base, "cat": event.type_name, "ph": "C",
                "args": {"value": event.value,
                          "raw": bool(event.flags & EVENT_FLAG_RAW_VALUE)},
            })
        else:
            trace_events.append({
                **base, "cat": event.type_name, "ph": "i", "s": "t",
                "args": {"value": event.value,
                          "correlation_id": event.correlation_id,
                          "flags": list(event.flag_names)},
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
            "unmatched_zone_begins": len(zones.unmatched_begins),
            "unmatched_zone_ends": len(zones.unmatched_ends),
        },
    }


def receive_socket(connection: socket.socket, max_bytes: int,
                   idle_timeout: float | None = None,
                   total_timeout: float | None = DEFAULT_CAPTURE_TIMEOUT,
                   cancelled: Callable[[], bool] | None = None,
                   on_progress: Callable[[int], None] | None = None,
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
                                      on_progress), peer


def _read_capture(path: Path, max_bytes: int = DEFAULT_MAX_BYTES) -> TraceCapture:
    if max_bytes < 1:
        raise ValueError("max_bytes must be positive")
    with path.open("rb") as capture_file:
        data = capture_file.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise TraceFormatError(
            f"capture exceeds the {max_bytes}-byte safety limit")
    return decode_capture(data)


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

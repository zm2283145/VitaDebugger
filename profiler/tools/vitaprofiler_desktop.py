"""Non-visual model and controller for the VitaProfiler desktop viewer."""

from __future__ import annotations

import dataclasses
import queue
import threading
import time
from pathlib import Path
from typing import Callable, Generic, Iterable, TypeVar

import vitaprofiler_trace as trace


SAMPLED_EVENT_TYPES = frozenset({
    trace.EVENT_COUNTER,
    trace.EVENT_MEMORY_SAMPLE,
    trace.EVENT_THREAD_SAMPLE,
    trace.EVENT_PROCESS_SAMPLE,
})
Row = TypeVar("Row")


@dataclasses.dataclass(frozen=True)
class FrameRow:
    event_index: int
    timestamp_us: int
    duration_us: int | None
    fps: float | None
    name: str
    thread_id: int
    thread_generation: int | None
    sequence: int
    flags: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class CounterRow:
    event_index: int
    timestamp_us: int
    name: str
    value: int
    sample_type: str
    thread_id: int
    thread_generation: int | None
    flags: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class TimelineRow:
    kind: str
    name: str
    thread_id: int
    thread_generation: int | None
    begin_us: int
    end_us: int
    duration_us: int
    flags: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class ReceiveConfig:
    output: Path
    bind: str = "0.0.0.0"
    port: int = trace.DEFAULT_PORT
    source: str | None = None
    max_bytes: int = trace.DEFAULT_MAX_BYTES
    accept_timeout: float | None = trace.DEFAULT_ACCEPT_TIMEOUT
    idle_timeout: float | None = trace.DEFAULT_IDLE_TIMEOUT
    capture_timeout: float = trace.DEFAULT_CAPTURE_TIMEOUT
    force: bool = False


@dataclasses.dataclass(frozen=True)
class LoadedCapture:
    capture: trace.TraceCapture
    source: str
    peer: tuple[str, int] | None
    view_model: "ProfilerViewModel"


@dataclasses.dataclass(frozen=True)
class FilteredRows(Generic[Row]):
    rows: tuple[Row, ...]
    truncated: bool


@dataclasses.dataclass(frozen=True)
class TableRows:
    timeline: FilteredRows[TimelineRow]
    frames: FilteredRows[FrameRow]
    zones: FilteredRows[trace.ZoneSpan]
    counters: FilteredRows[CounterRow]
    events: FilteredRows[trace.Event]


class ProfilerViewModel:
    """Prepared capture data with deterministic filtering for the GUI."""

    def __init__(self, capture: trace.TraceCapture, source: str,
                 peer: tuple[str, int] | None = None) -> None:
        self.capture = capture
        self.source = source
        self.peer = peer
        self.zone_analysis = trace.analyze_zones(capture)
        self.zones = self.zone_analysis.spans
        self.frames = tuple(self._frame_row(event) for event in capture.events
                            if event.event_type == trace.EVENT_FRAME)
        self.counters = tuple(
            CounterRow(
                event_index=event.index,
                timestamp_us=event.timestamp_us,
                name=capture.resolve_name(event.name_id),
                value=event.value,
                sample_type=event.type_name,
                thread_id=event.thread_id,
                thread_generation=self._thread_generation(event),
                flags=event.flag_names,
            )
            for event in capture.events
            if event.event_type in SAMPLED_EVENT_TYPES
        )
        self.threads = tuple(sorted({
            event.thread_id for event in capture.events
        }))
        timeline = [
            TimelineRow(
                "zone", span.name, span.thread_id,
                self._thread_generation_for_index(
                    span.thread_id, span.begin_event_index),
                span.begin_us, span.end_us, span.duration_us,
                tuple(name for bit, name in trace.EVENT_FLAG_NAMES.items()
                      if span.flags & bit))
            for span in self.zones
        ]
        timeline.extend(
            TimelineRow(
                "frame", frame.name, frame.thread_id,
                frame.thread_generation,
                frame.timestamp_us - (frame.duration_us or 0),
                frame.timestamp_us, frame.duration_us or 0, frame.flags)
            for frame in self.frames
        )
        self.timeline = tuple(sorted(
            timeline, key=lambda item: (item.begin_us, item.kind, item.name)))

    def _thread_generation(self, event: trace.Event) -> int | None:
        return self._thread_generation_for_index(event.thread_id, event.index)

    def _thread_generation_for_index(
            self, thread_id: int, event_index: int) -> int | None:
        candidates = [
            item for item in self.capture.thread_identities
            if item.thread_id == thread_id and
            item.first_event_index <= event_index
        ]
        if not candidates:
            return None
        return candidates[-1].generation

    def _frame_row(self, event: trace.Event) -> FrameRow:
        duration = (None if event.flags & trace.EVENT_FLAG_FIRST or
                    event.value <= 0 else event.value)
        return FrameRow(
            event_index=event.index,
            timestamp_us=event.timestamp_us,
            duration_us=duration,
            fps=(1_000_000.0 / duration if duration else None),
            name=self.capture.resolve_name(event.name_id),
            thread_id=event.thread_id,
            thread_generation=self._thread_generation(event),
            sequence=event.correlation_id,
            flags=event.flag_names,
        )

    @property
    def duration_us(self) -> int:
        if not self.capture.events:
            return 0
        timestamps = [event.timestamp_us for event in self.capture.events]
        return max(timestamps) - min(timestamps)

    @property
    def unresolved_name_ids(self) -> tuple[int, ...]:
        referenced = {
            event.name_id for event in self.capture.events if event.name_id
        }
        return tuple(sorted(referenced - set(self.capture.names)))

    @property
    def clock_regressions(self) -> int:
        return sum(
            bool(event.flags & trace.EVENT_FLAG_CLOCK_REGRESSION)
            for event in self.capture.events
        )

    def metadata_rows(self) -> tuple[tuple[str, str], ...]:
        peer = (f"{self.peer[0]}:{self.peer[1]}" if self.peer is not None else
                "local file")
        session = self.capture.session
        if session is None:
            timer_source = "Unavailable (not encoded by VPRF v1)"
            timer_unit = "Unavailable (timestamps are normalized)"
            session_identity = "Unavailable (not encoded by VPRF v1)"
            process_identity = "Unavailable (not encoded by VPRF v1)"
            thread_generations = "Unavailable (only thread IDs encoded)"
            module_ranges = "Unavailable (not encoded by VPRF v1)"
            execution_state = "Unavailable (not encoded by VPRF v1)"
        else:
            timer_source = session.timer_source or "Unavailable (not supplied)"
            timer_unit = session.timer_unit or "Unavailable (not supplied)"
            session_identity = f"0x{session.session_id:016x}"
            process_parts = []
            if session.process_id is not None:
                process_parts.append(str(session.process_id))
            if session.process_identity is not None:
                process_parts.append(f"0x{session.process_identity:016x}")
            process_identity = ", ".join(process_parts) or \
                "Unavailable (not supplied)"
            thread_generations = str(len(self.capture.thread_identities))
            module_ranges = str(len(self.capture.module_ranges))
            states = []
            if any(item.arm for item in self.capture.module_ranges):
                states.append("ARM")
            if any(item.thumb for item in self.capture.module_ranges):
                states.append("Thumb")
            execution_state = ", ".join(states) or \
                "Unavailable (not supplied)"
        return (
            ("Source", self.source),
            ("Peer", peer),
            ("Wire version", str(self.capture.header.version)),
            ("Timestamp frequency", f"{self.capture.header.clock_hz:,} Hz"),
            ("Timestamp unit", "microseconds"),
            ("Stream start", f"{self.capture.header.stream_start_us:,} us"),
            ("Session state",
             "Complete" if self.capture.complete else "LIVE / INCOMPLETE"),
            ("Raw timer source", timer_source),
            ("Raw timer unit", timer_unit),
            ("Session/process ID",
             f"{session_identity}; process {process_identity}"
             if session is not None else session_identity),
            ("Thread generations", thread_generations),
            ("Module ranges", module_ranges),
            ("ARM/Thumb state", execution_state),
            ("Capture bytes", f"{self.capture.raw_size:,}"),
            ("Capture span", f"{self.duration_us:,} us"),
            ("Events", f"{len(self.capture.events):,}"),
            ("Dictionary names", f"{len(self.capture.names):,}"),
            ("Threads", f"{len(self.threads):,}"),
        )

    def loss_rows(self) -> tuple[tuple[str, str], ...]:
        analysis = self.zone_analysis
        observable = (
            len(analysis.unmatched_begins) +
            len(analysis.unmatched_ends) +
            len(analysis.duplicate_begins) +
            len(analysis.negative_durations) +
            self.clock_regressions
        )
        status = ("No structural loss indicators" if observable == 0 else
                  f"{observable} structural timing issue(s)")
        loss = self.capture.loss
        producer = (f"{loss.producer_dropped:,} dropped / "
                    f"{loss.producer_accepted:,} accepted"
                    if loss is not None else
                    "Unavailable (VPRF v1 does not encode lifetime counters)")
        transport = (f"{loss.transport_lost:,} transport / "
                     f"{loss.sink_lost:,} sink"
                     if loss is not None else
                     "Unavailable (VPRF v1 has no transport-loss trailer)")
        return (
            ("Capture integrity",
             status if self.capture.complete else
             f"INCOMPLETE SESSION; {status}"),
            ("Producer ring drops", producer),
            ("TCP/sink loss", transport),
            ("Unmatched zone begins", str(len(analysis.unmatched_begins))),
            ("Unmatched zone ends", str(len(analysis.unmatched_ends))),
            ("Duplicate active correlations",
             str(len(analysis.duplicate_begins))),
            ("Negative zone durations",
             str(len(analysis.negative_durations))),
            ("Clock regression flags", str(self.clock_regressions)),
            ("Unresolved name IDs", str(len(self.unresolved_name_ids))),
        )

    def filter_zones(self, query: str = "",
                     thread_id: int | None = None
                     ) -> tuple[trace.ZoneSpan, ...]:
        return tuple(
            span for span in self.zones
            if _thread_matches(span.thread_id, thread_id) and
            _text_matches(query, (
                span.name, f"0x{span.name_id:08x}",
                f"0x{span.thread_id:08x}", str(span.correlation_id),
            ))
        )

    def filter_frames(self, query: str = "",
                      thread_id: int | None = None) -> tuple[FrameRow, ...]:
        return tuple(
            frame for frame in self.frames
            if _thread_matches(frame.thread_id, thread_id) and
            _text_matches(query, (
                frame.name, f"0x{frame.thread_id:08x}",
                str(frame.sequence), str(frame.event_index),
            ))
        )

    def filter_counters(self, query: str = "",
                        thread_id: int | None = None
                        ) -> tuple[CounterRow, ...]:
        return tuple(
            counter for counter in self.counters
            if _thread_matches(counter.thread_id, thread_id) and
            _text_matches(query, (
                counter.name, counter.sample_type,
                f"0x{counter.thread_id:08x}", str(counter.event_index),
            ))
        )

    def filter_events(self, query: str = "",
                      thread_id: int | None = None
                      ) -> tuple[trace.Event, ...]:
        return tuple(
            event for event in self.capture.events
            if _thread_matches(event.thread_id, thread_id) and
            _text_matches(query, (
                self.capture.resolve_name(event.name_id),
                event.type_name, f"0x{event.name_id:08x}",
                f"0x{event.thread_id:08x}", str(event.correlation_id),
                str(event.index),
            ))
        )

    def filter_tables(self, query: str, thread_id: int | None,
                      limit: int) -> TableRows:
        """Prepare bounded display rows without allocating full result sets."""
        if limit < 1:
            raise ValueError("limit must be positive")
        return TableRows(
            timeline=_bounded_filter(
                self.timeline,
                lambda row: _thread_matches(row.thread_id, thread_id) and
                _text_matches(query, (
                    row.kind, row.name, f"0x{row.thread_id:08x}",
                    row.thread_generation, row.begin_us, row.duration_us,
                )),
                limit),
            frames=_bounded_filter(
                self.frames,
                lambda frame: _thread_matches(frame.thread_id, thread_id) and
                _text_matches(query, (
                    frame.name, f"0x{frame.thread_id:08x}",
                    str(frame.sequence), str(frame.event_index),
                )),
                limit),
            zones=_bounded_filter(
                self.zones,
                lambda span: _thread_matches(span.thread_id, thread_id) and
                _text_matches(query, (
                    span.name, f"0x{span.name_id:08x}",
                    f"0x{span.thread_id:08x}", str(span.correlation_id),
                )),
                limit),
            counters=_bounded_filter(
                self.counters,
                lambda counter:
                _thread_matches(counter.thread_id, thread_id) and
                _text_matches(query, (
                    counter.name, counter.sample_type,
                    f"0x{counter.thread_id:08x}",
                    str(counter.event_index),
                )),
                limit),
            events=_bounded_filter(
                self.capture.events,
                lambda event:
                _thread_matches(event.thread_id, thread_id) and
                _text_matches(query, (
                    self.capture.resolve_name(event.name_id),
                    event.type_name, f"0x{event.name_id:08x}",
                    f"0x{event.thread_id:08x}",
                    str(event.correlation_id), str(event.index),
                )),
                limit),
        )

    def details(self, row: object) -> tuple[tuple[str, str], ...]:
        if isinstance(row, trace.Event):
            values = dataclasses.asdict(row)
            values["name"] = self.capture.resolve_name(row.name_id)
            values["type_name"] = row.type_name
            values["flag_names"] = ", ".join(row.flag_names) or "-"
        elif dataclasses.is_dataclass(row) and not isinstance(row, type):
            values = dataclasses.asdict(row)
        else:
            raise TypeError(f"unsupported selected row {type(row).__name__}")
        return tuple((key.replace("_", " ").title(), _display(value))
                     for key, value in values.items())


class ProfilerController:
    """File, receiver, and export operations shared by GUI and tests."""

    def open_capture(self, path: Path,
                     max_bytes: int = trace.DEFAULT_MAX_BYTES) -> LoadedCapture:
        capture = trace.read_capture(path, max_bytes)
        source = str(path)
        return LoadedCapture(
            capture, source, None, ProfilerViewModel(capture, source))

    def receive_capture(
            self, config: ReceiveConfig,
            cancelled: Callable[[], bool] | None = None,
            on_listening: Callable[[tuple[str, int]], None] | None = None,
            on_progress: Callable[[int], None] | None = None,
            on_live_update: Callable[[LoadedCapture], None] | None = None,
    ) -> LoadedCapture:
        if config.output.exists() and not config.force:
            raise FileExistsError(
                f"refusing to overwrite {config.output}")
        decoder = trace.IncrementalTraceDecoder(config.max_bytes)
        analysis_queue: queue.Queue[trace.TraceCapture | None] | None = None
        analysis_thread: threading.Thread | None = None
        analysis_errors: list[BaseException] = []
        last_live_update = 0.0
        next_event_update = 1

        if on_live_update is not None:
            analysis_queue = queue.Queue(maxsize=1)

            def analyze_live() -> None:
                assert analysis_queue is not None
                while True:
                    capture = analysis_queue.get()
                    try:
                        if capture is None:
                            return
                        source = str(config.output)
                        loaded = LoadedCapture(
                            capture, source, None,
                            ProfilerViewModel(capture, source))
                        on_live_update(loaded)
                    except BaseException as error:
                        if not analysis_errors:
                            analysis_errors.append(error)
                    finally:
                        analysis_queue.task_done()

            analysis_thread = threading.Thread(
                target=analyze_live, name="vitaprofiler-live-analysis")
            analysis_thread.start()

        def decode_chunk(chunk: bytes) -> None:
            nonlocal last_live_update, next_event_update
            changed = decoder.feed(chunk)
            if not changed or not decoder.is_v2 or on_live_update is None:
                return
            try:
                capture = decoder.snapshot()
            except trace.TraceFormatError:
                return
            if capture.complete:
                return
            event_count = len(capture.events)
            now = time.monotonic()
            if (event_count < next_event_update and
                    now - last_live_update < 2.0):
                return
            last_live_update = now
            next_event_update = max(event_count + 64, event_count * 2)
            assert analysis_queue is not None
            try:
                analysis_queue.put_nowait(capture)
            except queue.Full:
                try:
                    analysis_queue.get_nowait()
                    analysis_queue.task_done()
                except queue.Empty:
                    pass
                analysis_queue.put_nowait(capture)

        try:
            raw, peer = trace.receive_tcp_once(
                config.bind, config.port, config.source, config.max_bytes,
                config.accept_timeout, config.idle_timeout, on_listening,
                config.capture_timeout, cancelled, on_progress, decode_chunk)
        finally:
            if analysis_queue is not None and analysis_thread is not None:
                analysis_queue.join()
                analysis_queue.put(None)
                analysis_thread.join()
        if analysis_errors:
            raise analysis_errors[0]
        capture = decoder.finish()
        trace.write_capture(config.output, raw, config.force)
        source = str(config.output)
        return LoadedCapture(
            capture, source, peer, ProfilerViewModel(capture, source, peer))

    def export_decoded(self, loaded: LoadedCapture, output: Path,
                       force: bool = False) -> None:
        trace.export_decoded_json(loaded.capture, output, force)

    def export_perfetto(self, loaded: LoadedCapture, output: Path,
                        force: bool = False) -> None:
        trace.export_chrome_trace(loaded.capture, output, force)


def _thread_matches(actual: int, selected: int | None) -> bool:
    return selected is None or actual == selected


def _bounded_filter(values: Iterable[Row], matches: Callable[[Row], bool],
                    limit: int) -> FilteredRows[Row]:
    selected: list[Row] = []
    for value in values:
        if not matches(value):
            continue
        if len(selected) == limit:
            return FilteredRows(tuple(selected), True)
        selected.append(value)
    return FilteredRows(tuple(selected), False)


def _text_matches(query: str, values: Iterable[object]) -> bool:
    needle = query.strip().casefold()
    if not needle:
        return True
    return any(needle in str(value).casefold() for value in values)


def _display(value: object) -> str:
    if isinstance(value, tuple):
        return ", ".join(str(item) for item in value) or "-"
    if value is None:
        return "-"
    return str(value)

# Binary trace pipeline

VitaProfiler now has a complete, host-tested path from its bounded event ring to
a validated capture and standard timeline output. The path stays independent of
GDB and does not require a kernel plugin.

## Capture layouts

The decoder preserves the version-1 layout:

1. one sealed `VPNM` name-dictionary block;
2. one `VPRF` header; and
3. zero or more fixed 32-byte events through end of file or TCP EOF.

There is deliberately no native-struct dump and no implicit host byte order.
Every integer is encoded little endian. `VPNM.total_size` locates the `VPRF`
header. Version-1 `VPRF` has no trailer or embedded event count, so a
file boundary or a clean TCP half-close is the framing boundary. A truncated
final event is rejected.

Wire version 2 is an outer live-session envelope. It does not change `VPNM` or
the 32-byte event record. A v2 capture is a sequence of independently framed
chunks:

1. `SESSION`, exactly once and first;
2. `DICTIONARY`, exactly once and second;
3. zero or more `THREAD`, `MODULE`, `EVENTS`, and `STATS` chunks; and
4. `END`, exactly once and last.

Every chunk has a 32-byte little-endian header with magic `VPC2`, version 2,
type, payload size, contiguous sequence number, CRC32 of the payload, and
zeroed reserved fields. Payloads are capped at 2 MiB. `EVENTS` contains one or
more unchanged 32-byte records. `SESSION` carries a required nonzero 64-bit
session ID and only the bounded process identity, process ID, timer source, and
timer unit whose presence bits the producer sets. `THREAD` carries a numeric
thread ID, generation, optional exact identity, and optional dictionary name
ID. `MODULE` carries an ID/generation, half-open 32-bit address range, optional
name ID, and explicitly supplied executable/ARM/Thumb bits. No missing value is
inferred.

A `THREAD` declaration becomes effective at the next decoded event index.
Producers that observe a new generation must flush preceding events and emit
the declaration before emitting events from that generation. This positional
boundary keeps unchanged v1 event records usable without inventing identity for
events that preceded the declaration. A session permits at most 64 thread declarations and 64 module
declarations, and each `(ID, generation)` pair must be unique. Writers reject
duplicates or excess metadata before emission; receivers enforce the same
limits.

`STATS` snapshots carry producer accepted/dropped, caller-reported transport
loss, writer sink loss, events written, and bytes written. `END` repeats the
final counters. A clean `END` must have zero sink loss and an event count equal
to the records already framed. Missing `END`, trailing data, sequence gaps,
64-bit event/byte counter regression, CRC failure, malformed metadata, and
partial chunks all fail closed. The four 32-bit counters use raw wrapping
unsigned arithmetic, so a numerically smaller snapshot is not by itself
malformed. An incremental viewer may display a validated prefix as
**LIVE / INCOMPLETE**, but `decode_capture()` and file publication require the
clean `END`.

The portable receiver functions in `vitaprofiler.h` validate and iterate an
event block without allocating:

```c
struct vp_wire_info info;
struct vp_wire_cursor cursor;
struct vp_event event;

if (vp_wire_cursor_init(&cursor, vprf, vprf_size, &info) == VP_RESULT_OK) {
    while (vp_wire_cursor_next(&cursor, &event) == VP_RESULT_OK)
        consume_event(&event);
}
```

Use `vp_name_wire_cursor_init()` on the preceding block to resolve `name_id`
values. Both cursors fully validate their own framing before returning the first
record.

## Application-side drain

`vitaprofiler_stream.h` turns the sealed dictionary and ring into that combined
stream. It has no file or socket dependency; the application supplies a sink
callback that consumes each complete buffer.

```c
#include <vitaprofiler_stream.h>

static uint8_t dictionary_wire[8192];

struct vp_stream_writer_config config = {
    .context = &profiler,
    .names = &profiler_names,
    .write = send_all_bytes,
    .write_user = &connection,
    .dictionary_buffer = dictionary_wire,
    .dictionary_buffer_capacity = sizeof(dictionary_wire),
};
struct vp_stream_writer_v2 writer;
size_t sent;

struct vp_stream_v2_session session = {
    .session_id = application_session_epoch,
    .process_id = process_id,
    .flags = VP_STREAM_V2_SESSION_PROCESS_ID |
             VP_STREAM_V2_SESSION_TIMER_SOURCE |
             VP_STREAM_V2_SESSION_TIMER_UNIT,
    .timer_source = "sceKernelGetProcessTimeWide",
    .timer_unit = "microseconds",
};

if (vp_stream_writer_init_v2(&writer, &config) == VP_RESULT_OK &&
    vp_stream_writer_begin_v2(
        &writer, capture_start_us, &session) == VP_RESULT_OK) {
    while (capturing)
        vp_stream_writer_drain_v2(&writer, 64, &sent);
    vp_stream_writer_drain_v2(&writer, SIZE_MAX, &sent);
    vp_stream_writer_close_v2(&writer);
    shutdown_send_side(&connection); /* END already frames clean v2 close. */
}
```

The original `vp_stream_writer`, `vp_stream_writer_stats`, and entry points
remain the byte-for-byte, layout-compatible v1 path. V2 uses the distinct
`vp_stream_writer_v2` storage and `*_v2()` lifecycle so a new library never
writes beyond legacy caller-owned objects.
`vp_stream_writer_write_thread_v2()` and
`vp_stream_writer_write_module_v2()` publish only source-owned metadata.
`vp_stream_writer_write_stats_v2()` can expose loss while a session is live,
and `vp_stream_writer_set_transport_loss_v2()` snapshots caller-owned wrapping
`uint32_t` accounting. The dictionary buffer is reused as bounded event chunk
staging after v2 begin, so no new allocation or unbounded queue is introduced.
Each drain still removes at most the requested count and at most one
staging-buffer batch.

Call `vp_name_dictionary_wire_size()` to size `dictionary_wire`. Initialization
checks that the dictionary is sealed and the buffer is large enough before it
writes anything. The sink is single-consumer code and may block; do not run it
on a game/render producer thread.

The sink callback must consume its entire buffer before returning zero. A
nonzero result permanently fails the writer: previously written bytes may
already be visible, so silently restarting would create an ambiguous stream. If
one event was removed from the ring before its send failed,
`events_lost_to_sink` accounts it. The writer cannot then claim a clean close.
Ring-pressure drops remain separately available through `vp_get_stats()`.

The optional [Vita TCP stream sink](vita-tcp-stream.md) now supplies a bounded,
caller-owned SceNet adapter for this callback. It deliberately provides no
reconnect/resume protocol, authentication, encryption, compression, or
multi-capture container. A partial or ambiguous send ends that capture rather
than reconnecting into the middle of the byte stream.

## Host receiver and viewer

The Python tool uses only the standard library:

```sh
python tools/vitaprofiler_trace.py receive game.vptrace \
  --bind 0.0.0.0 --port 18195 --source 192.168.1.42
python tools/vitaprofiler_trace.py view game.vptrace --events 20
python tools/vitaprofiler_trace.py json game.vptrace game.decoded.json
python tools/vitaprofiler_trace.py chrome game.vptrace game.perfetto.json
python tools/vitaprofiler_trace.py runclocks game.vptrace \
  experiment.json game.runclocks.json
```

`receive` accepts one IPv4 TCP sender, reads through clean EOF, enforces a
16-MiB default wire bound and a hard 262,144-event decoded-object bound,
validates the entire dictionary and event stream, then atomically saves it.
Existing output is not overwritten unless `--force` is explicit, including if
another process creates the destination during publication. The default
accept, idle, and absolute post-accept capture limits are 30, 5, and 300
seconds. `--accept-timeout`, `--idle-timeout`, `--capture-timeout`, and
`--max-bytes` tune those bounds; only accept and idle may be set to zero because
the absolute capture deadline remains mandatory in the CLI.

For v2, `IncrementalTraceDecoder.feed()` validates and exposes each complete
chunk before EOF while retaining only one bounded partial-chunk buffer and the
bounded decoded event list. The same decoder's `finish()` enforces clean
session completion. Version 1 remains EOF-framed.

The text view reports:

- capture size, time span, event types, and producer threads;
- unresolved referenced name IDs;
- named complete-zone count, total, average, minimum, and maximum duration;
- named counter sample count, latest value, minimum, and maximum;
- average frame time and FPS; and
- unmatched zone records and other correlation diagnostics.

The `json` output is a complete decoded event listing. The `chrome` output uses
the Chrome Trace Event format and can be opened in Perfetto or Chrome's trace
viewer. Matched zone records become complete-duration events, counters and
Vita snapshots become counter tracks, and frames become global instants.
Both exports identify `SceKernelThreadInfo.runClocks` as a raw counter with an
unknown unit. Decoded JSON includes unsigned raw values, generation-aware raw
deltas, and discontinuity status. Perfetto uses explicit
`raw_unknown_unit` tracks and never presents the metric as CPU utilization.

The `runclocks` command creates the stricter, reproducible characterization
artifact described in
[`run-clocks-characterization.md`](run-clocks-characterization.md). It binds
the source capture hash and session ID to bounded experiment metadata, requires
schema-v2 evidence to contain a complete wire-v2 SESSION/END lifecycle and
final loss statistics, cross-checks experiment generations against wire
THREAD identities, and preserves raw values, raw deltas, and the wrap/reset
policy. Explicit schema-v1 metadata retains the legacy v1 report behavior. The
command does not alter or reinterpret the captured wire data.

For an interactive desktop workflow using these same APIs, launch:

```sh
python tools/vitaprofiler_gui.py
```

The dependency-free Tk viewer opens existing captures, controls one bounded TCP
receive, displays metadata, structural loss indicators, frames, zones,
counters, and events with filtering/selection, and invokes both existing JSON
exporters in background tasks. See the
[desktop GUI guide](desktop-gui.md) for setup and wire-version limitations.

Name bytes are hash-validated exactly as they are on the C receiver. The PC
viewer additionally rejects invalid UTF-8, terminal controls, and bidirectional
format controls before any name can be printed. Applications should therefore
register ordinary human-readable UTF-8 labels.

## Trust boundary and current limits

The TCP receiver is unauthenticated and unencrypted. Bind it only on a private,
trusted development network and use `--source` to narrow the accepted peer.
The source filter is not cryptographic authentication.

The host pipeline is tested with fragmented loopback TCP delivery, byte/event
and absolute-time bounds, corrupt and truncated headers/events/dictionaries,
all-prefix v2 truncation, deterministic byte-flip fuzzing, CRC/version/
sequence/lifecycle rejection, incremental chunk delivery,
safe display-name enforcement, linear-time named-zone pairing, built-in metric
resolution, race-safe no-clobber publication, summaries, and both JSON exports.
A C-generated v1 fixture and a C-generated v2 session are decoded by Python so
the two implementations do not merely self-validate duplicated constants. The library has been
cross-compiled for Vita, but its socket integration has not been run on Vita
hardware for wire v2 in this increment.

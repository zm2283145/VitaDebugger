# Binary trace pipeline

VitaProfiler now has a complete, host-tested path from its bounded event ring to
a validated capture and standard timeline output. The path stays independent of
GDB and does not require a kernel plugin.

## Capture layout

One `.vptrace` capture is:

1. one sealed `VPNM` name-dictionary block;
2. one `VPRF` header; and
3. zero or more fixed 32-byte events through end of file or TCP EOF.

There is deliberately no native-struct dump and no implicit host byte order.
Every integer is encoded little endian. `VPNM.total_size` locates the `VPRF`
header. The version-1 `VPRF` format has no trailer or embedded event count, so a
file boundary or a clean TCP half-close is the framing boundary. A truncated
final event is rejected.

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
struct vp_stream_writer writer;
size_t sent;

if (vp_stream_writer_init(&writer, &config) == VP_RESULT_OK &&
    vp_stream_writer_begin(&writer, capture_start_us) == VP_RESULT_OK) {
    while (capturing)
        vp_stream_writer_drain(&writer, 64, &sent);
    vp_stream_writer_drain(&writer, SIZE_MAX, &sent);
    vp_stream_writer_close(&writer);
    shutdown_send_side(&connection); /* TCP EOF frames the capture. */
}
```

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
the source capture hash to bounded experiment metadata, explicit thread
generations, raw values, raw deltas, and the wrap/reset policy. It does not
alter or reinterpret the captured wire data.

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
safe display-name enforcement, linear-time named-zone pairing, built-in metric
resolution, race-safe no-clobber publication, summaries, and both JSON exports.
A C-generated stream fixture is decoded by Python so the two implementations do
not merely self-validate duplicated constants. The library has been
cross-compiled for Vita, but its socket integration has not been run on Vita
hardware in this increment.

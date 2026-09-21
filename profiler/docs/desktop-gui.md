# VitaProfiler desktop GUI

VitaProfiler includes a dependency-free desktop viewer built on Python's
standard `tkinter` module. It uses the same bounded decoder, zone analyzer, TCP
receiver, decoded JSON exporter, and Chrome Trace/Perfetto exporter as
`tools/vitaprofiler_trace.py`; it does not implement a second wire parser.

## Requirements and launch

Use Python 3.10 or newer with Tcl/Tk support. The official Windows and macOS
Python installers normally include it. Some Linux distributions package it
separately (for example, `python3-tk`).

From the repository root:

```sh
python profiler/tools/vitaprofiler_gui.py
```

Or from `profiler/`:

```sh
make desktop
```

No Python packages, browser runtime, web server, or project-local virtual
environment are required.

## Workflows

**Open a capture** loads, validates, analyzes, and prepares a bounded
`.vptrace` file in a worker thread. The Overview tab shows wire metadata,
capture size/span, dictionary and thread counts, and the structural indicators
available from the capture.
Frames, complete timing zones, counters/snapshots, and the underlying events
have separate tables. A text filter matches names, types, IDs, sequences, and
correlations; the thread selector narrows all tables. Selecting a row shows its
complete decoded fields.

**Start receiver** runs the existing one-capture TCP receiver. Choose the
destination file, bind address, port, and optional allowlisted Vita IPv4
address before starting the Vita sender. The status bar reports the listening
endpoint and received byte count. Cancel closes the wait or active receive
within a short polling interval. A capture is decoded completely before it is
published atomically; malformed or incomplete input is reported and is not
saved as a successful capture.

**Export decoded JSON** and **Export Perfetto JSON** call the existing export
pipeline. The Perfetto result can be loaded into
[ui.perfetto.dev](https://ui.perfetto.dev/) or Chrome's trace viewer. Existing
files require an explicit replacement confirmation.

File reads, network ingestion, decoding, analysis, filtering, and exports run
off the Tk event thread. The receiver callback only publishes a small progress
notification; it never parses, symbolizes, filters, or renders capture rows.
Only bounded row insertion and other UI updates run on the event thread, so GUI
rendering cannot apply backpressure to Vita-side profiler callbacks. At most
one background operation runs at a time, and closing the window requests
receiver cancellation.

For wire v2, the receiver worker feeds the shared incremental decoder and
builds the same analyzer/view model after complete chunks. A size-one GUI
update queue replaces stale snapshots instead of blocking the receiver. The
Timeline tab combines complete CPU zones and frame intervals, ordered by start
time with thread-generation labels when supplied. Frames, counters, and the
overview update before EOF. Parsing, CRC work, zone pairing, and bounded table
filtering remain off the Tk event thread. The Overview labels the prefix
**LIVE / INCOMPLETE** until a valid `END` chunk arrives; cancellation leaves
the latest prefix visibly incomplete and never publishes it as a completed
capture file.

## Loss reporting and limitations

The viewer reports unmatched zone records, duplicate active correlation IDs,
negative zone durations, clock-regression flags, and unresolved names. A clean
result means that no structural loss indicator was observed in the records
that reached the file.

VPRF wire version 1 does **not** encode the producer ring's lifetime
`accepted`/`dropped` counters or the stream writer's
`events_lost_to_sink`/transport counters. The Overview therefore labels those
values **Unavailable** rather than inferring zero loss. Applications should
retain or display the Vita-side statistics when proving a loss-free run.

VPRF v1 also has no session or process identity, thread-generation identity,
raw timer source/unit, module ranges, or per-event ARM/Thumb execution state.
The GUI labels those fields **Unavailable**; it does not infer them from
addresses or invent placeholder values. Timestamps are the normalized
microseconds and fixed frequency actually encoded by v1.

Wire v2 displays producer, transport, and sink counters from the latest
validated `STATS`/`END` chunk, plus only the session, process, thread
generation, timer, module range, and ARM/Thumb metadata actually supplied.
Absence remains **Unavailable**, never inferred.

Other current limits:

- Version-1 TCP captures still appear only after clean EOF; incremental live
  viewing requires wire v2.
- The GUI accepts one sender and one capture per receiver start.
- Filtering and tables retain at most 3,000 matching display rows each to keep
  memory and Tk work bounded; exports always include the complete validated
  capture.
- The GUI does not embed Perfetto, plot charts, symbolize threads, or control
  profiler instrumentation inside the Vita application.
- Automated tests cover the model, controller, receiver cancellation, and
  parsing/export integration without opening a display. Visual behavior
  depends on the platform Tcl/Tk build and is not exercised by headless tests.

The receiver remains unauthenticated and unencrypted. Bind it only on a trusted
private development network and use the source allowlist where practical.

# Vita TCP hardware gate

`vitaprofiler-tcp-gate.vpk` is a disposable, ordinary user-mode application
that validates the production VitaProfiler TCP sink against a real Vita and PC
receiver. It does not import or call the VitaDebugger kernel companion.

The committed build defaults to `127.0.0.1:18195`, which deliberately cannot
reach a development PC from a Vita. Override all four numeric PC IPv4 octets
when building; no DNS lookup is performed:

```sh
make -B -C profiler vita-tcp-gate \
  VITA_TCP_GATE_HOST_A=192 VITA_TCP_GATE_HOST_B=168 \
  VITA_TCP_GATE_HOST_C=1 VITA_TCP_GATE_HOST_D=25 \
  VITA_TCP_GATE_PORT=18195
```

For repeated local builds, put the same assignments in the ignored file
`profiler/local/vita-tcp-gate.mk`. Command-line values can still override it.
The gate has the fixed, distinct title ID `VDPT00001`.

Start the receiver before pressing X in the application, substituting the
Vita's displayed address for `VITA_IP`:

```sh
python profiler/tools/vitaprofiler_trace.py receive tcp-gate.vptrace \
  --bind 0.0.0.0 --port 18195 --source VITA_IP
```

The application itself loads the network sysmodule, owns the SceNet memory
pool, initializes NetCtl, waits for a connected interface, and displays every
stage. It does not create or connect a TCP socket until X is pressed. Circle
exits safely before the run or after results are visible.

One run sends a sealed dictionary followed by this ordered 13-event sequence:

1. `tcp_gate.zone` begin/end;
2. fixed `tcp_gate.counter = 314`;
3. first and timed `tcp_gate.frame` marks;
4. four live memory/process samples; and
5. four live current-thread samples.

The order, types, names, counter value, and event count are deterministic;
timestamps, durations, memory values, and thread statistics are intentionally
live measurements. The gate drains until empty, closes the writer, produces a
clean TCP EOF, and releases or retries every socket/epoll obligation before it
terminates NetCtl/SceNet. It prints ring, writer, sink, and cleanup statistics
and reports Vita-side `PASS` only for 13 delivered events with no drops, sink
loss, or transport/cleanup failure. TCP has no application-level peer ACK, so
the receiver's successful save and decode of all 13 events is the independent
second half of the hardware gate.

Decode and inspect the capture after the receiver exits:

```sh
python profiler/tools/vitaprofiler_trace.py view tcp-gate.vptrace --events -1
python profiler/tools/vitaprofiler_trace.py json tcp-gate.vptrace \
  tcp-gate.json
```

TCP telemetry is unauthenticated and unencrypted. Use a trusted development
LAN, retain the receiver's `--source` filter, and restrict the listening port
with the PC firewall.

## Retail 3.65 result

The gate passed end to end on retail 3.65 on 2026-09-15. Two independent
captures delivered and decoded all 13 expected events with zero unresolved
names. Pre-connect cancellation, a forced TCP peer reset, and a successful
post-reset relaunch were also exercised. Raw captures, decoded JSON, a Perfetto
export, artifact hashes, and the exact scope of the result are retained in the
[hardware journal](../../docs/hardware/profiler-tcp-stream-retail-3.65.md).

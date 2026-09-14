# Bounded GDB console transport

VitaDebugger can route application `stdout` and `stderr` to GDB as standard
remote-protocol `O` packets. The implementation is intended for light,
interactive diagnostics during an attached debug session. It is deliberately
bounded and lossy so a slow or disconnected debugger cannot stall the target.
Use the independent DebugNet-compatible UDP path for sustained or high-rate
logging, profiler streams, and output that must continue while GDB is stopped
or disconnected.

## Implemented data path

`uvdb_redirect_stdio()` installs the bridge in four stages:

1. It saves the original Vita newlib descriptor mappings for file descriptors
   1 and 2.
2. It creates a Vita socket pair and makes the application-facing write end
   nonblocking.
3. It starts a joinable capture thread, then redirects both `stdout` and
   `stderr` to the write end.
4. The capture thread reads available bytes and calls only the bounded console
   queue. It never writes to the GDB socket itself.

The queue in `uvdb_console.c` contains 64 fixed records of 128 bytes, for an
8192-byte maximum payload capacity. Capture performs no allocation, network
operation, sleep, or lock wait. It tries the queue lock once, splits accepted
input into records, and drops any remainder when the console session is closed,
the queue is contended, the queue is full, or the record belongs to a stale
connection. Its counters distinguish those post-capture loss paths.

The nonblocking newlib endpoint is an earlier bound. If its socket buffer is
full, an application write may be short or fail with `EAGAIN` before the helper
can place those bytes in the queue. That loss cannot appear in the queue's
statistics; the return value from `write()`, `fwrite()`, or the relevant stdio
operation is the only evidence available to the application.

## RSP ownership and no-ack negotiation

Only the debugger protocol loop owns the GDB TCP stream. The capture thread and
application threads never send packets, which prevents console output from
racing command replies, acknowledgements, Ctrl-C, or disconnect cleanup.

The stub advertises `QStartNoAckMode+` and a usable `PacketSize` in
`qSupported`. A new console generation opens only after the framed
`QStartNoAckMode` `OK` response and the final acknowledgement transition have
completed. A client that stays in acknowledgement mode can still use the
debugger, but captured console bytes are dropped instead of risking RSP stream
desynchronization.

While the target runs, the service loop checks inbound commands, Ctrl-C, and
disconnect state before attempting at most one queued console record per
iteration. Each record is encoded as `O` followed by two hexadecimal characters
per raw byte and is sent as one complete RSP frame. A would-block result leaves
the record queued. A partial frame or hard socket error is connection-fatal
because resuming a partially sent RSP frame would corrupt the stream; the stub
closes the connection and invalidates that output generation.

At most one bounded console record may be completed before a pending stop reply.
Once the stop reply has been sent, the stopped protocol loop emits no unsolicited
`O` packets. Ordinary GDB commands therefore retain priority and a busy output
producer cannot prevent Ctrl-C from stopping the target.

## Disconnect and reconnect behavior

Every accepted GDB connection has a nonzero generation token. Clean detach,
socket failure, protocol/session failure, server shutdown, and reconnect close
and purge that generation. Bytes captured for one client are never replayed to
the next client. Reconnection performs a fresh `qSupported` and
`QStartNoAckMode` negotiation before capture is enabled again.

Queue and transport counters are cumulative 32-bit diagnostics and can wrap.
They are currently internal implementation data rather than a stable public
API.

## Application API and lifecycle

Call the redirect function after Vita networking is ready and before application
threads begin writing concurrently when practical:

```c
#include <stdio.h>
#include <uvdb.h>

int console_capture_enabled =
    uvdb_start_server() == 0 && uvdb_redirect_stdio() == 0;

/* Later, during normal running work. Pre-negotiation output is dropped. */
if (console_capture_enabled)
    fputs("frame started\n", stdout);
```

`uvdb_redirect_stdio()` is idempotent after a successful setup. It requires the
original newlib `stdout` and `stderr` descriptors to be valid. A partial setup
failure attempts to restore both streams and retains enough state for a later
cleanup retry if restoration itself fails.

Vita newlib does not export `dup2`, so this module uses its private descriptor
map and mutex. Deferred close retry is restricted to descriptors owned solely
by the bridge. The behavior was audited against the installed Vita newlib
commit `64aa7aa33d4f380451a1f100d19589226cdad334`; changing newlib revisions
requires revalidating this lifecycle path.

The bridge does not silently change newlib buffering. A newline is not a
universal flush guarantee, especially when a stream has already been used.
Applications that need predictable timing should deliberately use `fflush()`,
configure buffering before first use with `setvbuf()`, or use `write(1, ...)`
and `write(2, ...)` while checking their return values. Direct `write()` calls
are used by the hardware smoke fixture.

Stop the debugger server before restoring the descriptors:

```c
if (uvdb_stop_server() == 0 && uvdb_restore_stdio() < 0) {
    /* A later normal-control-thread call may retry restoration. */
}
```

This order prevents a new asynchronous all-stop from suspending the capture
helper while restoration waits to join it. `uvdb_shutdown()` performs the safe
order automatically: it stops DebugNet, stops the debugger server, restores
`stdout`/`stderr`, and then releases the remaining debugger state.
`uvdb_restore_stdio()` is idempotent and retryable, but it must be called only
from normal application control flow. The application must serialize redirect
and restore operations with its own concurrent stdio writers.

The capture helper is hidden from ordinary GDB thread inventory so debugger
implementation details do not appear as application workers. If that helper is
the faulting thread, it remains visible for fault attribution.

The current archive requires the VitaSDK `SceNet_stub` import library for the
running service thread's socket-readiness and nonblocking send path, in
addition to the existing `SceNetPs_stub` network dependency. Keep both after
`libuvdb.a` in static-library link order. The repository Makefile links both
automatically.

## Host validation

The complete native host suite includes queue, payload/framing, transport
no-ack state, partial-send failure, commit retry, generation, reconnect, and
concurrency coverage:

```sh
make host-tests
```

The focused targets are:

```sh
make host-test-rsp-console
make host-test-console-queue
make host-test-console-transport
```

These tests do not replace the on-device newlib, socket, scheduling, command
priority, stop-boundary, and GDB lifecycle gate.

## Vita/GDB smoke test

Build the repository test application with the deterministic console markers
enabled. Add the kernel option when testing the matching all-stop companion:

```sh
make package UVDB_GDB_CONSOLE_TEST=1
# or
make package UVDB_KERNEL_THREAD_CONTROL=1 UVDB_GDB_CONSOLE_TEST=1
```

Install the resulting `uvdb-test.vpk`, launch the test application, and leave it
running. From the repository root on the development computer, run:

```powershell
py -3 tools/gdb_console_smoke.py --host 10.1.1.93 --reconnect
```

Change the host address if the Vita uses a different address. The script runs
two complete sessions when `--reconnect` is present. In each session it:

- verifies `qSupported` and switches to no-ack mode;
- reads the initial stop and continues the target;
- requires both `[uvdb stdout]` and `[uvdb stderr]` markers from `O` packets;
- sends Ctrl-C and rejects console packets arriving after the stop reply; and
- detaches cleanly before the reconnect cycle.

Success ends with:

```text
PASS: GDB no-ack stdout/stderr console smoke test
```

### Recorded hardware result

On 2026-09-13, this command completed both consecutive sessions on one
homebrew-enabled retail PS Vita running system software 3.65. In each session:

- `qSupported` advertised `QStartNoAckMode+` and a usable `PacketSize`, and the
  client completed the no-ack transition;
- the initial stop reply was `T05`;
- continuing produced six complete `O` packets whose decoded bytes included
  both the `stdout` and `stderr` fixture markers;
- raw Ctrl-C produced `T02`, followed by a quiet stopped-boundary window with
  no unsolicited `O` packet; and
- detach succeeded, after which a new connection renegotiated no-ack mode and
  repeated the same result.

This validates no-ack framing, both captured streams, command priority, the
post-stop silence rule, clean detach, and fresh reconnect for that exact 3.65
test environment. It is not a losslessness or pressure result and does not
establish compatibility with another firmware, Vita model, newlib revision,
network environment, debugger client, or plugin combination.

The repository's current hardware-validation device is one homebrew-enabled
retail PS Vita running system software 3.65. This baseline is not a compatibility
claim for every Vita model, firmware, newlib revision, network environment, or
plugin combination. Record the exact application/library build, system-software
version, kernel-companion build when used, GDB version, and transcript for each
broader validation run.

## Intended use and remaining limits

This path is best for occasional messages that belong beside an interactive GDB
session. It is not lossless, does not retain output across detach/reconnect, does
not deliver while the target is stopped, and deliberately drops rather than
blocking under pressure. `stdout` buffering and pre-capture `EAGAIN` remain
application-visible concerns. Use DebugNet for sustained logs and keep profiler
events on their dedicated bounded transport.

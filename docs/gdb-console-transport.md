# Bounded GDB console transport

This document defines the staged replacement for the experimental stdout and
stderr remote-file-I/O bridge. The first, host-tested foundation is present;
the live RSP transport and newlib redirection are not yet connected to it and
must not be advertised as complete.

## Implemented foundation

`uvdb_console.c` and its internal header provide a fixed 64-record queue with
128 raw bytes per record. A producer:

- checks that a GDB output session is open;
- attempts the queue lock exactly once;
- performs no allocation, network operation, sleep, or lock wait;
- splits input only until the fixed ring is full; and
- returns the exact accepted byte count while recording disconnected,
  contention, full-queue, and stale-session loss.

A single call can therefore copy at most the ring's fixed 8192-byte capacity,
even if the caller supplies a larger buffer. Session open/close never waits for
the queue lock, because an application thread could be suspended during its
bounded copy. Lifecycle or cleanup contention is returned to the caller for
retry; closing the session gate never depends on acquiring that queue lock.

Each connection receives a nonzero 31-bit generation. Opening or closing a
session invalidates queued records from the previous generation. They are
purged and counted as stale immediately when the queue is available, or lazily
by the next producer/consumer; output captured for one GDB client is never
replayed to another. The consumer copies the head record without removing it,
then commits it only with the same generation and 64-bit sequence token after
transport success.
Cumulative 32-bit counters currently wrap; queue depth and session state are
reported separately. This interface is internal and does not yet alter the
public library ABI.

Exactly one debugger transport owner serializes session lifecycle and the
peek/send/commit sequence. No record may remain in flight when ownership moves
between the running service loop and stopped RSP loop. A generation token must
not be retained after its session ends; because the token is 31 bits, it can
repeat after `2^31 - 1` later opens.

`uvdb_rsp_encode_console_payload()` converts one raw record into the GDB
remote-console payload `O` followed by two lowercase hexadecimal characters per
byte. It checks size overflow, capacity, null-pointer use, and overlapping
input/output spans. It deliberately does not add `$...#cc` framing or a NUL
terminator.

Host tests cover binary and reserved RSP bytes, exact capacity, size overflow,
overlap rejection, FIFO and ring wrap, partial acceptance, full and contended
loss, repeatable peek-before-commit, wrong tokens, disconnect/reconnect
isolation, generation wrap, a deterministic close/producer race, and concurrent
producer integrity. The lifecycle race specifically proves close and reopen can
finish while a producer remains suspended with the queue lock. Both new modules
also pass a VitaSDK cross-compile.

The producer contains no allocation, I/O, sleep, mutex wait, or queue-lock retry.
Its weak lock acquisition is one compare/exchange attempt. The small 32-bit
statistics operations are lock-free on the Vita target but can retry their ARM
exclusive sequence, so the capture routine is not claimed to be mathematically
wait-free under unlimited adversarial contention.

## Required transport integration

There must be only one reader/writer for the GDB TCP byte stream. A separate
console network thread would race command replies, acknowledgements, Ctrl-C,
and disconnect handling. The existing service loop owns console delivery while
the target runs; the exception/RSP loop owns it while the target is stopped.

The first integration will:

1. Advertise a truthful packet size and `QStartNoAckMode+`.
2. Reset acknowledgement state and the console generation on every accepted
   connection.
3. Enable capture only after GDB successfully enters no-ack mode. Older clients
   remain usable but console bytes are observably dropped rather than risking
   protocol desynchronization.
4. Replace the running service's blocking socket peek with a nonblocking poll
   that can recognize Ctrl-C/disconnect or send at most one queued console
   record per iteration.
5. Frame and send each small `O` packet through that sole owner. A partial TCP
   frame or hard send error invalidates the stream: close the connection, count
   the error, purge that generation, and require a clean reconnect.
6. Emit only a bounded amount of queued output before a stop reply. Do not send
   unsolicited console packets after the stop reply while GDB is at a stopped
   prompt.
7. Close and purge the console session on detach, shutdown, lease failure,
   protocol error, and every socket-disconnect path.

GDB permits `O` output while an all-stop target is running and GDB is waiting
for a stop reply. It is not valid in GDB non-stop mode; console delivery must be
disabled if non-stop support is added later. See the official
[stop-reply packet documentation](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Stop-Reply-Packets.html)
and
[packet-acknowledgement documentation](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Packet-Acknowledgment.html).

## Required stdio lifecycle

The current `stdio_redirect.c` helper still calls the blocking GDB remote-file-
I/O path and is not connected to the new queue. Its replacement must:

- save and restore the original newlib stdout and stderr mappings;
- be idempotent and roll back both streams after any partial start failure;
- fix the directional error in the plain `pipe()` fallback;
- use a joinable capture thread whose only output action is the bounded queue;
- make the application-facing write endpoint nonblocking, so a stopped helper
  cannot eventually block gameplay when its kernel buffer fills; and
- restore descriptors, wake and join the reader, and close its endpoints before
  debugger shutdown destroys the console session.

An `EAGAIN` returned before the reader sees a byte cannot be counted by the
queue; the application's stdio return value is the only evidence for that
pre-capture loss. Perfect accounting would require a supported custom newlib
file-descriptor backend rather than a pipe. stdout buffering also remains an
application/newlib policy and must not be changed silently after prior I/O.

## Promotion gate

After host-side fake-socket and real-GDB handshake tests pass, the Vita gate is
a multithreaded stdout/stderr stress run covering long stops, Ctrl-C during
output, clean detach, abrupt client closure, reconnect, repeated redirect and
restore, shutdown, and counter accounting. DebugNet must continue independently
through the same run. Until that evidence is recorded, use DebugNet for
sustained logs and treat GDB console output as unfinished.

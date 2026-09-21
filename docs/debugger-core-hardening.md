# Debugger-core hardening

This document records the safety invariants added to the embedded GDB stub and
the hardware checks that are still required. Host tests are evidence for the
portable state machines and parsers only; they do not prove Vita kernel,
exception-return, or socket-cancellation behavior.

## Packet and mutation invariants

- RSP framing is bounded by the configured packet limit. Invalid checksums,
  malformed checksum digits, superseded start markers, and oversized frames
  are consumed with forward progress. In ACK mode each malformed frame that
  began with `$` requests exactly one raw `-`; prefix noise does not, and
  no-ack mode suppresses NACKs. An incomplete frame is retained. Escaped `#`
  and `$` wire bytes remain payload rather than terminating or superseding a
  frame, and their encoded bytes remain part of the checksum and size bound.
- Memory addresses and lengths are strict hexadecimal fields. Empty fields,
  trailing data, fields wider than ARM32, address-range wrap, payload-length
  mismatches, and requests above the reply limit are rejected.
- `G`, `P`, and raw hexadecimal decoding validate the complete value before
  changing their output. Invalid packets leave the destination unchanged.
- Software-breakpoint, qXfer range, file-I/O result, and thread-ID parsing is
  exact and rejects trailing data and numeric overflow. The minimum valid
  `Z0,0,2` and `z0,0,2` forms are accepted without weakening those bounds.
  File-I/O recognizes
  the standard literal `C` interrupt field and exposes an optional semicolon
  attachment only as a bounded view of the packet.
- `m` preflights the complete source span before emitting data. `M` preflights
  the complete destination, validates all source hex, checks every KuBridge
  copy result, flushes caches, and verifies each written chunk. It never
  reports `OK` after a failed or short copy.

An arbitrary multi-chunk live `M` write is **not yet rollback-atomic**. A late
KuBridge copy or read-back failure can occur after an earlier chunk changed
memory. `uvdb_memory_write_transaction` demonstrates the required full-span
snapshot, write, verify, rollback, and retained-restoration result contract
with a fake kernel; it is foundation-only and is not called by `uvdb.c` yet.
Wiring that helper into live RSP requires durable caller-owned storage
for the entire original span and a shutdown/reconnect path that retains and
retries any failed restoration. Until then, callers must treat an `E0e` from
`M` as a potentially partial target mutation.

Response retransmission after a peer NACK is intentionally unsupported at
present. Such a NACK closes the operation rather than accepting an ambiguous
protocol state. This is separate from the raw request NACK emitted above when
VitaDebugger rejects a malformed inbound frame.

## Blocking I/O and lifetimes

Blocking `accept`, packet `recv`, and packet `send` calls no longer hold the
global debugger-state lock. The socket descriptor and generation are
snapshotted under a short lifecycle lock. A dedicated whole-protocol owner
pins both RSP buffers from the first receive through parsing, callbacks,
response transmission, and the final state transition; neither a second
`uvdb_remote_syscall`, a simultaneous exception, nor the service thread may
touch that byte stream while the owner has dropped the global lock. The
per-syscall active flag additionally pins the exact pointer passed to the
network call. Results are accepted only if descriptor, generation, and
non-closing state still match after reacquiring the debugger lock.

A successful TCP `accept` is only a candidate, not a debugger generation.
The service leaves the target running and waits for one complete,
checksum-valid, nonempty RSP frame with a fixed two-second bound. It uses
`MSG_PEEK`, so an admitted GDB client's first packet remains queued for the
normal receiver and acknowledgement path. Silent health checks, port scans,
HTTP probes, malformed frames, and peers which disconnect before that proof
are closed while the listening socket remains available for the next client.
Shutdown publishes and cancels the candidate descriptor just like connected
I/O, so admission cannot add a new unbounded wait.

A completed request also owns an explicit payload-borrow lifetime. Ordinary
replies discard the request and release that lifetime before `send_packet()`
can wait for a response ACK and compact or refill the shared receive buffer.
The synthetic `?` resume handoff releases the borrow only after rewriting the
current frame, and File-I/O resume follows the same rule. A second borrow,
double release, or response send while a borrow remains active fails closed.
The whole-protocol gate serializes this small state machine.

Close/reopen and owner publication share one 32-bit atomic state: its high bit
closes admission and its remaining bits hold the exact owner. This is distinct
from holding a lock across packet work. One compare/exchange claims an open,
idle generation, while close atomically adds the closed bit without disturbing
an owner that still must drain. No transition spin or scheduler progress is
required. Restart succeeds only from the closed-and-idle value.

Server stop first closes the protocol gate and publishes `network_closing`,
preventing a new owner or network operation from appearing after the
quiescence check, then uses socket shutdown/abort to wake the current
operation. A bounded five-second deadline waits for accept/send/receive and the
complete protocol owner. Service and lease thread joins now use the same
five-second bound instead of Vita's infinite `WaitThreadEnd` mode. A timeout
retains the live thread handle and all referenced objects so a later stop can
retry safely. Stop then waits for the exception guard to become idle; start
repeats that guard fence before reopening the protocol gate. This closes the
tail window in which an old callback had released the protocol owner but had
not yet left the guard, so an immediate reconnect's first trap remains a
primary exception. Failure retains referenced objects, so an unexpected
firmware-level stuck syscall or worker cannot become an infinite wait or
use-after-free.

An expected socket HUP after `server_stop` no longer enters a synthetic trap:
both the server recovery branch and `real_uvdb_enter()` check the stop request
before using a still-published connected descriptor. Once join, callback
drain, socket retirement, and restoration checks succeed, public stop
normalizes the lifecycle state to `IDLE` even if shutdown woke a blocked
receive through its ordinary transport-error return.

The timeout and cancellation contract has production-translation-unit host
coverage for connected blocked receive, connected blocked send, whole-protocol
exclusion during each wait, and an intentional connected HUP during shutdown.
Connected packet receive and send now use raw nonblocking syscalls with a
one-millisecond bounded poll. Each retry revalidates the exact published socket
generation and the closing flag, so shutdown cannot leave a packet operation
blocked in the kernel and descriptor reuse cannot redirect it. An ACK/no-ack
production-translation-unit regression injects a peer reset while the stopped
handler waits for its next packet, then proves target-running recovery,
protocol/exception-gate release, a new socket generation, and successful
`qSupported`/detach; the test repeats this sequence 100 times in both compile
modes. Send backpressure uses the same bounded cancellation path.

A retail run on 2026-09-20 passed malformed/nonhex/truncated checksum, escaped
delimiter, exact 262,140-byte payload, and connected one-byte-oversize cases.
It then found that a Windows `SO_LINGER` RST did not wake Vita's raw blocking
receive while the target was stopped, so no listener reopened within the
15-second bound. The title was destroyed cleanly and port 1234 was confirmed
closed without a reboot or kernel configuration change. The nonblocking fix
above remains hardware-pending; rerun the stopped RST, connected cancellation,
owner exclusion, command-specific disconnect, and bounded soak matrix before
claiming retail closure.

Stop-token acquisition/recovery, thread-context snapshots, cache maintenance,
and exception-slot replacement still execute inside the global state lock.
Those calls protect coupled breakpoint/lease/handler invariants, and moving
them requires a second published operation-lifetime gate plus retryable
teardown obligations. They have not been moved merely on the assumption that
the present kernel implementation returns quickly.

## Exception-handler ownership

The debugger keeps one predecessor token for each KuBridge exception slot
(data abort, prefetch abort, and undefined instruction). Partial installation
rolls back in reverse order. Teardown restores each non-NULL predecessor by
re-registering it and releases a slot only when its captured predecessor was
NULL. A failed predecessor restore retains the exact ownership obligation and
causes teardown to fail so unload cannot be reported as safe.

KuBridge copies the displaced callback to the supplied user pointer while
holding its per-process exception-handler spin lock, then publishes the new
callback before releasing that lock. VitaDebugger now passes each permanent
registry slot directly to that call. A callback that becomes dispatchable
inside the registration call therefore sees its predecessor before the call
returns; a temporary local token is never published after the callback.

The registry also keeps a monotonic `ever_published_mask`. Even when a partial
installation rolls back every visible kernel slot, the registry refuses both
reinitialization and a second installation generation. This preserves tokens
for a callback KuBridge may already have copied before rollback. A completely
zero-initialized registry has no ownership obligation, so shutdown before the
first handler installation is an explicit no-op rather than a teardown error.

KuBridge does not provide compare-and-restore, so this relies on exclusive
ownership of those process-wide user-handler slots while VitaDebugger is
active. In addition, `kuKernelReleaseExceptionHandler` returns no status;
restoring a captured NULL/default slot cannot be independently verified by the
current ABI. Both limitations require hardware/ABI validation.

The exception guard prevents a nested or simultaneous exception from spinning
on the global debugger lock. One distinct captured predecessor can be chained
at most once at a time, and a predecessor equal to VitaDebugger's own handler
is never recursively invoked. Every entered callback owns an active lifetime,
including a nested callback blocked inside its predecessor and a callback that
arrives after close. Primary return no longer clears a chain owned by a peer.

The global state lock now publishes the normalized owning thread ID rather than
a Boolean. After claiming the protocol gate, a primary exception makes exactly
one nonblocking owner-aware lock attempt before it inspects breakpoint or
controller state or edits the exception context. `SELF` (the fault interrupted
its own critical section) and `BUSY` both fail closed: protocol ownership is
released, the original context is offered once to the captured predecessor,
and the guard lifetime is retired. This removes the former permanent self-spin.
Foreign-thread contention is intentionally conservative and can reject an
otherwise genuine application fault; bounded retry is not enabled without a
hardware proof that every foreign critical section is exception-safe and has a
strict upper bound.
Unlock is owner-checked and cannot clear a different thread's published lock.

Terminal shutdown restores every kernel handler slot before closing callback
admission. Captured predecessor tokens then remain immutable, the closed guard
counts any late callback, and a bounded wait drains all callbacks visible in
user space. Full shutdown deliberately retains its terminal state; callers
must use `uvdb_stop_server()`/`uvdb_start_server()` for nonterminal reconnects.

This still does **not** prove dispatcher quiescence. KuBridge copies the current
handler pointer under its kernel spin lock and invokes it later in user mode,
but exposes no fence for a pointer copied before slot restoration and not yet
entered. For that reason, terminal shutdown does not authorize unloading an
injected debugger `.suprx`, resetting the handler gate, or discarding captured
predecessors. Safe dynamic unload remains blocked on a KuBridge/kernel callback
lifetime ABI (or a process-terminal policy).

The NULL-predecessor nested-fault case is **not contained on hardware yet**.
With the default build, returning from such a nested exception can re-enter at
the unchanged faulting PC. `UVDB_EXPERIMENTAL_NESTED_FAULT_EXIT` provides a
bounded process-exit trampoline, but it is disabled by default and must remain
opt-in until a disposable-device test proves the KuBridge exception-return and
process-exit transition on each supported firmware. Do not describe nested
fault containment as complete before that gate passes. The same hardware gate
is required for a recursive fault raised inside the predecessor itself: the
guard correctly blocks re-chaining, but the default build has no proven
bounded terminal transition for that second fault.

A late callback arriving after terminal slot restoration with a NULL captured
predecessor returns so the re-fault is dispatched through the restored kernel
default. That ordering avoids leaving VitaDebugger installed at the unchanged
PC, but the exact default-redispatch behavior remains a Vita hardware/KuBridge
gate and must not be described as proven containment.

## File-I/O interrupt state

The portable transition model distinguishes a real exception context with a
coherent all-stop from the legacy `uvdb_remote_syscall()` context. A literal
File-I/O `C` can produce exactly one `T02` and no resume only in the former.
The legacy API currently constructs a zeroed synthetic register context and
does not publish an all-stop before entering its packet loop; reporting T02
there would let GDB inspect or mutate invalid registers while peer threads run.
That path therefore consumes the reply, closes the protocol generation, and
returns failure instead. A truthful remote-syscall Ctrl-C requires first
transitioning through a real exception/all-stop and remains future work.

## Host evidence

The focused host suite covers:

- exact RSP wire boundaries plus 25,000 deterministic arbitrary byte streams,
  including escaped delimiter handling, trailing escapes, exact-maximum and
  one-byte-oversized payloads, and parser output immutability for breakpoint
  packets;
- ACK/no-ack malformed-frame disposition, bounded corrupt-to-valid
  resynchronization, request-borrow reuse/release rejection, and the exact
  minimum `Z0`/`z0` forms;
- zero-length and exact maximum-size memory packets;
- malformed/truncated registers and no-partial-output parsing;
- fake-kernel short reads, persistent and transient short writes, sync
  failures, verification corruption, successful rollback, and retained
  restoration obligations;
- fake register/VFP setter snapshot, verification, rollback, and retry paths;
- nested-guard serialization and fake predecessor chaining policy; and
- a controlled two-thread primary/nested predecessor race proving that primary
  return cannot clear peer chain ownership, plus a late closed callback whose
  full lifetime remains counted;
- an integrated host translation unit that includes the production `uvdb.c`
  and drives its real `recv_packet`, `discard_packet`, `send_packet`, status
  branch in `uvdb_main_loop`, exception handler, and public stop/start paths
  through scripted socket, thread, KuBridge, and kernel shims; the same suite
  is compiled and executed in forced library-only and kernel-assisted variants;
- production-loop disconnect injection after `m`, `M`, `g`, `p`, `G`, and `P`
  processing, proving each path closes the generation, releases the request
  borrow, reports transport failure, and returns the target-running state;
- same-thread global-lock interruption and protocol-gate contention proving
  the real exception handler returns without spinning, leaves the context and
  stopped state untouched, releases only ownership it acquired, and chains
  the predecessor exactly once;
- stop/start exception-tail interleaving, bounded restart timeout, immediate
  next-primary admission, socket-cancel-before-join ordering, stopped-state
  normalization only after patch obligations clear, and suppression of a
  shutdown-induced synthetic trap while a connected fd is still published;
- connected receive and send cancellation races proving close blocks a second
  owner, shutdown wakes the exact operation, and storage/descriptors remain
  live until both packet I/O and whole-protocol ownership drain;
- an intentional connected-HUP shutdown interleaving proving the server retires
  the descriptor without opening a listener or synthesizing a target trap; and
- a deterministic 1,000-generation stress run with three concurrent
  protocol/fault workers plus console pressure, checking exclusive ownership,
  guard quiescence, reconnect publication, and bounded shutdown drain;
- File-I/O literal-C/attachment fuzzing and a transition gate proving one T02
  only for real all-stop while the synthetic context fails closed; and
- per-slot fake exception-handler install, partial rollback, exact restore,
  restore failure, retry, publication-before-return callback interleaving,
  terminal generation-reuse rejection, zero-initialized teardown, and
  immutable late-dispatch predecessor tokens.

## Remaining hardware gates

1. Repeat the host malformed/escaped/exact-maximum RSP matrix over a real Vita
   connection, including disconnects during `m`, `M`, `g`, `p`, `G`, and `P`.
2. Verify client admission, candidate cancellation, Ctrl-C, detach, reconnect,
   connected receive/send cancellation, whole-protocol exclusion, an
   intentional connected HUP, and the host-modeled 100-generation alternating
   ACK/no-ack stopped-RST recovery sequence on Vita. Treat title relaunch as a
   separate lifecycle operation and wait two seconds after kill before
   relaunch.
3. Run the deterministic reconnect/fault/console/shutdown matrix for an
   equivalent long-duration window on retail hardware and confirm bounded
   cancellation latency, no launch lock, no stale stopped target, and no
   descriptor or thread leak.
4. Install alongside a known user exception handler, prove per-type chaining,
   then prove exact slot restoration and late default redispatch at terminal
   shutdown. Do not attempt dynamic unload.
5. Trigger a nested exception with a real predecessor and verify it is invoked
   once without deadlock.
6. Treat NULL-predecessor nested behavior as blocked until the opt-in fatal
   trampoline has its dedicated destructive hardware test.
7. Add durable live `M` rollback storage before claiming arbitrary memory
   writes are transactionally recoverable.
8. Add a KuBridge/kernel dispatcher-lifetime fence before supporting safe
   runtime reset or unload of an injected debugger module.
9. Route remote File-I/O Ctrl-C through a real saved exception context and
   coherent all-stop before enabling T02 for `uvdb_remote_syscall()`.

# DebugNet owner lifecycle

> **Ownership requirement:** the thread that calls `uvdb_debugnet_start()` owns
> the logger. Call it from the application's long-lived main thread unless an
> intentionally shorter logging lifetime is desired.

The optional UDP logger has a bounded queue and a dedicated sender thread. A
sender blocked forever on its semaphore can interfere with application teardown
when an application omits `uvdb_debugnet_stop()`. The stream is therefore owned
by the thread which successfully calls `uvdb_debugnet_start()`.

The implementation registers VitaSDK's documented
`sceKernelRegisterThreadEventHandler()` for only that thread and only
`SCE_KERNEL_THREAD_EVENT_TYPE_EXIT`. Registration is fail-closed: logging does
not start if the handler cannot be installed.

Two exit paths cover different termination sequences. The first successful
start registers one C `atexit()` handler for the lifetime of the process;
stop/restart cycles never accumulate handlers. Registration is fail-closed and
happens before logger resources are allocated. The installed VitaSDK CRT calls
`.fini_array` destructors, then `exit()`/`__call_exitprocs`, and only afterward
enters `_exit()` and `_free_vita_newlib`. An ordinary return from `main()` can
therefore synchronously quiesce the sender before newlib releases its heap.

The C exit hook cannot assume networking survived application destructors. It
closes the writer gate, marks the owner exited, drops queued datagrams, tells
the sender to skip socket shutdown/close, waits for accepted writers, and joins
the sender with fixed timeouts. It does not force-delete a running thread. If
the lifecycle lock, an accepted writer, or the sender cannot be positively
quiesced in time, the hook calls `sceKernelExitProcess(0)` as a narrowly scoped
last resort. That bypasses the remaining CRT cleanup instead of allowing
newlib to unmap the queue beneath a live sender.

The exact-owner thread-event callback handles an owner thread ending while its
process remains alive and provides a best-effort fallback for SceShell close.
Direct `_Exit()`, `abort()`, `sceKernelExitProcess()`, and crashes may bypass or
preempt both graceful mechanisms and are not guaranteed shutdown paths. When
the callback is delivered, it is deliberately minimal: atomic stores,
writer-gate closure, and at most one semaphore signal. It never waits,
allocates, frees, takes a logger lock, closes a socket, or deletes a kernel
object. The sender abandons queued datagrams and returns. If the owner was a
shorter-lived helper thread and the process remains alive, another thread can
call `uvdb_debugnet_stop()` to join/delete the ended sender and reclaim its
queue and semaphore before restarting the logger.

Each registered handler receives a nonzero, monotonically changing generation
token. Cleanup invalidates that token before unregistering the event or deleting
callback-visible resources. A callback which was queued before unregister but
runs afterward therefore cannot signal a new session, even if the new owner has
the same thread UID. The active-callback counter is process-wide and is never
reset between sessions. A second per-session `DISARMED`/`ARMED`/`ACTIVE` gate
protects semaphore lifetime and permits only one emergency signal when C exit,
owner exit, and explicit stop race.

Cleanup treats `SCE_KERNEL_ERROR_UNKNOWN_THREAD_EVENT_ID` as confirmation that
no registered object remains. Every other unregister failure retains the event
UID so a later `uvdb_debugnet_stop()` can retry safely. Socket close follows the
same rule: the descriptor is retained unless `sceNetSyscallClose()` succeeds,
and the logger does not return to `STOPPED` while that descriptor remains.

Thread cleanup records separate `CREATED`, `STARTED`, and `ENDED` phases. A
created thread whose start failed is deleted without waiting. Once
`sceKernelWaitThreadEnd()` succeeds, a later deletion retry does not wait a
second time. If start fails and the dormant thread object cannot initially be
deleted, the independent socket, semaphore, and queue are released immediately;
only the failed kernel handle remains retryable. A rare kernel cleanup failure
can therefore leave the logger in an internal cleanup state after
`uvdb_debugnet_start()` returns an error. Before retrying that initial start,
call `uvdb_debugnet_stop()` once to finish any retained-handle cleanup.

Normal explicit stop retains the previous behavior: it closes the writer gate,
waits for in-flight producers with a fixed timeout, drains queued datagrams on
a bounded best-effort basis, joins the sender with a fixed timeout, unregisters
the owner event, and frees resources. UDP sends remain `MSG_DONTWAIT`.

## Host coverage

`make host-test-debugnet-lifecycle` compiles the production sender against a
fake Vita kernel and checks:

- owner-event registration is mandatory and registration failure unwinds;
- process-exit registration fails closed before allocating resources and is
  installed exactly once across stop/restart cycles;
- a created worker whose start and first deletion both fail is retained and
  later deleted directly, without incorrectly waiting for an unstarted thread,
  while its independent network resources are released immediately;
- a successful worker wait followed by a failed deletion retries deletion
  without a second wait;
- failed socket closes retain the descriptor and succeed on a later stop;
- the handler targets the exact starting thread and only the exit event;
- the callback only signals the semaphore and performs no blocking/cleanup API;
- a delayed callback from an old generation remains harmless after restart and
  simulated owner-thread UID reuse;
- an early stop timeout leaves the owner callback armed so owner exit can still
  wake the sender;
- a simulated C exit with a full queue drops work, avoids socket calls, and
  joins the sender before the owner callback;
- explicit stop followed by C exit is idempotent;
- lifecycle-lock contention resolves without deadlock; and
- an injected sender-join timeout selects the direct process-exit fallback
  without force-deleting the sender;
- writes fail immediately after owner exit;
- the sender exits and closes its socket;
- a surviving thread can reap the session, including the case where an exited
  owner's event object is already retired;
- the logger can restart with a new owner; and
- ordinary explicit stop still unregisters and releases everything.

## Required retail hardware gate

Build a tiny logger-only VPK from the candidate DebugNet source. It should start
DebugNet from its main thread, emit several datagrams, and deliberately return
or call its normal process-exit path without `uvdb_debugnet_stop()`. The code
which contains the registered exit callback must remain loaded until process
termination; unloading a separately linked module which owns it is unsupported.

1. Launch it and confirm at least one datagram reaches the PC receiver.
2. Close it normally without calling stop.
3. Immediately launch VitaDevDeploy, VitaShell, and the logger VPK again.
4. Repeat at least 20 cycles, including a full queue and a disconnected PC.
5. Confirm no LiveArea/application-launch lock, no lingering network worker,
   and no crash dump.
6. Separately test an owner helper thread exiting while its process stays alive;
   a second thread must be able to call stop and then start a new session.

The host fake proves ordering and bounded callback behavior. Only this retail
gate can prove the complete CRT exit hook, sender join, and forced-close
fallback behavior on firmware 3.65 strongly enough to close the observed crash
and launch-lock regression.

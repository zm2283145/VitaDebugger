# PMU failure and safe-rearm verification matrix

This matrix separates native fake-kernel evidence from retail Vita evidence.
A host pass is required before a hardware run, but it never promotes a row to
hardware-passed.  The PMU transport remains experimental until every required
retail row has its own durable record and independent review.

The native cross-layer fixture is
`tests/host/test_pmu_failure_matrix.c`.  It links the production PMU transport
and Vita TCP sink to an injected bridge/backend, complete fake Cortex-A9 PMU
image, retained owner identity, and socket.  This complements, rather than
replaces, the register-level backend/session suites:

- `tests/host/test_kernel_pmu_backend.c` proves register semantics, complete
  snapshot restoration, conflicts, timeouts, and late-command recovery;
- `tests/host/test_kernel_pmu_session.c` proves lease ownership, deadlines,
  restore obligations, and exact post-restore comparison;
- `tests/host/test_kernel_pmu_profiler_bridge.c` proves the real bridge,
  provider, transport, owner-identity, and compile-gate behavior; and
- `profiler/tests/test_vitaprofiler_tcp_vita.c` proves bounded socket
  operations and cleanup in isolation.

The cross-layer fixture adds the missing assertion that a TCP failure while a
real-event lease is live cannot silently strand, forget, or re-arm PMU state.
The independently default-off
[`VDCP00013` cleanup gate](../../kernel/pmu-profiler-cleanup-gate/README.md)
turns the remaining acceptance rows into five serialized packages with
checksummed baseline/restored/final snapshots. These packages remain
hardware-pending.

## Coverage and promotion status

| Scenario | Native evidence | Required observable result | Retail 3.65 status |
| --- | --- | --- | --- |
| Peer disconnect during a live lease | Cross-layer fixture and hardware stage use a 5 s lease; require successful initial PMU open/read and network start/connect/prelude plus an initial sample authenticated to the same nonzero handle and fixed event/core/lane; then require exact `VP_ERROR_IO`, another authenticated PMU read, successful PMU close, socket close, and network teardown | Incomplete setup, zero/mismatched identities, wrong metadata, and partial cleanup are rejected; socket failure remains visible and cannot be confused with the lease timeout path; PMU stays owned until close; close restores the complete snapshot; only then may a later real event open | Required |
| Stage-5 kill deadline or failed-slot race | Host tests expire the absolute two-second deadline during final slot-C transfer, deliver slot-C bytes followed by FTP `550`, and expose slot C from the companion's pre-send callback | The companion transmits no command after deadline expiry or any observed slot-C payload; FTP timeouts are recomputed for every receive and final response | Required |
| Send deadline expires | Cross-layer fixture injects repeated would-block plus a finite wait; existing TCP tests cover partial-send and late-success variants | Sink reports `SEND_TIMEOUT`, closes its socket, and the PMU lease is still recoverable by exact close or owner-exit cleanup | Required |
| Other send error | Cross-layer fixture injects a non-disconnect I/O error | Native error is retained, socket is closed, and PMU cleanup follows the same fail-closed path | Required |
| Owner process exits without close | Cross-layer fixture and process-event transport tests prove the callback only releases the retained identity once and records terminal proof; watchdog-only restoration survives failure/retry without querying a torn-down UID; lock-contention deferral restores but quarantines; terminal delivery both before and after watchdog observation of an expired lease is explicitly injected | Process callback remains bounded, watchdog restores exactly, the old handle is retired, and re-arm occurs only after exact reference release; active-exit/kill evidence increments only if the callback observed an `ACTIVE`, unexpired lease, so a prior timeout cannot masquerade as process cleanup; uncertain late release never re-arms | Required |
| Controller thread exits while process remains | Cross-layer fixture proves the captured thread gone while the process remains | Same result as process exit; process survival must not keep an exited controller's lease live | Required |
| Competing owner/process/thread | Session, bridge, and cross-layer fixtures attempt concurrent open plus foreign read/close; cleanup-journal tests cover raw host `-42`, exact Vita syscall encoding `0xBFFFFFD6`, neighboring values, and unrelated errors | Open returns exactly provider busy in one of its two approved representations; foreign read/close returns owner error; no PMU callback or live-owner state mutation is performed for the intruder; only the exact busy result permits the bounded same-boot re-arm | Required |
| Registers match but independent restore evidence is pending | Cross-layer fixture restores the fake PMU bytes while retaining backend recovery/restore flags | A new Open services one bounded recovery pass, returns `RESTORE_REQUIRED`, leaves the transport `CLEANUP_REQUIRED`, and cannot advertise or grant re-arm | Host passed; retain as a hardware fault-injection stop condition |
| Lease timeout with owner alive | Bridge and cross-layer fixtures expire a lease without an owner terminal event | Hardware restores exactly, but transport remains `RESTORED_AWAITING_OWNER`; a new real event stays blocked | Required |
| Lease timeout with owner unknown | Cross-layer fixture makes liveness indeterminate before and after exact restore | Unknown is never treated as gone; quarantine and the original handle remain intact | Required |
| Matching explicit close after timeout/late restore | Bridge and cross-layer fixtures echo the exact process, thread, token, generation, and lease token | Close acknowledges already-proved restoration, releases retained identity once, retires the old handle, and enables one new lease | Required |
| Wrong process, thread, token, or generation after restore | Bridge and cross-layer fixtures corrupt owner fields and generation | Acknowledgement is refused; re-arm remains blocked; the valid owner can still finish cleanup | Required |
| Exact restore plus proven owner gone | Bridge and cross-layer fixtures combine independent restore proof with retained-identity terminal proof | Watchdog releases the retained reference once, retires the old generation, and permits a subsequent real-event lease | Required |
| Retained-owner reference release has uncertain result | Cross-layer fixture injects one release failure | Watchdog, close, shutdown, and new Open all return `RESTORE_REQUIRED`; the reference release is never retried blindly; plugin must remain resident | Required as a fail-closed review case; do not deliberately induce on a daily-use unit |
| UID lookup resolves to a different object or immutable identity | Bridge and cross-layer fixtures change the retained-object identity during watchdog observation and immediately before an explicit close release | Object mismatch enters permanent reference quarantine, never proof that the retained owner is safely releasable; re-arm, repeat lookup, blind UID-based release, and unload stay blocked | Required as a refusal case; do not attempt acceptance without a proven release-by-retained-object primitive |

The already archived `0x01`, `0x03`, and `0x10` owner-attended normal-close
runs prove only the immediate-close row.  They do not satisfy any row marked
`Required` above.

## Native gate

Compile the cross-layer fixture with all four experimental host-test gates
set to the numeric value `1`:

```text
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic-errors \
  -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1 \
  -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_TRANSPORT=1 \
  -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=1 \
  -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_SAFE_REARM=1 \
  -Iprofiler/include -Ikernel/include -Ikernel/src \
  kernel/src/pmu_profiler_transport.c \
  profiler/src/vitaprofiler_tcp_vita.c \
  tests/host/test_pmu_failure_matrix.c \
  -o test-pmu-failure-matrix
```

A pass prints exactly:

```text
PASS: PMU/TCP failure recovery and safe-rearm matrix
```

Also run the backend, session, bridge default-off, real-event, safe-rearm, TCP,
and full host suites.  Do not accept this focused fixture as a substitute for
those suites.

## Hardware evidence contract

Use a separate default-off candidate and disposable application for each
failure mode. Do not add these modes to a normal application or companion.
Preserve the fixed application core 0, physical lane 5, reviewed event
allowlist, bounded lease, boot-loaded plugin, physical attendance, and
known-good recovery path from `pmu-hardware-gate.md`.
The normative remaining-gate procedure and binary schema are in the
[`VDCP00013` runbook](../../kernel/pmu-profiler-cleanup-gate/README.md).

Every attempted hardware row needs two durable, checksummed journal phases:

1. Before mutation, record the candidate hashes, build gates, firmware,
   requested event, process/thread identity metadata, owner token/generation,
   complete pre-mutation PMU snapshot, intended fault, and monotonically
   increasing revision. Sync, reopen, validate, and byte-compare the record
   before allowing the kernel open.
2. After cleanup, record all transport return values, TCP failure/status,
   watchdog count, owner-terminal classification, complete post-cleanup PMU
   snapshot, backend recovery/obligation flags, transport state, re-arm count,
   old-handle rejection, and the result of the single planned follow-up open.
   Mark `RESTORE_PROVEN` only when the independent complete snapshot comparison
   and every backend/bridge obligation check pass.

The cleanup record mirrors the compiled C ABI, including four zero alignment
bytes before `samples[]`: handles end at offset 284 and samples begin at 288.
Both C and Python validators reject the obsolete unaligned sample placement.
The current competing-owner retry writes only the explicit
`pmu-cleanup-v2-conflict-r2-{a,b,c}.bin` namespace. Host tests require all 15
stage/slot paths to be unique, ASCII, within the fixed C storage bound, and
mirrored exactly by the producer and Python tools. Hardware decoding records
the exact remote source name plus expected stage and slot; the immutable
historical `pmu-cleanup-v2-conflict-{a,b,c}.bin` names fail that provenance
check and cannot satisfy the retry.

For a refusal row, success means the follow-up open is refused and the exact
restoration/owner obligation remains observable.  For an acceptance row,
success means the first lease has both independent exact-restore proof and an
authenticated terminal-owner proof before the follow-up open succeeds.  A
return code alone is not sufficient evidence for either outcome.

## Hardware sequence

Run the least disruptive cases first and use a fresh boot plus archived journal
slots for each case unless the row explicitly validates same-boot re-arm:

1. competing owner and foreign-handle refusal;
2. lease timeout while the owner is alive, followed by matching close;
3. receiver disconnect followed by matching close;
4. bounded send timeout/error followed by matching close;
5. controller-thread exit, then process exit, each with watchdog cleanup;
6. exact-restore-plus-owner-gone same-boot re-arm; and
7. UID-reuse/fingerprint mismatch refusal and retained-reference quarantine.

Stop immediately on a missing/torn journal, unexpected owner classification,
failed socket cleanup, ambiguous callback timeout, nonzero backend obligation,
snapshot mismatch, unload attempt, or any successful re-arm lacking both
proofs.  Preserve the files and exact binaries, keep the plugin resident, and
recover only through the reviewed boot path.

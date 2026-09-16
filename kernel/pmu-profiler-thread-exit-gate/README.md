# PMU profiler thread-exit hardware gate

`VDCP00012` is a disposable, default-off, one-shot hardware gate for the
safe-rearm transport. It tests only an owner **thread** becoming dormant while
the application stays alive. Its test source contains no explicit process-exit
operation, it never arms an owner-process-exit stage, and it never deliberately
leaves a live lease for a relaunch. Normal application shutdown still follows
the Vita runtime after all gate activity has stopped.

This is the narrower replacement for the unapproved process-exit portion of
`VDCP00011`. A passing result does not authorize process-exit rearm.

## Exact test boundary

After X is pressed, the app:

1. creates and verifies an exclusive durable attempt record before PMU access;
2. pins a worker to application core 0;
3. opens reviewed event `0x10` on physical lane 5 with a 5000 ms lease;
4. runs bounded work, reads a sample, and lets that worker return without
   calling Close;
5. waits until the same process has positively observed the worker terminate;
6. from the still-live main thread, gives Open for reviewed event `0x01` a
   2000 ms proof window, shorter than the abandoned 5000 ms lease, while a
   failure-only cleanup window may continue through 6000 ms;
7. reads and explicitly closes the new lease, deletes the dormant worker, and
   restores the main thread's prior affinity; and
8. writes and verifies a terminal PASS or FAIL record.

The shorter-than-lease re-open deadline distinguishes dormant-owner recovery
from ordinary lease expiry. The durable record contains both the main-thread
poll duration and an upper-bound delta measured from immediately before the
owner's Open call through the successful rearm Open. PASS requires the poll
duration to be at most 2,000,000 microseconds and that upper-bound delta to be
strictly below the original 5,000,000-microsecond lease. On failure, the app
continues bounded recovery polling through six seconds; any late timeout-based
acquisition is explicitly closed but cannot pass the gate. A pass also requires
a new owner token or generation, valid samples for both leases, a successful
exact-restoring Close, worker deletion, and affinity restoration.

Normal exit is enabled after X only when the post-recovery lease was acquired
and its exact-restoring Close succeeded. If quiescence cannot be proven, the UI
enters a hard stop and ignores Circle so this gate cannot accidentally turn its
own failure into an owner-process-exit test. Leave the app open, power the Vita
off fully, and restore/review the candidate at the next boot; do not peel or
force-close the app.

## Build boundary

Use a separate build directory and set every experimental option explicitly:

```text
-DVITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT=ON
-DVITADEBUG_EXPERIMENTAL_HW_DEBUG=OFF
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER=ON
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=ON
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_SAFE_REARM=ON
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_PROCESS_EXIT_GATE=OFF
-DVITADEBUG_PMU_PROFILER_GATE_EVENT=0x01
```

Before considering deployment, run at least:

```text
make host-test-kernel-pmu-profiler-bridge
make host-test-kernel-pmu-profiler-real-events-disabled
make host-test-kernel-pmu-profiler-real-events
make host-test-kernel-pmu-profiler-safe-rearm
make host-test-pmu-failure-matrix
make host-test-pmu-thread-exit-gate-record
```

The matching candidate must contain the terminal-status-before-mutable-metadata
owner classifier and the bounded Open recovery pass. The app is not useful
with the prior failed safe-rearm binary.

## Durable evidence

The two 256-byte records are:

```text
ux0:data/VitaDebugger/pmu-thread-exit-v1-a.bin
ux0:data/VitaDebugger/pmu-thread-exit-v1-b.bin
```

Each slot is created exclusively, checksummed, file-synced, volume-synced,
reopened, validated, and compared byte for byte. Opening the UI performs only
GetInfo and journal inspection; no PMU mutation occurs before X. Any existing,
partial, corrupt, duplicated, or otherwise ambiguous record locks the gate.
Archive both slots and a screenshot before removing anything.

## Hardware procedure and stop rules

This procedure received explicit approval and passed its first retail 3.65
hardware run on 2026-09-15. It remains a disposable diagnostic gate; do not
repeat it merely because it builds or treat the result as process-exit proof.

1. Confirm the reviewed matching kernel candidate and VPK hashes.
2. Confirm both thread-exit journal paths are absent and no profiler is active.
3. With physical recovery access available, install `VDCP00012`, replace the
   companion only if separately approved, and cold reboot.
4. Launch the app and verify `no process-exit stage`, the safe-rearm capability,
   core 0, lane 5, and `Journal: empty`.
5. Press X once. Do not close the app during the bounded run.
6. A success must end with `Thread-exit gate: PASS` and explicitly state that
   no process-exit lease was armed. Preserve the screenshot and both records.

The archived first result is in
[`hardware-results/2026-09-15-first-attempt/`](hardware-results/2026-09-15-first-attempt/README.md).
Its checksummed completion record reports a 251 us re-arm poll and a 3,951 us
owner-open-to-replacement-open bound, safely below the abandoned 5-second
lease. The owner identity changed from token/generation `1/1` to `2/2`, both
samples were valid, and the replacement lease closed with exact restoration.

Any negative result, BUSY after the two-second bound, RESTORE_REQUIRED,
unexpected reboot, freeze, reference quarantine, missing terminal record, or
storage anomaly is a hard stop. Do not retry in the same boot, do not advance
to a process-exit test, and preserve all evidence before restoring the known
good plugin at boot.

## Remaining boundary

Host fake-kernel tests cover liveness, UID reuse, exact restoration, stale
handles, and release quarantine. This gate adds only the missing Vita thread
status/GUID integration observation. It does not test process-exit detection,
process teardown, crash cleanup, hot unload, other firmware, Vita TV, arbitrary
events, arbitrary cores/lanes, or coexistence with another PMU owner.

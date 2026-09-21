# PMU hardware gate

VitaProfiler originally planned to use VitaSDK's public `ScePerf` imports for
application-owned PMU counters. Read-only discovery on retail 3.65 showed that
this route is not available in the tested runtime. VitaProfiler therefore
fails closed instead of calling an unresolved import and now uses the optional
VitaDebugger kernel companion as the development path for hardware counters.

## Retail 3.65 user-mode result

The zero-PMU-call discovery application recorded all of the following:

- `sceSysmoduleLoadModule(SCE_SYSMODULE_PERF)` returned `0x805A1000`;
- loading `vs0:sys/external/libperf.suprx` through the ordinary user module
  loader returned `0x8002D003` (`SCE_KERNEL_ERROR_MODULEMGR_NO_LIB`);
- all six `ScePerf` imports remained unresolved; and
- the unresolved `scePerfArmPmonReset` stub ended in a branch to address zero.

Two earlier attempts which called that unresolved stub produced user-process
prefetch aborts at PC zero. The second attempt proved that checking an import
stub for nonzero bytes is not a valid binding test. The no-call discovery run
then completed safely and preserved the loader/export evidence.

`vp_vita_pmu_owned_init()` consequently returns `VP_ERROR_UNSUPPORTED` on
Vita. `vp_vita_pmu_owned_init_with_ops()` remains available for host tests and
for a future resolver which can prove every target and retain the supplying
module for the entire lease. A caller must not pass raw import-stub addresses
or revive the old nonzero-byte heuristic.

The complete result, dump identifiers, and hashes are recorded in the
[retail 3.65 PMU evidence](../../docs/hardware/profiler-pmu-retail-3.65.md).

## Kernel discovery gate

Kernel ABI v1.13 adds a narrowly scoped, read-only PMU inventory call. It reads
the calling CPU ID, Cortex-A9 identity, `PMCR`, counter-enable, overflow,
selector, cycle-counter, user-enable, and interrupt-enable registers. The call:

- has no target-core or arbitrary-register selector;
- executes no `MCR` instruction and exposes no write operation;
- suspends local interrupts for the short per-core snapshot so the thread
  cannot migrate partway through it;
- resumes interrupts before copying the result to user memory; and
- performs no PMU access at plugin load time.

The probe takes two successful snapshots from the same core and requires the
control registers to match exactly. Cortex-A9 `PMCR.N` must decode to exactly
six programmable event counters. This gate establishes only that bounded PMU
reads are available; it does not enable or configure a counter.

## Isolated mutation gate and host bridge

The disposable session probe has now passed a fixed lane-5, event-`0x00`
software-increment transaction on application cores 0–2 with `17/17` read-back,
exact gate-snapshot restoration, and no retained obligation. See the
[per-core evidence](../../docs/hardware/profiler-pmu-session-gate-3.65.md).

A reviewed bridge adapts that session to VitaProfiler's exact-restore provider
ABI. It retains a global lease through verification and tests normal release,
watchdog timeout, late restoration, failed-acquire orphan cleanup, and competing
owners.

A separate versioned user/kernel transport is now implemented and build-tested
behind default-off CMake options. Its four narrow exports negotiate an exact
ABI, open one owner-bound lease, read one fixed-lane sample, and close with
verified exact restoration. Every request is bound to the calling process and
controller thread through a complete kernel-generated handle. The transport
has no raw-register, core, lane, cycle-counter, or arbitrary-event selector.
The ordinary experimental transport admits only the already-proved software
increment event `0x00`.

A second build switch admits only three additional Cortex-A9 event selections
(`0x01`, `0x03`, and `0x10`) when both that switch and a per-request
acknowledgement are present. An explicit compile value of `0` remains disabled.
The host matrix verifies exact structure/version validation, fixed-lane
configuration, owner isolation, stale-handle rejection, normal restoration,
watchdog expiry, late restoration, failed-acquire orphan cleanup, real-event
allowlisting, and the one-real-attempt-per-boot latch against the fake PMU. The
kernel refuses unload after a real-event attempt so unload/reload cannot reset
that latch. The fake-PMU result alone did not establish hardware behavior;
event `0x01` subsequently passed the separate gate below.

## Bounded real-event hardware gate

The separately built disposable `VDCP00010` probe and matching default-off
kernel candidate are used for this gate, never a normal production build.
Review its generated imports and ARM code before deployment, retain physical
recovery access, and do not run another profiler. The complete build and retail
3.65 runbook is in the
[PMU profiler transport gate](../../kernel/pmu-profiler-gate/README.md). The
gate is intentionally bounded as follows:

1. Use only application core 0 and fixed programmable lane 5. Test one event
   per reboot in this order: `0x01`, `0x03`, then `0x10`. All three
   normal-close events passed separately on 2026-09-15.
2. Require the same completely idle baseline and complete pre-mutation
   snapshot used by the passed software-increment gate. Write and verify a
   checksummed durable `attempted` record before entering the kernel mutation.
3. Use a 250 ms lease. Select the event, zero lane 5, leave PMU interrupts and
   user-mode PMU access disabled, enable only lane 5, and read back every
   selector, type, enable, and control field before running a short fixed
   workload.
4. Record the bounded count and exact transport results. A nonzero delta is
   characterization evidence, not proof of a universal semantic or unit.
5. Restore immediately, then require a zero close result, which the backend
   emits only after its complete saved state passes exact restore verification.
   Archive both durable journal slots before advancing to the next event.
6. After all three normal releases pass, repeat `0x10` once with no explicit
   release and require the bounded watchdog to restore at lease expiry. The
   outer provider must acknowledge the already-restored token before another
   lease is accepted.
7. Any timeout, read-back mismatch, foreign PMU activity, overflow, clock
   regression, journal ambiguity, or restore mismatch is a hard stop. Retain
   the resident recovery path and do not clear or unload an unresolved record.

The hardware-tested candidate keeps its real-event latch boot-scoped. A
separate, default-off safe-rearm candidate is now implemented and host-tested.
It clears the latch only with no active lease, independently verified exact
restoration, a matching owner token and generation, and either a matching
explicit close or positive proof that the retained owner thread object is
dormant/gone. Unknown liveness, UID/object mismatch, or any reference-release
uncertainty restores the PMU state and permanently quarantines re-arm and
unload. Process exit/kill observation is independently compiled only for the
new
[`VDCP00013` serialized cleanup gate](../../kernel/pmu-profiler-cleanup-gate/README.md).
Its callback only performs a bounded one-shot UID lookup, exact retained-object
comparison, and reference release before teardown, then records
process-terminal proof; watchdog context alone dispatches PMU restoration.
Proof survives bounded restore retries without querying a torn-down UID. Lock
contention uses one nonblocking atomic handoff and restores into permanent
quarantine rather than guessing that a late UID release is safe. The
candidate is not promoted until all five `VDCP00013` stages pass on hardware.

The three owner-attended retail-3.65 runs passed events `0x01`, `0x03`, and
`0x10`: all transport calls succeeded, sample values were 56, 23, and 97 on
core 0/lane 5, and every durable completion record proved exact restoration.
The reviewed artifacts, journal slots, and screenshots are in the
[event-0x01](../../kernel/pmu-profiler-gate/hardware-results/2026-09-15-event-01/README.md),
[event-0x03](../../kernel/pmu-profiler-gate/hardware-results/2026-09-15-event-03/README.md),
and [event-0x10](../../kernel/pmu-profiler-gate/hardware-results/2026-09-15-event-10/README.md)
records. These are bounded samples, not approval for continuous sampling. The
narrower dormant-owner-thread safe-rearm gate now passes; disconnect and
whole-process exit/crash cleanup still require their hardware gates. This plan
does not use or claim support for the unavailable public `ScePerf` route.

## Serialized `VDCP00013` cleanup matrix

The independently default-off `VDCP00013` candidate is the only approved
runner for the remaining lifecycle rows. Its authoritative build and evidence
details are in the
[cleanup-gate runbook](../../kernel/pmu-profiler-cleanup-gate/README.md).
Run exactly one package at a time, in this order, without rebooting between a
stage's cleanup and its re-arm proof:

1. stage `1`: competing-owner refusal and first-owner exact close;
2. stage `2`: 250 ms timeout/watchdog restore and matching acknowledgement;
3. stage `3`: production TCP sink receiver disconnect and authenticated close;
4. stage `4`: owning-process normal return and same-boot relaunch; and
5. stage `5`: approved Vita Companion `kill VDCP00013`, exercising the
   SceShell `.kill` callback, and same-boot relaunch.

Every build requires these explicit flags:

```text
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER=ON
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=ON
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_SAFE_REARM=ON
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_PROCESS_EXIT_GATE=ON
-DVITADEBUG_PMU_CLEANUP_GATE_STAGE=<1..5>
```

Stage 3 additionally requires all four
`VITADEBUG_PMU_CLEANUP_HOST_{A,B,C,D}` octets and
`VITADEBUG_PMU_CLEANUP_PORT=18196`; a loopback host is rejected. Start its
one-shot host peer before pressing X:

```powershell
py -3 tools/pmu_disconnect_receiver.py `
  --bind 0.0.0.0 --port 18196 `
  --evidence .\evidence\disconnect-receiver.json
```

Before each stage, capture the exact device/firmware identity, active-title and
installed-title inventory, plugin path/hash, Git commit/tree, stage VPK hash,
host timestamp, and absence of that stage's three journal paths. Verify and
install only through the approved signed deployment client:

```powershell
py -3 -m host.vitadevdeploy verify $StageVpk
py -3 -m host.vitadevdeploy deploy $StageVpk `
  --vita 10.1.1.217 --private-key $PrivateKey --action install_launch `
  --output $StageDeployEvidence
```

Retrieve each immutable journal slot into a stage-specific evidence directory,
hash it, and decode it without deleting the device copy:

```powershell
curl.exe --fail --show-error `
  "ftp://10.1.1.217:1337/ux0:/data/VitaDebugger/$Record" `
  --output ".\evidence\$Record"
Get-FileHash ".\evidence\$Record" -Algorithm SHA256
py -3 tools/decode_pmu_cleanup_record.py `
  ".\evidence\$Record" --output ".\evidence\$Record.json"
```

Use the exact stage/suffix loop and hash-addressed immutable archive naming
from the cleanup-gate runbook. Device journal slots are never deleted or
reused during this matrix; each later stage uses its distinct paths.

An accepted terminal record contains idle baseline, restored, and final
snapshots that match byte-for-byte; zero recovery/restore/reference-uncertainty
flags; an action-specific cleanup result; a later bounded open/read/close; and
an increased re-arm count. Stage 4 also requires an increased normal-exit
cleanup count with no kill-count change; stage 5 requires an increased
SceShell-kill cleanup count with no normal-exit-count change across the same
boot.

Stop the whole matrix on a device/identity mismatch, missing or conflicting
journal, checksum/schema failure, cleanup failure, PMU snapshot mismatch,
uncertain state, unexpected process-return path, reboot/power loss, or re-arm
outside the bound. Preserve evidence and do not retry, unload, erase records,
or advance. The matrix is PMU-only: no debugger/GDB attach, external-attach
listener, loader/injection experiment, system-process target, unrelated
hardware work, or concurrent test is authorized. Unrestricted counters remain
disabled regardless of the outcome.

## Remaining promotion gates for live counters

The transport remains separate from the read-only discovery call. Event `0x01`
now has both one durable immediate-close sample and a 75-read lease from a
real-world VitaGL-based 3D application.
Before unrestricted production PMU sampling, the new lifecycle candidate must
still prove the following on hardware:

1. retain the already-proved exact snapshot/restore scope for every field the
   selected lane changes;
2. bind each operation to a known CPU and prevent migration during the critical
   section;
3. configure only the current `0x01`, `0x03`, and `0x10` Cortex-A9 allowlist;
4. read back every write and fail closed on disagreement;
5. restore the exact prior state on normal release, error, disconnect, process
   exit, and lease timeout;
6. retain restoration obligations until cleanup succeeds; and
7. refuse coexistence when ownership cannot be established safely.

The host/fake-kernel matrix now covers all seven conditions, including timeout,
read/config/restore errors, late completion, receiver-send failure mapped to
authenticated close, process/thread exit, ownership conflict, UID/object
mismatch, and one-shot quarantine after uncertain reference release. The
original `VDCP00011` multi-stage lifecycle attempt exposed a process-exit
safety problem and was stopped. Its replacement `VDCP00012` thread-only gate
passed dormant-owner recovery and same-boot re-arm with a fresh identity and
exact-restoring close. `VDCP00013` now provides separate conflict, timeout,
receiver-disconnect, normal-exit, and abrupt-exit packages with complete
baseline/restored/final PMU snapshots; every row remains hardware-pending.

The real-world application gate proves that bounded samples can enter the
existing named event ring and VitaProfiler TCP transport. Sony's unavailable
`usbhostfs`/trace-host pipeline is not required and is not a project target.

## Firmware scope

The current PMU evidence is for one retail handheld running 3.65. An Enso_ex
version spoof may make the public version API report 3.74; that does not change
the actual tested firmware baseline. The same PMU gate has not yet been run on
Vita TV. Firmware 3.60 and all other firmware versions remain untested.

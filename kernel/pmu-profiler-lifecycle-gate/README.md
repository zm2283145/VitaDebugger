# PMU profiler lifecycle hardware gate

`VDCP00011` is an archived, disposable two-launch design for the PMU
transport's recovery and safe-rearm state machine. Its first retail 3.65 run
failed closed at the owner-thread-exit stage, before process exit was armed.
The corrected implementation was then moved to the smaller, process-exit-free
`VDCP00012` gate, which passed. Do not deploy this full gate while its process-
exit path remains unresolved and default-off.

See the [stopped first attempt](hardware-results/2026-09-15-first-attempt/README.md)
and the [passing thread-only replacement](../pmu-profiler-thread-exit-gate/hardware-results/2026-09-15-first-attempt/README.md).

The gate keeps the already reviewed boundary: application core 0, physical
programmable lane 5, and only events `0x01`, `0x03`, and `0x10`. It adds no
raw-register, arbitrary-event, core, lane, interrupt, or cycle-counter API.

## Build boundary

Use a separate build directory and set every experimental option explicitly:

```text
-DVITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT=ON
-DVITADEBUG_EXPERIMENTAL_HW_DEBUG=OFF
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER=ON
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=ON
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_SAFE_REARM=ON
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_PROCESS_EXIT_GATE=ON
-DVITADEBUG_PMU_PROFILER_GATE_EVENT=0x01
```

`VITADEBUG_EXPERIMENTAL_PMU_PROFILER_PROCESS_EXIT_GATE` is independently
default-OFF. Enabling safe rearm no longer packages this unresolved two-launch
process-exit artifact unless that additional switch is explicitly enabled.

The safe-rearm switch is `OFF` by default and CMake rejects it unless both the
PMU transport and real-event gate are enabled. The public PMU ABI remains
version 1; the candidate advertises the additive
`VD_KERNEL_PMU_PROFILER_CAP_SAFE_POST_RESTORE_REARM` capability instead of the
single-real-attempt-per-boot capability.

Before considering deployment, run these host gates and review the generated
kernel imports and ARM code:

```text
make host-test-kernel-pmu-session
make host-test-kernel-pmu-backend
make host-test-kernel-pmu-profiler-bridge
make host-test-kernel-pmu-profiler-real-events-disabled
make host-test-kernel-pmu-profiler-real-events
make host-test-kernel-pmu-profiler-safe-rearm
make host-test-pmu-lifecycle-gate-record
```

The candidate retains the exact owner thread object through VitaSDK's kernel
GUID reference API. A liveness observation can authorize re-arm only after
exact PMU restoration and either a matching explicit close or positive proof
that the retained owner is dormant/gone. `UNKNOWN`, an object-pointer mismatch,
or any temporary/final GUID-release uncertainty never authorizes re-arm. The
transport restores an active PMU lease and then permanently quarantines that
boot; it never retries an uncertain reference decrement and refuses unload.
The final release also re-resolves the numeric UID and compares its object
pointer with the retained identity, so even a late authenticated close cannot
decrement a replacement object after UID reuse.

## Durable evidence

Five fixed 512-byte journal slots record the two-launch sequence:

```text
ux0:data/VitaDebugger/pmu-lifecycle-v1-a.bin
ux0:data/VitaDebugger/pmu-lifecycle-v1-b.bin
ux0:data/VitaDebugger/pmu-lifecycle-v1-c.bin
ux0:data/VitaDebugger/pmu-lifecycle-v1-d.bin
ux0:data/VitaDebugger/pmu-lifecycle-v1-e.bin
```

Every slot is created exclusively, checksummed, file-synced, volume-synced,
reopened, validated, and compared byte for byte. Opening the UI performs no
PMU mutation. Any invalid slot, duplicate revision, unexpected intermediate
state, partial record, or I/O error locks the gate. Pull and archive every slot
before removing anything; never erase ambiguous evidence merely to retry.

## Archived staged retail 3.65 procedure

This procedure records the original design for review. It is not currently
authorized for another hardware run. Keep the process-exit build switch off;
use only separately reviewed, smaller gates for the remaining lifecycle paths.

1. Confirm no other PMU user or profiler is running and all five lifecycle
   journal paths are absent.
2. Install the reviewed `VDCP00011` VPK, replace the boot-loaded companion with
   the exact matching safe-rearm `.skprx`, and perform a full reboot.
3. Launch the gate. Verify ABI 1, the safe-rearm capability, core 0, lane 5,
   lease range 250--5000 ms, and `Journal: empty; first launch ready`.
4. Press X once. The first launch performs, in order:
   - an event-`0x01` read and matching explicit exact-restoring close;
   - event-`0x03` ownership contention from a second thread, which must return
     `BUSY` without acquiring a lease;
   - event-`0x03` lease expiry, a 600 ms wait, a read which must report the
     watchdog-restored state, and a matching close acknowledgement;
   - an event-`0x10` 5000 ms lease owned by a worker which exits without close,
     followed by a different thread acquiring within 2000 ms. This deadline is
     shorter than the lease, so a pass proves owner-exit cleanup rather than
     ordinary timeout fallback; and
   - a final event-`0x03` 5000 ms lease, read, and durable `process-exit armed`
     record. The application then exits deliberately without close.
5. Do not reboot. Relaunch `VDCP00011`; it must show the process-exit resume
   stage. Press X once. A new event-`0x10` lease must open within 2000 ms, read,
   close with exact restoration, and produce `Lifecycle gate: PASS`.
6. Take a screenshot, exit, pull all five records, and hash the VPK, `.skprx`,
   and journals before independent review.

Any negative close, missing completion record, unexpected successful competing
open, inability to re-arm within the shorter-than-lease deadline, freeze,
reboot, reference quarantine, or restore-required result is a hard stop. Keep
the candidate resident until power-off, preserve all evidence, then restore the
known-good plugin at boot.

## Coverage boundary

This gate covers matching explicit close, ownership conflict, timeout/watchdog
restoration, owner-thread exit, owner-process exit, and repeated same-boot
leases. It does not deliberately inject a PMU write/readback failure on
hardware; fake-kernel tests retain that obligation and cover late completion,
orphan restore, ambiguous errors, exact-restore refusal, and release
quarantine. Network receiver disconnect is not a kernel event: the profiler
adapter must map a send failure to the same authenticated `Close` path, while
an abruptly terminated thread/process is covered by retained-owner liveness.

A passing run would promote the implementation from host-complete to this
single retail-3.65 hardware configuration only. Other firmware, Vita TV,
continuous sampling, arbitrary events, and coexistence with an unknown PMU
owner remain outside this gate.

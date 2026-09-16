# PMU profiler transport hardware gate

`VDCP00010` is a disposable, default-off hardware gate for VitaDebugger's
versioned PMU profiler transport. It is not a general PMU utility and must not
ship in a normal kernel build.

The gate can request exactly one of three reviewed Cortex-A9 events:

| Build value | Event |
| --- | --- |
| `0x01` | L1 instruction-cache miss/refill |
| `0x03` | L1 data-cache miss/refill |
| `0x10` | branch mispredict |

Every request is fixed to application core 0, physical programmable lane 5,
and a 250 ms lease. The public ABI has no core, lane, raw-register, arbitrary
event, cycle-counter, or generic CP15 selector. The kernel accepts a real event
only when both experimental build switches and the request acknowledgement are
present. It permits one real-event attempt per boot and refuses module unload
after that attempt, so unload/reload cannot reset the latch.

## Build boundary

Use a separate build directory and set all options explicitly:

```text
-DVITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT=ON
-DVITADEBUG_EXPERIMENTAL_HW_DEBUG=OFF
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER=ON
-DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=ON
-DVITADEBUG_PMU_PROFILER_GATE_EVENT=0x01
```

Change only the final event value for the later gates. The CMake defaults for
both PMU profiler options are `OFF`; with those defaults, no PMU backend,
transport, or `VDCP00010` VPK is built. VFP snapshot remains enabled in the
candidate only to preserve the capability of the previously tested companion;
experimental hardware debug remains disabled.

Do not deploy a candidate merely because it builds. Before each hardware run:

1. Run the fake-kernel transport matrix and journal validator.
2. Review the generated export table and ARM code. The gate executable itself
   must contain no `MCR` or `MRC` instructions.
3. Hash the kernel plugin and gate VPK and record the exact build options.
4. Keep the known-good kernel plugin available for recovery and be physically
   present at the Vita. Never hot-load or hot-unload this candidate.

## Durable evidence latch

The gate uses two fixed 256-byte journal slots:

```text
ux0:data/VitaDebugger/pmu-profiler-gate-v1-a.bin
ux0:data/VitaDebugger/pmu-profiler-gate-v1-b.bin
```

Before the first kernel PMU call, slot A is created exclusively with an
`attempted` record, checksummed, file-synced, volume-synced, reopened, validated,
and compared byte for byte. If any step fails, the gate makes no PMU request.
After the immediate close/restore call, slot B receives a similarly synced and
verified `complete` or `restore-required` record.

Opening the app performs no PMU register access. Any existing slot, invalid
slot, torn record, equal-revision conflict, or journal I/O error locks the X
button. Pull and archive both files before removing them; never delete ambiguous
evidence simply to make the gate run again.

## Retail 3.65 procedure

Only retail firmware 3.65 is in scope for this first gate. Test one event per
full reboot, in this order: `0x01`, then `0x03`, then `0x10`.

1. Confirm no other profiler or PMU experiment is running. Archive both journal
   slots from any earlier run and verify they are absent.
2. Install the reviewed gate VPK and replace the boot-loaded companion with the
   matching reviewed `.skprx`. Reboot fully; do not load the plugin at runtime.
3. Launch `VDCP00010`. Confirm the displayed ABI, capabilities, core 0, lane 5,
   lease range, compiled event, and `Journal: empty and ready`. If any value is
   wrong, press Circle and stop.
4. Press X exactly once. The gate durably records the attempted state, opens a
   250 ms lease, runs a bounded workload, reads one sample, and requests exact
   restoration before any further display output.
5. A valid normal run requires `open=0`, `read=0`, `close=0`, successful
   affinity restoration, core 0/lane 5 sample metadata, a verified completion
   journal, and `Gate result: PASS`. A zero count is allowed at this stage; the
   event's universal semantic or unit is not being claimed.
6. Exit, pull both journal files, preserve a screenshot and artifact hashes,
   and shut down or reboot. Do not run a second event in the same boot.
7. Independently review the result before building the next event candidate.

### Completed normal-close events

The first owner-attended run passed on retail firmware 3.65 on 2026-09-15.
All four transport results were zero, the core-0/lane-5 sample value was 56,
and the second durable journal slot recorded `COMPLETE` with
`RESTORE_PROVEN | SAMPLE_VALID | PASS`. The exact binaries, both journal slots,
and unedited screenshots are retained in the
[event-0x01 evidence directory](hardware-results/2026-09-15-event-01/README.md).

Two later owner-attended fresh-boot runs passed the remaining allowlisted
normal-close events. Event `0x03` returned 23, event `0x10` returned 97, and
both completion journals recorded `RESTORE_PROVEN | SAMPLE_VALID | PASS` with
exact restoration. Their reviewed artifacts are retained in the
[event-0x03](hardware-results/2026-09-15-event-03/README.md) and
[event-0x10](hardware-results/2026-09-15-event-10/README.md) evidence
directories. All three normal-close event gates now pass; lifecycle and
ownership recovery gates remain.

Any reboot, freeze, timeout, journal ambiguity, negative close, missing
completion record, `restore-required` state, or inability to prove the plugin
remained resident is a hard stop. Preserve both records and the exact binaries,
power-cycle if necessary, restore the known-good plugin at boot, and do not
advance to another event.

## What this gate does not prove

A passing run proves only one bounded sample and the transport's immediate
exact-restore result for one event on one retail 3.65 boot. It does not yet
approve continuous PMU sampling, cycle counting, arbitrary event selection,
concurrent owners, another firmware, Vita TV, or production use.

After all three normal-close gates pass, separate disposable tests are still
required for lease-expiry/watchdog restoration, process exit, client
disconnect, competing ownership, and safe repeated leases/re-arm. Bounded
multi-read `0x01` sampling has separately passed in a real-world VitaGL-based
3D application. The remaining tests must keep the same fixed core/lane,
durable journal, one-attempt-per-reboot rule, and fail-closed recovery
procedure.

The hardware-tested build deliberately retains its boot-scoped latch. A
separate default-off safe-rearm candidate now implements the stricter design:
no active lease, independently verified exact restoration, matching owner and
generation, plus either matching explicit close or positive retained-owner
exit proof. It permanently quarantines unknown liveness, UID/object mismatch,
or uncertain reference release. Its narrower dormant-owner-thread gate now
passes on retail 3.65; process-exit/crash and the remaining lifecycle paths are
still pending. See the
[thread-exit result](../pmu-profiler-thread-exit-gate/hardware-results/2026-09-15-first-attempt/README.md)
and [two-launch lifecycle gate](../pmu-profiler-lifecycle-gate/README.md).

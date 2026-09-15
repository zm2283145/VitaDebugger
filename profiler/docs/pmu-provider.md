# Guarded CPU/PMU providers

`vitaprofiler_pmu.h` defines a fail-closed ownership boundary for CPU
performance-monitor counters. It contains no raw ARM CP15 access and supports
two distinct provider contracts:

- an **exact-restore** provider saves every field it changes and restores the
  prior state on release; and
- an **owned-reset** provider may destructively initialize one PMU context only
  after the application explicitly grants it complete ownership. Release stops
  and resets that context to a clean baseline; it does not pretend to recreate
  inaccessible prior state.

The default session configuration accepts only exact-restore providers. A
caller must set `VP_PMU_CONFIG_FLAG_ALLOW_OWNED_RESET` before an owned-reset
provider can run.

## Public Vita ScePerf adapter

`vitaprofiler_pmu_vita.h` supplies an injectable user-mode adapter which models
the public VitaSDK `psp2/perf.h` functions:

- `scePerfArmPmonReset`
- `scePerfArmPmonSelectEvent`
- `scePerfArmPmonSetCounterValue`
- `scePerfArmPmonStart`
- `scePerfArmPmonGetCounterValue`
- `scePerfArmPmonStop`

VitaSDK exposes no matching call to read the old event selector or the old
start/stop state. Consequently, this adapter deliberately advertises
`VP_PMU_PROVIDER_FLAG_OWNED_RESET`, not `EXACT_RESTORE`. It never touches CP15,
does not call undocumented kernel PMU functions, and needs no VitaDebugger
kernel companion.

The direct Vita entry point currently returns `VP_ERROR_UNSUPPORTED`. Retail
3.65 hardware proved that linking `ScePerf_stub` does not bind the imports,
`SCE_SYSMODULE_PERF` cannot be loaded through the documented sysmodule call,
and direct user loading of `libperf.suprx` fails. Calling an unresolved stub
branches to address zero. The injectable form remains for native tests and a
future resolver which can prove all callback targets and retain their module.

The ownership acknowledgement means all of the following are true:

- the target thread belongs to this application;
- Razor, another profiler, and application code are not using that thread's
  PMU state before or during the session;
- resetting the thread's complete PMU state is acceptable; and
- the application will keep both the provider state and target thread alive
  until release succeeds.

Do not add the acknowledgement merely to silence an error. If the prior state
matters, no safe provider is currently available through the public API.

### Counter mapping

The adapter uses documented programmable event counters instead of guessing a
special raw PMCCNTR index. If cycle sampling is requested, programmable counter
0 selects `SCE_PERF_ARM_PMON_CYCLE_COUNT` (`0x11`). Requested event lanes then
use counters 1 through 4. Without the cycle lane, requested events use counters
0 through 3. The adapter therefore uses at most five counters.

`scePerfArmPmonGetCounterValue` returns 32 bits. Samples are zero-extended into
VitaProfiler's 64-bit record fields without guessing how many wraps occurred
between reads. Consumers which calculate deltas must use modulo-2^32 arithmetic
and choose a sampling interval suitable for the selected event.

### Integration status

There is intentionally no copy-and-paste Vita example for this backend yet.
`vp_vita_pmu_owned_init_with_ops()` may be used only after a platform resolver
has supplied verified callable targets and a retained module lifetime. Read
with `vp_pmu_session_read()`. End with `vp_pmu_session_end()` and retry the same
call if it returns `VP_ERROR_RESTORE_REQUIRED`. A failed acquire can leave no
`vp_pmu_session` to own cleanup; in that narrow case inspect
`vp_vita_pmu_owned_get_status()` and call
`vp_vita_pmu_owned_retry_orphan_cleanup()` until it succeeds before discarding
the provider state or exiting the thread.

Only one owned-reset lease is allowed per linked copy of the adapter. This is a
deliberately conservative conflict guard, not system-wide exclusivity. If
separate user modules each statically link their own adapter copy, the
application must coordinate one shared PMU owner across those modules.

The normal Vita cross-build proves the fail-closed adapter and portable
provider compile for Vita. The separate zero-call discovery target includes
VitaSDK's `psp2/perf.h` and links the six adapter-used stubs solely to inspect
their runtime loader state. The exact hardware evidence is in
[PMU hardware gate](pmu-hardware-gate.md).

The optional companion now has a reviewed host/build-only exact-restore
adapter in `kernel/src/pmu_profiler_bridge.c`. It binds one owner, application
core, fixed physical lane 5, and 250–5000 ms lease; retains a linked-copy
global lease through restore verification; and exposes watchdog and
failed-acquire cleanup. It is not compiled into the normal Vita kernel target
and has no production kernel export.

Software increment (`0x00`, `cpu.pmu.software_increment`) is its only default
selection. A deliberately small real-event catalog is prepared for later
hardware gating:

- `0x01` — `cpu.pmu.icache_miss`
- `0x03` — `cpu.pmu.dcache_miss`
- `0x10` — `cpu.pmu.branch_mispredict`

Those three selections require both
`VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=1` at build time and
`VD_PMU_PROFILER_BRIDGE_CONFIG_ALLOW_REAL_EVENTS` in that bridge instance.
Undefined and explicitly `=0` builds reject the acknowledgement itself;
experimental `=1` builds still reject real events unless the caller opts in.
The backend independently rechecks the same ID/code pairs before its first
write. Cycles, multiple lanes, and every other event remain rejected.

`vdPmuProfilerBridgePrepareEvent()` validates one enabled selection, registers
its stable name in an unsealed VitaProfiler dictionary, and returns the
matching one-lane `vp_pmu_config` and `vp_pmu_name_ids`. It performs no PMU
access. Host tests take that result through provider acquire/read, then
`vp_pmu_record_sample()`, and verify the ordinary named counter in the profiler
ring. This is build-time integration evidence only, not a usable user/kernel
transport; that boundary still needs an independently reviewed versioned ABI.

## Generic provider contract

An exact-restore platform provider must implement three operations:

- `acquire`: exclusively reserve the requested cycle/event lanes, save every
  control, selector, event-type, count, overflow, and enable state it may
  change, establish the required execution/core ownership, and return an opaque
  lease token. A failed acquire must leave no changed state behind;
- `read`: verify the lease and execution context before returning exactly the
  requested lanes; and
- `release`: disable the provider's changes and restore the complete saved state
  before reporting success.

An owned-reset provider follows the same lease and read rules, but `acquire`
establishes a known baseline in state already granted by the application and
`release` returns it to that baseline. Its provider flag and the matching
per-session opt-in are both mandatory.

The session object will not overwrite an active lease. If `release` fails, the
lease remains active with `restore_pending` set. Reads stop and begin remains
blocked until a retry succeeds. There is intentionally no force-forget API that
could discard a cleanup obligation.

Provider callbacks and their user data must outlive the session. Sessions are
single-owner and not internally synchronized. Zero-initialize a session before
its first begin. A provider ABI mismatch returns `VP_ERROR_UNSUPPORTED`;
unknown flags, missing operations, or inconsistent counter masks are rejected.

## Recording a provider sample

Map requested lanes to names registered by the application:

```c
struct vp_pmu_name_ids names = {
    .cycles = cycle_name_id,
    .events = {l1_miss_name_id, 0, 0, 0},
};
struct vp_pmu_sample sample;

if (vp_pmu_session_read(&pmu_session, &sample) == VP_RESULT_OK)
    vp_pmu_record_sample(&profiler, &sample, &names);
```

The helper emits ordinary named `VP_EVENT_COUNTER` records with
`VP_EVENT_FLAG_RAW_VALUE`. Multi-lane publication is non-transactional under
ring pressure, like the existing Vita snapshot helpers. The desktop trace
viewer resolves and displays the application-supplied names.

The native suite uses injected providers to verify exact-restore selection,
explicit owned-reset opt-in, physical counter mapping, process-local conflict
handling, raw error reporting, failed-acquire cleanup, retained release
obligations, and successful retry. The Vita cross-build proves the fail-closed
adapter compiles for the target. Retail 3.65 read-only discovery rejected the
direct ScePerf route. Kernel PMU inventory and the isolated lane-5 mutation
gate are hardware-tested, while the host-only kernel/provider bridge passes
normal release, timeout, late-restore, failed-acquire cleanup, default-off
real-event gating, the three-event catalog, stable dictionary names, and named
ring publication. The default-off transport has since passed one bounded
event-`0x01` sample and exact close/restore on retail 3.65. Separate fresh-boot
gates subsequently passed `0x03` and `0x10` with exact restoration. A
Render96EX lease produced 75 bounded `0x01` reads and also closed cleanly. Vita
timeout/disconnect/process-exit/conflict cleanup remains disabled and unproved
on hardware.

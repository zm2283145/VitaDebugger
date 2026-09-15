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

## Promotion gates for live counters

The next implementation must be a separate lease-protected session ABI, not an
extension which silently mutates the discovery call. Before VitaProfiler can
advertise PMU samples, that session must prove all of the following on hardware:

1. snapshot every selector, event type, count, overflow, enable, interrupt,
   user-access, and global-control field it may change;
2. bind each operation to a known CPU and prevent migration during the critical
   section;
3. configure only a bounded allowlist of documented Cortex-A9 events;
4. read back every write and fail closed on disagreement;
5. restore the exact prior state on normal release, error, disconnect, process
   exit, and lease timeout;
6. retain restoration obligations until cleanup succeeds; and
7. refuse coexistence when ownership cannot be established safely.

After that session passes, its samples can enter the existing named event ring
and VitaProfiler TCP/file transport. Sony's unavailable `usbhostfs`/trace-host
pipeline is not required and is not a project target.

## Firmware scope

The current PMU evidence is for one retail handheld running 3.65. An Enso_ex
version spoof may make the public version API report 3.74; that does not change
the actual tested firmware baseline. The same PMU gate has not yet been run on
Vita TV. Firmware 3.60 and all other firmware versions remain untested.

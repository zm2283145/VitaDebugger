# VitaProfiler PMU discovery on retail 3.65

Date: 2026-09-14

This record separates three different questions which must not be conflated:

1. whether VitaSDK's user-mode `ScePerf` imports are callable;
2. whether the Cortex-A9 PMU registers can be read safely by VitaDebugger's
   kernel companion; and
3. whether VitaProfiler may configure counters and later restore their exact
   prior state.

On the tested retail 3.65 system the answers are **no**, **yes**, and **not yet**
respectively. Enso_ex version spoofing made
`sceKernelGetSystemSwVersion()` report 3.74 during discovery; the device's
actual system-software baseline remained retail 3.65.

## User-mode ScePerf route: unavailable

The first direct probe crashed at unresolved `scePerfArmPmonReset` with a
prefetch abort at PC zero. Its dump was:

- `psp2core-1789426888-0x00029724ab-eboot.bin.psp2dmp`
- SHA-256
  `BA7C8695BEC30D030294BAE3AC7B4087992946A4D637DB8CB2AED96955562486`

A second build incorrectly treated nonzero import-stub bytes as proof that the
import was bound. It reached the same PC-zero prefetch abort. Preserved files:

- dump: `psp2core-1789428053-0x0002572e73-eboot.bin.psp2dmp`
- dump SHA-256:
  `D0D1D4D0C34D7FC79F6118F9F0398E68281F2B55C64B268B65AC2813B0FD3E77`
- VPK SHA-256:
  `23ABC9F34A330EC372258B8FA248279A04F8F4C7F16A4AEB62F5A2548B7932E9`

The follow-up discovery build made **zero PMU calls** and completed normally.
Its [synchronized journal](profiler-pmu-discovery-3.65.txt) is retained beside
this record. It established:

- `sceSysmoduleLoadModule(SCE_SYSMODULE_PERF)` returned `0x805A1000`;
- `sceKernelLoadStartModule("vs0:sys/external/libperf.suprx", ...)`
  returned `0x8002D003`, or `SCE_KERNEL_ERROR_MODULEMGR_NO_LIB`;
- no `libperf` module identity or exports became available; and
- all six adapter-used ScePerf stubs remained
  `E24FC008,E3A0F000,E1A00000,00000000`, including the final branch to zero.

The public convenience initializer now returns `VP_ERROR_UNSUPPORTED` on Vita.
Only the injected-operations initializer remains available, and only for a
resolver which can prove its targets and module lifetime.

## Kernel PMU discovery: passed

Kernel ABI v1.13 adds capability `VD_KERNEL_CAP_PMU_DISCOVERY` and the
read-only `vdKernelGetPmuInfo()` call. Static review and the final ELF show only
ARM `MRC` reads for PMU state; there are no PMU `MCR` writes. The code suspends
local interrupts around each per-core snapshot and resumes them before copying
the result to user memory. Nothing accesses the PMU during plugin startup.

The first on-device run passed the new PMU checks and every existing kernel
regression check. It exposed a host-side interpretation bug: the test added one
to Cortex-A9 `PMCR.N` and displayed seven counters although the raw field
already means six. The unedited pre-correction screenshot is retained as
`profiler-pmu-kernel-gate-pre-correction-3.65.jpg`; it is evidence of the safe
register-read boundary, not evidence for the incorrect decoded count.

The corrected build:

- decodes `PMCR.N` as exactly six counters;
- accepts physical CPU IDs 0 through 3 independently of VitaDebugger's normal
  three-application-core debug-worker limit;
- retries the second snapshot until it comes from the same CPU;
- requires both calls to succeed before comparing control state; and
- preserves every previous kernel probe capability while experimental
  hardware-debug mutation remains disabled.

The corrected hardware run passed every line. In particular, the
[unedited final screenshot](profiler-pmu-kernel-discovery-3.65.jpg) shows
`[PASS] same-core PMU comparison`, `[PASS] PMU discovery leaves controls
unchanged`, and `counters=6`. Its SHA-256 is
`A8AB74D6ABFE13115F4D334F753B0A7ACF1D60E8B0F5223994459DE5D85903B6`.
The plugin and probe hashes were:

- `vitadebug.skprx` SHA-256
  `2BA906D26608C6D1DE972E18A350A85FA95545087354FECC4FC1104C8168FAC6`
- `vitadebug-kernel-probe.vpk` SHA-256
  `1D621E3964A8D6C8FEA4C9F2951622ED9918522A4E1F5995173219244ABCD956`

## What this proves

The tested retail kernel permits a narrowly bounded companion syscall to read
the Cortex-A9 PMU identity and control inventory without changing it. That is
enough to proceed with a separately versioned PMU session design.

It does **not** yet prove that VitaDebugger can safely enable a counter. The
next gate must claim one CPU, snapshot every field it changes, configure and
read an allowlisted event, verify every write, and restore the exact snapshot
on release, error, timeout, disconnect, and process exit. Live PMU profiler
support remains disabled until those restoration tests pass.

Sony's debug-host trace transport is not needed. Once the leased PMU provider
passes, samples will use VitaProfiler's own bounded event ring and TCP/file
transport.

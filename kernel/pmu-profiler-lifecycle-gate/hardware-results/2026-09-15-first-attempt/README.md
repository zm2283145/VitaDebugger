# PMU lifecycle gate: first retail 3.65 attempt

Date: 2026-09-15

Status: **failed closed before the process-exit stage; not promoted**

This directory preserves the first hardware run of the PMU safe-rearm
lifecycle gate on a retail 3.65 Vita. The run stopped at the worker-thread exit
case. The process-exit case and same-boot relaunch case were not attempted.

## Installed artifacts

| Artifact | SHA-256 |
| --- | --- |
| Safe-rearm kernel candidate | `FF8617734E125CAAB6E9D81E3053A98385115D9AEB2D522B0FE165E095E619C4` |
| Lifecycle-gate VPK | `55CDC15336A0D318B87C7032E634311142540974A772786B45077E153B949D4C` |
| Pre-test kernel restored after failure | `1D73EADE0601A4B9285F86EE63978E375364ABDDCD4EB81AA8F34F767A9C0270` |

The configured kernel path was `ur0:tai/vitadebug-vfp-live.skprx`. The failed
candidate was retained on the device as
`ur0:tai/vitadebug-vfp-live.failed-safe-rearm-20260915.skprx`; the configured
path was restored from the verified pre-test backup before the recovery reboot.

## Durable result

Both recovered records are valid 512-byte checksummed journals:

| Record | SHA-256 | Meaning |
| --- | --- | --- |
| `pmu-lifecycle-v1-a.bin` | `3E1C1B8ABD70689109DF58140685E1BB81C3740E3B2061D7DF106228FAB33A28` | Revision 1, `ATTEMPTED` |
| `pmu-lifecycle-v1-b.bin` | `DE35664969F9AC61B9D643529D6FE5CBA70D02F504E20269467D7BA8F626A0DF` | Revision 2, `FAILED` |

The explicit open/read/close transaction passed. A live contender correctly
received `BUSY`. The lease-timeout case restored the counter and reported
`RESTORE_REQUIRED` as expected. The worker then opened and read the counter and
exited without closing it. Waiting for and deleting that worker both succeeded,
but every subsequent open during the two-second bounded poll returned `BUSY`.
The gate therefore stopped before process exit. No second launch was made.

## Diagnosis

The saved record proves the externally visible failure but does not include the
transport state, owner-query status, retained-object result, or release result.
Source review found two concrete thread-exit hazards:

1. `vdPmuProfilerTransportOpen()` returns `BUSY` immediately whenever the
   transport is not `IDLE`; it does not service one bounded owner-liveness and
   recovery pass first. Recovery is left to the background watchdog.
2. The owner query compares mutable thread metadata before checking terminal
   thread status. If the kernel changes that metadata as the thread becomes
   dormant, the exact retained thread can be classified `UNKNOWN` instead of
   `GONE`.

The gate and watchdog both use a 20 ms cadence, while the watchdog takes the PMU
lock non-blockingly. That creates an additional phase-lock/starvation risk.

Process-exit rearm has a separate unresolved design issue: UID teardown can be
accepted as evidence that an owner is gone, but the current retained-reference
release path attempts to resolve that same UID again. Process-exit support must
remain disabled until a process-event-backed or otherwise verified release path
is hardware-gated.

## Required correction before another hardware run

- Check an exact object's terminal status before comparing mutable metadata.
- Let a new open service one bounded watchdog/recovery pass while holding the
  existing PMU lock, and surface `RESTORE_REQUIRED` rather than hiding a failed
  cleanup as `BUSY`.
- Add read-only diagnostics for transport state, owner status, status bits,
  object match, GUID reference/info/release results, and the last cleanup
  outcome.
- Add host regressions for dormant-owner rearm, live contenders, mutable
  metadata after exit, UID/object mismatch, release failure, watchdog lock
  starvation, and stale-handle rejection.
- Run a smaller thread-exit diagnostic gate before re-enabling the complete
  lifecycle gate. Do not advance to process exit in that run.

## Recovery evidence

The current taiHEN configuration was copied and hashed before recovery; it still
contained the original YAMT and VitaDebugger entries and had not been modified
by this test. On the first post-test boot, `yamt_helper.log` showed successful
bootfs setup and USB patching but failed to discover the PSVSD device during all
26 bounded checks (`uma fd = 0x80010013`), so no `ux0` mount was attempted. The
log and kernel binaries are preserved under `recovery/`. No format, database
rebuild, YAMT setting change, or PSVSD filesystem write was performed.

After the pre-test kernel was restored and verified, the device was fully
powered off. For isolation, SD2Vita was assigned to `ux0` and PSVSD to `uma0`.
SD2Vita mounted successfully and SceShell completed its database update, but the
FTP root contained no `uma0:` mount. The fresh `yamt_helper.log` accepted the
requested `umaid = 0xF00`, again completed all 26 PSVSD checks, and again ended
with `uma fd = 0x80010013`. This localizes the remaining PSVSD problem below
filesystem mounting: the block device did not enumerate. It does not establish
filesystem corruption, and no initialize, format, repair, or database operation
was performed against PSVSD.

The PSVSD activity LED remained powered after the Vita was shut down. Removing
the battery cleared that latched hardware/controller state. After reconnecting
the battery and restoring PSVSD as `ux0`, SceShell detected the storage and
completed its database update. The new YAMT log recorded `found uma` and a valid
positive device handle (`uma fd = 0x1191B`, `umaid = 0x800`), and the owner
confirmed that the original PSVSD content was fully available again. This
recovery further rules against `config.txt` corruption and does not supply
evidence of microSD filesystem corruption.

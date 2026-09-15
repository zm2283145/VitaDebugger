# Isolated PMU session hardware gate

This directory contains the disposable first write test for the Cortex-A9 PMU
backend. It is intentionally separate from the production VitaDebugger kernel
ABI. It is **not installed or enabled by a normal VitaDebugger build**.

The only measurement performed is the narrowest deterministic gate:

1. dispatch to one fixed-affinity application-core worker;
2. verify Cortex-A9 identity and a completely idle PMU baseline;
3. snapshot all restorable state needed by the gate;
4. own programmable counter 5 with architectural event `0x00`;
5. issue exactly 17 `PMSWINC` writes;
6. read back exactly 17;
7. restore the original lane, selector, and safe persistent controls; and
8. prove that the final snapshot exactly equals the initial snapshot.

The module starts three fixed-affinity workers but performs **no CP15 PMU
access at module load**. PMU access occurs only through the explicit self-test
or recovery syscall. Core 0 must pass before the UI permits meaningful testing
of cores 1 or 2.

## Safety boundary

- The gate rejects a PMU whose implemented enable, interrupt, or overflow bits
  are nonzero. Do not run another profiler at the same time.
- It never enables PMU interrupts or user-mode PMU access.
- It never uses the cycle counter and only selects lane 5.
- An incomplete, corrupt, or failed journal locks out another run. Archive both
  journal files before deciding whether to clear them.
- The UI never hot-unloads the kernel module. `module_stop` refuses to unload
  while the backend has an unresolved in-memory restoration obligation.
- A dispatch timeout is ambiguous. The backend remains resident and disabled;
  recovery first reaps any late worker completion and then retries the retained
  exact-restore record on the original core.

The test currently targets the already-established retail Vita 3.65 research
environment. Other firmware versions remain untested. This probe is not a
claim that the general profiler session API is production ready.

## Files

- `kernel_probe.c` — resident syscall boundary, safety latch, and durable
  journal owner.
- `loader.c` — small user UI; defaults to core 0 and shows raw before/after
  state, the reached stage, count 17, and restoration status.
- `include/vitadebug_pmu_probe.h` — versioned journal/status ABI plus pure
  validation helpers.
- `test_record.c` — host validation of checksums, torn/corrupt records,
  revision selection (including wrap), result validation, and restore latches.
- `kernel_exports.yml` — three deliberately narrow syscalls: run, status, and
  recovery.

The alternating, checksummed records are:

```text
ux0:data/VitaDebugger/pmu-session-gate-v1-a.bin
ux0:data/VitaDebugger/pmu-session-gate-v1-b.bin
```

Every pre-mutation transition is written, file-synchronised, volume-synchronised,
read back, and checksummed before the hardware call begins. A missing final
record therefore remains distinguishable from a completed restore.

## Host record test

From the repository root on Windows:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
  -File tools/invoke-vita-env.ps1 C:/msys64/mingw64/bin/gcc.exe `
  -std=c11 -Wall -Wextra -Werror `
  -DVD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION=1 `
  -Ikernel/pmu-session-probe/include -Ikernel/src `
  kernel/pmu-session-probe/test_record.c `
  -o kernel/pmu-session-probe/test-record.exe
kernel/pmu-session-probe/test-record.exe
```

Expected output:

```text
PASS: PMU probe two-slot journal and result validation
```

## Vita build

Build from a repository path without spaces (the canonical
`D:\Claude\VitaDebugger` checkout is suitable):

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
  -File tools/invoke-vita-env.ps1 cmake `
  -S kernel/pmu-session-probe `
  -B kernel/pmu-session-probe/build-final `
  -G "Unix Makefiles" `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/usr/bin/make.exe
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
  -File tools/invoke-vita-env.ps1 cmake --build `
  kernel/pmu-session-probe/build-final --clean-first
```

Both targets compile with `-Wall -Wextra -Werror`. The outputs are:

```text
kernel/pmu-session-probe/build-final/vitadebug-pmu-session-probe.skprx
kernel/pmu-session-probe/build-final/vitadebug-pmu-session-probe.vpk
```

## Hardware runbook

Do not deploy this directory merely because it builds. Review the generated
imports and ARM code first, keep the prior kernel plugin available as a
rollback, and have physical recovery access to the Vita.

1. Confirm no PMU profiler/plugin is active.
2. Copy `vitadebug-pmu-session-probe.skprx` to
   `ur0:tai/vitadebug-pmu-session-probe.skprx`.
3. Add this line under `*KERNEL` in `ur0:tai/config.txt`:

   ```text
   ur0:tai/vitadebug-pmu-session-probe.skprx
   ```

4. Reboot. Do not hot-load or hot-unload the probe.
5. Install `vitadebug-pmu-session-probe.vpk` and open **VD PMU gate**.
6. Confirm `ready=1`, `obligation=0`, and either an empty journal or a prior
   passing journal. If the journal is corrupt, incomplete, or failed, stop and
   pull both slots.
7. Leave core 0 selected and press **X** once. Do not leave the app until the
   result is displayed.
8. A pass requires all of the following on screen:
   - reached stage `complete`;
   - call, operation, and restore all equal zero;
   - observed/expected equals `17/17`;
   - raw before and after snapshots match; and
   - `Exact restoration: PROVEN`, `obligation=0`.
9. Pull both journal slots and retain them with the VPK/SKPRX hashes. Only
   after reviewing the core-0 evidence should cores 1 and 2 be considered.

If `obligation=1`, press **Triangle** once to request bounded exact recovery.
If recovery does not return zero with `obligation=0`, stop testing and reboot.
Do not delete the journal first. A journal left at `kernel entered`,
`recovering`, or `RESTORE REQUIRED` after a reboot is intentionally not treated
as proof of restoration; archive it before manually clearing the latch.

The Circle button exits only the user UI. It does not unload the resident
kernel code.

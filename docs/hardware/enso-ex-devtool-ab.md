# Enso_ex DEVTOOL identity A/B runbook

Status: secondary contingency, planned and not executed. The narrower KBL
[DIP-switch 228 investigation](dipsw-228-hw-debug.md) now comes first. This
runbook does not authorize a boot-manager change. Perform it only if the
bit-specific evidence leaves an identity-policy question, with the Vita owner
present after the exact backup and restore artifacts have been reviewed.

## Question being tested

The staged retail read ladder reached MIDR, DIDR, and DSCRint, then rebooted at
its first DBGVCR read. Enso_ex can rewrite the secure boot-time device type, but
its public source does not explicitly drive the Cortex-A9 `DBGSWENABLE` input.
The narrow experiment asks whether Sony firmware derives debug authorization
from that device type. It must not be combined with a bit-228 mutation; changing
one variable per boot is necessary to interpret the result.

The KBL documentation now provides a more direct lead: SKBL can force-set DIP
switch 228 for TEST/TOOL-class units under a particular fallback condition.
Enso_ex's public boot-manager spoof runs later during NSKBL, so this runbook
cannot test that early SKBL behavior. It remains useful only as a secondary
check for later identity-dependent policy in ProcessMgr/ThreadMgr or another
consumer.

If this secondary branch is reached, use DEVTOOL/devkit (`0x0101`) for its first
identity comparison because it is the closest development-unit identity.
DEX/testkit (`0x0201`) is retail-like and is useful only as a later control.
Neither reported identity proves debug-register access. Miaki and DevKit
firmware are not needed for this test.

The pinned Enso_ex source is
[`bfedbd877994dd414a0b89e4efb8110d3a40c10a`](https://github.com/SKGleba/enso_ex/tree/bfedbd877994dd414a0b89e4efb8110d3a40c10a/bootmgr/lv0-typespoof).
Its example currently calls `set_type(TEST)`, so do not deploy the bundled
example unchanged. A test payload must be rebuilt with the single intentional
selection `set_type(DEVTOOL)` and its source revision and SHA-256 recorded.

## Hard prerequisites

- Confirm a cold boot with Volume Down really skips the boot manager and custom
  kernel loader on this installation. This is a bypass, not by itself a repair
  route: skipping the homebrew-enabler may also prevent the installer or
  VitaShell from launching.
- Confirm a separate, usable recovery/homebrew route before changing anything.
  Do not rely on an untested key combination after a failed boot.
- Keep the Vita charged and connected to power. Disable updates and do not mix
  this test with firmware, storage, plugin, or TaiHEN configuration changes.
- Use a newly built read-only diagnostic title and fresh journal. Do not unlock
  or reuse the ladder journal that already stopped at DBGVCR.
- The owner must be physically present. Do not perform this boot-path change as
  an unattended FTP operation.

Enso_ex documents Volume Down as the boot-manager/custom-loader bypass in its
[recovery guide](https://github.com/SKGleba/enso_ex/blob/bfedbd877994dd414a0b89e4efb8110d3a40c10a/README-recovery.md).

## Capture the exact starting state

Before building or uploading the spoof, pull a read-only inventory and preserve
both file contents and file absence. At minimum include:

- `os0:bootmgr.e2xp`;
- the complete `ux0:eex/boot/` staging directory;
- the complete `ux0:eex/custom/` staging directory;
- `ur0:tai/boot_config*.txt` and `ux0:eex/boot_config*.txt`; and
- the installed Enso_ex version plus the existing recovery files/configuration.

Create a SHA-256 manifest on the computer and a separate presence manifest.
Keep `os0:bootmgr.e2xp` and `ux0:eex/boot/bootmgr.e2xp` as distinct backups:
they may intentionally differ if staging has not been synchronized. Preserve
the backup outside any directory later uploaded to the Vita.

The Enso_ex installer synchronizes the whole staged boot/custom set, removes an
installed boot manager when its staging file is absent, and recreates `os0:ex/`.
Therefore verify the complete staging inventory before selecting Synchronize;
do not treat that menu item as a one-file copy. See the pinned
[installation documentation](https://github.com/SKGleba/enso_ex/blob/bfedbd877994dd414a0b89e4efb8110d3a40c10a/README.md#synchronize-enso_ex-plugins).

## Test sequence

1. Build the pinned type-spoof boot manager with only `set_type(DEVTOOL)` as the
   intended source change. Record the source diff and payload SHA-256.
2. Prepare the restore files on the computer before uploading the test payload.
3. Replace only `ux0:eex/boot/bootmgr.e2xp` in staging. Re-list and hash the
   remote file after upload.
4. In the Enso_ex installer, select **Synchronize enso_ex plugins**, review the
   result, and perform its cold reboot.
5. Verify the effective device type with a read-only diagnostic. Do not infer it
   solely from a changed label or menu.
6. Run a fresh copy of the staged diagnostic: lifecycle, MIDR, DIDR, DSCRint,
   then only one DBGVCR read. Record firmware, selected type, payload hash, CPU
   core, journal before/after, and whether the Vita rebooted.
7. Stop after DBGVCR regardless of its result. Do not read or write BCR/BVR/WCR/
   WVR, guessed MMIO, authentication, power, clock, Syscon, OTP, or eFuse state.

If the Vita fails to boot normally, cold boot with Volume Down and use the
already-proven recovery route. Do not improvise additional register or storage
writes while recovering.

## Exact return to the original state

Use the installer rather than writing `os0:` directly:

1. To restore the installed state, temporarily place the backed-up original
   `os0:bootmgr.e2xp` at `ux0:eex/boot/bootmgr.e2xp` and synchronize. If the
   installed boot manager was originally absent, remove the staging boot manager
   and synchronize so the installer removes it from `os0:`.
2. After that synchronization, restore the staging path to its exact original
   contents or original absence. If it originally differed from `os0:`, do not
   synchronize a second time; that difference is part of the saved state.
3. Cold reboot, verify the original device type, and pull/hash the installed and
   staged files against both manifests.
4. Run the known-good lifecycle/MIDR/DIDR/DSCRint baseline once. Do not repeat
   DBGVCR merely to prove restoration.

This two-part restore matters because the installed and staged boot-manager
files are separate state. Merely deleting the uploaded file without running
Synchronize does not remove the installed copy.

## Interpretation

- If DEVTOOL still reboots at DBGVCR, a device-type rewrite is insufficient;
  keep hardware breakpoints/watchpoints disabled and end this branch.
- If DEVTOOL makes DBGVCR readable, record only that correlation. It does not
  establish why access changed or make comparator writes safe. Design a new,
  separately reviewed read-only gate before any further hardware work.
- A later DEX/testkit run can distinguish development-policy behavior from a
  broader non-CEX behavior, but it is optional and uses the same full restore
  procedure.

In every outcome, ordinary software breakpoints, guarded software stepping,
thread control, logging, profiling, and software-watchpoint research continue
independently of this experiment.

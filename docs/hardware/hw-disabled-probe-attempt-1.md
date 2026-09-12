# Disabled hardware-debug probe: attempt 1

Date: 2026-09-12

This was the first retail-hardware attempt to extend the earlier read-only DIDR
discovery gate into a disabled (`E=0`) comparator write/readback/restore test.
The disposable application was installed and launched through VitaDevDeploy;
the user deliberately pressed X once after reading the warning.

The Vita rebooted during the private kernel module's critical section. The
module was dynamically loaded, returned no resident service by design, and was
never added to `ur0:tai/config.txt`, so the reboot did not create a boot loop.

## Exact artifact

- Title ID: `VDCP00003`
- VPK SHA-256:
  `BEFE1C9F1AC8BCB1BEE7D5C63C3AA085CF30E69D9049D4EC9ACB6366FA46494F`
- Embedded SKPRX SHA-256:
  `CAA1F35CD1A3A034D82A895EE10E5E769E750CE824E0BAE6AAC46F21A29F0E35`

An independent package and disassembly audit confirmed that the SKPRX had no
hooks, syscall exports, resident thread, tai configuration access, DSCR/DBGVCR
writes, enabled comparator writes, or compiler-generated VFP/NEON instructions.
Its ten CP14 writes were restricted to disabled BVR0/BCR0/WVR0/WCR0 test and
restore operations.

## Recovered journal

The checksummed pre-probe record was recovered over FTP after reboot:

- Record size: 132 bytes
- Record SHA-256:
  `E951F27B37707879552DA0106A11A641B70C0DDB0D5912393CE8DE73F3801C92`
- FNV-1a checksum: `0xba90f758` (valid)
- Stage: `ENTERED` (`1`)
- Result: `NOT_RUN` (`-100`)
- Core: unset (`0xffffffff`)
- Snapshot/write/restore flags: none

The record proves that `module_start` ran and durably committed its entry marker,
then failed before the final journal commit. It does **not** identify the exact
instruction inside the short critical section because that section intentionally
did no filesystem I/O between snapshot and restore. In particular, it does not
prove that a comparator write occurred; no write or snapshot completion was
durably confirmed.

The earlier v7 resident probe successfully read DIDR, so this reboot must not be
summarized as "all CP14 is disabled." The next gate is a separate staged,
read-only ladder. It records the selected operation before each non-resident
kernel invocation and attempts only one control operation or CP14 register read
per X press. Disabled writes remain blocked until that ladder identifies which
operation traps.

# Retail 3.65 PMU event `0x01` hardware gate

This owner-attended `VDCP00010` run took place on 2026-09-15 on a retail PS
Vita running system software 3.65. It tested the fixed PMU profiler transport
configuration: application core 0, physical programmable counter 5, a 250 ms
lease, and event `0x01` (L1 instruction-cache miss/refill).

The gate was installed through VitaDevDeploy direct TCP job
`1b168f87d6cb83dad248f35a4739e845`. The exact reviewed artifacts were:

- Kernel companion SHA-256:
  `1D73EADE0601A4B9285F86EE63978E375364ABDDCD4EB81AA8F34F767A9C0270`
- Gate VPK SHA-256:
  `334514F03C81E717E7E3D84B3308B69F8141823D53B17CC3A3F4D949AD6DFFA1`
- Installed gate `eboot.bin` SHA-256:
  `4A5FD733B2CC0B8800C6EDB28B68F7CB1E735358FDF2B6E8B1E09DF67438D56C`

Both 256-byte journal slots validate against the version-1 ABI and FNV-1a
checksum:

- Slot A: revision 1, `ATTEMPTED`, event `0x01`, flags zero; SHA-256
  `FEF05E9E2EA6104EDD9CE17FBE829C94CED722DD35ED0B298C27659EE2C81787`
- Slot B: revision 2, `COMPLETE`, event `0x01`, flags `0x00000007`
  (`RESTORE_PROVEN | SAMPLE_VALID | PASS`); SHA-256
  `4F9F44684F87BC0455791C9BA2EF273B3596915C218E97AF63B5D4B16E54D25C`

The completion record reports `info=0`, `open=0`, `read=0`, `close=0`, and
journal result zero. Affinity set and restore both returned nonnegative prior
masks. The sample belongs to owner token 1, generation 1, core 0, physical
counter 5, and contains value `0x38` (56). All reserved fields are zero. The
result screen independently reports `Gate result: PASS` and exact restore
proven.

Screenshot hashes:

- Pre-test: `804F2ED3D442B2BEB32AFF11133944A184B76E35342B2FEE26E6AAED526B5F86`
- Result: `428B6FA81A99F9774F9A57EBF8DAAA266315C9055AD14AD26C5EABE4BAB5A160`

This proves that the reviewed kernel bridge can program, sample, close, and
exactly restore one bounded real PMU event on this retail 3.65 system. It does
not by itself approve arbitrary events, continuous sampling, concurrent PMU
owners, other firmware versions, or other device classes. Subsequent separate
fresh-boot gates passed events
[`0x03`](../2026-09-15-event-03/README.md) and
[`0x10`](../2026-09-15-event-10/README.md); watchdog, process-exit,
disconnect, conflict, and safe re-arm gates remain.

# Retail 3.65 PMU event `0x10` hardware gate

This owner-attended `VDCP00010` run took place on 2026-09-15 on a retail PS
Vita running system software 3.65. It tested the fixed PMU profiler transport
configuration: application core 0, physical programmable counter 5, a 250 ms
lease, and event `0x10` (branch misprediction).

The gate was installed through VitaDevDeploy direct TCP job
`6f1fdecdb05acb1e75b4f547a100afcd`. The reviewed build artifacts were:

- Kernel companion SHA-256:
  `1D73EADE0601A4B9285F86EE63978E375364ABDDCD4EB81AA8F34F767A9C0270`
- Gate VPK SHA-256:
  `93BE1EFEF5F12A7400B2B3C1A06E4050246C50B6D0B33F31972FF67A4FD03033`
- Installed gate `eboot.bin` SHA-256:
  `49897A835D511BB0C75F8CA4BC73D01EF287F94EBE78E8EB9E44501D8A50645C`

Both 256-byte journal slots validate against the version-1 ABI and their
stored FNV-1a checksums:

- Slot A: revision 1, `ATTEMPTED`, event `0x10`, flags zero, checksum
  `7062D9FD`; SHA-256
  `A4DD2C13F3F7A3C609DB15856C245A3A80037C6E0722FF36E149C0278FAA3397`
- Slot B: revision 2, `COMPLETE`, event `0x10`, flags `0x00000007`
  (`RESTORE_PROVEN | SAMPLE_VALID | PASS`), checksum `4DF5BB30`; SHA-256
  `93D7FEEC9025B459E8171FF060C797E4FB3B9A6A95FD0E0F10EEC07005CE8EA4`

The attempted record reports successful capability discovery and the original
application affinity mask `0x00070000`; transport result fields use the
pre-attempt sentinel `-599`. The completion record reports `info=0`,
`open=0`, `read=0`, `close=0`, and journal result zero. Affinity selection
returned zero, while restoration returned the nonnegative prior forced-core-0
mask `0x00010000`; that positive return is successful Vita affinity API
behavior, not an error. The sample belongs to owner token 1, generation 1,
core 0, physical counter 5, and contains value `0x61` (97). All reserved
fields are zero. The result screen independently reports `Gate result: PASS`
and exact restore proven.

Result screenshot SHA-256:
`3548A4C4D9F35655590615C881F33F0ACFE4C14B0BD7161085EF31B69428897D`.

This proves that the reviewed kernel bridge can program, sample, close, and
exactly restore one bounded `0x10` PMU event on this retail 3.65 system. All
three allowlisted normal-close gates (`0x01`, `0x03`, and `0x10`) now pass. It
does not approve arbitrary events, continuous sampling, concurrent owners,
other firmware versions, or other device classes. The watchdog/process-exit,
disconnect, ownership-conflict, and safe repeated-lease/re-arm gates remain.
The current real-event attempt latch is still boot-scoped; no safe same-boot
re-arm is implemented.

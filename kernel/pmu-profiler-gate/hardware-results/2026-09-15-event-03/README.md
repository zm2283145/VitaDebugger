# Retail 3.65 PMU event `0x03` hardware gate

This owner-attended `VDCP00010` run took place on 2026-09-15 on a retail PS
Vita running system software 3.65. It tested the fixed PMU profiler transport
configuration: application core 0, physical programmable counter 5, a 250 ms
lease, and event `0x03` (L1 data-cache miss/refill).

The gate was installed through VitaDevDeploy direct TCP job
`dacc129decc609696d5bf670c180feef`. The reviewed build artifacts were:

- Kernel companion SHA-256:
  `1D73EADE0601A4B9285F86EE63978E375364ABDDCD4EB81AA8F34F767A9C0270`
- Gate VPK SHA-256:
  `22DC33A700D3E061E8CCEF30EB8D8C31BF6F50B2D4813C86E2C8C272C9850234`
- Installed gate `eboot.bin` SHA-256:
  `F531D357B9D48E8873545674BD79F0481A8D343E28638C871E729027D45A84F1`

Both 256-byte journal slots validate against the version-1 ABI and their
stored FNV-1a checksums:

- Slot A: revision 1, `ATTEMPTED`, event `0x03`, flags zero, checksum
  `B5ABD6E6`; SHA-256
  `340644247BEDE096D05F8707489B431EBAE46341713520BA6020CBD0B70675B5`
- Slot B: revision 2, `COMPLETE`, event `0x03`, flags `0x00000007`
  (`RESTORE_PROVEN | SAMPLE_VALID | PASS`), checksum `FBB3675D`; SHA-256
  `98F51EFD0E0557BD8A03FEB2D5F09CCC433B78B5795129E6052A3B3470893834`

The attempted record reports successful capability discovery and the original
application affinity mask `0x00070000`; transport result fields use the
pre-attempt sentinel `-599`. The completion record reports `info=0`,
`open=0`, `read=0`, `close=0`, and journal result zero. Affinity selection
returned zero, while restoration returned the nonnegative prior forced-core-0
mask `0x00010000`; that positive return is successful Vita affinity API
behavior, not an error. The sample belongs to owner token 1, generation 1,
core 0, physical counter 5, and contains value `0x17` (23). All reserved
fields are zero. The result screen independently reports `Gate result: PASS`
and exact restore proven.

Result screenshot SHA-256:
`2D551EF76B2803966E82849931B10067051E726A966622241E6EA7AD9BB1E8D8`.

This proves that the reviewed kernel bridge can program, sample, close, and
exactly restore one bounded `0x03` PMU event on this retail 3.65 system. Along
with the earlier `0x01` gate, it does not approve arbitrary events, continuous
sampling, concurrent owners, other firmware versions, or other device classes.
The subsequent separate fresh-boot
[`0x10`](../2026-09-15-event-10/README.md) gate also passed. The
watchdog/process-exit/disconnect/conflict recovery and safe repeated-lease
gates remain separate hardware tests. The current real-event attempt latch is
still boot-scoped; no safe same-boot re-arm is implemented.

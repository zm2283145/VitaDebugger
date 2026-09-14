# Pre-set read-only baseline

This clean-boot `VDCP00005` sample was taken immediately before the DIP 228 API
round-trip test on 2026-09-13. The newest valid slot was B, sequence 2, revision
6. It reported result zero, validity flags `0x0000000F`, core 2, zero
debug/system words, and direct bits 203 and 228 both clear.

> **Hardware baseline:** the owner later confirmed that this test device was a
> retail PS Vita running system software 3.65. The firmware value was not
> embedded in this artifact's journal or screenshots, so this is later device
> metadata rather than contemporaneous probe output. Do not generalize this
> result to another firmware or device class.

SHA-256:

- A: `EDC329FE472AA534021436C883CFB41A55FAFA6884F2D1EE61579AE887259D5C`
- B: `D449AFA2EB8FFA65D05CF07D71580B31370BAC889FB022BAE06993CC053237AA`

These files are raw checksummed probe journals, not synthesized fixtures.

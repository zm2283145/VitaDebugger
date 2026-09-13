# Post-set reboot verification

After the set/read/restore test and a clean reboot, `VDCP00005` completed again
on 2026-09-13. The newest valid slot was A, sequence 3, revision 9. It reported
result zero, validity flags `0x0000000F`, core 1, zero debug/system words, and
direct bits 203 and 228 both clear.

SHA-256:

- A: `6F47034E56A157752A5858217C9445995CD5B99F54A90D0C97D907DFB6856EDE`
- B: `60F9996B0EF97EE6BE3DA6992A6D678215EAA9EE21A53692F03C7826E4A277EB`

These files are raw checksummed probe journals, not synthesized fixtures.

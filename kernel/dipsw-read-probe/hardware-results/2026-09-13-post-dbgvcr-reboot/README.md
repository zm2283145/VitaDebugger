# Post-DBGVCR reboot recovery

After the Vita rebooted during the owner-attended DIP 228 + DBGVCR experiment,
`VDCP00005` was run as a read-only recovery check on 2026-09-13. Both journal
slots validate. The newest slot was A, revision 15, sequence 5. It reported
result zero, validity flags `0x0000000F`, core 1, zero raw/debug/system words,
and direct bits 203 and 228 both clear.

> **Hardware baseline:** the owner later confirmed that this test device was a
> retail PS Vita running system software 3.65. The firmware value was not
> embedded in this artifact's journal or screenshots, so this is later device
> metadata rather than contemporaneous probe output. Do not generalize this
> result to another firmware or device class.

SHA-256:

- A: `CFB486C53837CADA36F3B8DBA34F00A143F5B86B2605BB5A84073DE28ACF8CE7`
- B: `BE52A48F70C4BEAB9A1B93AE4BB54A8154FBE300FB4B87EB4CF89E6632641F29`

The DBGVCR journal slots retained their previously recorded hashes after the
reboot and this recovery run. This sample establishes that the cached DIP
state recovered to clear; it does not identify the exact instruction that
caused the preceding reboot. These files are raw checksummed probe journals,
not synthesized fixtures.

The late runtime bit-228 path did not yield a safely returning DBGVCR read, so
debug comparator access remains blocked. Preserve the DBGVCR journals and its
one-shot lock; do not repeat that probe.

Related evidence:

- [Immediate pre-run read-only baseline](../2026-09-13-pre-dbgvcr-baseline/README.md)
- [DBGVCR hardware run](../../../dipsw-dbgvcr-probe/hardware-results/2026-09-13-first-run/README.md)

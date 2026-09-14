# First DIP 228 + DBGVCR hardware run

This owner-attended `VDCP00007` run took place on 2026-09-13 after a clean
read-only DIP baseline. The Vita rebooted immediately after the owner
physically pressed **L+R+X**. The two journal files were pulled after reboot
and again matched the hashes below after the separate read-only recovery
check.

> **Hardware baseline:** the owner later confirmed that this test device was a
> retail PS Vita running system software 3.65. The firmware value was not
> embedded in this artifact's journal or screenshots, so this is later device
> metadata rather than contemporaneous probe output. Do not generalize this
> result to another firmware or device class.

Both 192-byte journal slots validate:

- A: revision 3, sequence 1, `ORIGINAL_CAPTURED`; SHA-256
  `3A7F523863DA8F976B8FD93403A87238E6048DDFB01D8327C9973B21C44853D3`
- B: revision 4, sequence 1, `READ_PENDING`; SHA-256
  `80409C54CFA2BA756058E147E5B301044AF342FD642E4627F3FC11A84B27B857`

Slot B is the newest durable record. It reports primary result zero, restore
result `-100` (`NOT_RUN`), flags `0x0000003F`, `hazard_armed=1`, and journal
error zero. Its durable set/read/clear counters are all zero. The captured
pre-state is clean on core 1: CP/build, debug, and system words are zero, and
direct bits 203 and 228 are clear. No `COMPLETE` record was committed.

The zero counters are expected in the durable `READ_PENDING` snapshot because
the counters are changed only after that commit, in memory. They do not prove
that Set, the MRC, or Clear was not reached. The audited binary intentionally
does no file I/O between `SetDipsw(228)` and `ClearDipsw(228)`, so a reboot in
that interval cannot be narrowed to one exact instruction from the journal
alone.

The earlier API round-trip showed that setting, reading, and clearing cached
bit 228 can return safely. Combined with this probe's audited single-MRC
control flow, the immediate reboot is strongly localized to the
post-`READ_PENDING` critical path and is consistent with the sole DBGVCR MRC
not returning. This remains an inference, not instruction-level evidence.

The post-reboot read-only recovery sample found all raw words and direct bits
clear. The result is therefore: a late runtime `SetDipsw(228)` did not provide
a safely returning DBGVCR path on this retail Vita, and it does not authorize
breakpoint or watchpoint comparator access. Preserve both files permanently;
their presence is the probe's one-shot lock.

Related evidence:

- [Immediate pre-run read-only baseline](../../../dipsw-read-probe/hardware-results/2026-09-13-pre-dbgvcr-baseline/README.md)
- [Post-reboot read-only recovery](../../../dipsw-read-probe/hardware-results/2026-09-13-post-dbgvcr-reboot/README.md)

# DIP 228 + one-read DBGVCR probe

Status: the first audited hardware run completed on 2026-09-13 and rebooted
the Vita in the post-`READ_PENDING` critical path. Do not rerun this one-shot
probe; preserve both journal slots.

This disposable `VDCP00007` title tests whether a late runtime
`ksceKernelSetDipsw(228)` makes one same-core Cortex-A9 DBGVCR read accessible.
It follows the successful cached-state round trip from `VDCP00006`. It never
writes DBGVCR, DSCR, a breakpoint/watchpoint comparator, MMIO, boot state, or
device identity.

DBGVCR previously rebooted the retail test Vita when bit 228 was clear. This
probe may therefore reboot or hang the Vita. It is owner-attended, one-shot,
and requires a fresh **L+R+X** chord. Installing or opening the title does not
run the transaction. Circle exits.

## First hardware result: 2026-09-13

A read-only sample immediately before the experiment completed with slot B as
the newest record at revision 12, sequence 4. It reported PASS, flags
`0x0000000F`, core 1, zero debug/system words, and direct bits 203 and 228
clear.

The Vita rebooted immediately after the owner physically pressed **L+R+X**.
Both probe slots were recovered after reboot with their hashes unchanged.
Slot A is a valid revision-3 `ORIGINAL_CAPTURED` record. The newest slot B is
a valid revision-4, sequence-1 `READ_PENDING` record with primary result zero,
restore result `-100` (`NOT_RUN`), flags `0x0000003F`, `hazard_armed=1`, and
durable set/read/clear counters all zero. Its captured pre-state is clean on
core 1: debug/system words zero and direct bits 203 and 228 clear. No
`COMPLETE` record was committed.

The zero counters are the deliberately durable pre-operation values; they do
not establish which later instruction executed. Because the audited probe
performs no file I/O while bit 228 is high, the journal cannot distinguish a
failure in Set/readback, the interrupt/core guard, the DBGVCR MRC, or the
subsequent Clear path. Together with the earlier successful DIP 228 API
round-trip and the audited control flow, the immediate reboot strongly
localizes the event to that post-pending critical path and is consistent with
the sole DBGVCR MRC not returning. It is not instruction-level proof.

After the reboot, the separate read-only inventory completed with slot A as
the newest record at revision 15, sequence 5. It reported PASS, flags
`0x0000000F`, core 1, zero raw/debug/system words, and direct bits 203 and 228
clear. The late runtime bit-228 change therefore did not produce a safely
returning DBGVCR path on this retail Vita. Comparator access remains blocked.
The detailed evidence is in
[`hardware-results/2026-09-13-first-run`](hardware-results/2026-09-13-first-run/README.md),
with the linked pre-run and post-reboot read-only samples. The probe journals
remain the one-shot lock and must not be removed for another attempt.

## Exact transaction

The loader first creates, synchronizes, reopens, and validates an `ATTEMPTED`
record. Its private nonresident SKPRX then:

1. commits `KERNEL_ENTERED`;
2. captures CP/build, DIP words 6 and 7, and direct bits 203 and 228;
3. refuses mutation unless the named checks are Boolean, word-consistent, and
   both bits are clear;
4. commits the original snapshot;
5. records the expected application core and durably commits `READ_PENDING`;
6. repeats the complete baseline and refuses mutation if any value changed;
7. calls `SetDipsw(228)` once and verifies direct bit 228 is one and word 7
   changed by exactly `0x10`;
8. disables interrupts only around a core recheck and exactly one
   `MRC p14, 0, Rt, c0, c7, 0` DBGVCR read;
9. resumes interrupts and calls `ClearDipsw(228)` once on every returning path
   after Set;
10. captures and validates exact restoration, commits `COMPLETE`, and returns
    `SCE_KERNEL_START_NO_RESIDENT`.

There is no file, UI, allocation, delay, hook, or thread call while bit 228 is
high. The unavoidable exception is the DIP read APIs used to prove the Set and
the short CPU interrupt/core guard around DBGVCR. If Set does not validate, the
probe skips DBGVCR and still clears the bit.

## Durable evidence and recovery

The alternating 192-byte FNV-checked records are:

- `ux0:data/VitaDebugger/dipsw-dbgvcr-v1-a.bin`
- `ux0:data/VitaDebugger/dipsw-dbgvcr-v1-b.bin`

Decode pulled copies with:

```sh
python decode_record.py dipsw-dbgvcr-v1-a.bin dipsw-dbgvcr-v1-b.bin
```

Any existing slot locks all later mutation, including a valid PASS. Any I/O,
lifecycle, cleanup, validation, or core anomaly also locks the title. The
loader accepts `COMPLETE` only after a second user-side `ux0:` volume sync.

If the Vita reboots, hangs, loses power, or leaves `READ_PENDING` newest, do not
rerun. Reboot or force-power-cycle once, pull both slots, and run the separate
read-only `VDCP00005` inventory. Cached bit 228 is considered unknown until
that independent check reports direct and raw values clear and consistent.
The journals must remain on the Vita until they have been preserved and
reviewed.

## Interpretation boundary

- `COMPLETE/PASS` proves one DBGVCR read returned while the cached bit was
  observed high on that core. It does not authorize comparator access.
- A reboot or hang at `READ_PENDING` shows that this late runtime path did not
  make the read usable on the selected core. It does not rule out an earlier
  boot-time policy path or a different hardware class.
- This first experiment runs in the module-start context. It does not test a
  process or thread created after bit 228 becomes high.
- No automated retry is permitted, and no result from one core establishes
  all-core support.

## Build and mandatory binary audit

Build from a source/build path without spaces. Before hardware use, reject the
artifact unless all host tests and VPK verification pass and the disassembly
proves:

- exactly one literal Set(228), one literal Clear(228), and one DBGVCR MRC;
- zero other CP14 or CP15 instructions and zero debug-register writes;
- every returning path after Set reaches Clear;
- the MRC is reachable only after exact Set readback, the interrupt guard, an
  unchanged application core, and rejection of core 3;
- no file/UI/allocation/delay/thread/hook call occurs from Set through Clear;
- imports are limited to the expected DIP, CPU/interrupt, journal I/O,
  compiler `memcpy`, and module lifecycle surfaces.

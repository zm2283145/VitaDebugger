# DIP 228 set/read/restore probe

Status: source-complete, host-tested, and successfully exercised once on
hardware on 2026-09-13. The exact API round trip passed and a clean reboot plus
read-only probe confirmed restoration. The raw checksummed records and decoded
summary are preserved under `hardware-results/2026-09-13-first-run/`. This is
hardware evidence for this audited artifact, not approval to treat the probe as
a general-purpose DIP mutator or to reuse rebuilt binaries without review.

The owner later confirmed that this hardware baseline was a retail PS Vita
running system software 3.65. That value was not embedded in the probe journal,
and this result does not establish behavior on another firmware or device class.

This disposable `VDCP00006` title tests only whether the installed retail 3.65
kernel's documented DIP-switch API can temporarily expose global bit 228 in
its observable software state and restore the exact original state. It does
not access CP14, `DBGSWENABLE`, or any breakpoint/watchpoint register. A pass
therefore proves cached API round-trip behavior only; it does not prove that
ARM hardware debugging became available.

## Prerequisite and confirmation

Run the separate read-only `VDCP00005` inventory after the same clean boot and
preserve both of its journal slots. Do not run this probe if bit 203 or bit 228
is set, if either direct check disagrees with its containing word, or if the
Vita is not in an owner-attended recoverable state.

Installing or launching this VPK does not execute the kernel transaction. The
loader requires one fresh **L+R+X** chord. Circle exits. A failed or incomplete
newest journal locks all retries. The experiment is intentionally one-shot:
even a valid completed journal prevents another run until both slots have been
pulled and explicitly removed by the host.

## Exact transaction

The loader first commits and verifies an `ATTEMPTED` record. Its private,
nonresident SKPRX then:

1. commits `KERNEL_ENTERED` before its first inventory call;
2. captures CP/build, words 6 and 7, and direct bits 203 and 228;
3. refuses mutation unless the checks are Boolean, agree with the raw words,
   and both named bits are clear;
4. commits the original state and then a `SET_PENDING` hazard record;
5. repeats the complete baseline after that file I/O and refuses mutation if
   any value changed;
6. performs one straight-line in-memory transaction:
   `Set(228) -> GetInfo(7) -> Check(228) -> Clear(228)`;
7. captures the complete restored state, validates all observations, commits
   `COMPLETE`, and returns `SCE_KERNEL_START_NO_RESIDENT`.

The loader then requires a second user-side `ux0:` volume sync before it will
accept the reopened `COMPLETE` record as durable proof. Any sync, lifecycle,
cleanup, record, or exact-validation anomaly is a failure and remains locked
on subsequent launches.

There is deliberately no file, screen, allocation, delay, thread, lock,
conditional branch, or unrelated kernel call between Set and the unconditional
Clear. The VitaSDK setters return `void`; the two read APIs are the only success
signals. A pass requires word 7 to change by exactly `0x10`, direct bit 228 to
be one during that window, and the complete CP/debug/system/direct-bit snapshot
to equal the baseline after Clear.

## Durable evidence and interruption policy

The alternating 192-byte FNV-checked records are:

- `ux0:data/VitaDebugger/dipsw-set-v1-a.bin`
- `ux0:data/VitaDebugger/dipsw-set-v1-b.bin`

Decode pulled copies with:

```sh
python decode_record.py dipsw-set-v1-a.bin dipsw-set-v1-b.bin
```

The checksum detects torn or corrupted writes; it is not authentication. If
the Vita crashes, reboots, hangs, loses power, or lacks a verified `COMPLETE`
record after `SET_PENDING`, treat bit 228 as unknown. Do not rerun the mutator.
Reboot, pull both slots, and rerun the read-only inventory. Never guess by
calling Clear when the current direct and raw states disagree.

## Safety boundary

- The only DIP mutation calls are exactly one compile-time-literal
  `ksceKernelSetDipsw(228)` and, on the same normal path, one unconditional
  `ksceKernelClearDipsw(228)`.
- Bit 203, raw KBL memory, device identity, firmware identity, and every other
  DIP bit are read-only.
- No CP14/CP15 instruction, MMIO, interrupt/preemption mask, hook, callback,
  worker thread, public export, persistent configuration, or resident kernel
  service is present.
- The module never treats `r0` after Set/Clear as a return value because the
  documented functions return `void`.
- An unexpected resident return is a failure and triggers an immediate
  stop/unload attempt.
- A successful API round trip is not authorization to access debug registers;
  that remains a separate owner-attended gate.

## Known uncertainty

VitaSDK documents the API and 3.60 NIDs but not the installed firmware's
setter implementation. Its storage target, synchronization, bounds behavior,
persistence, and hardware side effects have not been established from a
decrypted `SceSysmem` image. The short set-to-clear window could still be seen
by another CPU core. The probe does not mask interrupts or preemption around
opaque functions because doing so could deadlock and would not establish
cross-core atomicity.

## Build and audit

Use a source/build path without spaces for the VitaSDK packaging tools:

```sh
cmake -S kernel/dipsw-set-restore-probe \
  -B kernel/dipsw-set-restore-probe/build-final \
  -G "Unix Makefiles" \
  -DCMAKE_MAKE_PROGRAM=/usr/bin/make.exe
cmake --build kernel/dipsw-set-restore-probe/build-final --clean-first
```

Before every hardware run, repeat the host record test, VPK verification,
symbol/NID inventory, and ARM/Thumb disassembly audit. Reject a binary unless
the active window is exactly Set(228), GetInfo(7), Check(228), Clear(228), and
contains no branch or additional call before Clear. Reject any CP14/CP15,
VFP/NEON, MMIO, thread, hook, or unexpected import.

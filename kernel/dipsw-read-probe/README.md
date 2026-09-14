# Read-only KBL DIP-switch probe

Status: source-complete, host-tested, and successfully exercised on the
project's retail Vita, later confirmed to run system software 3.65, on
2026-09-13 before and after the DIP 228 set/restore rung. The raw checksummed
records and decoded summaries are preserved under `hardware-results/`. This
does not establish behavior on another firmware or device class.

This disposable title reads the current KBL/DIP-switch policy state needed for
the hardware-breakpoint investigation. It does not set or clear any DIP switch
and does not access CP14 or comparator registers.

One deliberate X-button edge first writes and verifies a durable `ATTEMPTED`
record, then dynamically loads a private one-shot kernel module. The module
writes `KERNEL_ENTERED`, calls only documented read-side DIP APIs, writes
`COMPLETE` into the other checksummed journal slot, and returns
`SCE_KERNEL_START_NO_RESIDENT`. Circle exits. Relaunch the title for another
sample.

## Values collected

- CP version and build-ID word: `ksceKernelGetDipswInfo(1)`;
- debug and system-control words: indices 6 and 7;
- global DIP bits 203 and 228 through `ksceKernelCheckDipsw`;
- independent agreement between each named bit and its containing raw word.

Firmware and device identity belong in the accompanying test log. They are
deliberately excluded from this minimal kernel batch so a failed run has the
smallest possible call surface. The archived run metadata now records the
owner's later confirmation that this test device was running system software
3.65; that value is not contemporaneous probe output.

The alternating reports are
`ux0:data/VitaDebugger/dipsw-read-v1-{a,b}.bin`. Decode pulled copies with:

```sh
python decode_record.py dipsw-read-v1-a.bin dipsw-read-v1-b.bin
```

## Safety boundary

- No `ksceKernelSetDipsw` or `ksceKernelClearDipsw` import or call.
- No CP14/CP15 instruction, register write, MMIO, hook, callback, worker thread,
  public export, boot configuration, or resident kernel service.
- The only general kernel-library dependency is the compiler-generated
  `memcpy` used for fixed-size journal record copies.
- The loader and module accept only exact version, size, checksum, sequence,
  revision, state, and lifecycle transitions. An incomplete or failed newest
  record locks the probe for inspection.
- An unexpected resident return triggers an immediate stop/unload attempt and
  is a failed run.
- Installing the VPK does not execute kernel code; the single run requires an
  X-button press in title `VDCP00005`.

## Build

From a VitaSDK environment:

```sh
cmake -S kernel/dipsw-read-probe \
  -B kernel/dipsw-read-probe/build-final \
  -G "Unix Makefiles" \
  -DCMAKE_MAKE_PROGRAM=/usr/bin/make.exe
cmake --build kernel/dipsw-read-probe/build-final --clean-first
```

On Windows, keep the source and build paths free of spaces because the VitaSDK
VPK conversion tools do not consistently preserve quoted paths.

## Audited first-run artifact

The 2026-09-12 clean build passed the host record/decoder tests, VPK verifier,
source scan, import/NID audit, and ARM/Thumb machine-code audit. Its kernel call
manifest is exactly `GetDipswInfo(1, 6, 7)`, `CheckDipsw(203, 228)`, `CpuId`,
journal I/O, and compiler-generated `memcpy`.

- VPK SHA-256:
  `f51bf32932d2673760b372b31144b1bb4cf6f5875dfa4653112d9edc3a9ff796`
- SKPRX SHA-256:
  `6bbcc7da434b8a21736469d7f1af83b7e33bf6ed4873581cfff9f291c0cb48be`
- EBOOT SHA-256:
  `fd9791dd0eab25c78b2edf650c62c18794ad736fbb7fe8a91837aad61379fe53`

Rebuilds must be audited again. Reject any artifact containing a DIP setter or
clearer, CP14/CP15 `MRC`/`MCR`, other coprocessor debug access, an FP/NEON
instruction, or an unexpected import.

# ThreadMgr setter resolver probe

Status: source-complete and host-tested. **It has not yet been run on Vita
hardware.** A `PASS` from this probe would establish only that four fixed NIDs
resolve into executable `SceKernelThreadMgr` segments and that their first 64
bytes were captured. It would not establish a setter prototype, prove that a
setter is safe to call, or enable any writable VitaDebugger capability.

This is a disposable, opt-in discovery title for the next foreign-thread
register-mutation research rung. It is deliberately separate from the installed
VitaDebugger kernel companion and from its production mutation ABI.

## Fixed discovery set

The title accepts no address, NID, module, library, or byte-count input. Its
one-shot kernel module asks taiHEN's kernel module utility to resolve exactly:

| Record label | NID | Current evidence |
| --- | ---: | --- |
| core get | `0x5022689D` | VitaSDK names and declares `ksceKernelGetThreadCpuRegisters(SceUID, SceThreadCpuRegisters *)`; the output is 0x90 bytes and the documented precondition is a suspended thread |
| core set candidate | `0x64E89DE9` | provisional reverse-engineering candidate; absent from the installed VitaSDK NID database and no 3.65 prototype is known |
| VFP get | `0x5CDE387A` | VitaSDK names `ksceKernelGetVfpRegisterForDebugger(SceUID, void *)`; the project's separate hardware gate established the 256-byte D0-D31 layout, not a write contract |
| VFP set candidate | `0x49A0B679` | provisional reverse-engineering candidate; absent from the installed VitaSDK NID database and no 3.65 prototype is known |

All four lookups are fixed to module `SceKernelThreadMgr` and use
`TAI_ANY_LIBRARY` (`0xFFFFFFFF`). The two setter labels are hypotheses under
test, not supported names or callable APIs. A missing candidate is a valid and
important result.

The installed VitaSDK database independently exposes the public VM-context lead
`sceKernelSetThreadContextForVM` (`0x27E6DEDE`) and its internal lead
`_sceKernelSetThreadContextForVM` (`0xD4785C41`). Neither is called or resolved by
this title: its prototype, buffer sizes, VM-thread attribute requirements,
process boundary, suspension rule, and VFP coverage remain unverified on retail
3.65. See [`docs/hardware/research-review-2026-09-14.md`](../../docs/hardware/research-review-2026-09-14.md)
for the project-wide evidence boundary.

## Resolver and address proof

The SKPRX uses `module_get_export_func(SceUID pid, const char *module,
uint32_t library_nid, uint32_t function_nid, uintptr_t *address)` from
`libtaihenModuleUtils_stub.a` with `KERNEL_PID` and `TAI_ANY_LIBRARY`. This is the
kernel-side utility used by projects such as VitaShell. The public
`taiGetModuleExportFunc()` wrapper is not used because it is a user API whose
kernel syscall searches the calling process rather than accepting an explicit
kernel PID.

The returned address is never trusted on its own. The probe separately:

1. locates `SceKernelThreadMgr` with `taiGetModuleInfoForKernel()`;
2. obtains its four segment records with `ksceKernelGetModuleInfo()`;
3. verifies the taiHEN export-table range is wholly inside one module segment
   and no larger than 64 KiB;
4. removes only the ARM Thumb-state bit from each returned function pointer;
5. proves the normalized address belongs to a module segment marked executable;
6. proves the entire fixed 64-byte window remains inside that segment; and
7. copies exactly 64 bytes with a volatile byte loop for offline analysis.

The alternating 640-byte records include the module NID, module ID, export-table
range, segment bases/sizes/permissions, each fixed NID, lookup result, raw and
normalized addresses, ARM/Thumb state, segment index/offset, and fixed code
window. FNV-1a checksums and strict semantic validation protect the lifecycle
journal from partial writes and internally inconsistent records.

## Safety boundary

- No resolved function pointer is invoked.
- No target thread is enumerated, retained, suspended, resumed, or modified.
- No register context is supplied and no setter-like import exists.
- No hook, injection, memory poke, coprocessor instruction, callback, worker
  thread, public kernel export, or boot configuration change exists.
- The kernel module always returns `SCE_KERNEL_START_NO_RESIDENT`.
- Disclosure is fixed at four compile-time NIDs and at most 4 × 64 code bytes.
- Installing the VPK does not execute the kernel module. One X-button edge in
  title `VDCP00008` durably arms and runs a single sample. Circle exits.
- An incomplete or corrupt newest journal locks the loader so evidence is not
  overwritten before it can be pulled.

The title records `ksceKernelGetSystemSwVersion()` only as spoofable metadata.
Enso_ex can make an actual retail 3.65 installation report 3.74, so no reported
version is a pass/fail condition. Hardware conclusions must instead be bound to
the independently documented actual baseline, the captured
`SceKernelThreadMgr` module NID/segments, and the generated fixed-code
fingerprint.

## Host tests

From the repository root with the validated Windows tool wrapper:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
  -File tools/invoke-vita-env.ps1 cc -std=c11 -O2 -Wall -Wextra -Werror `
  kernel/thread-setter-resolver-probe/test_record.c `
  -o kernel/thread-setter-resolver-probe/test-record.exe
kernel/thread-setter-resolver-probe/test-record.exe
py -3 -m unittest discover `
  -s kernel/thread-setter-resolver-probe -p test_decode_record.py -v
```

The tests cover the exact 640-byte ABI, checksum and wrap-safe journal ordering,
fixed target identity, successful 3.65 and spoofed 3.74 reports, unavailable
firmware metadata, missing exports, out-of-module addresses, non-executable
segments, truncated code windows, and bounded extraction.

## Build

From a VitaSDK environment:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
  -File tools/invoke-vita-env.ps1 cmake `
  -S kernel/thread-setter-resolver-probe `
  -B kernel/thread-setter-resolver-probe/build-audit `
  -G "Unix Makefiles" `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/usr/bin/make.exe
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
  -File tools/invoke-vita-env.ps1 cmake --build `
  kernel/thread-setter-resolver-probe/build-audit --clean-first
```

The output VPK is
`kernel/thread-setter-resolver-probe/build-audit/vitadebug-thread-setter-resolver-probe.vpk`.
Every rebuild requires a fresh import, NID, source, and machine-code audit before
hardware use.

## Hardware runbook

Do not place the SKPRX in `ur0:tai/config.txt`; it is packaged privately inside
the disposable VPK.

1. Record the actual device/firmware baseline independently of the spoofable
   system-version API, plus Enso_ex/version-spoof state.
2. Install and open title `VDCP00008`.
3. Confirm the screen says `READ ONLY` and `No setter/getter call`.
4. Press X once. Do not press it repeatedly and do not relaunch until both
   journal slots have been collected.
5. A normal lifecycle ends in either `Resolver gate passed` or `Discovery
   complete; gate blocked`. Both are useful. A kernel-entered but incomplete
   record is a failure that must be preserved for review.
6. Pull both files, if present:
   - `ux0:data/VitaDebugger/thread-setter-resolver-v1-a.bin`
   - `ux0:data/VitaDebugger/thread-setter-resolver-v1-b.bin`
7. Decode and extract only the validated fixed windows:

```powershell
py -3 kernel/thread-setter-resolver-probe/decode_record.py `
  thread-setter-resolver-v1-a.bin thread-setter-resolver-v1-b.bin `
  --extract-dir thread-setter-resolver-analysis `
  --actual-baseline "retail 3.65; Enso_ex version spoof state documented"
```

The decoder selects the newest valid journal revision, rejects equal-revision
conflicts, writes at most four 64-byte `.bin` files, and creates
`metadata.json` with per-window SHA-256 values and a combined fixed-code
fingerprint. It prints an `arm-vita-eabi-objdump` command for each ARM or Thumb
window.

## Promotion gate after a hardware sample

Even a resolver `PASS` is **not** permission to wire a setter into the
production backend. The following remain mandatory:

1. preserve the two raw journals and decoded metadata alongside the independently
   documented actual firmware baseline;
2. confirm module NID, segment layout, normalized addresses, and code hashes are
   repeatable across a clean app relaunch and reboot;
3. disassemble every captured window and determine whether it is an
   implementation, veneer, or error stub; obtain more static evidence through a
   separately reviewed bounded step if 64 bytes cannot establish the contract;
4. establish the exact 3.65 setter prototype, structure size/alignment, writable
   fields, return values, thread attributes, process boundary, and suspension
   preconditions from lawful static analysis;
5. validate a read-only object-lifetime lease before any write. The current
   candidates are `ksceGUIDReferObject`/`ksceGUIDReleaseObject`, but their exact
   thread-object/class use is not yet a proven contract for this backend;
6. keep the exact retained thread and process objects alive across suspend,
   snapshot, stage, read-back, rollback, and cleanup; reject UID churn/reuse and
   leave an uncertain target stopped;
7. perform the first mutation only in a disposable same-process worker, with a
   supported debug-suspend state and one controlled callee-saved GPR, followed
   by read-back, exact restore, resume, detach, reconnect, exit, timeout, and
   unload gates; and
8. treat core and VFP as separate transactions because their snapshots overlap
   FPSCR state. A VFP write also has to account for lazy VFP ownership. The safer
   cooperative in-thread VFP trampoline remains a separate design lead.

Until all applicable gates pass, the production kernel backend must continue to
advertise zero writable foreign register banks and GDB must reject those writes.

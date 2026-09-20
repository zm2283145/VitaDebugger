# ThreadMgr setter prerequisite probe

Status: **v2 is compile-tested but has not been run on Vita hardware. It is
read-only, non-dereferencing, and does not enable register writes.**

The earlier v1 resolver (`VDCP00008`) is retired. Its first retail 3.65 attempt
failed before `module_start()` with `SCE_KERNEL_ERROR_MODULEMGR_NO_LIB`. After
the unavailable static import was replaced, its second attempt durably reached
only `kernel entered`; the Vita powered off inside the all-at-once metadata/code
collector. No getter or setter candidate was called, but v1 must not be rerun.

This v2 title (`VDCP00009`) is the bounded fallback gate. It performs one fixed
operation between durable checkpoints:

1. enter the kernel module;
2. query spoofable firmware metadata;
3. query `SceKernelThreadMgr` identity through
   `taiGetModuleInfoForKernel()`;
4. resolve each of four fixed NIDs separately with
   `module_get_export_func()`; and
5. finalize a presence-only record.

The probe never invokes or dereferences a dynamically resolved pointer. It does
not read code bytes, inspect a target thread, retain an object, suspend/resume a
thread, or write any register or kernel memory. A power loss therefore leaves a
validated journal identifying the last completed operation rather than an
ambiguous all-at-once collector.

## Fixed lookup set

| Label | NID | Evidence boundary |
| --- | ---: | --- |
| core get | `0x5022689D` | Current VitaSDK declares `int ksceKernelGetThreadCpuRegisters(SceUID, SceThreadCpuRegisters *)`; `SceThreadCpuRegisters` is 0x90 bytes and contains only 32-bit fields; the header requires a suspended thread |
| core set candidate | `0x64E89DE9` | Provisional historical lead only; absent from the installed VitaSDK headers and NID database |
| VFP get | `0x5CDE387A` | Current VitaSDK declares `int ksceKernelGetVfpRegisterForDebugger(SceUID, void *)`; the pointer type does not establish a setter layout |
| VFP set candidate | `0x49A0B679` | Provisional historical lead only; absent from the installed VitaSDK headers and NID database |

All lookups use module `SceKernelThreadMgr`, `KERNEL_PID`, and
`TAI_ANY_LIBRARY`. Returned addresses are recorded only as untrusted presence
observations. v2 does not prove that an address belongs to the module, is
executable, has stable bytes, has any proposed signature, or is callable.

The installed VitaSDK 3.60 NID catalog also lists the specialized user export
`sceKernelSetThreadContextForVM` (`0x27E6DEDE`) and internal export
`_sceKernelSetThreadContextForVM` (`0xD4785C41`). No installed header declares
either prototype or a context structure. Their size, alignment, writable
fields, VM-thread attributes, process boundary, suspension preconditions,
return semantics, and VFP coverage are unresolved. A database name/NID is not a
generic kernel setter contract and is not included in this hardware probe.

## Journal ABI

v2 preserves the 640-byte bounded record size but uses a new magic/version and
new paths:

- `ux0:data/VitaDebugger/thread-setter-prerequisite-v2-a.bin`
- `ux0:data/VitaDebugger/thread-setter-prerequisite-v2-b.bin`

The alternating records include sequence/revision, checkpoint state, reported
firmware metadata, module identity/export-table metadata, each fixed lookup
result, raw address, normalized ARM/Thumb address, and the number of completed
target lookups. Code, segment, executable, and fingerprint fields must remain
zero. FNV-1a checksums and strict semantic validation reject torn records,
changed NIDs, skipped checkpoints, partial address payloads, code bytes, or any
claim that an address was authenticated.

A complete `PASS` means only that all four fixed lookups returned nonzero
addresses during that one sample. It does **not** verify a setter, ABI,
fingerprint, lifetime contract, or writable capability.

## Host tests

```powershell
C:\msys64\usr\bin\make.exe host-test-thread-setter-resolver-record `
  HOST_CC=C:/msys64/mingw64/bin/gcc.exe
```

The C and Python tests cover the exact 640-byte ABI, every durable checkpoint,
fixed target identities, firmware spoof/failure handling, missing exports,
forbidden code/trust metadata, checksum rejection, wrap-safe journal selection,
and metadata output with no extracted binaries.

## Build

```powershell
$env:VITASDK = "/c/vitasdk"
$env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;C:\vitasdk\bin;$env:PATH"
& C:\msys64\usr\bin\cmake.exe `
  -S kernel/thread-setter-resolver-probe `
  -B kernel/thread-setter-resolver-probe/build-audit `
  -G "Unix Makefiles" `
  '-DCMAKE_MAKE_PROGRAM=/usr/bin/make'
& C:\msys64\usr\bin\cmake.exe --build `
  kernel/thread-setter-resolver-probe/build-audit --clean-first
```

The output is
`kernel/thread-setter-resolver-probe/build-audit/vitadebug-thread-setter-prerequisite-probe.vpk`.
Build success is compile evidence only.

## Hardware runbook

The following steps are required for a future reviewed v2 run. They have not
been performed by this change.

1. Confirm the package is title `VDCP00009`, version `02.00`, and not retired
   v1 title `VDCP00008`.
2. Record the actual device/firmware baseline independently of
   `ksceKernelGetSystemSwVersion()`, including Enso_ex and version-spoof state.
3. Preserve and remove any prior v2 journal only after confirming both slots
   were copied; never mix v1 and v2 files.
4. Install/open the disposable VPK. Confirm the screen says
   `No dynamic call, dereference, code read, or write`.
5. Press X once. Do not repeat or relaunch until both journal slots are pulled.
   Circle exits without running.
6. Pull both v2 files listed above. An incomplete newest checkpoint is evidence
   of the failing operation and must be preserved, not overwritten.
7. Validate and emit metadata:

```powershell
py -3 kernel/thread-setter-resolver-probe/decode_record.py `
  thread-setter-prerequisite-v2-a.bin `
  thread-setter-prerequisite-v2-b.bin `
  --extract-dir thread-setter-prerequisite-v2-analysis `
  --actual-baseline "retail 3.65; Enso_ex/version spoof state documented"
```

8. Repeat only after review, once after a clean application relaunch and once
   after reboot. Compare module NID, export-table metadata, lookup results, and
   normalized addresses. Address stability is still not code authentication.

## Remaining promotion gates

1. Obtain lawful static evidence for an actual setter implementation and its
   exact prototype, context size/alignment, writable fields, thread attributes,
   process boundary, suspension rules, return semantics, and firmware/module
   fingerprints. v2 intentionally collects none of this code evidence.
2. Establish an atomic process/thread retain contract that survives close,
   exit, UID reuse, parent changes, timeout, disconnect, cleanup retry, and
   plugin unload. Current GUID release is by numeric UID, and no public
   generation/non-reuse guarantee is documented.
3. Only then bind the separately reviewed, default-off host-modeled R4/R5
   restorable contract to a disposable hardware title. Its provider ABI still
   requires proven native size/alignment, writable mask, full-snapshot and
   return semantics, authenticated binding, exact retained objects, parent
   relation, debug suspension, and serialization. Require read-back, exact
   restore, resume, detach/reconnect, and watchdog recovery.
4. Keep VFP/NEON separate. A core setter must not be assumed to update D0-D31
   or FPSCR, especially with lazy VFP ownership.

Production VitaDebugger continues to advertise zero writable foreign core and
VFP banks.

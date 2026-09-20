# Exception, memory, and File-I/O hardware gate

This is a local preparation and bounded hardware procedure for the safety work
based on commit `838e235740968583cee3fbfc2ef26f17fd0423ce`. The candidate
manifest names the later fixture commit that was actually built. Do not use a
dirty worktree or substitute an ELF/VPK whose SHA-256 differs from that
manifest.

## Companion decision

The unchanged VitaDebugger kernel companion is sufficient. A candidate starts
the debugger only when `vdKernelGetStatus()` reports ABI `0x0001000E`,
`VD_KERNEL_MAX_THREADS`, and all of:

- `VD_KERNEL_CAP_THREAD_LIST`
- `VD_KERNEL_CAP_THREAD_CONTROL`
- `VD_KERNEL_CAP_THREAD_REGISTERS`
- `VD_KERNEL_CAP_STOP_RECONCILE`

Those operations provide the coherent all-stop, register inspection, stop
renewal/recovery, and explicit resume required by these gates. Live `M` uses
KuBridge unrestricted memory copy, cache synchronization, and readback while
that stop is held; it does not use the disabled foreign-thread register
mutation backend. Exception predecessor chaining is a user/KuBridge callback
operation. File-I/O needs no new kernel ABI.

KuBridge still does **not** provide the dispatcher-lifetime fence required for
safe handler reset and image unload. `uvdb_prepare_unload()` therefore remains
fail-closed. Do not call `uvdb_shutdown()`, reset the handler registry, unload a
SUPRX, unload KuBridge, or unload the VitaDebugger kernel plugin during any
gate. Reset/unload remains blocked until a separately versioned KuBridge fence
ABI has passed its own hardware validation.

## Frozen local candidates

Build KuBridge at its unchanged checkout and the kernel companion first, then
commit the diagnostic fixture. From a clean worktree at that exact commit:

```powershell
$env:PATH = 'C:\msys64\usr\bin;' + $env:PATH
Set-Location 'D:\Claude\kuBridge'
C:\msys64\usr\bin\bash.exe -c 'export PATH=/usr/bin:$PATH; export VITASDK=/c/vitasdk; cmake -S . -B build-local -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release && cmake --build build-local --parallel'
Set-Location '<VITADEBUGGER-WORKTREE>'
C:\msys64\usr\bin\bash.exe -c 'export PATH=/usr/bin:$PATH; export VITASDK=/c/vitasdk; cmake -S kernel -B kernel/build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release && cmake --build kernel/build --parallel'

$Out = 'C:\Users\ZachMinton\.copilot\session-state\fc87b59d-45a5-4dde-b97f-b502d73dcedc\files\exception-memory-fileio-gate'
.\tools\freeze_exception_memory_fileio_candidates.ps1 `
  -ExpectedCommit '<fixture-commit>' `
  -OutputDirectory $Out `
  -KuBridgeDirectory 'D:\Claude\kuBridge'
Get-Content "$Out\manifest.json"
Get-FileHash -Algorithm SHA256 "$Out\manifest.json"
```

The script refuses a dirty tree, a wrong commit, missing local companion
artifacts, or an existing output directory. It freezes four distinct
VPK/ELF/Vita-ELF/eboot variants plus the exact kernel and KuBridge SKPRX files:

- `predecessor`: known predecessors for all three exception types, memory fault
  injection, and File-I/O control.
- `null-data-abort`, `null-prefetch-abort`, and
  `null-undefined-instruction`: one deliberately NULL predecessor each, with
  `UVDB_EXPERIMENTAL_NESTED_FAULT_EXIT` enabled. These are disposable
  exit-only candidates and do not establish default-build containment.

Store hardware results under a new timestamped directory beside the manifest,
for example `hardware-2026-04-01T120000Z`. Never overwrite prior evidence.

## Close, install, and launch

Set placeholders explicitly. These commands are documentation only; local
preparation must not set `$VitaIp` to a reachable Vita.

```powershell
$VitaIp = '<VITA-IP>'
$Key = '<ABSOLUTE-PATH-TO-DEPLOY-PRIVATE-KEY>'
$Out = 'C:\Users\ZachMinton\.copilot\session-state\fc87b59d-45a5-4dde-b97f-b502d73dcedc\files\exception-memory-fileio-gate'
$Evidence = Join-Path $Out 'hardware-<UTC-TIMESTAMP>'
New-Item -ItemType Directory -Path $Evidence
$env:PYTHONPATH = (Resolve-Path '.\deploy\host').Path

py -3 -c "from vitadevdeploy.companion import VitaCompanionClient; print(VitaCompanionClient('$VitaIp').kill('SLRS00001'))"
Start-Sleep -Seconds 2
py -3 -m vitadevdeploy deploy `
  "$Out\predecessor\uvdb-test.vpk" `
  --vita $VitaIp --private-key $Key --action install_launch `
  --output "$Evidence\predecessor-install-job"
```

**Hard stop:** do not continue unless the on-device screen says both
`Kernel thread gate: PASS` and
`Exception/memory/File-I/O safety fixture: READY`, and the candidate VPK/ELF
hashes exactly match `manifest.json`. Any unexpected core dump, title exit,
kernel gate failure, RSP timeout, checksum error, stale symbol address, or
non-pass evidence JSON ends the run. Preserve the evidence and close the title;
do not improvise recovery with reset or unload.

## Gate 1: live `M` and File-I/O

Use the predecessor candidate and substitute its two hashes from the manifest:

```powershell
py -3 .\tools\gdb_exception_memory_fileio_gate.py `
  --host $VitaIp `
  --elf "$Out\predecessor\test.elf" `
  --vpk "$Out\predecessor\uvdb-test.vpk" `
  --elf-sha256 '<ELF-SHA256>' `
  --vpk-sha256 '<VPK-SHA256>' `
  --mode memory-fileio `
  --evidence "$Evidence\memory-fileio.json"
```

The bounded driver performs these exact checks:

1. Resolves symbols from one live `qOffsets`/library snapshot and rejects an
   ELF or VPK hash mismatch.
2. Commits a 192-byte write, forcing three internal 64-byte copy chunks, and
   reads it back.
3. Arms `Z0` inside that range, commits different bytes, verifies all bytes
   outside the physical UDF while armed, removes `z0`, and verifies the newly
   committed bytes.
4. Fails copy chunk two once, requires `E0e`, and proves immediate restoration
   of all original bytes.
5. Fails chunk two plus the first two restore attempts. It requires peer loss,
   reconnects under the retained stop/rollback obligation, detaches to retry
   restoration, reconnects, and proves the original bytes.
6. Arms a fixture call to `uvdb_remote_syscall("write", ...)`, sends the
   literal standard interrupted result `F-1,4,C`, requires exactly one `T02`,
   performs `g` and `p0` inspection while stopped, proves the worker has not
   completed, resumes only with explicit `c`, stops again with Ctrl-C, and
   verifies result `-1` and completion.

This is fault injection, not an organic KuBridge copy failure. It is
range-scoped to the 192-byte fixture buffer and auto-expires after a bounded
copy-call window so the control words remain writable.

## Gate 2: exact predecessor chaining

Relaunch the same predecessor candidate, wait for both READY lines, then run:

```powershell
py -3 .\tools\gdb_exception_memory_fileio_gate.py `
  --host $VitaIp `
  --elf "$Out\predecessor\test.elf" `
  --vpk "$Out\predecessor\uvdb-test.vpk" `
  --elf-sha256 '<ELF-SHA256>' `
  --vpk-sha256 '<VPK-SHA256>' `
  --mode predecessor `
  --evidence "$Evidence\predecessor.json"
```

For data abort, prefetch abort, and undefined instruction, the fixture injects
a real nested fault while VitaDebugger owns the outer exception callback. The
known predecessor records the exact type, rewrites the nested return PC, and
deliberately mutates `exceptionType`. The driver requires two successful
iterations of each type, proving that VitaDebugger releases the chain bit using
the pre-callback type rather than the mutated field. Each nested fault is
expected to retire its RSP generation; the fixture performs only
`uvdb_stop_server()`/`uvdb_start_server()` after the callback has returned.

## Gate 3: NULL predecessor

These are manual, disposable, one-case-per-launch gates. They prove only the
opt-in `UVDB_EXPERIMENTAL_NESTED_FAULT_EXIT` trampoline. They must never be
reported as containment by the default build.

For each `null-*` candidate:

1. Close `SLRS00001`, wait two seconds, install/launch the selected VPK, and
   verify the manifest hashes and both READY lines.
2. Attach with GDB using the matching ELF, write exception request `1`, `2`, or
   `3` to `uvdb_safety_gate_exception_request`, and continue.
3. Require the title to exit within five seconds without a new core dump.
   Record the GDB transcript, before/after `ux0:data` dump listing, and the
   Companion response in the evidence directory.
4. Hard stop on a hang, relaunch, kernel error, core dump, or return to the
   fixture loop. Do not try another exception type in that process.

The request/type mapping is:

| Candidate | Request | Nested exception |
|---|---:|---|
| `null-data-abort` (`kind=4E554C00`) | `1` | data abort |
| `null-prefetch-abort` (`kind=4E554C01`) | `2` | prefetch abort |
| `null-undefined-instruction` (`kind=4E554C02`) | `3` | undefined instruction |

Use these exact GDB commands after substituting the matching request:

```powershell
arm-vita-eabi-gdb '<MATCHING-CANDIDATE-DIRECTORY>\test.elf' `
  -ex 'set pagination off' `
  -ex "target remote ${VitaIp}:1234" `
  -ex 'set {unsigned int}uvdb_safety_gate_exception_request = <REQUEST>' `
  -ex 'continue'
```

Before and after that command, capture the bounded dump listing:

```powershell
curl.exe --max-time 10 "ftp://${VitaIp}:1337/ux0:/data/" `
  | Set-Content "$Evidence\<VARIANT>-dumps-before.txt"
curl.exe --max-time 10 "ftp://${VitaIp}:1337/ux0:/data/" `
  | Set-Content "$Evidence\<VARIANT>-dumps-after.txt"
```

Loss of the RSP socket alone is not proof of orderly exit. That is why the
automated driver refuses NULL candidates: the Companion/title state and absence
of a new dump are mandatory corroboration.

## Rollback

On any failure, preserve evidence, close only the test title, and reinstall the
previously hashed known-good VPK:

```powershell
py -3 -c "from vitadevdeploy.companion import VitaCompanionClient; print(VitaCompanionClient('$VitaIp').kill('SLRS00001'))"
Start-Sleep -Seconds 2
py -3 -m vitadevdeploy deploy `
  '<ABSOLUTE-PATH-TO-KNOWN-GOOD-VPK>' `
  --vita $VitaIp --private-key $Key --action install_launch `
  --output "$Evidence\rollback-install-job"
```

If VitaDevDeploy reports an interrupted job, inspect it without launching or
unloading anything:

```powershell
py -3 -m vitadevdeploy recovery-status --vita $VitaIp
```

Do not use reboot, plugin reload, handler reset, or SUPRX/SKPRX unload as a gate
recovery technique.

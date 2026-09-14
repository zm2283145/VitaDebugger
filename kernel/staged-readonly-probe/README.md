# VitaDebugger staged read-only ladder

This is a disposable diagnostic VPK for locating the exact instruction that
caused the project's retail Vita, later confirmed to run system software 3.65,
to reboot during the original disabled-comparator probe. It is separate from
that probe and does not replace or modify it.

## Safety boundary

- The app never edits `ur0:tai/config.txt` and installs no boot plugin.
- Its private kernel module has no exports, hooks, callbacks, or worker threads.
- Every kernel start performs at most one selected diagnostic operation and
  returns `SCE_KERNEL_START_NO_RESIDENT`.
- It contains no CP14 write (`MCR`) instructions. The debug-register rungs are
  reads only.
- The kernel target is built with `-mgeneral-regs-only`, vectorization disabled,
  no stack protector, and no standard-library dependency.
- The loader records the selected rung durably before loading kernel code. The
  kernel records entry before the selected operation and completion afterward.
- Kernel entry also records the expected core. The module then disables local
  interrupts only long enough to recheck that core and execute the one selected
  operation. A migration or core 3 aborts without executing the selected rung;
  this same critical-section choreography is exercised by rung 1.
- Two alternating checksummed files preserve the last valid journal state if a
  write is interrupted.

Read-only CP14 access can still trap on this 3.65 hardware baseline and reboot
the Vita. This does not establish behavior on another firmware or device class.
Run the rungs in order. After a reboot, do not retry the incomplete rung;
inspect its journal first.

The ARMv7 debug-access table makes the ordering important: when
DBGSWENABLE/DSE is low, DIDR and the internal DSCR view can remain readable
while DBGVCR and comparator accesses are UNDEFINED. Therefore, DIDR or DSCR
success is not permission to jump to a comparator rung. If DBGVCR is the first
incomplete rung, stop there; do not run BCR0/BVR0/WCR0/WVR0.

## Rungs

| Rung | Operation | CP14 |
| ---: | --- | :---: |
| 1 | Kernel lifecycle/no-op constant | No |
| 2 | `ksceKernelCpuId()` | No |
| 3 | MIDR via CP15 | No |
| 4 | DIDR | Read |
| 5 | DSCR | Read |
| 6 | DBGVCR | Read |
| 7 | BCR0 | Read |
| 8 | BVR0 | Read |
| 9 | WCR0 | Read |
| 10 | WVR0 | Read |

Each accepted rising edge of X executes only the currently displayed rung; O
exits. Rungs are strictly sequential and cannot be skipped. A completed rung
selects the next one. A failed or incomplete rung locks the ladder on restart,
so neither that rung nor a later one can execute accidentally. After inspecting
the result, intentionally delete both journal-slot files to restart at rung 1.

## Journal interpretation

The two slots are:

- `ux0:data/VitaDebugger/hw-read-ladder-v1-a.bin`
- `ux0:data/VitaDebugger/hw-read-ladder-v1-b.bin`

The valid slot with the newest `revision` is authoritative:

- `loader armed`: the user-space record was durable, but kernel entry was not
  durably confirmed. The load/start path or the kernel's pre-operation journal
  may have failed.
- `kernel entered`: the selected rung was durably identified immediately before
  its guarded operation, together with the expected core. If the Vita rebooted,
  the short interrupt-guard sequence or that single operation is the remaining
  boundary. Because rung 1 executes the identical guard sequence without a
  register read, a prior rung-1 pass makes the selected operation the leading
  cause. A failure while writing the completion record remains a lesser
  ambiguity.
- `complete`: the selected operation returned and its 32-bit value is recorded.

`decode_record.py slot-a.bin slot-b.bin` validates both checksums and prints the
newest record. The app also displays the newest valid record on launch.

This ladder identifies whether a specific CP14 read is accepted or traps. It
does not by itself distinguish a fixed hardware authentication input from a
secure-kernel policy that controls that input, and it does not test whether
debug-register writes or enabled comparators are usable.

## Build

Use the repository's validated Windows wrapper so the Vita compiler receives
all of its required runtime DLLs. If the repository path contains spaces, use a
no-space junction because VitaSDK's generated export rules do not quote every
path and CMake canonicalizes ordinary 8.3 paths back to their long names.

```powershell
$repo = (Resolve-Path ..\..).Path
$cmake = (Get-Command cmake.exe).Source
New-Item -ItemType Junction -Path C:\vdrepo -Target $repo

& "$repo\tools\invoke-vita-env.ps1" $cmake --fresh `
  -S C:\vdrepo\kernel\staged-readonly-probe `
  -B C:\vdrepo\kernel\staged-readonly-probe\build `
  -G "Unix Makefiles" `
  -DCMAKE_MAKE_PROGRAM=C:\msys64\usr\bin\make.exe `
  -DCMAKE_TOOLCHAIN_FILE=C:\vitasdk\share\vita.toolchain.cmake

& "$repo\tools\invoke-vita-env.ps1" $cmake --build `
  C:\vdrepo\kernel\staged-readonly-probe\build --parallel 4
```

Choose a different unused junction name if `C:\vdrepo` already exists. Remove
only the junction after the build when it is no longer wanted.

The record ABI/checksum test is host-native:

```powershell
& "$repo\tools\invoke-vita-env.ps1" C:\msys64\mingw64\bin\gcc.exe `
  -std=c11 -Wall -Wextra -Werror -pedantic `
  -I C:\vdrepo\kernel\staged-readonly-probe\include `
  C:\vdrepo\kernel\staged-readonly-probe\test_record.c `
  -o C:\vdrepo\kernel\staged-readonly-probe\build\vd-read-record-test.exe
& C:\vdrepo\kernel\staged-readonly-probe\build\vd-read-record-test.exe
```

The output is `build/vitadebug-staged-readonly-probe.vpk`, title ID
`VDCP00004`. Installing the VPK does not execute kernel code; pressing X on a
selected rung does.

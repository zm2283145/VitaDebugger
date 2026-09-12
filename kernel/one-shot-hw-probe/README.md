# One-shot disabled hardware-debug probe

This is the first write-access gate for VitaDebugger's experimental Cortex-A9
hardware breakpoint work. It is deliberately separate from the resident
`vitadebug.skprx` service.

The VPK contains an unsafe loader application and a private kernel module. The
loader does nothing until a new X-button press is observed. It then uses the
taiHEN on-demand loader; no `config.txt` entry and no reboot are required.

The kernel module:

- has no syscall exports and installs no hooks;
- suspends local interrupts so its synchronous register sequence cannot migrate,
  accepts application cores 0-2, and refuses reserved core 3;
- reads MIDR, DIDR, DSCR, DBGVCR, BCR0/BVR0, and WCR0/WVR0;
- validates the Cortex-A9 inventory (at least 6 BRPs, 4 WRPs, and 2
  context-capable BRPs) before constructing the linked disabled test values;
- refuses to proceed if debug is already active or either comparator is enabled;
- writes only legal comparator controls whose enable bit is zero;
- never writes DSCR and never writes a comparator with `E=1`;
- is compiled with general-purpose registers only so compiler-generated
  VFP/NEON instructions cannot enter the kernel probe path;
- reads back the writable fields, restores the exact captured values, and
  verifies the restoration;
- writes a checksummed result record at
  `ux0:data/VitaDebugger/hw-disabled-roundtrip-v1.bin`; and
- returns `SCE_KERNEL_START_NO_RESIDENT` immediately after restoration and
  final result journaling.

There is no helper thread, hook, or service that can outlive `module_start`.
Because this module is never added to taiHEN configuration, a reboot after a
fault removes it and cannot create a plugin boot loop.

## Build

Use the repository's validated VitaSDK wrapper so the MinGW runtime DLLs needed
by `cc1.exe` are present in the child process PATH:

```powershell
$repo = (Resolve-Path ..\..).Path
$cmake = (Get-Command cmake.exe).Source
New-Item -ItemType Junction -Path C:\vdrepo -Target $repo

& "$repo\tools\invoke-vita-env.ps1" $cmake --fresh `
  -S C:\vdrepo\kernel\one-shot-hw-probe `
  -B C:\vdrepo\kernel\one-shot-hw-probe\build `
  -G "Unix Makefiles" `
  -DCMAKE_MAKE_PROGRAM=C:\msys64\usr\bin\make.exe `
  -DCMAKE_TOOLCHAIN_FILE=C:\vitasdk\share\vita.toolchain.cmake
& "$repo\tools\invoke-vita-env.ps1" $cmake --build `
  C:\vdrepo\kernel\one-shot-hw-probe\build --parallel 4
```

The no-space junction is required when the repository path contains spaces.
VitaSDK's generated ELF-conversion command does not quote its export-file path,
and CMake canonicalizes ordinary 8.3 source paths back to the long path. Choose
a different unused junction name if `C:\vdrepo` already exists, and remove only
the junction after the build when it is no longer wanted.

The record ABI and checksum can be tested on the host through the same wrapper:

```powershell
& "$repo\tools\invoke-vita-env.ps1" C:\msys64\mingw64\bin\gcc.exe `
  -std=c11 -Wall -Wextra -Werror `
  -I C:\vdrepo\kernel\one-shot-hw-probe\include `
  C:\vdrepo\kernel\one-shot-hw-probe\test_record.c `
  -o C:\vdrepo\kernel\one-shot-hw-probe\build\vd-hw-probe-record-test.exe
& C:\vdrepo\kernel\one-shot-hw-probe\build\vd-hw-probe-record-test.exe
```

The installable artifact is:

```text
build/vitadebug-one-shot-hw-probe.vpk
```

This probe must not be added to `ur0:tai/config.txt`. Install the VPK as an
unsafe homebrew application, launch it, read the warning, and press X exactly
once. A pass only establishes that disabled comparator state can be written,
read back, and restored on the reported application core. It does not establish
that monitor mode, enabled breakpoints, debug exceptions, or watchpoint
exceptions work.

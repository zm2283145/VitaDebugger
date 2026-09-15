# External-attach API and architecture audit

This audit covers the repository at kernel ABI `0x0001000D` and the VitaSDK
headers installed for the Windows development environment. It records declared
APIs, not a hardware-safety claim.

## Existing VitaDebugger boundary

The current companion is intentionally process-local:

- `vdKernelGetThreadList()` obtains `ksceKernelGetProcessId()` and enumerates
  only that PID.
- `vdKernelBeginStop()` records the calling PID and calling controller thread.
- stop renewal/end, register reads, the new transactional register-mutation
  scaffold, VFP snapshots, and hardware-debug ownership all require the current
  caller PID to match the session owner.
- user-visible thread IDs are resolved only among the calling process's kernel
  threads.

Consequently, calling the existing exports from a separate broker cannot stop
or inspect another application. The host protocol must not advertise external
attach merely because `vdKernelGetStatus()` succeeds.

The ABI has advanced for other companion capabilities, but this milestone does
not change the external-attach boundary described here.

## VitaSDK declarations relevant to a safe design

### Exact process discovery

`$VITASDK/arm-vita-eabi/include/psp2/appmgr.h` declares:

- `sceAppMgrGetIdByName(SceUID *pid, const char *name)` for exact title-to-PID
  resolution;
- `sceAppMgrGetNameById(SceUID pid, char *name)` for the reverse check;
- `sceAppMgrGetRunningAppIdListForShell()` and
  `sceAppMgrGetProcessIdByAppIdForShell()` for shell-style enumeration.

The proposed first broker should use the exact-title pair and refuse raw
host-selected PIDs. The shell-suffixed enumeration APIs may have capability
requirements and are unnecessary for a first allowlisted implementation. None
of these calls has been hardware-tested for the attach broker yet.

### Kernel identity revalidation

The kernel headers declare enough read-only metadata to close a PID-reuse race:

- `ksceKernelSysrootGetProcessTitleId()` / `ksceKernelGetProcessTitleId()`;
- `ksceKernelGetProcessInfo()` and `ksceKernelGetProcessStatus()`;
- `ksceKernelGetModuleIdByPid()` for the main module;
- `ksceKernelGetModuleInfo()` and
  `ksceKernelGetModuleInfoForDebugger()` for module/segment metadata; and
- `ksceKernelGetModuleFingerprint()` for an additional main-module identity
  value.

A future kernel loader must capture these values into a random, expiring target
ticket and repeat the checks immediately before load, start, stop, or unload.
PID alone is never an authorization handle.

### Per-PID module lifecycle

`$VITASDK/arm-vita-eabi/include/psp2kern/kernel/modulemgr.h` declares:

- `ksceKernelLoadModuleForPid()`;
- `ksceKernelStartModuleForPid()`;
- `ksceKernelLoadStartModuleForPid()`;
- `ksceKernelStopModuleForPid()`;
- `ksceKernelUnloadModuleForPid()`; and
- `ksceKernelStopUnloadModuleForPid()`.

The installed SDK also supplies `SceModulemgrForKernel` import stubs. These
declarations support investigating a debugger `.suprx` loaded through the
normal module lifecycle. They do not prove retail-firmware permissions,
supported target types, loader reentrancy, safe unload timing, or compatibility
with every application.

The kernel sysmem headers also expose per-PID copy routines. They are **not**
selected for the initial architecture. A general remote memory writer or thread
hijack would greatly widen the persistent kernel attack and crash surface while
duplicating work the normal module loader can perform.

## Proposed ownership split

### Host

- Select one exact nine-character title ID.
- Require the exact attach-protocol and kernel ABI versions.
- Verify a future broker challenge and sign any future mutation authorization.
- Never select a PID, kernel address, target memory address, or module path.

### User-mode broker

- Run as a narrowly scoped shell-resident `*main` user module, or an equivalent
  privileged development service. A normal foreground homebrew application
  cannot be assumed to remain active alongside its target.
- Own network parsing, pairing, rate limits, and title allowlists.
- Resolve title to PID and reverse-check PID to title.
- Ask the kernel only for an identity ticket and a fixed debugger-module action.
- Never hold a stop token or manipulate target memory itself.

Because a `*main` module is loaded into SceShell, it has no separate process
title or auth ID. Those values identify the SceShell container, not the broker
module, and another installed SceShell plugin shares that address space. The
kernel must therefore verify the paired host's signed, replay-resistant mutation
envelope itself. Caller title/auth-ID checks remain useful defense in depth but
are not sufficient authorization.

Vita Companion is a useful precedent for the shell-resident user-module
lifecycle and always-available network service. Its existing command port is
not authenticated or encrypted, so a future attach implementation must not add
mutating commands to that transport without the signed control protocol and
Vita-side allowlist required below.

### Narrow future kernel loader

- Verify the paired-host signature and kernel-maintained replay state before
  mutation; use SceShell title/auth ID only as an additional caller check.
- Reject the shell, kernel/system processes, the broker itself, and titles not
  in the Vita-side allowlist.
- Accept an opaque target ticket, not a caller-selected PID or path.
- Load only one compiled-in canonical path whose module fingerprint/digest was
  validated before the request.
- Track PID, title, main-module identity, injected module ID, controller, and a
  short lease in kernel memory.
- Roll back a partially loaded/started module and refuse a second owner.

### Injected user `.suprx`

- Run inside the target so existing caller-scoped thread control remains true.
- Initialize the existing GDB server without assuming the application already
  initialized networking.
- Publish a fresh connection nonce and target identity before GDB attaches.
- Restore exception handlers, breakpoints, stdio hooks, network ownership, and
  kernel sessions before allowing unload.

## Required proof before enabling load/start

1. Hardware-test only a disposable, purpose-built target whose title is
   compiled into the test loader allowlist.
2. Prove exact title/PID/main-module revalidation across target exit and rapid
   relaunch.
3. Verify load/start argument ownership and module-start threading semantics.
4. Inject failures after load, during start, after listener creation, during
   disconnect, and during stop/unload; every case must restore target state.
5. Prove lease expiry cannot leave threads suspended or a module half-owned.
6. Add authenticated, replay-resistant control messages in a new protocol
   version. Version 1 must remain read-only forever.
7. Complete firmware gates separately; header availability is not firmware
   coverage.

Until those gates pass, external application attach remains a design target,
not an advertised debugger capability.

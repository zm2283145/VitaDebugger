# VFP register layout validation gate

## What VitaSDK proves

VitaSDK currently exposes this declaration in
`psp2kern/kernel/threadmgr/debugger.h`:

```c
int ksceKernelGetVfpRegisterForDebugger(SceUID thid, void *pVfpRegister);
```

Its firmware 3.60 NID database maps that name to `0x5CDE387A`. Neither public
artifact defines the pointed-to type, its size, its alignment, the register
order, or whether FPSCR is included. Consequently, the D0-D31 layout cannot be
claimed from VitaSDK alone.

Here, "3.60" identifies the VitaSDK NID database entry; it is not a claim that
this path has been validated on firmware 3.60. The documented hardware gate
ran on a retail handheld Vita running system software 3.65.

The implementation therefore fails closed. Normal kernel builds omit the VFP
capability, do not import the undocumented call, and return
`VD_KERNEL_ERROR_VFP_DISABLED`. The candidate path exists only when CMake is
configured with `VITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT=ON`.

## Candidate-boundary protections

The experimental syscall retains the existing stop-session security boundary:

- The caller must own the active session and present its nonzero token.
- The target must be one of the exact kernel thread IDs suspended and tracked
  by that session; an arbitrary same-process or system thread is rejected.
- The operation is read-only and never accepts target register values.
- The candidate D32 array lives in a 64-byte-aligned global scratch object,
  avoiding a large allocation on the syscall stack. It is preceded by 256
  bytes of canaries and followed by 4096 bytes of canaries.
- Both canary regions are checked after the undocumented call whether it
  succeeds or fails. Any changed canary overrides every other result and
  rejects the call. Only the initialized public snapshot structure can be
  copied to the caller.
- VitaSDK's exact raw `SCE_KERNEL_ERROR_CAN_NOT_USE_VFP` result and the exact
  `SCE_KERNEL_ERROR_ILLEGAL_PERMISSION` result observed for valid suspended
  non-VFP threads on retail 3.65 are normalized immediately at this API
  boundary to `VD_KERNEL_ERROR_VFP_CONTEXT_UNAVAILABLE`. The public result uses
  VitaSDK's stable `SCE_KERNEL_ERROR_CAN_NOT_USE_VFP` encoding so it survives
  the syscall boundary unchanged. It is the only failure for which the GDB
  stub may retain the ARM core bank and emit unavailable VFP slots. Session,
  ownership, target lookup, CPU-register, user-copy, canary, layout, and every
  other raw failure reject the complete register read.
- Both raw FPSCR banks from `ksceKernelGetThreadCpuRegisters` are returned. The
  D32 v1 GDB mapping selects entry 0, as established by the known-pattern
  hardware probe, while retaining both entries as evidence and for future
  firmware checks.
- The GDB stub negotiates the D32 target description only after an exact ABI
  and capability query succeeds. Otherwise it keeps the legacy core-only
  packet shape even if its VFP code was compiled in.

Canaries detect a mismatched nearby layout; they do not turn an undocumented
ABI into a memory-safe contract. The call therefore remains absent from normal
builds. Its known-pattern and live-GDB gates pass on the documented retail 3.65
baseline, but every new firmware baseline must repeat both before enabling it.

## Required hardware gate

The separate `vitadebug-vfp-probe.vpk` gives one disposable worker unique
64-bit values in every register from D0 through D31 and a distinct FPSCR value.
It then starts an owned, leased stop session and verifies:

1. NULL output, a wrong token, and the unsuspended caller thread are rejected.
2. The guarded kernel read succeeds without a changed canary.
3. Layout version and register count match the D32 candidate contract.
4. Every D register equals its exact bit pattern, not merely a floating-point
   approximation.
5. Raw `fpscr_entry[0]` contains the worker's `0x00400000` value.
6. Ending the session resumes the worker, which restores its callee-saved VFP
   state and exits before a bounded timeout.

Every probe line passed on a retail handheld Vita running system software 3.65.
This particular gate has not been repeated on Vita TV, other firmware versions,
development hardware, or other kernel-plugin combinations. The later live GDB
gate also passes; the feature remains explicit opt-in because the underlying
kernel ABI is undocumented. Register writes remain out of scope.

### First hardware result

The first known-pattern run passed the D0-D31 bit-pattern, guarded snapshot,
ownership-rejection, session-end/resume, and worker-restoration checks. Its raw
display was `fpscr=00400000/00000000`. The sole FPSCR check failed because that
probe build expected entry 1. This establishes the deliberately independent
mapping used by D32 v1: ARM core registers use state-dependent raw-bank
selection, while FPSCR comes from entry 0.

A later launch of that same older installed build again returned the exact D32
patterns and `fpscr=00400000/00000000`, with zero return codes for begin,
snapshot, end, and worker wait. It additionally failed the end-session and
worker-exit assertions, whose `resumed_count` and `worker_status` operands were
not displayed by that build. The corrected probe therefore shows a distinct
bank-0 diagnostic banner, a per-run marker and thread IDs, the complete stop
counts, `resumed_count`, and `worker_status`. It does not relax either
assertion. The original raw result is preserved as
[FPSCR bank discovery evidence](hardware/kernel-vfp-probe-v8-fpscr-bank-discovery.jpg).

### Corrected hardware result

The corrected bank-0 diagnostic build passed every displayed assertion. It
captured the expected exact D0 and D31 endpoints, reported
`fpscr=00400000/00000000`, suspended two non-controller threads, resumed both,
and observed the worker exit with status zero after restoring its saved state.
The unedited result is preserved as
[the corrected all-pass VFP probe](hardware/kernel-vfp-probe-v8-bank0-pass.jpg).
This completes the known-pattern layout gate. The subsequent foreign-thread
live GDB gate also passed D0, D31, and FPSCR through continue/interrupt, clean
detach/reconnect, transport loss without `D`, and final reconnect/detach on the
same retail 3.65 baseline. Its machine-readable record is
[the retail 3.65 live-GDB evidence](hardware/gdb-vfp-live-3.65.json). The
capability remains opt-in because the snapshot ABI is undocumented and must be
revalidated for every firmware and hardware baseline.

The final ABI v1.11 evidence replaced an earlier v1.8 run whose client treated
nearly every negative snapshot result as if the target merely lacked a saved
VFP context. Two diagnostic builds then identified the exact retail 3.65
non-VFP result and the syscall-boundary encoding rule. ABI v1.11 keeps the
strict policy, validates the canaries on error paths, and passes the complete
live-GDB lifecycle gate. Mixed artifacts deliberately fail ABI negotiation
rather than weakening this rule.

## External reference policy

The pinned `cerwym/kvdb` commit is retained only as a research pointer to the
same undocumented API. Its repository root did not provide an explicit license
during review, so no source or structure definition was copied from it.

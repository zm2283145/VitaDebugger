# Hardware-debug and foreign-VFP research review

Date: 2026-09-14

This note reviews the externally supplied hardware-breakpoint and foreign-thread
register/VFP research against the current VitaDebugger evidence, the installed
VitaSDK headers, ARM's Cortex-A9 errata, and upstream Linux's ARM hardware-
breakpoint implementation. It records hypotheses and safe next gates; it does
not authorize a kernel or CP14 hardware experiment.

> **Later PMU result:** retail 3.65 discovery rejected both user-mode
> `ScePerf` loading paths and proved that the unresolved import stubs branch to
> zero. Direct ScePerf initialization now fails closed. A separately reviewed,
> read-only kernel PMU inventory subsequently passed with six Cortex-A9 event
> counters and unchanged control state. See
> [the hardware record](profiler-pmu-retail-3.65.md).
>
> **Later foreign-mutation result:** the first ThreadMgr resolver title was
> retired after a retail 3.65 power-off inside its all-at-once metadata/code
> collector. It reached only the durable `kernel entered` checkpoint and
> invoked no getter or setter. The replacement v2 prerequisite title is
> compile-tested but unrun; it checkpoints fixed export lookups separately,
> never invokes or dereferences resolved pointers, and captures no code bytes.
> See
> [`kernel/thread-setter-resolver-probe/README.md`](../../kernel/thread-setter-resolver-probe/README.md).

## Findings retained

- Debug-power, monitor-mode, authorization, and Vita-specific exception policy
  remain plausible explanations for the boundary where `DBGDIDR` and
  `DBGDSCRint` returned but `DBGVCR` rebooted the tested retail 3.65 Vita. The
  Cortex-A9 TRM marks `DBGOSLAR`, `DBGOSLSR`, and `DBGOSSRR` RAZ/WI and the OS
  Lock as not implemented on this core, so an OS Lock is not a sound primary
  explanation for this particular CPU.
- VitaSDK's NID database contains `sceKernelGetThreadContextForVM`
  (`0x22C9595E`) and `sceKernelSetThreadContextForVM` (`0x27E6DEDE`). This is the
  best current lead for a firmware-exported foreign-thread general-register
  mutation backend, but its signature and contract are not documented.
- VitaSDK declares user-mode performance-monitor functions in
  `psp2/perf.h`: reset, event selection, start, stop, counter read/write, and
  software increment. Most operations name a thread ID; software increment
  takes only a counter mask. Later retail discovery found these imports
  unavailable in the tested runtime, so declaration and link success must not
  be treated as runtime support. The practical path is now a narrow, leased
  kernel provider built on the completed read-only CP15 gate.
- Mismatch breakpoints would be useful for stepping instructions that change
  `PC`, if hardware debug eventually becomes available. They do not make the
  current hardware path safe or usable.

## Corrections and safety boundaries

### `DBGPRSR` and `DBGOSLSR` are not safe unguarded probes

ARM Cortex-A9 erratum 764319 affects every revision and states that CP14 reads of
both registers can raise Undefined Instruction when the external
`DBGSWENABLE` input is low, even from privileged mode. Linux installs an
Undefined Instruction hook before probing the OS Lock on affected CPUs.
Consequently, neither read may be added to the existing Vita ladder until a
Vita-specific, unload-safe exception recovery path has independently passed. A
caught `DBGOSLSR` fault would be strong evidence consistent with a low
`DBGSWENABLE` input in the tested platform state. It must not be described as
proof that no other Vita-specific trap or policy could produce the exception.
Conversely, a returning read would show that erratum 764319 did not block that
instruction; it would not by itself authorize `MDBGEN`, comparator access, or
any other debug-register write.

References:

- [ARM Cortex-A9 errata notice, erratum 764319](https://documentation-service.arm.com/static/608117c55e70d934bc69f136)
- [ARM Cortex-A9 Technical Reference Manual](https://documentation-service.arm.com/static/5f0377b2cafe527e86f5c247)
- [Upstream Linux ARM hardware-breakpoint implementation](https://github.com/torvalds/linux/blob/master/arch/arm/kernel/hw_breakpoint.c)

Linux's workaround also supports one useful implementation detail from the
addendum: a future recovery hook should match the exact 32-bit ARM `MRC`
encoding. On Vita that must be an additional condition, not the only one: also
require the expected PC/range, processor mode, armed one-shot token, CPU, and
thread. It does not remove the need to prove the Vita handler ABI, chaining
behavior, PC bias, lifetime, and release path first.

### `DBGDRAR` is a lead, not a free extra read

The Cortex-A9 TRM identifies `DBGDRAR` as a baseline CP14 read-only view of the
optional CoreSight ROM-table address and separately documents an optional
memory-mapped Debug APB interface. `DBGDRAR` itself has no APB offset, a returned
address would not prove that Vita maps or authorizes CPU access to that APB, and
the memory-mapped path must not be assumed to bypass `DBGSWENABLE`. It remains a
legitimate static-analysis lead, not proof of a fallback. A future `DBGDRAR`
read is its own guarded, high-risk hardware gate after recovery has passed; it
must not be silently combined with the first `DBGOSLSR` experiment.

### The proposed SceExcpmgr catcher is not ready to run

The installed VitaSDK declaration differs materially from the sketch:

- the handler type returns `void` and receives an exception context plus a
  handling-code argument;
- registration expects eight writable bytes immediately before the handler
  code;
- the public exception-context layout is explicitly labelled for firmware
  3.60, not validated for 3.65; and
- the installed header and NID database expose registration but no matching
  release function. External reverse-engineering notes report a possible 3.65
  release export at NID `0xDA7BB671`, but that NID, its signature, and its
  unlink/lifetime semantics have not been verified against the target module.

A wrong context layout, ARM/Thumb entry address, PC adjustment, chain action, or
module lifetime can panic the kernel or leave a dangling callback after unload.
The absence of a VitaSDK stub does not prove that no firmware removal mechanism
exists; it means the reported one cannot yet be treated as a supported contract.
Before any CP14 recovery test, static 3.65 analysis must establish the exact
handler lifecycle and removal path. Until removal is proven, any test handler
must stay resident rather than returning non-resident. A deliberately
undefined, non-CP14 instruction in a disposable user process must then prove
forwarding, recovery, shutdown, and reboot behavior while the handler remains
resident and fail-closed.

Reference: [VitaSDK SceExcpmgr kernel header](https://github.com/vitasdk/vita-headers/blob/master/include/psp2kern/kernel/excpmgr.h)

### Do not mutate undocumented thread-object fields

Scanning an internal thread object for bytes that resemble a register snapshot
does not prove that the match is the scheduler's authoritative saved context.
Duplicate banks, lazy VFP ownership, a running or migrated thread, and firmware-
specific layouts can make a write corrupt unrelated kernel state. Direct writes
through inferred `pStatus`, `pNeonInfo`, or similar offsets are outside the
project's hardware-test boundary.

Read-only pattern comparison can remain an offline layout-mapping aid when it is
combined with supported object lifetime handling and static analysis. It must
not be promoted into a live write technique.

### Cooperative VFP mutation is a separate, safer design lead

The addendum's strongest VFP proposal is not a foreign kernel write: stage a
requested D-register/FPSCR update and let a cooperatively parked target thread
apply it to itself in a verified resume trampoline. This avoids treating a lazy
kernel VFP save area as authoritative and is worth prototyping independently of
the foreign-thread kernel provider.

It is not implemented or hardware-validated by this review. `MVFR0` establishes
CPU feature availability only; it does not by itself prove that a particular
thread is allowed and prepared to execute VFP/NEON instructions. A design must
also validate the thread's Vita attributes and live VFP access state, preserve
all trampoline scratch state, make the staged update single-use and identity-
bound, read back the result, and restore the prior value on failure. It covers
cooperatively stopped in-process threads, not arbitrary foreign threads blocked
inside the kernel.

The inferred `SceUIDProcessObject + 0x0C` breakpoint context and fields beyond
VitaSDK's declared 0x40-byte `SceExcpmgrData` are reverse-engineered leads, not
3.65 contracts. Even read-only use requires exact static layout validation and
supported object reference/release handling first.

## Gated next work

1. Build a separately versioned kernel PMU session on top of the completed
   read-only inventory. It must snapshot every field it changes, bind operations
   to one CPU, verify writes, and restore the exact prior state across normal
   release, failure, timeout, disconnect, and process exit before counters are
   advertised to VitaProfiler.
2. Determine the exact 3.65 `SetThreadContextForVM` prototype, context size,
   attribute requirement, process boundary, suspension rule, and VFP coverage
   through lawful static analysis before importing or calling the NID.
3. If that ABI is established, use a disposable same-process worker for the
   first live A/B: control build versus VM-attribute build, supported suspend,
   change only one controlled callee-saved GPR (`r4` or `r5`), read back, restore
   exactly, resume, detach, and reconnect. Do not touch `PC`, `SP`, `CPSR`, VFP,
   another process, or a kernel object in this first gate.
4. Prototype cooperative in-process VFP writes through a target-thread resume
   trampoline, with a disposable application and no kernel-object mutation.
5. Separately establish a complete 3.65 SceExcpmgr lifecycle. Only after the
   non-CP14 recovery test passes may one protected `DBGOSLSR` read be proposed.
   `MDBGEN`, comparator access, and enabled break/watchpoints remain later,
   separately reviewed gates.

Until these gates pass, the kernel mutation provider must continue advertising
zero writable foreign register banks, and GDB must continue rejecting hardware
`Z1`-`Z4` packets.

The host-side transaction scaffold also treats object lifetime as a promotion
gate rather than trusting a freshly resolved integer UID. Any future writable
backend must retain the exact process/thread objects across snapshot, stage,
verification, rollback, and cleanup; classify that retained target as suspended,
destroyed, or unknown; and keep an unknown target stopped. It must pass target
exit, process exit, UID churn/reuse, forced inventory failure, watchdog retry,
and unload tests before exposing a writable bank. Core and VFP are deliberately
separate transactions because their snapshots contain overlapping FPSCR views.

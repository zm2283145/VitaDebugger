# Foreign-thread mutation provider

Status: **host-only and fail-closed. No foreign-thread register setter has been
verified safe on retail 3.65.** The production kernel companion continues to
advertise zero writable foreign core/VFP banks.

This layer connects the existing snapshot/stage/read-back/restore transaction
to two capabilities that a future firmware-specific adapter must prove:

1. an immutable, authenticated setter/getter binding; and
2. retained references to the exact process and thread objects for the entire
   transaction.

It does not guess a firmware prototype, cast a provisional NID, inspect or
write inferred thread-object fields, or make either writable bank available in
`kernel/src/main.c`.

## Current evidence boundary

VitaSDK declares the read-only
`ksceKernelGetThreadCpuRegisters(SceUID, SceThreadCpuRegisters *)` and
`ksceKernelGetVfpRegisterForDebugger(SceUID, void *)` functions. It does not
declare a corresponding general foreign-thread core or VFP setter.

The provisional `0x64E89DE9` core-set and `0x49A0B679` VFP-set NIDs are absent
from the installed VitaSDK database, have no established retail 3.65 prototype,
and have never been invoked by this project. The paused resolver's newest valid
hardware journal reached only `kernel entered`; the Vita powered off before its
read-only metadata collector completed. That result proves neither candidate
exists nor that either candidate is callable.

`sceKernelSetThreadContextForVM` (`0x27E6DEDE`) and its internal
`_sceKernelSetThreadContextForVM` (`0xD4785C41`) remain static-analysis leads.
Their exact structures, thread-attribute restrictions, suspension contract,
process boundary, return behavior, and VFP coverage are not established for
retail 3.65. An emulator declaration or unimplemented emulator stub is not
hardware verification.

The pinned KVDB reference likewise leaves write-register support unavailable
because the public SDK has no supported setter. KuBridge provides fault-context
access for the current exception thread, not an arbitrary retained
foreign-thread setter.

## Provider contract

`kernel/src/thread_mutation_provider.c` is deliberately independent of Vita
imports. A firmware adapter has to supply callbacks which:

- resolve a fixed allowlist of functions and authenticate the complete module
  and code fingerprint;
- keep that exact binding pinned or otherwise stable and report replacement,
  unload, or uncertainty;
- make each process/thread retain operation atomic, unwind a retained process if
  the following thread retain fails, and return opaque object plus generation
  identities;
- classify the retained pair as alive and debug-suspended, definitely
  destroyed, or unknown without re-resolving the integer UID;
- acquire adapter-owned serialization that atomically revalidates and pins the
  binding, retained objects, parent relation, and suspended state across each
  snapshot or setter call;
- snapshot and set only through the authenticated binding and exact retained
  thread reference; and
- release the thread reference before the process reference.

The generic provider rejects missing addresses, zero fingerprints, unknown
bank bits, missing callbacks, malformed references, a process/thread object
alias, stale ownership, uncertain target state, and uncertain binding state.
It disables all writable banks after binding/target uncertainty or a release
failure. Provider storage is zero-initialized once; reinitialization cannot
erase an active or quarantined lease.

Release results have explicit exactly-once semantics. Zero means released; the
positive `VD_THREAD_REFERENCE_RELEASE_RETAINED` result means the adapter proves
the reference is still held and a later retry is safe. A negative result has an
unknown effect: the provider quarantines it and `vdThreadMutationProviderDrain()`
will not blindly retry. A future Vita adapter must establish which result, if
any, can safely represent each `ksceGUIDReleaseObject` failure. No new
transaction is admitted and the adapter must not unload while either quarantine
remains. An impossible release-identity mismatch is treated as a fatal invariant
violation rather than guessing which transaction owns the references.

The provider deliberately supplies both the original GUID and the retained
opaque reference. The GUID is for audit and ownership matching; an adapter must
never use it to look up a fresh object for a later snapshot, write, or rollback.
The interface therefore lets a correct adapter resist UID reuse, but cannot
make an incorrect callback honor the retained object. Hardware promotion must
verify that every concrete callback uses the opaque reference.

## Retained-object lead, not a completed adapter

VitaSDK exposes `ksceGUIDReferObject`,
`ksceGUIDReferObjectWithClass`, `ksceGUIDReleaseObject`, and
`ksceKernelGetUIDProcessClass`. A future adapter could plausibly class-retain a
process. For a thread, the available `ksceKernelGetThreadmgrUIDClass()` returns
an enum rather than a documented thread `SceClass *`, leaving only a type check
followed by generic GUID retain. The headers do not establish that this
two-operation sequence is race-free across close/reuse, and release is by GUID
rather than by object pointer. Therefore these declarations are sufficient for
the host-side model, but not sufficient to enable a Vita backend.

## Host validation

Run the transaction and provider suites from the repository root:

```powershell
C:\msys64\usr\bin\make.exe `
  host-test-kernel-thread-mutation `
  host-test-kernel-thread-mutation-provider `
  HOST_CC=C:/msys64/mingw64/bin/gcc.exe
```

The provider suite covers:

- authenticated binding acceptance and malformed/unverified fail-closed cases;
- exact retained process/thread references for core and D32 VFP operations;
- preservation of the exact retained reference after the integer UID mapping
  changes (with the concrete adapter still responsible for using it);
- partial process/thread acquisition unwind;
- provisional-write read-back failure and exact rollback;
- transient binding and target-state uncertainty without unsafe cleanup;
- independently proven thread exit and process exit;
- thread-before-process release order; and
- definitely-retained release quarantine plus explicit cleanup retry;
- ambiguous fail-after-effect release quarantine without a blind retry; and
- one-shot initialization that cannot erase active or pending cleanup state.

These are fake-platform tests. They validate orchestration and invariants, not
any undocumented Vita function or kernel object-lifetime contract.

## Remaining retail 3.65 promotion gate

Before changing the zero-bank production backend:

1. Replace the failed all-at-once resolver with separately reviewed, durable,
   non-dereferencing rungs and obtain repeatable module/address fingerprints.
2. Establish the exact setter prototype, structure size/alignment, writable
   fields, preconditions, and return semantics through lawful static analysis.
3. Prove the exact process/thread retain and release lifecycle in a disposable
   read-only gate, including exit, close/reuse, parent relation, release order,
   timeout, and plugin-unload behavior.
4. First mutate one controlled callee-saved GPR in a disposable, same-process,
   debug-suspended worker. Require read-back, exact restoration, resume,
   detach, reconnect, timeout, thread exit, process exit, UID reuse, forced
   inventory failure, and watchdog cleanup before advertising core writes.
5. Gate VFP separately. Lazy VFP ownership makes a cooperative in-thread
   trampoline the safer first experiment; a core setter must not be assumed to
   cover D0-D31 or FPSCR.

Any uncertainty keeps the target stopped and the writable capability bits
clear. A resolved address alone is never a promotion result.

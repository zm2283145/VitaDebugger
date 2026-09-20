# Foreign-thread mutation provider

Status: **host-modeled and fail-closed. No foreign-thread register setter is
verified safe on retail 3.65. Production advertises zero writable foreign core
and VFP/NEON banks.**

The transaction/provider code models the safety contract a future
firmware-specific adapter must satisfy. It does not cast a provisional NID,
infer a function signature, write an internal thread-object field, or bind the
host fake to the production kernel companion. A disposable one-GPR contract is
compiled out by default and is not linked into the production kernel target.

## Evidence ledger

Evidence was rechecked against the installed `C:\vitasdk` tree and repository
history. The table separates declarations from unresolved hypotheses.

| Item | Strongest evidence | Established | Unresolved |
| --- | --- | --- | --- |
| core getter | `arm-vita-eabi/include/psp2kern/kernel/threadmgr/debugger.h`; `share/vita-headers/db/360/SceKernelThreadMgr.yml` | `int ksceKernelGetThreadCpuRegisters(SceUID, SceThreadCpuRegisters *)`; NID `0x5022689D`; `SceThreadCpuRegisters` size 0x90; two 0x48 entries; header says target must be suspended; zero success / negative error | Header warns the two entries may be current/exception rather than user/kernel; it does not define a setter contract |
| VFP getter | same header/database | `int ksceKernelGetVfpRegisterForDebugger(SceUID, void *)`; NID `0x5CDE387A` | Public pointer is untyped; getter does not establish a write ABI or lazy-VFP ownership contract |
| general core setter | installed headers/database search | No `ksceKernelSetThreadCpuRegisters` declaration or public kernel NID was found | Prototype, NID, context size/alignment, writable fields, attributes, process boundary, suspension contract, return semantics, firmware identity, and code fingerprint |
| provisional core/VFP NIDs | repository history only | `0x64E89DE9` and `0x49A0B679` are fixed hypotheses in the disposable probe | Both are absent from the installed VitaSDK database; no signature or retail 3.65 implementation evidence exists |
| VM-context setters | `share/vita-headers/db/360/SceLibKernel.yml` and `SceKernelThreadMgr.yml` | Names/NIDs `sceKernelSetThreadContextForVM` / `0x27E6DEDE` and `_sceKernelSetThreadContextForVM` / `0xD4785C41` exist in the 3.60 catalog | No installed header defines either prototype or context structure. VM-thread specialization, size/alignment, writable fields, preconditions, return semantics, kernel-call suitability, 3.65 identity, and VFP coverage are unknown |
| object retain/release | `psp2kern/kernel/sysmem/uid_guid.h` | `ksceGUIDReferObject`, `ksceGUIDReferObjectWithClass`, and `ksceGUIDReleaseObject`; refer increments and release decrements an internal reference count | Release accepts a numeric GUID, not the retained pointer; failure effects and behavior after close/reuse are undocumented |
| PUID/GUID conversion | `psp2kern/kernel/sysmem/uid_puid.h` | `kscePUIDOpenByGUID`, `kscePUIDClose`, and `kscePUIDtoGUID` declarations | No generation/non-reuse guarantee makes a later integer UID lookup equivalent to the originally retained object |
| process/thread class | `psp2kern/kernel/processmgr.h`, `threadmgr/misc.h` | Process class returns `SceClass *`; thread UID class query returns `SceKernelIdListType` | No documented thread `SceClass *` is available for an atomic class-qualified retain |
| retail fingerprints | preserved v1 failure record described in the prerequisite runbook | v1 reached `kernel entered`; source review proves no candidate getter/setter was invoked | No valid `SceKernelThreadMgr` module NID, segment map, candidate address, code bytes, or fingerprint was collected |

The VitaSDK database directory name `360` identifies catalog provenance, not
proof that the same private contract exists on retail 3.65. Emulator names,
unimplemented stubs, Dev Wiki internal labels, and SGI-style
`setCpuRegisterForDebugger` leads do not supply a verified public
signature/NID. They are not promotion evidence.

The installed headers, NID YAML catalogs, and static stub archives were also
searched for `ksceKernelSetThreadCpuRegisters`,
`sceKernelSetThreadCpuRegisters`, and `setCpuRegisterForDebugger`. The only
setter-related symbols found were the two VM-context names above. Repository
history contains the provisional NIDs and unresolved hypotheses, but no
prototype, implementation fingerprint, hardware sample, or ABI proof. No local
firmware/module image or resolver result exists from which more evidence can be
lawfully established.

The exact setter prototype/ABI requested by the roadmap is therefore
**unresolved**. No native setter address, signature, size, alignment, writable
field, or return contract is populated by this change.

## Provider setter contract v2

Binding ABI v2 makes the evidence a future adapter must provide explicit rather
than allowing callback presence to imply writability. A core binding now
contains a 48-byte `vd_thread_core_setter_contract` with:

- its exact structure size and version;
- the proven native setter context size and power-of-two alignment;
- an explicit writable GPR mask, limited by the provider to R0-R12;
- the complete required precondition set: authenticated/pinned binding,
  retained process, retained thread, verified parent relation,
  debug-suspended target, and serialized access;
- exact full-snapshot replacement semantics; and
- zero-success/negative-error return semantics.

Any missing, extra, or unknown contract value rejects the entire core
capability. The transaction copies no implicit writable default: a zero mask or
unknown mask bit removes core support, and staging a GPR outside the
authenticated mask returns unsupported before a setter call.

VFP/NEON uses a separate 48-byte `vd_thread_vfp_setter_contract`, separate
getter/setter addresses, an independently proven native size/alignment, and an
explicit D32 layout/version. A core contract never enables VFP. Contract fields
in the host fake, including its `0x90` core context size and 8-byte alignment,
are test data only and are **not** Vita ABI evidence. The binding itself is an
in-process structure whose `struct_size` is checked against the target build;
it is not a portable serialized ABI.

## Provider lifetime contract

`kernel/src/thread_mutation_provider.c` requires an adapter to:

- authenticate a fixed allowlist and complete module/code fingerprint;
- keep the binding pinned and report replacement, unload, or uncertainty;
- atomically retain the exact process and thread objects and return opaque
  object plus nonzero generation identities;
- unwind a retained process if the following thread retain fails;
- classify the exact retained pair as alive/debug-suspended, definitely
  destroyed, or unknown without re-resolving the integer UID;
- atomically revalidate and pin binding, objects, parent relation, and suspend
  state around every snapshot/setter call; and
- release thread before process with explicit exactly-once semantics.

The provider rejects malformed identities, object aliasing, missing callbacks,
unknown bank bits, zero fingerprints, stale ownership, uncertain parent/target
state, and uncertain binding state. The original GUID is audit metadata only;
an adapter must perform all access through the retained opaque object.

Retention failures also obey a strict output contract: a negative retain result
must leave its output entirely zero and hold no reference. If a callback returns
failure with a nonzero reference, the provider cannot know whether the retain
took effect. It now enters a permanent acquisition quarantine, does not guess
whether to release, disables writable banks, rejects drain, and blocks unload.

Release results remain explicit:

- `VD_THREAD_REFERENCE_RELEASED`: definite release;
- `VD_THREAD_REFERENCE_RELEASE_RETAINED`: reference is definitely still held
  and a later retry is safe; or
- negative: effect unknown, so no blind retry is permitted.

A retryable release keeps both admission and unload blocked until
`vdThreadMutationProviderDrain()` succeeds. An ambiguous release is
non-drainable. Reinitialization cannot erase an active lease or quarantine.

`vdThreadMutationProviderPrepareUnload()` is terminal and must be called under
the adapter's owner serialization. It blocks new transactions immediately. If
a transaction is active, the adapter and watchdog must remain loaded while the
existing exact-target cleanup runs. Pending retained cleanup or any ambiguous
acquisition/release blocks unload; successful shutdown never re-enables a bank.

## Lifecycle outcomes

| Event | Required classification/action |
| --- | --- |
| thread/process exit proven on retained object | Retire obsolete restore without writing, then release thread before process |
| close, integer UID reuse, or UID churn | Never redirect through the new UID mapping; use retained object only |
| parent relation changes | Unknown, not destroyed; keep stopped and retry |
| inventory or suspend-state query fails | Unknown; preserve restore obligation and disable admission |
| disconnect/timeout | Watchdog performs exact-target cleanup; no resume before restore or proven destruction |
| release definitely retained | Quarantine and bounded explicit retry |
| release effect unknown | Permanent quarantine; no retry or unload |
| plugin unload | Terminal admission stop; fail unload while lease/reference/quarantine remains |

These rules are modeled with host fakes. They do not prove that Vita's public
GUID APIs can implement the required atomic contract. In particular, release by
numeric GUID remains the close/reuse blocker for a production adapter.

## Non-mutating prerequisite gate

The retired v1 resolver powered off during its all-at-once metadata/code
collector. The replacement v2 title is new title `VDCP00009`, version `02.00`,
with new journal paths. It durably checkpoints firmware metadata, module
identity, and each of four fixed NID lookups separately.

v2 never invokes or dereferences a resolved pointer and captures no code bytes.
Its addresses are explicitly unauthenticated presence observations. A v2
`PASS` therefore cannot populate `vd_thread_setter_binding`, cannot establish a
fingerprint, and cannot enable a writable bank. See
[`kernel/thread-setter-resolver-probe/README.md`](../kernel/thread-setter-resolver-probe/README.md)
for its exact ABI and hardware runbook.

## Disposable one-GPR contract

`kernel/src/thread_gpr_gate.c` is the next host-testable layer. It is disabled
unless `VD_THREAD_GPR_GATE_COMPILED=1`, accepts only R4 or R5, and requires an
authenticated ABI-v2 core contract that explicitly includes the selected
register. It has no commit operation: after one apply, the only successful
terminal path is exact restoration or proof that the retained target was
destroyed.

The gate retains and snapshots first, writes a full context with exactly one
selected-user-bank GPR changed, reads back the complete context, and rolls back
to the exact snapshot on setter or verification failure. Failed rollback keeps
the lease and restore obligation active for watchdog cleanup. Parent/inventory
uncertainty keeps the target stopped; proven thread/process exit retires an
obsolete write; release retry and quarantine retain the provider rules above.
Release uncertainty is returned to the gate caller even after register
restoration succeeds; it is never reported as fully clean completion.
The forced-enabled tests use host callbacks only. There is no production
provider, no native setter call, no resume integration, no Vita package for
this gate, and no hardware validation.

## Host validation

```powershell
C:\msys64\usr\bin\make.exe `
  host-test-kernel-thread-mutation `
  host-test-kernel-thread-mutation-provider `
  host-test-kernel-thread-gpr-gate-enabled `
  host-test-thread-setter-resolver-record `
  HOST_CC=C:/msys64/mingw64/bin/gcc.exe
```

The suites cover snapshot/stage/read-back/exact rollback, core/VFP separation,
forced-disabled and forced-enabled R4/R5 admission,
partial retain unwind, retain-contract violations, UID mapping churn,
process/thread exit, parent changes, forced inventory uncertainty, timeout and
disconnect cleanup, binding uncertainty, ordered release, retryable and
ambiguous release, watchdog retry/drain, terminal unload, and checkpoint record
validation. They remain fake-platform tests.

## Remaining retail 3.65 promotion gate

1. Run and preserve repeatable v2 presence samples after independent review.
2. Through lawful static analysis, establish a real setter implementation,
   exact prototype, structure size/alignment, writable fields, preconditions,
   return semantics, and firmware/module/code fingerprints. Do not infer these
   from a resolved address.
3. Prove an atomic retained-object lifecycle on disposable hardware across
   exit, close/reuse, UID reuse, parent changes, timeout, disconnect, release
   uncertainty, watchdog retry, and plugin unload.
4. Only after steps 2-3 and independent review, bind the existing default-off
   R4/R5 restorable contract to a separate disposable hardware title. Require
   read-back, exact restoration, resume, detach/reconnect, timeout, exit, UID
   churn, forced inventory failure, watchdog recovery, and complete removal
   after the experiment.
5. Gate VFP/NEON independently. A core setter cannot be assumed to cover
   D0-D31 or FPSCR; lazy VFP ownership keeps a cooperative in-thread trampoline
   the safer lead.

Any uncertainty keeps the target stopped and all production writable
capability bits clear.

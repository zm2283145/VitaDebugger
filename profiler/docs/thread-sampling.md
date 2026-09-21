# Bounded thread-sampling provider

VitaProfiler now has an allocation-free provider boundary for program-counter
and call-stack samples. It is policy and storage infrastructure, not a claim
that arbitrary-thread sampling is ready on Vita hardware.

## Implemented contract

`vitaprofiler_sampling.h` provides:

- explicit capability bits for current-thread PC, current-thread stack,
  foreign-thread PC, foreign-thread stack, bounded stack reads, stable foreign
  identity, exit-aware generation validation, fault-contained reads, bounded
  callbacks/watchdog response, retryable release rollback, and confident
  foreign register-context selection;
- a caller-owned sampler and caller-owned frame array with a hard maximum of
  64 frames;
- separate current-thread and foreign-thread entry points, with no fallback
  from a foreign request to the calling thread;
- an exact nonzero identity requirement for every foreign target;
- a per-capture confidence marker before a foreign provider may publish one
  selected register context;
- begin/next/end provider leases, including quarantine and retry when release
  fails;
- an ABI-v2 `validate_sample` callback after acquisition and before every
  foreign unwind step, with explicit stale-generation, thread-exit, timeout,
  and protected-read-fault outcomes;
- deterministic partial results when a bounded provider read or unwind fails
  after the initial PC;
- zeroed output on acquisition failure and monotonic stack-pointer validation
  for each returned parent frame.

The core never allocates, dereferences a target address, chooses an ARM register
bank, starts a worker, or suspends/resumes a thread. A provider performs the
architecture- and platform-specific capture while retaining every identity and
memory lifetime needed until `end_sample` succeeds.

Providers must advertise only capabilities they can prove. A current-thread
provider runs synchronously on the caller and may use an application-owned
cooperative unwinder. A foreign provider must pin or otherwise retain the exact
target represented by the supplied identity; a numeric thread ID alone is not
enough. Its identity must include a logical lifetime epoch: thread exit makes
the identity stale even if the platform can retain a dormant/restartable object.

ABI-v1 providers remain accepted for cooperative current-thread sampling only.
Their original provider/config/sampler/status layouts and entry points are
unchanged. The stronger contract uses the explicitly versioned
`vp_sample_provider_v2`, `vp_sampler_config_v2`, `vp_sampler_v2`, and
`vp_sampler_status_v2` structures with the corresponding `*_v2()` entry
points; v2 extensions are never read from or written through v1 storage.
`VP_SAMPLE_CAP_ALL` retains the original v1 mask, while
`VP_SAMPLE_CAP_ALL_V2` includes the new proof capabilities. A v1 provider may
continue advertising its legacy foreign bits when a caller requests only
current-thread sampling; requesting foreign sampling through v1 remains
disabled.
A foreign provider must use ABI v2 and advertise
`STABLE_IDENTITY`, `EXIT_AWARE_IDENTITY`,
`FOREIGN_CONTEXT_CONFIDENCE`, `BOUNDED_CALLBACKS`, and
`RELEASE_ROLLBACK`; foreign stacks additionally require
`BOUNDED_STACK_READ` and `FAULT_CONTAINED_READ`. It supplies a nonzero
worst-case callback bound no larger than the caller's
`callback_timeout_us`. The core invokes `validate_sample` before exposing the
first frame and before every unwind read. This does not make an untrusted
callback preemptible: the capability is an audited provider promise backed by
its own watchdog. A provider returning `VP_ERROR_TIMEOUT`,
`VP_ERROR_THREAD_EXITED`, `VP_ERROR_STALE_IDENTITY`, or
`VP_ERROR_READ_FAULT` yields an explicit bounded stop reason. Release is still
mandatory; failure quarantines the sampler until retry succeeds.

```c
#include <vitaprofiler_sampling.h>

static struct vp_sample_frame frame_storage[16];
static struct vp_sampler sampler;

int start_sampling(const struct vp_sample_provider* app_provider)
{
    const struct vp_sampler_config config = {
        .frames = frame_storage,
        .frame_capacity = 16,
        .max_depth = 16,
        .required_capabilities =
            VP_SAMPLE_CAP_CURRENT_THREAD_PC |
            VP_SAMPLE_CAP_CURRENT_THREAD_STACK |
            VP_SAMPLE_CAP_BOUNDED_STACK_READ,
    };
    return vp_sampler_init(&sampler, app_provider, &config);
}

int sample_this_thread(struct vp_sample* sample)
{
    return vp_sampler_sample_current(&sampler, sample);
}
```

The application owns `app_provider`, its callback state, `sampler`, and
`frame_storage`. They must remain alive and writable until
`vp_sampler_deinit()` succeeds. The provider function table must not change
while active; callback state may be updated by the provider. Callers must not
mutate or inspect sampler-owned state concurrently with a sampling call.
Sampling calls on one sampler are serialized.
`VP_RESULT_PARTIAL` means the returned prefix is valid and
`sample->stop_reason`/`provider_error` explain why it ended. If an end callback
fails, no new sample or deinitialization is allowed until
`vp_sampler_retry_release()` succeeds.

`sample->frames` borrows `frame_storage`; it is not an independent snapshot.
Every later sampling attempt clears that array before acquisition, including an
attempt that then fails. Copy the reported `frame_count` frames before the next
call when a sample must be retained.

## Why no built-in foreign Vita provider ships yet

The existing kernel companion is read-only at the relevant boundary and is the
only acceptable foundation for a future adapter:

- `vdKernelGetStatus()` exposes explicit thread-list, thread-control, register,
  and stop-reconciliation capabilities.
- VitaSDK declares `ksceKernelGetThreadIdList(pid, ...)` and
  `ksceKernelGetThreadCpuRegisters(thid, SceThreadCpuRegisters*)`; the latter
  explicitly requires a suspended target. `SceThreadCpuRegisters` is two
  0x48-byte `SceArmCpuRegisters` entries (0x90 bytes total).
- `vdKernelBeginStop()` creates an owner-bound 250-5000 ms all-stop lease with
  watchdog recovery; `vdKernelGetThreadRegisters()` preserves both raw banks
  and reads only a process-owned thread suspended by that exact lease.
- Current VitaSDK headers warn that the historical user/kernel names for those
  banks are unreliable. The debugger's host-tested user-mode selector is a
  policy, not proof that one bank may be silently discarded. A future provider
  must preserve both entries until it can mark the selected context confident,
  or fail the sample closed.
- `SceKernelThreadInfo` is 0x80 bytes and exposes candidate `stack` and
  `stackSize` values, but the header does not guarantee that `stack` is the low
  allocation address. Those fields cannot become read bounds by assumption.
- PID-aware module APIs expose segment ranges and ARM EHABI `exidx`/`extab`
  metadata. They are a basis for a bounded unwinder only after module lifetime,
  table bounds, and every target-memory read are validated.
- `ksceGUIDReferObject`, `ksceGUIDReferObjectWithClass`, and
  `ksceGUIDReleaseObject` can retain an object, but VitaSDK documents no UID
  generation/non-reuse guarantee. Retention alone is not a stable sampling
  identity, and logical thread exit must terminate its epoch.

Those facts are not yet a complete profiler provider. The public read-only
thread ABI does not expose a retained generation-stable identity to user mode,
and it does not provide a protected, bounded target-memory reader or a
hardware-validated unwind layout. It also has not proven a callback deadline
and watchdog path that survives target exit/UID reuse while guaranteeing
retryable stop-token release. Periodically beginning all-stop sessions
inside a profiler would also need a dedicated latency, watchdog, thread-exit,
and process-exit hardware gate. This increment therefore does **not** call
`vdKernelBeginStop()`, guess frame records, dereference candidate stack
addresses, or advertise foreign-thread capability.

A future Vita adapter must reuse the existing stop token, process-owned thread
inventory, both raw register banks, and watchdog cleanup. It must not invent
suspend-status constants, resume suspension owned by another subsystem, or
call the Dev Wiki-reported `ksceKernelBacktrace` while that symbol remains
absent from current VitaSDK headers. Before it can enable
foreign PC sampling it must add and hardware-gate an exact target identity
contract. Before it can enable foreign stack sampling it must additionally
provide a fault-contained bounded memory reader and validated ARM/Thumb unwind
rules, likely using validated EHABI metadata where available. Until then,
initialization without an audited provider returns `VP_ERROR_UNSUPPORTED`.

The primary declarations for these constraints are in VitaSDK's
`psp2kern/kernel/threadmgr/thread.h`,
`psp2kern/kernel/threadmgr/debugger.h`,
`psp2kern/kernel/sysmem/uid_guid.h`, and
`psp2kern/kernel/modulemgr.h`.

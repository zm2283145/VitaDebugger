# Bounded thread-sampling provider

VitaProfiler now has an allocation-free provider boundary for program-counter
and call-stack samples. It is policy and storage infrastructure, not a claim
that arbitrary-thread sampling is ready on Vita hardware.

## Implemented contract

`vitaprofiler_sampling.h` provides:

- explicit capability bits for current-thread PC, current-thread stack,
  foreign-thread PC, foreign-thread stack, and stable foreign identity;
- a caller-owned sampler and caller-owned frame array with a hard maximum of
  64 frames;
- separate current-thread and foreign-thread entry points, with no fallback
  from a foreign request to the calling thread;
- an exact nonzero identity requirement for every foreign target;
- begin/next/end provider leases, including quarantine and retry when release
  fails;
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
enough.

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
            VP_SAMPLE_CAP_CURRENT_THREAD_STACK,
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
- `vdKernelBeginStop()` creates an owner-bound 250-5000 ms all-stop lease with
  watchdog recovery; `vdKernelGetThreadRegisters()` reads only a process-owned
  thread suspended by that exact lease.
- The debugger has a host-tested selector for the two raw, state-dependent ARM
  register banks and rejects banks without a valid user-mode PC and SP.

Those facts are not yet a complete profiler provider. The public read-only
thread ABI does not expose a retained generation-stable identity to user mode,
and it does not provide a protected, bounded target-memory reader or a
hardware-validated unwind layout. Periodically beginning all-stop sessions
inside a profiler would also need a dedicated latency, watchdog, thread-exit,
and process-exit hardware gate. This increment therefore does **not** call
`vdKernelBeginStop()`, guess frame records, dereference candidate stack
addresses, or advertise foreign-thread capability.

A future Vita adapter must reuse the existing stop token, process-owned thread
inventory, raw-bank selector, and watchdog cleanup. Before it can enable
foreign PC sampling it must add and hardware-gate an exact target identity
contract. Before it can enable foreign stack sampling it must additionally
provide a fault-contained bounded memory reader and validated ARM/Thumb unwind
rules (or trustworthy unwind metadata). Until then, initialization without an
audited provider returns `VP_ERROR_UNSUPPORTED`.

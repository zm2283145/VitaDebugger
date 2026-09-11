#pragma once

#include <psp2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_KERNEL_ABI_VERSION 0x00010003u
#define VD_KERNEL_MAX_THREADS 64

enum vd_kernel_capability {
    VD_KERNEL_CAP_THREAD_LIST = 1u << 0,
    VD_KERNEL_CAP_THREAD_CONTROL = 1u << 1,
    VD_KERNEL_CAP_PROBE_SUSPEND = 1u << 31,
};

struct vd_kernel_stop_result {
    unsigned int token;
    int suspended_count;
    int already_suspended_count;
    SceUID failed_thread;
    int failure_code;
};

struct vd_kernel_probe_suspend_result {
    int suspend_result;
    int state_while_suspended;
    int resume_result;
    int state_after_resume;
    int recovery_result;
};

struct vd_kernel_status {
    unsigned int abi_version;
    unsigned int capabilities;
    unsigned int max_threads;
    unsigned int reserved;
};

// Copy the companion ABI and supported capability bits to user memory.
int vdKernelGetStatus(struct vd_kernel_status* status);

// Enumerate only threads owned by the calling process. Kernel-global thread
// IDs are translated to process-visible user IDs before being returned.
// `capacity` must be between zero and VD_KERNEL_MAX_THREADS. `ids` may be NULL
// only when capacity is zero. copied_count and total_count are required.
int vdKernelGetThreadList(SceUID* ids, int capacity, int* copied_count,
                          int* total_count);

// Experimental boundary probe only. The target must belong to the calling
// process and cannot be the calling thread. The call always attempts to resume
// the target before returning and accepts only 100-500 ms durations.
int vdKernelProbeSuspendThread(
    SceUID target_user_thread,
    unsigned int duration_us,
    struct vd_kernel_probe_suspend_result* probe_result);

// Begin an all-stop session for the calling process. The calling thread is
// always excluded so it can service GDB and end or renew the session. A lease
// between 250 and 5000 ms is required; expiration automatically resumes every
// thread owned by this session.
int vdKernelBeginStop(unsigned int lease_ms, SceUID exempt_user_thread,
                      struct vd_kernel_stop_result* stop_result);

// Extend an active session owned by the calling process.
int vdKernelRenewStop(unsigned int token, unsigned int lease_ms);

// Resume only threads suspended by the matching session token.
int vdKernelEndStop(unsigned int token, int* resumed_count);

#ifdef __cplusplus
}
#endif

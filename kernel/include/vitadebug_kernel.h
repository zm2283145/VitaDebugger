#pragma once

#include <psp2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_KERNEL_ABI_VERSION 0x00010000u
#define VD_KERNEL_MAX_THREADS 64

enum vd_kernel_capability {
    VD_KERNEL_CAP_THREAD_LIST = 1u << 0,
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

#ifdef __cplusplus
}
#endif

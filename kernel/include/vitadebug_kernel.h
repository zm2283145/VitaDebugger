#pragma once

#include <psp2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_KERNEL_ABI_VERSION 0x00010007u
#define VD_KERNEL_MAX_THREADS 64
#define VD_KERNEL_VFP_D_REGISTER_COUNT 32
#define VD_KERNEL_VFP_LAYOUT_D32_V1 1u
#define VD_KERNEL_VFP_FPSCR_ENTRY_D32_V1 0u
#define VD_KERNEL_ERROR_VFP_DISABLED (-8)

enum vd_kernel_capability {
    VD_KERNEL_CAP_THREAD_LIST = 1u << 0,
    VD_KERNEL_CAP_THREAD_CONTROL = 1u << 1,
    VD_KERNEL_CAP_THREAD_REGISTERS = 1u << 2,
    VD_KERNEL_CAP_STOP_RECONCILE = 1u << 3,
    VD_KERNEL_CAP_HW_DEBUG_DISCOVERY = 1u << 4,
    VD_KERNEL_CAP_THREAD_VFP_REGISTERS = 1u << 5,
    VD_KERNEL_CAP_PROBE_SUSPEND = 1u << 31,
};

struct vd_arm_registers {
    unsigned int r[13];
    unsigned int sp;
    unsigned int lr;
    unsigned int pc;
    unsigned int cpsr;
    unsigned int fpscr;
};

struct vd_thread_registers {
    struct vd_arm_registers entry[2];
};

// Experimental read-only VFP snapshot. A known-pattern hardware probe
// validated the D0-D31 ordering and established that the D32 v1 FPSCR value is
// in raw CPU-register entry 0, even though saved user-mode ARM core state is in
// entry 1. Both raw FPSCR entries remain available as validation evidence.
struct vd_thread_vfp_registers {
    unsigned int layout_version;
    unsigned int d_register_count;
    uint64_t d[VD_KERNEL_VFP_D_REGISTER_COUNT];
    unsigned int fpscr_entry[2];
};

typedef char vd_thread_vfp_registers_size_must_be_272[
    sizeof(struct vd_thread_vfp_registers) == 272 ? 1 : -1];

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

struct vd_kernel_hw_debug_info {
    unsigned int raw_didr;
    unsigned int breakpoint_count;
    unsigned int watchpoint_count;
    unsigned int context_breakpoint_count;
};

// Copy the companion ABI and supported capability bits to user memory.
int vdKernelGetStatus(struct vd_kernel_status* status);

// Read the current CPU's architected debug identification register. This does
// not enable monitor mode or alter any breakpoint/watchpoint comparator.
int vdKernelGetHardwareDebugInfo(struct vd_kernel_hw_debug_info* info);

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

// Read both saved ARM register banks for a thread suspended and owned by the
// caller's active stop session. Bank interpretation is intentionally left raw
// until validated across Vita firmware and exception states.
int vdKernelGetThreadRegisters(unsigned int token, SceUID target_user_thread,
                               struct vd_thread_registers* registers);

// Capture a read-only VFP candidate layout for a thread suspended and owned by
// the caller's active stop session. The session token and target ownership are
// checked exactly as for vdKernelGetThreadRegisters. This call never writes
// target state. A normal kernel build returns VD_KERNEL_ERROR_VFP_DISABLED;
// the candidate implementation and capability bit exist only in an explicit
// VITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT build until hardware validation passes.
int vdKernelGetThreadVfpRegisters(
    unsigned int token,
    SceUID target_user_thread,
    struct vd_thread_vfp_registers* registers);

#ifdef __cplusplus
}
#endif

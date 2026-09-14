#pragma once

#include <psp2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_KERNEL_ABI_VERSION 0x0001000Bu
#define VD_KERNEL_MAX_THREADS 64
#define VD_KERNEL_HW_CORE_COUNT 3
#define VD_KERNEL_HW_BREAKPOINT_COUNT 6
#define VD_KERNEL_HW_WATCHPOINT_COUNT 4
#define VD_KERNEL_VFP_D_REGISTER_COUNT 32
#define VD_KERNEL_VFP_LAYOUT_D32_V1 1u
#define VD_KERNEL_VFP_FPSCR_ENTRY_D32_V1 0u
#define VD_KERNEL_ERROR_VFP_GUARD (-7)
#define VD_KERNEL_ERROR_VFP_DISABLED (-8)
#define VD_KERNEL_ERROR_HW_DISABLED (-9)
#define VD_KERNEL_ERROR_HW_BUSY (-10)
#define VD_KERNEL_ERROR_HW_OWNER (-11)
#define VD_KERNEL_ERROR_HW_STOP_REQUIRED (-12)
#define VD_KERNEL_ERROR_HW_INVALID (-13)
#define VD_KERNEL_ERROR_HW_CORE (-14)
#define VD_KERNEL_ERROR_HW_RESTORE (-15)
#define VD_KERNEL_ERROR_HW_RANGE (-16)
// Public normalized VFP-unavailable result. Use VitaSDK's stable ThreadMgr
// error encoding so the value survives the user/kernel syscall boundary.
#define VD_KERNEL_ERROR_VFP_CONTEXT_UNAVAILABLE ((int)0x80028031u)

enum vd_kernel_capability {
    VD_KERNEL_CAP_THREAD_LIST = 1u << 0,
    VD_KERNEL_CAP_THREAD_CONTROL = 1u << 1,
    VD_KERNEL_CAP_THREAD_REGISTERS = 1u << 2,
    VD_KERNEL_CAP_STOP_RECONCILE = 1u << 3,
    VD_KERNEL_CAP_HW_DEBUG_DISCOVERY = 1u << 4,
    VD_KERNEL_CAP_THREAD_VFP_REGISTERS = 1u << 5,
    VD_KERNEL_CAP_HW_BREAKPOINT = 1u << 6,
    VD_KERNEL_CAP_HW_WATCHPOINT = 1u << 7,
    VD_KERNEL_CAP_PROBE_SUSPEND = 1u << 31,
};

#define VD_KERNEL_REQUIRED_THREAD_CONTROL_CAPABILITIES \
    (VD_KERNEL_CAP_THREAD_LIST | \
     VD_KERNEL_CAP_THREAD_CONTROL | \
     VD_KERNEL_CAP_THREAD_REGISTERS | \
     VD_KERNEL_CAP_STOP_RECONCILE)

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
// in raw CPU-register entry 0. ARM core-register selection is independent and
// state-dependent: runnable/current user state may be in entry 0, while a
// syscall-return user context may be in entry 1. Both raw FPSCR entries remain
// available as validation evidence.
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

enum vd_kernel_hw_point_operation {
    VD_KERNEL_HW_POINT_INSERT = 1,
    VD_KERNEL_HW_POINT_REMOVE = 2,
};

enum vd_kernel_hw_point_type {
    VD_KERNEL_HW_POINT_EXECUTE = 1,
    VD_KERNEL_HW_POINT_WRITE = 2,
    VD_KERNEL_HW_POINT_READ = 3,
    VD_KERNEL_HW_POINT_ACCESS = 4,
};

struct vd_kernel_hw_point_request {
    unsigned int size;
    unsigned int operation;
    unsigned int type;
    unsigned int address;
    unsigned int length;
    unsigned int flags;
};

struct vd_kernel_hw_core_info {
    unsigned int core_id;
    unsigned int raw_mpidr;
    unsigned int raw_midr;
    unsigned int raw_didr;
    unsigned int raw_dscr;
    unsigned int raw_vcr;
    unsigned int breakpoint_control[VD_KERNEL_HW_BREAKPOINT_COUNT];
    unsigned int watchpoint_control[VD_KERNEL_HW_WATCHPOINT_COUNT];
    unsigned int breakpoint0_value;
    unsigned int context_breakpoint_value;
    unsigned int watchpoint0_value;
};

struct vd_kernel_hw_session_result {
    unsigned int token;
    unsigned int core_count;
    unsigned int context_id;
    int failure_core;
    int failure_code;
    struct vd_kernel_hw_core_info core[VD_KERNEL_HW_CORE_COUNT];
};

typedef char vd_kernel_hw_point_request_size_must_be_24[
    sizeof(struct vd_kernel_hw_point_request) == 24 ? 1 : -1];
typedef char vd_kernel_hw_core_info_size_must_be_76[
    sizeof(struct vd_kernel_hw_core_info) == 76 ? 1 : -1];
typedef char vd_kernel_hw_session_result_size_must_be_248[
    sizeof(struct vd_kernel_hw_session_result) == 248 ? 1 : -1];

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

// Read both raw, state-dependent ARM register banks for a thread suspended and
// owned by the caller's active stop session. Bank interpretation is
// intentionally left raw
// until validated across Vita firmware and exception states.
int vdKernelGetThreadRegisters(unsigned int token, SceUID target_user_thread,
                               struct vd_thread_registers* registers);

// Capture a read-only VFP candidate layout for a thread suspended and owned by
// the caller's active stop session. The session token and target ownership are
// checked exactly as for vdKernelGetThreadRegisters. This call never writes
// target state. A normal kernel build returns VD_KERNEL_ERROR_VFP_DISABLED;
// the candidate implementation and capability bit exist only in an explicit
// VITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT build until hardware validation passes.
// VD_KERNEL_ERROR_VFP_CONTEXT_UNAVAILABLE means the target has no readable
// saved VFP context; every other negative result is a fatal snapshot failure.
int vdKernelGetThreadVfpRegisters(
    unsigned int token,
    SceUID target_user_thread,
    struct vd_thread_vfp_registers* registers);

// Acquire exclusive, lease-protected ownership of the experimental ARMv7
// debug comparators for the calling process. The default kernel build returns
// VD_KERNEL_ERROR_HW_DISABLED and never touches CP14 debug state. An enabled
// candidate snapshots and inventories all three application CPU cores and
// refuses acquisition if any comparator is already enabled rather than risk
// conflicting with another debugger or an existing link to reserved BRP5.
int vdKernelAcquireHardwareDebug(
    unsigned int lease_ms,
    struct vd_kernel_hw_session_result* session_result);

// Renew hardware-debug ownership. The full caller CONTEXTIDR must still match
// the value captured at acquisition. Expiration restores every owned register
// on every application core before another process may acquire the resource.
int vdKernelRenewHardwareDebug(unsigned int token, unsigned int lease_ms);

// Insert or remove the single supported execution breakpoint or data
// watchpoint. Mutation is allowed only while the caller owns both this hardware
// token and the supplied active all-stop token. For the current experimental
// implementation, that stop must have no extra exempt thread and the original
// stop controller must issue the mutation. Requests are copied and strictly
// validated in kernel memory; flags must be zero.
int vdKernelUpdateHardwarePoint(
    unsigned int token,
    unsigned int stop_token,
    const struct vd_kernel_hw_point_request* request);

// Restore the exact pre-acquisition comparator and monitor state. Unlike point
// mutation, release intentionally needs no all-stop token so disconnect and
// shutdown cleanup remain possible.
int vdKernelReleaseHardwareDebug(unsigned int token);

#ifdef __cplusplus
}
#endif

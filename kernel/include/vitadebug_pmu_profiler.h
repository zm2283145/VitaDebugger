#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This ABI is intentionally separate from the general VitaDebugger kernel
 * ABI.  Every structure crossing the syscall boundary carries and validates
 * both its exact size and this version. */
#define VD_KERNEL_PMU_PROFILER_ABI_VERSION 1u
#define VD_KERNEL_PMU_PROFILER_MAX_EVENTS 4u
#define VD_KERNEL_PMU_PROFILER_FIXED_CORE 0u
#define VD_KERNEL_PMU_PROFILER_FIXED_COUNTER 5u
#define VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS 250u
#define VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS 5000u

#define VD_KERNEL_PMU_PROFILER_EVENT_SOFTWARE_INCREMENT UINT32_C(0x00)
#define VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS UINT32_C(0x01)
#define VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS UINT32_C(0x03)
#define VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT UINT32_C(0x10)

enum vd_kernel_pmu_profiler_capability {
    VD_KERNEL_PMU_PROFILER_CAP_EXACT_RESTORE = 1u << 0,
    VD_KERNEL_PMU_PROFILER_CAP_LEASE_WATCHDOG = 1u << 1,
    VD_KERNEL_PMU_PROFILER_CAP_SOFTWARE_INCREMENT = 1u << 2,
    VD_KERNEL_PMU_PROFILER_CAP_REAL_EVENTS = 1u << 3,
    VD_KERNEL_PMU_PROFILER_CAP_SINGLE_REAL_EVENT_PER_BOOT = 1u << 4,
    /* Advertised only by a separately enabled candidate that can re-arm after
     * independently verified exact restore. Explicit close and dormant-owner-
     * thread recovery are hardware-gated separately; process-exit/crash and
     * other terminal-owner classes remain disabled until their own gates pass. */
    VD_KERNEL_PMU_PROFILER_CAP_SAFE_POST_RESTORE_REARM = 1u << 5,
    /* Advertised only by the separate process-event candidate.  The kernel
     * observes the exact owning process's exit/kill event before UID teardown
     * and completes serialized exact restoration before authorizing re-arm. */
    VD_KERNEL_PMU_PROFILER_CAP_PROCESS_EXIT_CLEANUP = 1u << 6,
};

/* A real counter selection requires an explicit caller acknowledgement in
 * addition to the compile-time kernel gate.  Event 0x00 requires flags=0. */
#define VD_KERNEL_PMU_PROFILER_OPEN_ACK_REAL_EVENT (UINT32_C(1) << 0)
#define VD_KERNEL_PMU_PROFILER_OPEN_ALLOWED_FLAGS \
    VD_KERNEL_PMU_PROFILER_OPEN_ACK_REAL_EVENT

/* Stable transport errors.  Backend-private errors are never exposed as an
 * API contract; the status field in a successful sample remains reserved. */
#define VD_KERNEL_ERROR_PMU_PROFILER_DISABLED (-40)
#define VD_KERNEL_ERROR_PMU_PROFILER_INVALID (-41)
#define VD_KERNEL_ERROR_PMU_PROFILER_BUSY (-42)
#define VD_KERNEL_ERROR_PMU_PROFILER_OWNER (-43)
#define VD_KERNEL_ERROR_PMU_PROFILER_UNSUPPORTED_EVENT (-44)
#define VD_KERNEL_ERROR_PMU_PROFILER_EXPIRED (-45)
#define VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED (-46)
#define VD_KERNEL_ERROR_PMU_PROFILER_STATE (-47)
#define VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM (-48)
#define VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED (-49)

struct vd_kernel_pmu_profiler_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t capabilities;
    uint32_t fixed_core;
    uint32_t fixed_counter;
    uint32_t min_lease_ms;
    uint32_t max_lease_ms;
    uint32_t event_count;
    uint32_t event_codes[VD_KERNEL_PMU_PROFILER_MAX_EVENTS];
    uint32_t reserved[4];
};

struct vd_kernel_pmu_profiler_open_request {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t event_code;
    uint32_t lease_ms;
    uint32_t flags;
    uint32_t reserved[3];
};

/* The kernel-generated token and generation are bound to the exact calling
 * process and controller thread.  Callers must echo the complete handle
 * without modification on read and close. */
struct vd_kernel_pmu_profiler_handle {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t owner_token;
    uint32_t generation;
    uint32_t event_code;
    uint32_t lease_ms;
    uint32_t lease_token_low;
    uint32_t lease_token_high;
    uint32_t reserved[2];
};

struct vd_kernel_pmu_profiler_sample {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t owner_token;
    uint32_t generation;
    uint32_t event_code;
    uint32_t core_id;
    uint32_t physical_counter;
    uint32_t flags;
    uint64_t value;
    uint32_t reserved[2];
};

/* Complete PMU scope captured by the fixed-core backend.  Lanes 0..4 remain
 * zero because this gate never selects or mutates them; lane 5 and every
 * shared register that can affect the transaction are captured exactly. */
struct vd_kernel_pmu_profiler_snapshot {
    uint32_t event_counter_count;
    uint32_t raw_pmcr;
    uint32_t raw_pmcntenset;
    uint32_t raw_pmovsr;
    uint32_t raw_pmselr;
    uint32_t raw_pmccntr;
    uint32_t raw_pmuserenr;
    uint32_t raw_pmintenset;
    uint32_t raw_pmxevtyper[6];
    uint32_t raw_pmxevcntr[6];
};

#define VD_KERNEL_PMU_PROFILER_STATUS_ABI_VERSION 1u
#define VD_KERNEL_PMU_PROFILER_TERMINAL_NONE 0u
#define VD_KERNEL_PMU_PROFILER_TERMINAL_EXIT 1u
#define VD_KERNEL_PMU_PROFILER_TERMINAL_KILL 2u
#define VD_KERNEL_PMU_PROFILER_TERMINAL_UNCERTAIN 3u

struct vd_kernel_pmu_profiler_status {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t transport_state;
    int32_t last_result;
    int32_t owner_pid;
    int32_t owner_thread;
    uint32_t owner_token;
    uint32_t generation;
    uint32_t real_event_attempted;
    uint32_t exact_restore_proven;
    uint32_t owner_identity_valid;
    uint32_t owner_identity_release_uncertain;
    uint32_t rearm_count;
    uint32_t process_normal_exit_cleanup_count;
    uint32_t process_kill_cleanup_count;
    uint32_t owner_process_terminal_kind;
    uint32_t owner_terminal_reference_released;
    uint32_t backend_ready;
    uint32_t backend_recovery_pending;
    uint32_t backend_restore_obligation;
    int32_t snapshot_result;
    struct vd_kernel_pmu_profiler_snapshot snapshot;
    uint32_t reserved[5];
};

typedef char vd_kernel_pmu_profiler_info_size_must_be_64[
    sizeof(struct vd_kernel_pmu_profiler_info) == 64 ? 1 : -1];
typedef char vd_kernel_pmu_profiler_open_request_size_must_be_32[
    sizeof(struct vd_kernel_pmu_profiler_open_request) == 32 ? 1 : -1];
typedef char vd_kernel_pmu_profiler_handle_size_must_be_40[
    sizeof(struct vd_kernel_pmu_profiler_handle) == 40 ? 1 : -1];
typedef char vd_kernel_pmu_profiler_sample_size_must_be_48[
    sizeof(struct vd_kernel_pmu_profiler_sample) == 48 ? 1 : -1];
typedef char vd_kernel_pmu_profiler_snapshot_size_must_be_80[
    sizeof(struct vd_kernel_pmu_profiler_snapshot) == 80 ? 1 : -1];
typedef char vd_kernel_pmu_profiler_status_size_must_be_184[
    sizeof(struct vd_kernel_pmu_profiler_status) == 184 ? 1 : -1];

/* GetInfo is also a negotiation call: initialize the complete input object to
 * zero, then set only struct_size and abi_version.  Unknown/nonzero input is
 * rejected before the kernel writes the negotiated result. */
int vdKernelPmuProfilerGetInfo(struct vd_kernel_pmu_profiler_info* info);

/* Gate-only readback. Input must be zero except for struct_size and
 * abi_version. A zero return includes a complete current idle snapshot;
 * snapshot_result is negative and the call fails closed while a lease or
 * backend obligation is active. */
int vdKernelPmuProfilerGetStatus(
    struct vd_kernel_pmu_profiler_status* status);

int vdKernelPmuProfilerOpen(
    const struct vd_kernel_pmu_profiler_open_request* request,
    struct vd_kernel_pmu_profiler_handle* handle);

int vdKernelPmuProfilerRead(
    const struct vd_kernel_pmu_profiler_handle* handle,
    struct vd_kernel_pmu_profiler_sample* sample);

/* A zero return proves the owned lane and persistent controls were restored
 * and verified.  Restore-required is retryable with the same complete handle;
 * the kernel watchdog also retains and retries the obligation. */
int vdKernelPmuProfilerClose(
    const struct vd_kernel_pmu_profiler_handle* handle);

#ifdef __cplusplus
}
#endif

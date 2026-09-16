#pragma once

#include <stdint.h>

#include "pmu_profiler_bridge.h"
#include "vitadebug_pmu_profiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_TRANSPORT)

#define VD_PMU_PROFILER_TRANSPORT_INITIALIZATION_COOKIE \
    UINT32_C(0x56505431)

/* Safe re-arm remains an independently gated experiment until its complete
 * lifecycle matrix passes on hardware.  Presence with value zero must not
 * enable it: build systems commonly emit -D...=0 for disabled options. */
#if defined(VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_SAFE_REARM) && \
    ((VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_SAFE_REARM + 0) == 1)
#define VD_PMU_PROFILER_SAFE_REARM_COMPILED 1
#else
#define VD_PMU_PROFILER_SAFE_REARM_COMPILED 0
#endif

enum vd_pmu_profiler_transport_state {
    VD_PMU_PROFILER_TRANSPORT_IDLE = 0,
    VD_PMU_PROFILER_TRANSPORT_ACTIVE = 1,
    VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED = 2,
    /* Hardware has been restored and independently verified, but a real-event
     * attempt is still bound to its original owner/generation.  Only a
     * matching explicit close or proof that the captured owner is gone may
     * retire this state and re-arm another real event. */
    VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER = 3,
};

enum vd_pmu_profiler_owner_status {
    /* The liveness probe itself lost reference accounting certainty.  The
     * transport must restore PMU state, then remain permanently quarantined. */
    VD_PMU_PROFILER_OWNER_QUARANTINE = -2,
    VD_PMU_PROFILER_OWNER_UNKNOWN = -1,
    VD_PMU_PROFILER_OWNER_GONE = 0,
    VD_PMU_PROFILER_OWNER_ALIVE = 1,
};

/* Captured before a lease is published.  The production implementation uses
 * immutable thread attributes in addition to the UID, so a recycled UID is
 * never accepted as proof that the original owner is still present.  An
 * identity collision is safe: it merely leaves re-arm blocked. */
struct vd_pmu_profiler_owner_identity {
    int32_t process_id;
    int32_t thread_id;
    /* Kernel-only retained object identity.  Safe-rearm builds require this
     * to remain referenced until the lease has both restored exactly and
     * reached an authenticated terminal condition. */
    uintptr_t retained_thread_object;
    uintptr_t thread_entry;
    uintptr_t thread_stack;
    uint32_t thread_stack_size;
    int32_t initial_priority;
    int32_t initial_affinity;
    uint32_t thread_attributes;
};

struct vd_pmu_profiler_owner_backend {
    void* context;
    int (*capture)(void* context, int32_t owner_pid,
                   int32_t owner_thread,
                   struct vd_pmu_profiler_owner_identity* identity);
    int (*query)(void* context, int32_t owner_pid,
                 int32_t owner_thread,
                 const struct vd_pmu_profiler_owner_identity* identity);
    /* Release the exact reference established by capture.  Zero alone means
     * definite release.  A nonzero result has unknown effect and permanently
     * quarantines this transport instance; it must never be retried blindly. */
    int (*release)(void* context, int32_t owner_pid,
                   int32_t owner_thread,
                   const struct vd_pmu_profiler_owner_identity* identity);
};

/* One global instance is used by the kernel companion.  Its caller must
 * serialize every operation; the public syscall shim uses a dedicated lock
 * which is independent of the debugger all-stop lock. */
struct vd_pmu_profiler_transport {
    uint32_t initialization_cookie;
    uint32_t state;
    uint32_t next_owner_token;
    uint32_t next_generation;
    int32_t owner_pid;
    int32_t owner_thread;
    uint32_t owner_token;
    uint32_t generation;
    uint32_t event_code;
    uint32_t lease_ms;
    uint64_t lease_token;
    uint32_t real_event_attempted;
    uint32_t exact_restore_proven;
    uint32_t owner_identity_valid;
    /* Covers either final retained-reference release uncertainty or a
     * temporary liveness-query reference whose release was uncertain. */
    uint32_t owner_identity_release_uncertain;
    uint32_t rearm_count;
    int32_t last_result;
    struct vd_pmu_profiler_owner_identity owner_identity;
    struct vd_pmu_profiler_owner_backend owner_backend;
    struct vd_pmu_profiler_bridge bridge;
};

int vdPmuProfilerTransportInit(
    struct vd_pmu_profiler_transport* transport);

/* Install the trusted kernel-only owner identity provider while IDLE and
 * before any real-event attempt.  It is never supplied by an application.
 * Capture returns zero only after filling the complete identity.  Query
 * returns ALIVE, GONE, or UNKNOWN; errors and unknown states always fail
 * closed and can never authorize re-arm. */
int vdPmuProfilerTransportSetOwnerBackend(
    struct vd_pmu_profiler_transport* transport,
    const struct vd_pmu_profiler_owner_backend* backend);

int vdPmuProfilerTransportGetInfo(
    const struct vd_pmu_profiler_transport* transport,
    struct vd_kernel_pmu_profiler_info* info);

int vdPmuProfilerTransportOpen(
    struct vd_pmu_profiler_transport* transport,
    int32_t caller_pid,
    int32_t caller_thread,
    const struct vd_kernel_pmu_profiler_open_request* request,
    struct vd_kernel_pmu_profiler_handle* handle);

int vdPmuProfilerTransportRead(
    struct vd_pmu_profiler_transport* transport,
    int32_t caller_pid,
    int32_t caller_thread,
    const struct vd_kernel_pmu_profiler_handle* handle,
    struct vd_kernel_pmu_profiler_sample* sample);

int vdPmuProfilerTransportClose(
    struct vd_pmu_profiler_transport* transport,
    int32_t caller_pid,
    int32_t caller_thread,
    const struct vd_kernel_pmu_profiler_handle* handle);

/* The watchdog is bounded to one recovery pass.  A positive return means it
 * completed and retired an expired/orphaned transport session or an
 * ownerless backend obligation left by a failed first snapshot; zero means no
 * action was due; a negative return leaves recovery pending for a later
 * retry. */
int vdPmuProfilerTransportWatchdog(
    struct vd_pmu_profiler_transport* transport);

/* Called before module unload.  It never forgets a failed restoration.  Once
 * a real-event attempt has occurred it returns REBOOT_REQUIRED even after a
 * clean restoration, keeping the module resident so unload/reload cannot
 * bypass the one-real-event-per-boot latch. */
int vdPmuProfilerTransportShutdown(
    struct vd_pmu_profiler_transport* transport);

#endif

#ifdef __cplusplus
}
#endif

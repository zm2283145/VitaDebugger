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

enum vd_pmu_profiler_transport_state {
    VD_PMU_PROFILER_TRANSPORT_IDLE = 0,
    VD_PMU_PROFILER_TRANSPORT_ACTIVE = 1,
    VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED = 2,
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
    int32_t last_result;
    struct vd_pmu_profiler_bridge bridge;
};

int vdPmuProfilerTransportInit(
    struct vd_pmu_profiler_transport* transport);

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

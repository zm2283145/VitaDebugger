#pragma once

#include <stdint.h>

#include "pmu_backend.h"
#include "vitaprofiler_pmu.h"

#ifdef __cplusplus
extern "C" {
#endif

#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED && \
    !defined(VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION)
#error "real profiler events require the experimental PMU session"
#endif

#if defined(VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION)

#define VD_PMU_PROFILER_BRIDGE_ABI_VERSION 2u
#define VD_PMU_PROFILER_BRIDGE_INITIALIZATION_COOKIE UINT32_C(0x56505042)

/* The bridge always owns exactly one event on fixed physical lane 5.  Its
 * software-increment event is the only default-enabled selection.  The small
 * real-event catalog requires both the build-time experimental definition and
 * this per-instance acknowledgement; normal builds accept no such flag.
 */
#define VD_PMU_PROFILER_BRIDGE_COUNTER_MASK VP_PMU_COUNTER_EVENT0
#define VD_PMU_PROFILER_BRIDGE_MAX_EVENTS 1u
#define VD_PMU_PROFILER_BRIDGE_CONFIG_ALLOW_REAL_EVENTS \
    (UINT32_C(1) << 0)
#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED
#define VD_PMU_PROFILER_BRIDGE_ALLOWED_CONFIG_FLAGS \
    VD_PMU_PROFILER_BRIDGE_CONFIG_ALLOW_REAL_EVENTS
#else
#define VD_PMU_PROFILER_BRIDGE_ALLOWED_CONFIG_FLAGS UINT32_C(0)
#endif

#define VD_PMU_PROFILER_NAME_SOFTWARE_INCREMENT \
    "cpu.pmu.software_increment"
#define VD_PMU_PROFILER_NAME_ICACHE_MISS "cpu.pmu.icache_miss"
#define VD_PMU_PROFILER_NAME_DCACHE_MISS "cpu.pmu.dcache_miss"
#define VD_PMU_PROFILER_NAME_BRANCH_MISPREDICT \
    "cpu.pmu.branch_mispredict"

enum vd_pmu_profiler_bridge_operation
{
    VD_PMU_PROFILER_BRIDGE_OPERATION_NONE = 0,
    VD_PMU_PROFILER_BRIDGE_OPERATION_ACQUIRE = 1,
    VD_PMU_PROFILER_BRIDGE_OPERATION_READ = 2,
    VD_PMU_PROFILER_BRIDGE_OPERATION_RELEASE = 3,
    VD_PMU_PROFILER_BRIDGE_OPERATION_WATCHDOG = 4,
    VD_PMU_PROFILER_BRIDGE_OPERATION_ORPHAN_RESTORE = 5,
};

struct vd_pmu_profiler_bridge_config
{
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t owner_pid;
    uint32_t owner_token;
    uint32_t core_id;
    uint32_t lease_ms;
    uint32_t flags;
    uint32_t reserved;
};

struct vd_pmu_profiler_bridge_status
{
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t initialized;
    uint32_t session_state;
    uint32_t orphan_restore_pending;
    uint32_t auto_restored_waiting_release;
    uint32_t last_operation;
    int32_t last_kernel_error;
    int32_t last_provider_result;
    uint32_t active_event_id;
    uint32_t active_event_code;
    uint32_t lease_ms;
    uint32_t provider_lease_held;
    uint32_t config_flags;
};

typedef char vd_pmu_profiler_bridge_config_size_must_be_32[
    sizeof(struct vd_pmu_profiler_bridge_config) == 32 ? 1 : -1];
typedef char vd_pmu_profiler_bridge_status_size_must_be_56[
    sizeof(struct vd_pmu_profiler_bridge_status) == 56 ? 1 : -1];

/* Kernel-internal storage.  The owner must serialize all calls and retain the
 * object until every restoration obligation has completed.  There is no
 * force-forget/reset operation.
 */
struct vd_pmu_profiler_bridge
{
    uint32_t initialization_cookie;
    struct vp_pmu_provider provider;
    struct vd_pmu_session session;
    struct vd_pmu_session_owner owner;
    struct vd_pmu_session_backend backend;
    uint64_t active_token;
    uint64_t auto_restored_token;
    uint32_t lease_ms;
    uint32_t active_event_id;
    uint32_t active_event_code;
    uint32_t config_flags;
    uint32_t orphan_restore_pending;
    uint32_t provider_lease_held;
    uint32_t last_operation;
    int32_t last_kernel_error;
    int32_t last_provider_result;
};

/* Initialization obtains the already-started production PMU backend.  It does
 * not start workers and performs no PMU access.  The experimental backend and
 * this bridge are omitted from normal kernel builds unless explicitly enabled.
 */
int vdPmuProfilerBridgeInit(
    struct vd_pmu_profiler_bridge* bridge,
    const struct vd_pmu_profiler_bridge_config* config);

const struct vp_pmu_provider* vdPmuProfilerBridgeGetProvider(
    const struct vd_pmu_profiler_bridge* bridge);

/* Resolve one currently enabled event, register its stable VitaProfiler name,
 * and produce the matching single-lane config/name mapping.  This performs no
 * PMU access.  Register before sealing the caller-owned dictionary. */
#if !defined(VD_PMU_PROFILER_BRIDGE_NO_DICTIONARY)
int vdPmuProfilerBridgePrepareEvent(
    const struct vd_pmu_profiler_bridge* bridge,
    struct vp_name_dictionary* dictionary,
    uint32_t event_code,
    struct vp_pmu_config* config,
    struct vp_pmu_name_ids* names);
#endif

int vdPmuProfilerBridgeGetStatus(
    const struct vd_pmu_profiler_bridge* bridge,
    struct vd_pmu_profiler_bridge_status* status);

/* Zero means no expiry; one means a lease expired and was restored exactly.
 * A negative result is a provider error.  The caller must keep invoking this
 * bounded watchdog while a bridge can own a session.
 */
int vdPmuProfilerBridgeWatchdog(
    struct vd_pmu_profiler_bridge* bridge);

/* Only an acquire failure can create an orphan restoration obligation because
 * the outer vp_pmu_session never receives a token in that case.  Ordinary
 * release failures must be retried with vp_pmu_session_end().
 */
int vdPmuProfilerBridgeRetryOrphanRestore(
    struct vd_pmu_profiler_bridge* bridge);

#endif

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdint.h>

#include "pmu_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Treat only the documented numeric value 1 as enabled.  In particular,
 * build systems commonly emit -D...=0 for disabled options; presence alone
 * must not open the real-event path. */
#if defined(VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS) && \
    ((VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS + 0) == 1)
#define VD_PMU_PROFILER_REAL_EVENTS_COMPILED 1
#else
#define VD_PMU_PROFILER_REAL_EVENTS_COMPILED 0
#endif

#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION

#define VD_PMU_BACKEND_ABI_VERSION 1u
#define VD_PMU_BACKEND_APP_CORE_COUNT 3u
#define VD_PMU_BACKEND_TEST_INCREMENT_COUNT 17u

#define VD_PMU_BACKEND_ERROR_INVALID (-200)
#define VD_PMU_BACKEND_ERROR_DISABLED (-201)
#define VD_PMU_BACKEND_ERROR_BUSY (-202)
#define VD_PMU_BACKEND_ERROR_CORE (-203)
#define VD_PMU_BACKEND_ERROR_TIMEOUT (-204)
#define VD_PMU_BACKEND_ERROR_NOT_IDLE (-205)
#define VD_PMU_BACKEND_ERROR_CONFLICT (-206)
#define VD_PMU_BACKEND_ERROR_VERIFY (-207)
#define VD_PMU_BACKEND_ERROR_RESTORE (-208)
#define VD_PMU_BACKEND_ERROR_CLEANUP (-209)

/* A positive start result means initialization failed and the backend is
 * disabled, but at least one worker/object could not be proven gone.  A
 * module_start caller must remain resident and retry vdPmuBackendStop rather
 * than unload live code. */
#define VD_PMU_BACKEND_START_RESIDENT_DISABLED 1

enum vd_pmu_backend_test_stage {
    VD_PMU_BACKEND_TEST_NOT_RUN = 0,
    VD_PMU_BACKEND_TEST_SNAPSHOT = 1,
    VD_PMU_BACKEND_TEST_CONFIGURE = 2,
    VD_PMU_BACKEND_TEST_INCREMENT = 3,
    VD_PMU_BACKEND_TEST_READ = 4,
    VD_PMU_BACKEND_TEST_RESTORE = 5,
    VD_PMU_BACKEND_TEST_VERIFY = 6,
    VD_PMU_BACKEND_TEST_COMPLETE = 7,
};

/*
 * Result for the deliberately narrow first hardware mutation gate.  The
 * operation owns only programmable counter 5, uses event 0x00, performs
 * exactly 17 PMSWINC writes, and restores before returning.  A negative
 * restore_result means the plugin must remain resident and recovery must be
 * retried; unloading code with an outstanding PMU mutation is unsafe.
 */
struct vd_pmu_backend_test_result {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t core_id;
    uint32_t stage;
    int32_t operation_result;
    int32_t restore_result;
    uint32_t raw_midr;
    uint32_t raw_mpidr;
    uint32_t observed_count;
    uint32_t increment_count;
    /* Backend projections: lane 5 is populated; untouched lanes 0..4 are
     * intentionally zero/unobserved.  All shared controls are raw reads. */
    struct vd_pmu_snapshot before;
    struct vd_pmu_snapshot after;
};

/* Start creates one fixed-affinity worker for each application core (0..2).
 * It deliberately performs no CP15 access at module load time. */
int vdPmuBackendStart(void);

/* Stop first resolves any retained restoration obligation.  It refuses to
 * tear workers down if exact restoration or bounded worker cleanup fails. */
int vdPmuBackendStop(void);

int vdPmuBackendReady(void);
int vdPmuBackendHasRestoreObligation(void);

/* True while the watchdog must call vdPmuBackendRecover().  This is broader
 * than HasRestoreObligation: a timed-out worker command must be reaped even
 * when it completed without leaving a selector/full-snapshot restore flag. */
int vdPmuBackendRecoveryPending(void);

/* Retry the backend's retained exact-restore record.  This is suitable for a
 * plugin watchdog after an ambiguous callback timeout. */
int vdPmuBackendRecover(void);

/* Fill the callback table consumed by pmu_session.c.  The table uses a single
 * global, serialized backend because the first gate permits no external or
 * concurrent PMU owner. */
int vdPmuBackendMakeSessionBackend(
    struct vd_pmu_session_backend* backend);

/* Run the isolated event-0/PMSWINC hardware gate on one application core.
 * This operation never leaves the counter configured after success. */
int vdPmuBackendRunSelfTest(
    uint32_t core_id,
    struct vd_pmu_backend_test_result* result);

#endif

#ifdef __cplusplus
}
#endif

#ifndef VITAPROFILER_PMU_VITA_H
#define VITAPROFILER_PMU_VITA_H

#include "vitaprofiler_pmu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Passing this value is an explicit assertion that VitaProfiler owns the
 * target thread's complete PMU state for the session. ScePerf exposes no API
 * for reading the previous selector or run state, so this adapter cannot be
 * used to coexist with Razor, another profiler, or application PMU use. */
#define VP_VITA_PMU_APPLICATION_OWNERSHIP_ACK UINT32_C(0x56504f57)
#define VP_VITA_PMU_PHYSICAL_COUNTERS_USED 5u
#define VP_VITA_PMU_EVENT_CYCLE_COUNT UINT32_C(0x11)

enum vp_vita_pmu_operation {
    VP_VITA_PMU_OPERATION_NONE = 0,
    VP_VITA_PMU_OPERATION_GET_THREAD_ID = 1,
    VP_VITA_PMU_OPERATION_RESET = 2,
    VP_VITA_PMU_OPERATION_SELECT_EVENT = 3,
    VP_VITA_PMU_OPERATION_SET_COUNTER = 4,
    VP_VITA_PMU_OPERATION_START = 5,
    VP_VITA_PMU_OPERATION_GET_COUNTER = 6,
    VP_VITA_PMU_OPERATION_STOP = 7,
};

/* Injectable surface used by the native tests and by ports which provide the
 * same documented ScePerf semantics. Every callback returns zero on success
 * and a platform error otherwise. get_thread_id is used only when init receives
 * thread_id == 0 (the ScePerf SELF value). */
struct vp_vita_pmu_ops {
    int32_t (*get_thread_id)(void* user);
    int (*reset)(void* user, int32_t thread_id);
    int (*select_event)(void* user, int32_t thread_id, uint32_t counter,
                        uint8_t event_code);
    int (*start)(void* user, int32_t thread_id);
    int (*stop)(void* user, int32_t thread_id);
    int (*get_counter)(void* user, int32_t thread_id, uint32_t counter,
                       uint32_t* value);
    int (*set_counter)(void* user, int32_t thread_id, uint32_t counter,
                       uint32_t value);
    void* user;
};

/* Public for static allocation. Zero-initialize before the first init and
 * treat every field as private thereafter. One instance may own one thread;
 * each linked copy of the implementation permits only one owned-reset ScePerf
 * lease at a time. Applications with separately linked copies in multiple
 * modules must provide their own shared ownership coordination. */
struct vp_vita_pmu_owned {
    struct vp_pmu_provider provider;
    struct vp_vita_pmu_ops ops;
    int32_t thread_id;
    int32_t last_platform_error;
    uint64_t active_token;
    uint32_t event_physical[VP_PMU_MAX_EVENT_COUNTERS];
    uint32_t cycles_physical;
    uint32_t active;
    uint32_t cleanup_pending;
    uint32_t initialized;
    uint32_t last_operation;
};

struct vp_vita_pmu_owned_status {
    int32_t thread_id;
    int32_t last_platform_error;
    uint32_t active;
    uint32_t cleanup_pending;
    uint32_t last_operation;
};

int vp_vita_pmu_owned_init_with_ops(
    struct vp_vita_pmu_owned* owned, const struct vp_vita_pmu_ops* ops,
    int32_t thread_id, uint32_t ownership_ack);

#if defined(__vita__)
/* Reserved convenience entry point for a future loader-verified ScePerf
 * binding. It currently fails closed with VP_ERROR_UNSUPPORTED on Vita:
 * retail 3.65 hardware proved that a nonzero import stub can still branch to
 * address zero. Use vp_vita_pmu_owned_init_with_ops() only with function
 * targets obtained from a resolver whose result and module lifetime the
 * application can verify. */
int vp_vita_pmu_owned_init(struct vp_vita_pmu_owned* owned,
                           int32_t thread_id, uint32_t ownership_ack);
#endif

const struct vp_pmu_provider* vp_vita_pmu_owned_get_provider(
    const struct vp_vita_pmu_owned* owned);
int vp_vita_pmu_owned_get_status(
    const struct vp_vita_pmu_owned* owned,
    struct vp_vita_pmu_owned_status* status);

/* Only for a failed acquire, where no vp_pmu_session exists to retain the
 * cleanup obligation. An ordinary release failure must instead be retried by
 * calling vp_pmu_session_end() again. */
int vp_vita_pmu_owned_retry_orphan_cleanup(
    struct vp_vita_pmu_owned* owned);

#ifdef __cplusplus
}
#endif

#endif

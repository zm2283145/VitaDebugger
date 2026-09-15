#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_PMU_SESSION_ABI_VERSION 1u
#define VD_PMU_SESSION_INITIALIZATION_COOKIE UINT32_C(0x504D5531)
#define VD_PMU_SESSION_MIN_LEASE_MS 250u
#define VD_PMU_SESSION_MAX_LEASE_MS 5000u
#define VD_PMU_SESSION_MAX_EVENTS 4u
#define VD_PMU_SESSION_MAX_ACTIVE_EVENTS 1u
#define VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS 6u
#define VD_PMU_SESSION_PHYSICAL_CORE_COUNT 4u

#define VD_PMU_SESSION_FLAG_CYCLES (UINT32_C(1) << 0)
/* The first hardware mutation gate deliberately owns no cycle counter and
 * only one clean programmable lane. This mask can be expanded after that gate
 * proves restoration on hardware. */
#define VD_PMU_SESSION_ALLOWED_FLAGS UINT32_C(0)

#define VD_PMU_CONFIGURATION_ENABLE_GLOBAL (UINT32_C(1) << 0)

/* Internal state-machine errors. Backend errors are otherwise returned
 * unchanged when cleanup can be proven complete. */
#define VD_PMU_SESSION_ERROR_INVALID (-100)
#define VD_PMU_SESSION_ERROR_BUSY (-101)
#define VD_PMU_SESSION_ERROR_OWNER (-102)
#define VD_PMU_SESSION_ERROR_UNSUPPORTED_EVENT (-103)
#define VD_PMU_SESSION_ERROR_RESTORE_REQUIRED (-104)
#define VD_PMU_SESSION_ERROR_EXPIRED (-105)
#define VD_PMU_SESSION_ERROR_CLOCK (-106)
#define VD_PMU_SESSION_ERROR_STATE (-107)
#define VD_PMU_SESSION_ERROR_NOT_IDLE (-108)
#define VD_PMU_SESSION_ERROR_BACKEND_CONTRACT (-109)

/* A successful watchdog call returns this when it expired and restored a
 * lease. Zero means that there was no lease or that it is not yet due. */
#define VD_PMU_SESSION_WATCHDOG_RESTORED 1

enum vd_pmu_event_id {
    VD_PMU_EVENT_ICACHE_MISS = 1,
    VD_PMU_EVENT_DCACHE_MISS = 2,
    VD_PMU_EVENT_DCACHE_ACCESS = 3,
    VD_PMU_EVENT_BRANCH_MISPREDICT = 4,
    VD_PMU_EVENT_PREDICTED_BRANCH = 5,
    VD_PMU_EVENT_MAIN_PIPE = 6,
    VD_PMU_EVENT_SECOND_PIPE = 7,
    VD_PMU_EVENT_LOAD_STORE_PIPE = 8,
    VD_PMU_EVENT_FPU_RENAME = 9,
    VD_PMU_EVENT_NEON_RENAME = 10,
    VD_PMU_EVENT_SOFTWARE_INCREMENT = 11,
};

/* Only entries returned by vdPmuSessionLookupEvent can reach configure().
 * physical_counter is assigned by the state machine, not by its caller. */
struct vd_pmu_event_metadata {
    uint32_t event_id;
    uint32_t event_code;
    uint32_t physical_counter;
    uint32_t reserved;
};

struct vd_pmu_session_request {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t lease_ms;
    uint32_t flags;
    uint32_t event_count;
    uint32_t event_ids[VD_PMU_SESSION_MAX_EVENTS];
    uint32_t reserved[3];
};

struct vd_pmu_session_owner {
    int32_t owner_pid;
    uint32_t owner_token;
    uint32_t core_id;
};

/* Complete Cortex-A9 PMU state required by the backend's exact-restore
 * contract. PMCR.N is the exact programmable-counter count (six means lanes
 * 0..5). raw PMCR.P/C must never be replayed because they are reset commands,
 * and PMOVSR is W1C rather than ordinary writable state. The first live gate
 * requires all implemented enable, interrupt, and overflow bits (lanes 0..5
 * plus cycle bit 31) to be clear, and it assigns fixed lane 5. restore() must
 * use safe register semantics and never overwrite unrelated lanes. A following
 * snapshot verifies the complete idle snapshot; newly appearing external PMU
 * activity is a conflict and keeps the restoration obligation pending. */
struct vd_pmu_snapshot {
    uint32_t event_counter_count;
    uint32_t raw_pmcr;
    uint32_t raw_pmcntenset;
    uint32_t raw_pmovsr;
    uint32_t raw_pmselr;
    uint32_t raw_pmccntr;
    uint32_t raw_pmuserenr;
    uint32_t raw_pmintenset;
    uint32_t raw_pmxevtyper[VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS];
    uint32_t raw_pmxevcntr[VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS];
};

struct vd_pmu_configuration {
    uint32_t flags;
    uint32_t event_count;
    uint32_t control_flags;
    uint32_t reserved;
    struct vd_pmu_event_metadata events[VD_PMU_SESSION_MAX_EVENTS];
};

struct vd_pmu_counter_values {
    uint32_t cycles;
    uint32_t events[VD_PMU_SESSION_MAX_EVENTS];
};

struct vd_pmu_reading {
    uint64_t timestamp_ms;
    uint64_t lease_token;
    uint32_t flags;
    uint32_t event_count;
    uint32_t cycles;
    uint32_t reserved;
    struct {
        struct vd_pmu_event_metadata metadata;
        uint32_t value;
        uint32_t reserved;
    } events[VD_PMU_SESSION_MAX_EVENTS];
};

/* snapshot and read are read-only. configure may have partially changed PMU
 * state even when it returns an error, so the engine always attempts restore.
 * Every selector sequence must run on core_id with preemption and local IRQs
 * excluded. restore must exactly restore selected lane 5, PMSELR, and any safe
 * PMCR.E change from the snapshot
 * without replaying destructive commands or unrelated lane state. It must be
 * safe to retry after an ambiguous failure. now_ms must be monotonic. All
 * callbacks return zero on success and a negative error on failure. */
struct vd_pmu_session_backend {
    void* context;
    int (*snapshot)(void* context, uint32_t core_id,
                    struct vd_pmu_snapshot* snapshot);
    int (*configure)(void* context, uint32_t core_id,
                     const struct vd_pmu_configuration* configuration);
    int (*read)(void* context, uint32_t core_id,
                const struct vd_pmu_configuration* configuration,
                struct vd_pmu_counter_values* values);
    int (*restore)(void* context, uint32_t core_id,
                   const struct vd_pmu_configuration* configuration,
                   const struct vd_pmu_snapshot* snapshot);
    int (*now_ms)(void* context, uint64_t* now_ms);
};

enum vd_pmu_session_state {
    VD_PMU_SESSION_EMPTY = 0,
    VD_PMU_SESSION_ACTIVE = 1,
    VD_PMU_SESSION_RESTORE_PENDING = 2,
};

/* Public for static kernel allocation. Treat fields as private after init.
 * The owner of this object must serialize calls. There is deliberately no API
 * that discards a retained restoration obligation. */
struct vd_pmu_session {
    uint32_t initialization_cookie;
    uint64_t next_lease_token;
    uint64_t lease_token;
    uint64_t acquired_ms;
    uint64_t deadline_ms;
    enum vd_pmu_session_state state;
    int32_t last_backend_error;
    struct vd_pmu_session_owner owner;
    struct vd_pmu_configuration configuration;
    struct vd_pmu_snapshot original;
    struct vd_pmu_session_backend backend;
};

/* Call once on zero-initialized storage before publication. Repeating this
 * call is an idempotent no-op, including while restoration is pending, so it
 * cannot be used to discard a live hardware obligation. */
void vdPmuSessionInit(struct vd_pmu_session* session);

int vdPmuSessionLookupEvent(uint32_t event_id,
                            struct vd_pmu_event_metadata* metadata);

int vdPmuSessionAcquire(struct vd_pmu_session* session,
                        const struct vd_pmu_session_owner* owner,
                        const struct vd_pmu_session_request* request,
                        const struct vd_pmu_session_backend* backend,
                        uint64_t* lease_token);

int vdPmuSessionRead(struct vd_pmu_session* session,
                     const struct vd_pmu_session_owner* owner,
                     uint64_t lease_token,
                     struct vd_pmu_reading* reading);

int vdPmuSessionRestore(struct vd_pmu_session* session,
                        const struct vd_pmu_session_owner* owner,
                        uint64_t lease_token);

/* Intended for a bounded kernel watchdog/poll path. A pending restoration is
 * retried immediately; an active lease is restored once its deadline passes. */
int vdPmuSessionWatchdog(struct vd_pmu_session* session);

int vdPmuSessionIsActive(const struct vd_pmu_session* session);

#ifdef __cplusplus
}
#endif

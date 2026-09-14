#ifndef VITAPROFILER_PMU_H
#define VITAPROFILER_PMU_H

#include "vitaprofiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VP_PMU_PROVIDER_ABI_VERSION 2u
#define VP_PMU_MAX_EVENT_COUNTERS 4u
#define VP_PMU_COUNTER_CYCLES (UINT32_C(1) << 0)
#define VP_PMU_COUNTER_EVENT0 (UINT32_C(1) << 1)
#define VP_PMU_COUNTER_EVENT1 (UINT32_C(1) << 2)
#define VP_PMU_COUNTER_EVENT2 (UINT32_C(1) << 3)
#define VP_PMU_COUNTER_EVENT3 (UINT32_C(1) << 4)
#define VP_PMU_COUNTER_ALL (UINT32_C(0x1f))

/* A provider must advertise at least one ownership model. Exact-restore
 * providers preserve an existing PMU configuration. Owned-reset providers
 * destructively initialize a PMU context which the application explicitly
 * grants to the profiler, then stop and reset it on release. */
#define VP_PMU_PROVIDER_FLAG_EXACT_RESTORE (UINT32_C(1) << 0)
#define VP_PMU_PROVIDER_FLAG_OWNED_RESET (UINT32_C(1) << 1)
#define VP_PMU_PROVIDER_FLAG_ALL                                             \
    (VP_PMU_PROVIDER_FLAG_EXACT_RESTORE | VP_PMU_PROVIDER_FLAG_OWNED_RESET)

/* Zero keeps the original exact-restore contract. This opt-in flag is
 * required before a provider may destructively reset application-owned PMU
 * state. */
#define VP_PMU_CONFIG_FLAG_ALLOW_OWNED_RESET (UINT32_C(1) << 0)
#define VP_PMU_CONFIG_FLAG_ALL VP_PMU_CONFIG_FLAG_ALLOW_OWNED_RESET

/* This layer deliberately contains no ARM register access. A platform backend
 * may be supplied only when it can exclusively claim the selected counters
 * and keep reads on the owning execution context. The default contract saves
 * and restores every changed field exactly. An application may instead set
 * ALLOW_OWNED_RESET for a provider that explicitly advertises OWNED_RESET;
 * that grants destructive ownership of the selected thread's complete PMU
 * state for the session rather than pretending inaccessible state is saved. */
struct vp_pmu_config {
    uint32_t counter_mask;
    uint32_t event_count;
    uint32_t event_codes[VP_PMU_MAX_EVENT_COUNTERS];
    /* Unknown flags and nonzero reserved data are rejected. */
    uint32_t flags;
    uint32_t reserved;
};

struct vp_pmu_sample {
    uint64_t cycles;
    uint64_t events[VP_PMU_MAX_EVENT_COUNTERS];
    uint32_t counter_mask;
    uint32_t reserved;
};

/* Map a raw PMU sample into ordinary named profiler counters. IDs should come
 * from the application's sealed name dictionary and must be nonzero for every
 * bit present in the sample. */
struct vp_pmu_name_ids {
    uint32_t cycles;
    uint32_t events[VP_PMU_MAX_EVENT_COUNTERS];
};

typedef int (*vp_pmu_acquire_fn)(void* user,
                                 const struct vp_pmu_config* config,
                                 uint64_t* lease_token);
typedef int (*vp_pmu_read_fn)(void* user, uint64_t lease_token,
                              struct vp_pmu_sample* sample);
typedef int (*vp_pmu_release_fn)(void* user, uint64_t lease_token);

struct vp_pmu_provider {
    uint32_t abi_version;
    uint32_t flags;
    vp_pmu_acquire_fn acquire;
    vp_pmu_read_fn read;
    vp_pmu_release_fn release;
    void* user;
};

/* Public for static allocation; zero-initialize before the first begin and
 * treat fields as private thereafter. The provider object and its user data
 * must outlive an active session. */
struct vp_pmu_session {
    const struct vp_pmu_provider* provider;
    struct vp_pmu_config config;
    uint64_t lease_token;
    int32_t last_provider_error;
    uint32_t active;
    uint32_t restore_pending;
    uint32_t initialized;
};

struct vp_pmu_session_status {
    uint32_t active;
    uint32_t restore_pending;
    uint32_t counter_mask;
    int32_t last_provider_error;
};

/* Passing no provider is the supported fail-closed path on platforms without
 * an audited PMU API. If release/cleanup fails, the session keeps its lease and
 * every operation except another release returns VP_ERROR_RESTORE_REQUIRED. */
int vp_pmu_session_begin(struct vp_pmu_session* session,
                         const struct vp_pmu_provider* provider,
                         const struct vp_pmu_config* config);
int vp_pmu_session_read(struct vp_pmu_session* session,
                        struct vp_pmu_sample* sample);
int vp_pmu_session_end(struct vp_pmu_session* session);
int vp_pmu_session_get_status(const struct vp_pmu_session* session,
                              struct vp_pmu_session_status* status);
int vp_pmu_record_sample(struct vp_context* context,
                         const struct vp_pmu_sample* sample,
                         const struct vp_pmu_name_ids* names);

#ifdef __cplusplus
}
#endif

#endif

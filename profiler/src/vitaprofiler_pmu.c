#include "vitaprofiler_pmu.h"

#include <limits.h>
#include <string.h>

#define VP_PMU_SESSION_MAGIC UINT32_C(0x5650504d)

static int vp_pmu_config_is_valid(const struct vp_pmu_config* config)
{
    uint32_t expected_events;
    uint32_t i;
    if (config == NULL || config->counter_mask == 0u ||
        (config->counter_mask & ~VP_PMU_COUNTER_ALL) != 0u ||
        config->event_count > VP_PMU_MAX_EVENT_COUNTERS ||
        (config->flags & ~VP_PMU_CONFIG_FLAG_ALL) != 0u ||
        config->reserved != 0u)
        return 0;
    expected_events = config->event_count == 0u
                          ? 0u
                          : ((UINT32_C(1) << config->event_count) - 1u) << 1;
    if ((config->counter_mask & ~VP_PMU_COUNTER_CYCLES) != expected_events)
        return 0;
    for (i = config->event_count; i < VP_PMU_MAX_EVENT_COUNTERS; ++i) {
        if (config->event_codes[i] != 0u)
            return 0;
    }
    return 1;
}

static int vp_pmu_session_is_valid(const struct vp_pmu_session* session)
{
    return session != NULL && session->initialized == VP_PMU_SESSION_MAGIC &&
           session->provider != NULL;
}

int vp_pmu_session_begin(struct vp_pmu_session* session,
                         const struct vp_pmu_provider* provider,
                         const struct vp_pmu_config* config)
{
    uint64_t lease_token = 0u;
    int result;
    if (session == NULL || config == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (session->initialized == VP_PMU_SESSION_MAGIC && session->active != 0u)
        return session->restore_pending != 0u ? VP_ERROR_RESTORE_REQUIRED
                                              : VP_ERROR_BUSY;
    if (provider == NULL)
        return VP_ERROR_UNSUPPORTED;
    if (provider->abi_version != VP_PMU_PROVIDER_ABI_VERSION)
        return VP_ERROR_UNSUPPORTED;
    if (provider->flags == 0u ||
        (provider->flags & ~VP_PMU_PROVIDER_FLAG_ALL) != 0u ||
        provider->acquire == NULL || provider->read == NULL ||
        provider->release == NULL || !vp_pmu_config_is_valid(config))
        return VP_ERROR_INVALID_ARGUMENT;
    if ((config->flags & VP_PMU_CONFIG_FLAG_ALLOW_OWNED_RESET) != 0u) {
        if ((provider->flags & VP_PMU_PROVIDER_FLAG_OWNED_RESET) == 0u)
            return VP_ERROR_UNSUPPORTED;
    } else if ((provider->flags & VP_PMU_PROVIDER_FLAG_EXACT_RESTORE) == 0u) {
        return VP_ERROR_UNSUPPORTED;
    }

    result = provider->acquire(provider->user, config, &lease_token);
    if (result != VP_RESULT_OK)
        return result;
    memset(session, 0, sizeof(*session));
    session->provider = provider;
    session->config = *config;
    session->lease_token = lease_token;
    session->active = 1u;
    session->initialized = VP_PMU_SESSION_MAGIC;
    return VP_RESULT_OK;
}

int vp_pmu_session_read(struct vp_pmu_session* session,
                        struct vp_pmu_sample* sample)
{
    int result;
    if (sample != NULL)
        memset(sample, 0, sizeof(*sample));
    if (!vp_pmu_session_is_valid(session))
        return VP_ERROR_NOT_INITIALIZED;
    if (sample == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (session->restore_pending != 0u)
        return VP_ERROR_RESTORE_REQUIRED;
    if (session->active == 0u)
        return VP_ERROR_STATE;
    result = session->provider->read(session->provider->user,
                                     session->lease_token, sample);
    if (result != VP_RESULT_OK) {
        session->last_provider_error = result;
        memset(sample, 0, sizeof(*sample));
        return result;
    }
    if (sample->counter_mask != session->config.counter_mask ||
        sample->reserved != 0u) {
        session->last_provider_error = VP_ERROR_MALFORMED;
        memset(sample, 0, sizeof(*sample));
        return VP_ERROR_MALFORMED;
    }
    session->last_provider_error = VP_RESULT_OK;
    return VP_RESULT_OK;
}

int vp_pmu_session_end(struct vp_pmu_session* session)
{
    int result;
    if (!vp_pmu_session_is_valid(session))
        return VP_ERROR_NOT_INITIALIZED;
    if (session->active == 0u)
        return VP_ERROR_STATE;
    result = session->provider->release(session->provider->user,
                                        session->lease_token);
    if (result != VP_RESULT_OK) {
        session->last_provider_error = result;
        session->restore_pending = 1u;
        return VP_ERROR_RESTORE_REQUIRED;
    }
    session->lease_token = 0u;
    session->last_provider_error = VP_RESULT_OK;
    session->active = 0u;
    session->restore_pending = 0u;
    return VP_RESULT_OK;
}

int vp_pmu_session_get_status(const struct vp_pmu_session* session,
                              struct vp_pmu_session_status* status)
{
    if (!vp_pmu_session_is_valid(session))
        return VP_ERROR_NOT_INITIALIZED;
    if (status == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    status->active = session->active;
    status->restore_pending = session->restore_pending;
    status->counter_mask = session->config.counter_mask;
    status->last_provider_error = session->last_provider_error;
    return VP_RESULT_OK;
}

static int vp_pmu_merge_record_result(int aggregate, int result)
{
    if (result < 0)
        return result;
    if (result == VP_RESULT_DROPPED)
        return VP_RESULT_DROPPED;
    return aggregate;
}

int vp_pmu_record_sample(struct vp_context* context,
                         const struct vp_pmu_sample* sample,
                         const struct vp_pmu_name_ids* names)
{
    struct vp_stats stats;
    uint32_t i;
    int aggregate = VP_RESULT_OK;
    int result;
    if (context == NULL || sample == NULL || names == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (vp_get_stats(context, &stats) != VP_RESULT_OK)
        return VP_ERROR_NOT_INITIALIZED;
    if (sample->counter_mask == 0u ||
        (sample->counter_mask & ~VP_PMU_COUNTER_ALL) != 0u ||
        sample->reserved != 0u)
        return VP_ERROR_MALFORMED;
    if ((sample->counter_mask & VP_PMU_COUNTER_CYCLES) != 0u &&
        (names->cycles == 0u || sample->cycles > (uint64_t)INT64_MAX))
        return VP_ERROR_INVALID_ARGUMENT;
    for (i = 0u; i < VP_PMU_MAX_EVENT_COUNTERS; ++i) {
        uint32_t bit = UINT32_C(1) << (i + 1u);
        if ((sample->counter_mask & bit) != 0u &&
            (names->events[i] == 0u ||
             sample->events[i] > (uint64_t)INT64_MAX))
            return VP_ERROR_INVALID_ARGUMENT;
    }

    if ((sample->counter_mask & VP_PMU_COUNTER_CYCLES) != 0u) {
        result = vp_emit(context, VP_EVENT_COUNTER, VP_EVENT_FLAG_RAW_VALUE,
                         names->cycles, (int64_t)sample->cycles, 0u);
        aggregate = vp_pmu_merge_record_result(aggregate, result);
        if (aggregate < 0)
            return aggregate;
    }
    for (i = 0u; i < VP_PMU_MAX_EVENT_COUNTERS; ++i) {
        uint32_t bit = UINT32_C(1) << (i + 1u);
        if ((sample->counter_mask & bit) != 0u) {
            result = vp_emit(context, VP_EVENT_COUNTER,
                             VP_EVENT_FLAG_RAW_VALUE, names->events[i],
                             (int64_t)sample->events[i], 0u);
            aggregate = vp_pmu_merge_record_result(aggregate, result);
            if (aggregate < 0)
                return aggregate;
        }
    }
    return aggregate;
}

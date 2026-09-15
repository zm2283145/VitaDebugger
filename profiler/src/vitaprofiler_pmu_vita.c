#include "vitaprofiler_pmu_vita.h"

#include <limits.h>
#include <string.h>

#define VP_VITA_PMU_OWNED_MAGIC UINT32_C(0x56504f4d)
#define VP_VITA_PMU_COUNTER_UNUSED UINT32_MAX

static volatile uint32_t vp_vita_pmu_process_lease;
static volatile uint32_t vp_vita_pmu_next_token = 1u;

static int vp_vita_pmu_claim_process_lease(void)
{
    uint32_t expected = 0u;
    return __atomic_compare_exchange_n(&vp_vita_pmu_process_lease, &expected,
                                       1u, 0, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

static void vp_vita_pmu_release_process_lease(void)
{
    __atomic_store_n(&vp_vita_pmu_process_lease, 0u, __ATOMIC_RELEASE);
}

static uint64_t vp_vita_pmu_allocate_token(void)
{
    uint32_t token = __atomic_fetch_add(&vp_vita_pmu_next_token, 1u,
                                        __ATOMIC_RELAXED);
    if (token == 0u)
        token = __atomic_fetch_add(&vp_vita_pmu_next_token, 1u,
                                   __ATOMIC_RELAXED);
    return (UINT64_C(0x56504d55) << 32) | token;
}

static int vp_vita_pmu_record_platform_result(
    struct vp_vita_pmu_owned* owned, enum vp_vita_pmu_operation operation,
    int result)
{
    owned->last_operation = (uint32_t)operation;
    owned->last_platform_error = result;
    return result == 0 ? VP_RESULT_OK : VP_ERROR_PLATFORM;
}

static int vp_vita_pmu_reset(struct vp_vita_pmu_owned* owned)
{
    int result = owned->ops.reset(owned->ops.user, owned->thread_id);
    return vp_vita_pmu_record_platform_result(
        owned, VP_VITA_PMU_OPERATION_RESET, result);
}

static int vp_vita_pmu_stop(struct vp_vita_pmu_owned* owned)
{
    int result = owned->ops.stop(owned->ops.user, owned->thread_id);
    return vp_vita_pmu_record_platform_result(
        owned, VP_VITA_PMU_OPERATION_STOP, result);
}

static int vp_vita_pmu_cleanup(struct vp_vita_pmu_owned* owned)
{
    int stop_result;
    int stop_platform_error;
    int reset_result;

    stop_result = vp_vita_pmu_stop(owned);
    stop_platform_error = owned->last_platform_error;
    reset_result = vp_vita_pmu_reset(owned);
    if (stop_result != VP_RESULT_OK) {
        owned->last_operation = VP_VITA_PMU_OPERATION_STOP;
        owned->last_platform_error = stop_platform_error;
        return VP_ERROR_PLATFORM;
    }
    return reset_result;
}

static int vp_vita_pmu_event_supported(uint32_t event_code)
{
    /*
     * Keep this list in lockstep with the public ScePerf event constants in
     * psp2/perf.h.  Do not pass reserved byte values to the platform API.
     */
    switch (event_code) {
    case 0x00u:
    case 0x01u:
    case 0x02u:
    case 0x03u:
    case 0x04u:
    case 0x05u:
    case 0x06u:
    case 0x07u:
    case 0x09u:
    case 0x0au:
    case 0x0bu:
    case 0x0cu:
    case 0x0du:
    case 0x0fu:
    case 0x10u:
    case 0x11u:
    case 0x12u:
    case 0x50u:
    case 0x51u:
    case 0x60u:
    case 0x61u:
    case 0x62u:
    case 0x63u:
    case 0x64u:
    case 0x65u:
    case 0x66u:
    case 0x67u:
    case 0x68u:
    case 0x6eu:
    case 0x70u:
    case 0x71u:
    case 0x72u:
    case 0x73u:
    case 0x74u:
    case 0x80u:
    case 0x81u:
    case 0x82u:
    case 0x83u:
    case 0x84u:
    case 0x85u:
    case 0x86u:
    case 0x8au:
    case 0x8bu:
    case 0x90u:
    case 0x91u:
    case 0x92u:
    case 0x93u:
    case 0xa0u:
    case 0xa1u:
    case 0xa2u:
    case 0xa3u:
    case 0xa4u:
    case 0xa5u:
        return 1;
    default:
        return 0;
    }
}

static int vp_vita_pmu_config_supported(const struct vp_pmu_config* config)
{
    uint32_t expected_events;
    uint32_t i;
    if (config == NULL || config->counter_mask == 0u ||
        (config->counter_mask & ~VP_PMU_COUNTER_ALL) != 0u ||
        config->event_count > VP_PMU_MAX_EVENT_COUNTERS ||
        config->flags != VP_PMU_CONFIG_FLAG_ALLOW_OWNED_RESET ||
        config->reserved != 0u)
        return 0;
    expected_events = config->event_count == 0u
                          ? 0u
                          : ((UINT32_C(1) << config->event_count) - 1u) << 1;
    if ((config->counter_mask & ~VP_PMU_COUNTER_CYCLES) != expected_events)
        return 0;
    for (i = 0u; i < config->event_count; ++i) {
        if (!vp_vita_pmu_event_supported(config->event_codes[i]))
            return 0;
    }
    for (; i < VP_PMU_MAX_EVENT_COUNTERS; ++i) {
        if (config->event_codes[i] != 0u)
            return 0;
    }
    return 1;
}

static int vp_vita_pmu_select_and_zero(
    struct vp_vita_pmu_owned* owned, uint32_t physical,
    uint32_t event_code)
{
    int result = owned->ops.select_event(
        owned->ops.user, owned->thread_id, physical, (uint8_t)event_code);
    if (vp_vita_pmu_record_platform_result(
            owned, VP_VITA_PMU_OPERATION_SELECT_EVENT, result) !=
        VP_RESULT_OK)
        return VP_ERROR_PLATFORM;
    result = owned->ops.set_counter(owned->ops.user, owned->thread_id,
                                    physical, 0u);
    return vp_vita_pmu_record_platform_result(
        owned, VP_VITA_PMU_OPERATION_SET_COUNTER, result);
}

static int vp_vita_pmu_acquire(void* user,
                               const struct vp_pmu_config* config,
                               uint64_t* lease_token)
{
    struct vp_vita_pmu_owned* owned = (struct vp_vita_pmu_owned*)user;
    uint32_t physical = 0u;
    uint32_t i;
    int result;

    if (owned == NULL || lease_token == NULL ||
        owned->initialized != VP_VITA_PMU_OWNED_MAGIC ||
        !vp_vita_pmu_config_supported(config))
        return VP_ERROR_INVALID_ARGUMENT;
    if (owned->cleanup_pending != 0u)
        return VP_ERROR_RESTORE_REQUIRED;
    if (owned->active != 0u)
        return VP_ERROR_BUSY;
    physical = config->event_count;
    if ((config->counter_mask & VP_PMU_COUNTER_CYCLES) != 0u)
        ++physical;
    if (physical > VP_VITA_PMU_PHYSICAL_COUNTERS_USED)
        return VP_ERROR_INVALID_ARGUMENT;
    if (!vp_vita_pmu_claim_process_lease())
        return VP_ERROR_BUSY;

    physical = 0u;

    owned->cycles_physical = VP_VITA_PMU_COUNTER_UNUSED;
    for (i = 0u; i < VP_PMU_MAX_EVENT_COUNTERS; ++i)
        owned->event_physical[i] = VP_VITA_PMU_COUNTER_UNUSED;

    result = vp_vita_pmu_reset(owned);
    if (result != VP_RESULT_OK)
        goto acquire_failed;

    if ((config->counter_mask & VP_PMU_COUNTER_CYCLES) != 0u) {
        owned->cycles_physical = physical++;
        result = vp_vita_pmu_select_and_zero(
            owned, owned->cycles_physical, VP_VITA_PMU_EVENT_CYCLE_COUNT);
        if (result != VP_RESULT_OK)
            goto acquire_failed;
    }
    for (i = 0u; i < config->event_count; ++i) {
        owned->event_physical[i] = physical++;
        result = vp_vita_pmu_select_and_zero(
            owned, owned->event_physical[i], config->event_codes[i]);
        if (result != VP_RESULT_OK)
            goto acquire_failed;
    }
    result = owned->ops.start(owned->ops.user, owned->thread_id);
    if (vp_vita_pmu_record_platform_result(
            owned, VP_VITA_PMU_OPERATION_START, result) != VP_RESULT_OK) {
        result = VP_ERROR_PLATFORM;
        goto acquire_failed;
    }

    owned->active_token = vp_vita_pmu_allocate_token();
    owned->active = 1u;
    owned->cleanup_pending = 0u;
    *lease_token = owned->active_token;
    return VP_RESULT_OK;

acquire_failed:
    {
        uint32_t failed_operation = owned->last_operation;
        int32_t failed_platform_error = owned->last_platform_error;
        if (vp_vita_pmu_cleanup(owned) != VP_RESULT_OK) {
            owned->cleanup_pending = 1u;
            return VP_ERROR_RESTORE_REQUIRED;
        }
        owned->last_operation = failed_operation;
        owned->last_platform_error = failed_platform_error;
    }
    owned->cleanup_pending = 0u;
    vp_vita_pmu_release_process_lease();
    return result;
}

static int vp_vita_pmu_read(void* user, uint64_t lease_token,
                            struct vp_pmu_sample* sample)
{
    struct vp_vita_pmu_owned* owned = (struct vp_vita_pmu_owned*)user;
    uint32_t i;
    uint32_t value;
    int result;

    if (owned == NULL || sample == NULL ||
        owned->initialized != VP_VITA_PMU_OWNED_MAGIC)
        return VP_ERROR_INVALID_ARGUMENT;
    if (owned->cleanup_pending != 0u)
        return VP_ERROR_RESTORE_REQUIRED;
    if (owned->active == 0u || lease_token == 0u ||
        lease_token != owned->active_token)
        return VP_ERROR_STATE;

    memset(sample, 0, sizeof(*sample));
    if (owned->cycles_physical != VP_VITA_PMU_COUNTER_UNUSED) {
        result = owned->ops.get_counter(owned->ops.user, owned->thread_id,
                                        owned->cycles_physical, &value);
        if (vp_vita_pmu_record_platform_result(
                owned, VP_VITA_PMU_OPERATION_GET_COUNTER, result) !=
            VP_RESULT_OK)
            return VP_ERROR_PLATFORM;
        sample->cycles = value;
        sample->counter_mask |= VP_PMU_COUNTER_CYCLES;
    }
    for (i = 0u; i < VP_PMU_MAX_EVENT_COUNTERS; ++i) {
        if (owned->event_physical[i] == VP_VITA_PMU_COUNTER_UNUSED)
            continue;
        result = owned->ops.get_counter(owned->ops.user, owned->thread_id,
                                        owned->event_physical[i], &value);
        if (vp_vita_pmu_record_platform_result(
                owned, VP_VITA_PMU_OPERATION_GET_COUNTER, result) !=
            VP_RESULT_OK)
            return VP_ERROR_PLATFORM;
        sample->events[i] = value;
        sample->counter_mask |= UINT32_C(1) << (i + 1u);
    }
    owned->last_operation = VP_VITA_PMU_OPERATION_NONE;
    owned->last_platform_error = 0;
    return VP_RESULT_OK;
}

static int vp_vita_pmu_release(void* user, uint64_t lease_token)
{
    struct vp_vita_pmu_owned* owned = (struct vp_vita_pmu_owned*)user;
    if (owned == NULL || owned->initialized != VP_VITA_PMU_OWNED_MAGIC)
        return VP_ERROR_INVALID_ARGUMENT;
    if (owned->active == 0u || lease_token == 0u ||
        lease_token != owned->active_token)
        return VP_ERROR_STATE;
    if (vp_vita_pmu_cleanup(owned) != VP_RESULT_OK) {
        owned->cleanup_pending = 1u;
        return VP_ERROR_PLATFORM;
    }
    owned->active = 0u;
    owned->cleanup_pending = 0u;
    owned->active_token = 0u;
    owned->last_operation = VP_VITA_PMU_OPERATION_NONE;
    owned->last_platform_error = 0;
    vp_vita_pmu_release_process_lease();
    return VP_RESULT_OK;
}

static int vp_vita_pmu_ops_valid(const struct vp_vita_pmu_ops* ops)
{
    return ops != NULL && ops->reset != NULL && ops->select_event != NULL &&
           ops->start != NULL && ops->stop != NULL &&
           ops->get_counter != NULL && ops->set_counter != NULL;
}

int vp_vita_pmu_owned_init_with_ops(
    struct vp_vita_pmu_owned* owned, const struct vp_vita_pmu_ops* ops,
    int32_t thread_id, uint32_t ownership_ack)
{
    int32_t resolved_thread_id = thread_id;
    if (owned == NULL || !vp_vita_pmu_ops_valid(ops) ||
        ownership_ack != VP_VITA_PMU_APPLICATION_OWNERSHIP_ACK)
        return VP_ERROR_INVALID_ARGUMENT;
    if (owned->initialized == VP_VITA_PMU_OWNED_MAGIC &&
        (owned->active != 0u || owned->cleanup_pending != 0u))
        return owned->cleanup_pending != 0u ? VP_ERROR_RESTORE_REQUIRED
                                            : VP_ERROR_BUSY;
    if (resolved_thread_id == 0) {
        if (ops->get_thread_id == NULL)
            return VP_ERROR_INVALID_ARGUMENT;
        resolved_thread_id = ops->get_thread_id(ops->user);
        if (resolved_thread_id <= 0)
            return VP_ERROR_PLATFORM;
    }
    if (resolved_thread_id < 0)
        return VP_ERROR_INVALID_ARGUMENT;

    memset(owned, 0, sizeof(*owned));
    owned->ops = *ops;
    owned->thread_id = resolved_thread_id;
    owned->cycles_physical = VP_VITA_PMU_COUNTER_UNUSED;
    owned->provider.abi_version = VP_PMU_PROVIDER_ABI_VERSION;
    owned->provider.flags = VP_PMU_PROVIDER_FLAG_OWNED_RESET;
    owned->provider.acquire = vp_vita_pmu_acquire;
    owned->provider.read = vp_vita_pmu_read;
    owned->provider.release = vp_vita_pmu_release;
    owned->provider.user = owned;
    owned->initialized = VP_VITA_PMU_OWNED_MAGIC;
    return VP_RESULT_OK;
}

const struct vp_pmu_provider* vp_vita_pmu_owned_get_provider(
    const struct vp_vita_pmu_owned* owned)
{
    if (owned == NULL || owned->initialized != VP_VITA_PMU_OWNED_MAGIC)
        return NULL;
    return &owned->provider;
}

int vp_vita_pmu_owned_get_status(
    const struct vp_vita_pmu_owned* owned,
    struct vp_vita_pmu_owned_status* status)
{
    if (owned == NULL || owned->initialized != VP_VITA_PMU_OWNED_MAGIC)
        return VP_ERROR_NOT_INITIALIZED;
    if (status == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    status->thread_id = owned->thread_id;
    status->last_platform_error = owned->last_platform_error;
    status->active = owned->active;
    status->cleanup_pending = owned->cleanup_pending;
    status->last_operation = owned->last_operation;
    return VP_RESULT_OK;
}

int vp_vita_pmu_owned_retry_orphan_cleanup(
    struct vp_vita_pmu_owned* owned)
{
    if (owned == NULL || owned->initialized != VP_VITA_PMU_OWNED_MAGIC)
        return VP_ERROR_NOT_INITIALIZED;
    if (owned->active != 0u)
        return VP_ERROR_BUSY;
    if (owned->cleanup_pending == 0u)
        return VP_ERROR_STATE;
    if (vp_vita_pmu_cleanup(owned) != VP_RESULT_OK)
        return VP_ERROR_RESTORE_REQUIRED;
    owned->cleanup_pending = 0u;
    owned->last_operation = VP_VITA_PMU_OPERATION_NONE;
    owned->last_platform_error = 0;
    vp_vita_pmu_release_process_lease();
    return VP_RESULT_OK;
}

#if defined(__vita__)
int vp_vita_pmu_owned_init(struct vp_vita_pmu_owned* owned,
                           int32_t thread_id, uint32_t ownership_ack)
{
    (void)owned;
    (void)thread_id;
    (void)ownership_ack;

    /*
     * A nonzero Vita import stub is not proof that its target is callable.
     * Retail 3.65 can leave an unresolved ScePerf stub in a nonzero loader
     * state which still branches to address zero.  Keep this convenience path
     * disabled until a loader-supported binding proof exists.  Applications
     * with a verified resolver may use vp_vita_pmu_owned_init_with_ops().
     */
    return VP_ERROR_UNSUPPORTED;
}
#endif

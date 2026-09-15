#include "pmu_profiler_bridge.h"

#include <stddef.h>

#if defined(VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_SESSION)

/* The production backend is one global PMU engine.  Retain this linked-copy
 * lease through the session layer's post-restore verification so a second
 * bridge cannot acquire in the callback-to-verification window. */
static volatile uint32_t g_bridge_provider_lease;

struct bridge_event_definition
{
    uint32_t event_id;
    uint32_t event_code;
    const char* stable_name;
    uint32_t requires_real_event_opt_in;
};

/* Keep this list deliberately smaller than pmu_session's defensive parser
 * allowlist.  Only these stable names may be published through the bridge. */
static const struct bridge_event_definition g_bridge_events[] = {
    {VD_PMU_EVENT_SOFTWARE_INCREMENT, UINT32_C(0x00),
     VD_PMU_PROFILER_NAME_SOFTWARE_INCREMENT, 0},
    {VD_PMU_EVENT_ICACHE_MISS, UINT32_C(0x01),
     VD_PMU_PROFILER_NAME_ICACHE_MISS, 1},
    {VD_PMU_EVENT_DCACHE_MISS, UINT32_C(0x03),
     VD_PMU_PROFILER_NAME_DCACHE_MISS, 1},
    {VD_PMU_EVENT_BRANCH_MISPREDICT, UINT32_C(0x10),
     VD_PMU_PROFILER_NAME_BRANCH_MISPREDICT, 1},
};

static int claim_provider_lease(
    struct vd_pmu_profiler_bridge* bridge)
{
    uint32_t expected = 0;
    if(!__atomic_compare_exchange_n(&g_bridge_provider_lease, &expected,
                                    1u, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        return 0;
    bridge->provider_lease_held = 1;
    return 1;
}

static void release_provider_lease(
    struct vd_pmu_profiler_bridge* bridge)
{
    if(bridge->provider_lease_held == 0)
        return;
    bridge->provider_lease_held = 0;
    __atomic_store_n(&g_bridge_provider_lease, 0u, __ATOMIC_RELEASE);
}

static void zero_bytes(void* value, size_t size)
{
    volatile unsigned char* bytes = (volatile unsigned char*)value;
    for(size_t i = 0; i < size; ++i)
        bytes[i] = 0;
}

static void copy_bytes(void* destination, const void* source, size_t size)
{
    volatile unsigned char* output =
        (volatile unsigned char*)destination;
    const volatile unsigned char* input =
        (const volatile unsigned char*)source;
    for(size_t i = 0; i < size; ++i)
        output[i] = input[i];
}

static int bridge_initialized(
    const struct vd_pmu_profiler_bridge* bridge)
{
    return bridge &&
        bridge->initialization_cookie ==
            VD_PMU_PROFILER_BRIDGE_INITIALIZATION_COOKIE;
}

static int map_kernel_result(int result)
{
    switch(result)
    {
        case 0:
            return VP_RESULT_OK;
        case VD_PMU_SESSION_ERROR_INVALID:
            return VP_ERROR_INVALID_ARGUMENT;
        case VD_PMU_SESSION_ERROR_BUSY:
        case VD_PMU_SESSION_ERROR_NOT_IDLE:
        case VD_PMU_BACKEND_ERROR_BUSY:
        case VD_PMU_BACKEND_ERROR_NOT_IDLE:
        case VD_PMU_BACKEND_ERROR_CONFLICT:
            return VP_ERROR_BUSY;
        case VD_PMU_SESSION_ERROR_UNSUPPORTED_EVENT:
            return VP_ERROR_UNSUPPORTED;
        case VD_PMU_SESSION_ERROR_RESTORE_REQUIRED:
            return VP_ERROR_RESTORE_REQUIRED;
        case VD_PMU_SESSION_ERROR_OWNER:
        case VD_PMU_SESSION_ERROR_STATE:
            return VP_ERROR_STATE;
        default:
            return VP_ERROR_PLATFORM;
    }
}

static int record_result(struct vd_pmu_profiler_bridge* bridge,
                         enum vd_pmu_profiler_bridge_operation operation,
                         int kernel_result)
{
    const int provider_result = map_kernel_result(kernel_result);
    bridge->last_operation = (uint32_t)operation;
    bridge->last_kernel_error = kernel_result;
    bridge->last_provider_result = provider_result;
    return provider_result;
}

static int config_valid(
    const struct vd_pmu_profiler_bridge_config* config)
{
    return config && config->struct_size == sizeof(*config) &&
        config->abi_version == VD_PMU_PROFILER_BRIDGE_ABI_VERSION &&
        config->owner_pid >= 0 && config->owner_token != 0 &&
        config->core_id < VD_PMU_BACKEND_APP_CORE_COUNT &&
        config->lease_ms >= VD_PMU_SESSION_MIN_LEASE_MS &&
        config->lease_ms <= VD_PMU_SESSION_MAX_LEASE_MS &&
        (config->flags &
         ~VD_PMU_PROFILER_BRIDGE_ALLOWED_CONFIG_FLAGS) == 0 &&
        config->reserved == 0;
}

static int lookup_allowlisted_event(
    const struct vd_pmu_profiler_bridge* bridge,
    uint32_t event_code, uint32_t* event_id,
    const char** stable_name)
{
    if(!bridge_initialized(bridge) || !event_id)
        return VD_PMU_SESSION_ERROR_INVALID;
    for(size_t i = 0;
        i < sizeof(g_bridge_events) / sizeof(g_bridge_events[0]); ++i)
    {
        const struct bridge_event_definition* definition =
            &g_bridge_events[i];
        if(definition->event_code != event_code)
            continue;
        if(definition->requires_real_event_opt_in != 0)
        {
#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED
            if((bridge->config_flags &
                VD_PMU_PROFILER_BRIDGE_CONFIG_ALLOW_REAL_EVENTS) == 0)
                return VD_PMU_SESSION_ERROR_UNSUPPORTED_EVENT;
#else
            return VD_PMU_SESSION_ERROR_UNSUPPORTED_EVENT;
#endif
        }

        /* Revalidate the bridge's narrower table against the session layer's
         * canonical ID/code mapping before any request can reach configure. */
        struct vd_pmu_event_metadata metadata;
        zero_bytes(&metadata, sizeof(metadata));
        if(vdPmuSessionLookupEvent(definition->event_id, &metadata) != 0 ||
           metadata.event_id != definition->event_id ||
           metadata.event_code != definition->event_code ||
           metadata.physical_counter != 0 || metadata.reserved != 0)
            return VD_PMU_SESSION_ERROR_UNSUPPORTED_EVENT;
        *event_id = definition->event_id;
        if(stable_name)
            *stable_name = definition->stable_name;
        return 0;
    }
    return VD_PMU_SESSION_ERROR_UNSUPPORTED_EVENT;
}

static int provider_config_event(
    const struct vd_pmu_profiler_bridge* bridge,
    const struct vp_pmu_config* config, uint32_t* event_id)
{
    if(!config || !event_id)
        return VD_PMU_SESSION_ERROR_INVALID;
    if((config->counter_mask & VP_PMU_COUNTER_CYCLES) != 0 ||
       config->counter_mask != VD_PMU_PROFILER_BRIDGE_COUNTER_MASK ||
       config->event_count != VD_PMU_PROFILER_BRIDGE_MAX_EVENTS)
        return VD_PMU_SESSION_ERROR_UNSUPPORTED_EVENT;
    if(config->flags != 0 || config->reserved != 0)
        return VD_PMU_SESSION_ERROR_INVALID;
    for(uint32_t i = 1; i < VP_PMU_MAX_EVENT_COUNTERS; ++i)
        if(config->event_codes[i] != 0)
            return VD_PMU_SESSION_ERROR_INVALID;
    return lookup_allowlisted_event(bridge, config->event_codes[0],
                                    event_id, NULL);
}

static void remember_automatic_restore(
    struct vd_pmu_profiler_bridge* bridge, uint64_t token)
{
    bridge->active_token = 0;
    bridge->active_event_id = 0;
    bridge->active_event_code = 0;
    if(bridge->orphan_restore_pending != 0)
    {
        bridge->orphan_restore_pending = 0;
        bridge->auto_restored_token = 0;
        release_provider_lease(bridge);
    }
    else
    {
        bridge->auto_restored_token = token;
    }
}

static int bridge_acquire(void* user,
                          const struct vp_pmu_config* config,
                          uint64_t* lease_token)
{
    struct vd_pmu_profiler_bridge* bridge =
        (struct vd_pmu_profiler_bridge*)user;
    uint32_t event_id = 0;
    uint64_t kernel_token = 0;
    if(lease_token)
        *lease_token = 0;
    if(!bridge_initialized(bridge) || !lease_token)
        return VP_ERROR_INVALID_ARGUMENT;
    if(vdPmuSessionIsActive(&bridge->session) ||
       bridge->auto_restored_token != 0)
        return record_result(bridge,
                             VD_PMU_PROFILER_BRIDGE_OPERATION_ACQUIRE,
                             VD_PMU_SESSION_ERROR_BUSY);

    int result = provider_config_event(bridge, config, &event_id);
    if(result < 0)
        return record_result(bridge,
                             VD_PMU_PROFILER_BRIDGE_OPERATION_ACQUIRE,
                             result);

    if(!claim_provider_lease(bridge))
        return record_result(bridge,
                             VD_PMU_PROFILER_BRIDGE_OPERATION_ACQUIRE,
                             VD_PMU_SESSION_ERROR_BUSY);

    struct vd_pmu_session_request request;
    zero_bytes(&request, sizeof(request));
    request.struct_size = sizeof(request);
    request.abi_version = VD_PMU_SESSION_ABI_VERSION;
    request.lease_ms = bridge->lease_ms;
    request.event_count = 1;
    request.event_ids[0] = event_id;

    result = vdPmuSessionAcquire(&bridge->session, &bridge->owner,
                                 &request, &bridge->backend,
                                 &kernel_token);
    if(result < 0)
    {
        if(vdPmuSessionIsActive(&bridge->session))
        {
            bridge->active_token = kernel_token;
            bridge->active_event_id = event_id;
            bridge->active_event_code = config->event_codes[0];
            bridge->orphan_restore_pending = 1;
        }
        else
        {
            release_provider_lease(bridge);
        }
        return record_result(bridge,
                             VD_PMU_PROFILER_BRIDGE_OPERATION_ACQUIRE,
                             result);
    }

    bridge->active_token = kernel_token;
    bridge->active_event_id = event_id;
    bridge->active_event_code = config->event_codes[0];
    bridge->orphan_restore_pending = 0;
    bridge->auto_restored_token = 0;
    *lease_token = kernel_token;
    return record_result(bridge,
                         VD_PMU_PROFILER_BRIDGE_OPERATION_ACQUIRE, 0);
}

static int bridge_read(void* user, uint64_t lease_token,
                       struct vp_pmu_sample* sample)
{
    struct vd_pmu_profiler_bridge* bridge =
        (struct vd_pmu_profiler_bridge*)user;
    struct vd_pmu_reading reading;
    if(sample)
        zero_bytes(sample, sizeof(*sample));
    if(!bridge_initialized(bridge) || !sample || lease_token == 0)
        return VP_ERROR_INVALID_ARGUMENT;
    if(lease_token == bridge->auto_restored_token)
        return record_result(bridge,
                             VD_PMU_PROFILER_BRIDGE_OPERATION_READ,
                             VD_PMU_SESSION_ERROR_STATE);
    if(lease_token != bridge->active_token ||
       bridge->orphan_restore_pending != 0)
        return record_result(bridge,
                             VD_PMU_PROFILER_BRIDGE_OPERATION_READ,
                             VD_PMU_SESSION_ERROR_OWNER);

    zero_bytes(&reading, sizeof(reading));
    int result = vdPmuSessionRead(&bridge->session, &bridge->owner,
                                  lease_token, &reading);
    if(result < 0)
    {
        if(!vdPmuSessionIsActive(&bridge->session))
            remember_automatic_restore(bridge, lease_token);
        return record_result(bridge,
                             VD_PMU_PROFILER_BRIDGE_OPERATION_READ,
                             result);
    }

    if(reading.lease_token != lease_token || reading.flags != 0 ||
       reading.event_count != 1 || reading.events[0].reserved != 0 ||
       reading.events[0].metadata.event_id != bridge->active_event_id ||
       reading.events[0].metadata.event_code != bridge->active_event_code ||
       reading.events[0].metadata.physical_counter !=
           VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS - 1u ||
       reading.events[0].metadata.reserved != 0)
    {
        result = vdPmuSessionRestore(&bridge->session, &bridge->owner,
                                     lease_token);
        if(result == 0)
        {
            remember_automatic_restore(bridge, lease_token);
            bridge->last_operation = VD_PMU_PROFILER_BRIDGE_OPERATION_READ;
            bridge->last_kernel_error = 0;
            bridge->last_provider_result = VP_ERROR_MALFORMED;
            return VP_ERROR_MALFORMED;
        }
        return record_result(bridge,
                             VD_PMU_PROFILER_BRIDGE_OPERATION_READ,
                             result);
    }

    sample->events[0] = reading.events[0].value;
    sample->counter_mask = VD_PMU_PROFILER_BRIDGE_COUNTER_MASK;
    return record_result(bridge,
                         VD_PMU_PROFILER_BRIDGE_OPERATION_READ, 0);
}

static int bridge_release(void* user, uint64_t lease_token)
{
    struct vd_pmu_profiler_bridge* bridge =
        (struct vd_pmu_profiler_bridge*)user;
    if(!bridge_initialized(bridge) || lease_token == 0)
        return VP_ERROR_INVALID_ARGUMENT;
    if(lease_token == bridge->auto_restored_token &&
       bridge->active_token == 0)
    {
        bridge->auto_restored_token = 0;
        release_provider_lease(bridge);
        return record_result(bridge,
                             VD_PMU_PROFILER_BRIDGE_OPERATION_RELEASE, 0);
    }
    if(lease_token != bridge->active_token ||
       bridge->orphan_restore_pending != 0)
        return record_result(bridge,
                             VD_PMU_PROFILER_BRIDGE_OPERATION_RELEASE,
                             VD_PMU_SESSION_ERROR_OWNER);

    int result = vdPmuSessionRestore(&bridge->session, &bridge->owner,
                                     lease_token);
    if(result == 0)
    {
        bridge->active_token = 0;
        bridge->active_event_id = 0;
        bridge->active_event_code = 0;
        release_provider_lease(bridge);
    }
    return record_result(bridge,
                         VD_PMU_PROFILER_BRIDGE_OPERATION_RELEASE,
                         result);
}

int vdPmuProfilerBridgeInit(
    struct vd_pmu_profiler_bridge* bridge,
    const struct vd_pmu_profiler_bridge_config* config)
{
    if(!bridge || !config_valid(config))
        return VP_ERROR_INVALID_ARGUMENT;
    if(bridge_initialized(bridge))
    {
        if(bridge->session.state == VD_PMU_SESSION_RESTORE_PENDING ||
           bridge->orphan_restore_pending != 0 ||
           bridge->auto_restored_token != 0)
            return VP_ERROR_RESTORE_REQUIRED;
        if(vdPmuSessionIsActive(&bridge->session))
            return VP_ERROR_BUSY;
        if(bridge->provider_lease_held != 0)
            return VP_ERROR_RESTORE_REQUIRED;
    }

    struct vd_pmu_session_backend backend;
    zero_bytes(&backend, sizeof(backend));
    int result = vdPmuBackendMakeSessionBackend(&backend);
    if(result != 0)
        return VP_ERROR_PLATFORM;

    struct vd_pmu_profiler_bridge local;
    zero_bytes(&local, sizeof(local));
    local.initialization_cookie =
        VD_PMU_PROFILER_BRIDGE_INITIALIZATION_COOKIE;
    local.owner.owner_pid = config->owner_pid;
    local.owner.owner_token = config->owner_token;
    local.owner.core_id = config->core_id;
    local.lease_ms = config->lease_ms;
    local.config_flags = config->flags;
    copy_bytes(&local.backend, &backend, sizeof(local.backend));
    vdPmuSessionInit(&local.session);
    local.provider.abi_version = VP_PMU_PROVIDER_ABI_VERSION;
    local.provider.flags = VP_PMU_PROVIDER_FLAG_EXACT_RESTORE;
    local.provider.acquire = bridge_acquire;
    local.provider.read = bridge_read;
    local.provider.release = bridge_release;
    local.provider.user = bridge;
    copy_bytes(bridge, &local, sizeof(*bridge));
    return VP_RESULT_OK;
}

const struct vp_pmu_provider* vdPmuProfilerBridgeGetProvider(
    const struct vd_pmu_profiler_bridge* bridge)
{
    return bridge_initialized(bridge) ? &bridge->provider : NULL;
}

#if !defined(VD_PMU_PROFILER_BRIDGE_NO_DICTIONARY)
int vdPmuProfilerBridgePrepareEvent(
    const struct vd_pmu_profiler_bridge* bridge,
    struct vp_name_dictionary* dictionary,
    uint32_t event_code,
    struct vp_pmu_config* config,
    struct vp_pmu_name_ids* names)
{
    uint32_t event_id = 0;
    uint32_t name_id = 0;
    const char* stable_name = NULL;
    struct vp_pmu_config local_config;
    struct vp_pmu_name_ids local_names;

    if(config)
        zero_bytes(config, sizeof(*config));
    if(names)
        zero_bytes(names, sizeof(*names));
    if(!bridge_initialized(bridge))
        return VP_ERROR_NOT_INITIALIZED;
    if(!dictionary || !config || !names)
        return VP_ERROR_INVALID_ARGUMENT;
    int result = lookup_allowlisted_event(bridge, event_code, &event_id,
                                          &stable_name);
    if(result != 0)
        return map_kernel_result(result);
    if(event_id == 0 || !stable_name)
        return VP_ERROR_MALFORMED;
    result = vp_name_dictionary_register(dictionary, stable_name, &name_id);
    if(result != VP_RESULT_OK)
        return result;

    zero_bytes(&local_config, sizeof(local_config));
    zero_bytes(&local_names, sizeof(local_names));
    local_config.counter_mask = VD_PMU_PROFILER_BRIDGE_COUNTER_MASK;
    local_config.event_count = VD_PMU_PROFILER_BRIDGE_MAX_EVENTS;
    local_config.event_codes[0] = event_code;
    local_names.events[0] = name_id;
    copy_bytes(config, &local_config, sizeof(*config));
    copy_bytes(names, &local_names, sizeof(*names));
    return VP_RESULT_OK;
}
#endif

int vdPmuProfilerBridgeGetStatus(
    const struct vd_pmu_profiler_bridge* bridge,
    struct vd_pmu_profiler_bridge_status* status)
{
    if(!bridge_initialized(bridge))
        return VP_ERROR_NOT_INITIALIZED;
    if(!status)
        return VP_ERROR_INVALID_ARGUMENT;
    zero_bytes(status, sizeof(*status));
    status->struct_size = sizeof(*status);
    status->abi_version = VD_PMU_PROFILER_BRIDGE_ABI_VERSION;
    status->initialized = 1;
    status->session_state = (uint32_t)bridge->session.state;
    status->orphan_restore_pending = bridge->orphan_restore_pending;
    status->auto_restored_waiting_release =
        bridge->auto_restored_token != 0;
    status->last_operation = bridge->last_operation;
    status->last_kernel_error = bridge->last_kernel_error;
    status->last_provider_result = bridge->last_provider_result;
    status->active_event_id = bridge->active_event_id;
    status->active_event_code = bridge->active_event_code;
    status->lease_ms = bridge->lease_ms;
    status->provider_lease_held = bridge->provider_lease_held;
    status->config_flags = bridge->config_flags;
    return VP_RESULT_OK;
}

int vdPmuProfilerBridgeWatchdog(
    struct vd_pmu_profiler_bridge* bridge)
{
    if(!bridge_initialized(bridge))
        return VP_ERROR_NOT_INITIALIZED;
    const uint64_t token = bridge->active_token;
    int result = vdPmuSessionWatchdog(&bridge->session);
    bridge->last_operation = VD_PMU_PROFILER_BRIDGE_OPERATION_WATCHDOG;
    bridge->last_kernel_error = result < 0 ? result : 0;
    if(result == VD_PMU_SESSION_WATCHDOG_RESTORED)
    {
        remember_automatic_restore(bridge, token);
        bridge->last_provider_result = VP_RESULT_OK;
        return VD_PMU_SESSION_WATCHDOG_RESTORED;
    }
    bridge->last_provider_result = map_kernel_result(result);
    return bridge->last_provider_result;
}

int vdPmuProfilerBridgeRetryOrphanRestore(
    struct vd_pmu_profiler_bridge* bridge)
{
    if(!bridge_initialized(bridge))
        return VP_ERROR_NOT_INITIALIZED;
    if(bridge->orphan_restore_pending == 0 ||
       bridge->active_token == 0)
        return VP_ERROR_STATE;
    int result = vdPmuSessionRestore(&bridge->session, &bridge->owner,
                                     bridge->active_token);
    if(result == 0)
    {
        bridge->active_token = 0;
        bridge->active_event_id = 0;
        bridge->active_event_code = 0;
        bridge->orphan_restore_pending = 0;
        release_provider_lease(bridge);
    }
    return record_result(
        bridge, VD_PMU_PROFILER_BRIDGE_OPERATION_ORPHAN_RESTORE, result);
}

#endif

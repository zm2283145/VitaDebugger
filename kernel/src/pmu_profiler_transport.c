#include "pmu_profiler_transport.h"

#include <stddef.h>

#if defined(VD_KERNEL_ENABLE_EXPERIMENTAL_PMU_PROFILER_TRANSPORT)

static void zero_bytes(void* value, size_t size)
{
    volatile unsigned char* bytes = (volatile unsigned char*)value;
    for(size_t i = 0; i < size; ++i)
        bytes[i] = 0;
}

static int transport_initialized(
    const struct vd_pmu_profiler_transport* transport)
{
    return transport &&
        transport->initialization_cookie ==
            VD_PMU_PROFILER_TRANSPORT_INITIALIZATION_COOKIE;
}

static int is_real_event(uint32_t event_code)
{
    return event_code == VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS ||
        event_code == VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS ||
        event_code == VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT;
}

static int event_allowed(uint32_t event_code)
{
    if(event_code ==
       VD_KERNEL_PMU_PROFILER_EVENT_SOFTWARE_INCREMENT)
        return 1;
#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED
    return is_real_event(event_code);
#else
    (void)event_code;
    return 0;
#endif
}

static uint32_t take_nonzero(uint32_t* next)
{
    uint32_t value = *next;
    if(value == 0)
        value = 1;
    *next = value + 1u;
    if(*next == 0)
        *next = 1;
    return value;
}

static void clear_session(struct vd_pmu_profiler_transport* transport)
{
    transport->state = VD_PMU_PROFILER_TRANSPORT_IDLE;
    transport->owner_pid = -1;
    transport->owner_thread = -1;
    transport->owner_token = 0;
    transport->generation = 0;
    transport->event_code = 0;
    transport->lease_ms = 0;
    transport->lease_token = 0;
    zero_bytes(&transport->bridge, sizeof(transport->bridge));
}

static int info_query_valid(
    const struct vd_kernel_pmu_profiler_info* info)
{
    if(!info || info->struct_size != sizeof(*info) ||
       info->abi_version != VD_KERNEL_PMU_PROFILER_ABI_VERSION ||
       info->capabilities != 0 || info->fixed_core != 0 ||
       info->fixed_counter != 0 || info->min_lease_ms != 0 ||
       info->max_lease_ms != 0 || info->event_count != 0)
        return 0;
    for(uint32_t i = 0; i < VD_KERNEL_PMU_PROFILER_MAX_EVENTS; ++i)
        if(info->event_codes[i] != 0 || info->reserved[i] != 0)
            return 0;
    return 1;
}

static int open_request_valid(
    const struct vd_kernel_pmu_profiler_open_request* request)
{
    if(!request || request->struct_size != sizeof(*request) ||
       request->abi_version != VD_KERNEL_PMU_PROFILER_ABI_VERSION ||
       request->lease_ms < VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS ||
       request->lease_ms > VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS ||
       (request->flags &
        ~VD_KERNEL_PMU_PROFILER_OPEN_ALLOWED_FLAGS) != 0)
        return 0;
    for(uint32_t i = 0; i < 3u; ++i)
        if(request->reserved[i] != 0)
            return 0;
    if(is_real_event(request->event_code))
        return request->flags ==
            VD_KERNEL_PMU_PROFILER_OPEN_ACK_REAL_EVENT;
    return request->flags == 0;
}

static uint64_t handle_lease_token(
    const struct vd_kernel_pmu_profiler_handle* handle)
{
    return (uint64_t)handle->lease_token_low |
        ((uint64_t)handle->lease_token_high << 32);
}

static int handle_valid(
    const struct vd_pmu_profiler_transport* transport,
    int32_t caller_pid,
    int32_t caller_thread,
    const struct vd_kernel_pmu_profiler_handle* handle)
{
    if(!handle || handle->struct_size != sizeof(*handle) ||
       handle->abi_version != VD_KERNEL_PMU_PROFILER_ABI_VERSION ||
       handle->owner_token == 0 || handle->generation == 0 ||
       (handle->lease_token_low == 0 &&
        handle->lease_token_high == 0) ||
       handle->reserved[0] != 0 || handle->reserved[1] != 0)
        return 0;
    return transport->state != VD_PMU_PROFILER_TRANSPORT_IDLE &&
        caller_pid == transport->owner_pid &&
        caller_thread == transport->owner_thread &&
        handle->owner_token == transport->owner_token &&
        handle->generation == transport->generation &&
        handle->event_code == transport->event_code &&
        handle->lease_ms == transport->lease_ms &&
        handle_lease_token(handle) == transport->lease_token;
}

static int map_bridge_result(
    int provider_result,
    const struct vd_pmu_profiler_bridge_status* status)
{
    if(provider_result == VP_RESULT_OK)
        return 0;
    if(status)
    {
        switch(status->last_kernel_error)
        {
            case VD_PMU_SESSION_ERROR_EXPIRED:
                return VD_KERNEL_ERROR_PMU_PROFILER_EXPIRED;
            case VD_PMU_SESSION_ERROR_UNSUPPORTED_EVENT:
                return VD_KERNEL_ERROR_PMU_PROFILER_UNSUPPORTED_EVENT;
            case VD_PMU_SESSION_ERROR_BUSY:
            case VD_PMU_SESSION_ERROR_NOT_IDLE:
            case VD_PMU_BACKEND_ERROR_BUSY:
            case VD_PMU_BACKEND_ERROR_NOT_IDLE:
            case VD_PMU_BACKEND_ERROR_CONFLICT:
                return VD_KERNEL_ERROR_PMU_PROFILER_BUSY;
            case VD_PMU_SESSION_ERROR_OWNER:
                return VD_KERNEL_ERROR_PMU_PROFILER_OWNER;
            case VD_PMU_SESSION_ERROR_RESTORE_REQUIRED:
            case VD_PMU_BACKEND_ERROR_RESTORE:
                return VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
            case VD_PMU_SESSION_ERROR_INVALID:
                return VD_KERNEL_ERROR_PMU_PROFILER_INVALID;
            case VD_PMU_SESSION_ERROR_STATE:
                return VD_KERNEL_ERROR_PMU_PROFILER_STATE;
            default:
                break;
        }
    }
    switch(provider_result)
    {
        case VP_ERROR_INVALID_ARGUMENT:
        case VP_ERROR_MALFORMED:
            return VD_KERNEL_ERROR_PMU_PROFILER_INVALID;
        case VP_ERROR_BUSY:
            return VD_KERNEL_ERROR_PMU_PROFILER_BUSY;
        case VP_ERROR_UNSUPPORTED:
            return VD_KERNEL_ERROR_PMU_PROFILER_UNSUPPORTED_EVENT;
        case VP_ERROR_RESTORE_REQUIRED:
            return VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
        case VP_ERROR_STATE:
            return VD_KERNEL_ERROR_PMU_PROFILER_STATE;
        default:
            return VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM;
    }
}

static void get_bridge_status(
    const struct vd_pmu_profiler_transport* transport,
    struct vd_pmu_profiler_bridge_status* status)
{
    zero_bytes(status, sizeof(*status));
    (void)vdPmuProfilerBridgeGetStatus(&transport->bridge, status);
}

static void publish_handle(
    const struct vd_pmu_profiler_transport* transport,
    struct vd_kernel_pmu_profiler_handle* handle)
{
    zero_bytes(handle, sizeof(*handle));
    handle->struct_size = sizeof(*handle);
    handle->abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION;
    handle->owner_token = transport->owner_token;
    handle->generation = transport->generation;
    handle->event_code = transport->event_code;
    handle->lease_ms = transport->lease_ms;
    handle->lease_token_low = (uint32_t)transport->lease_token;
    handle->lease_token_high =
        (uint32_t)(transport->lease_token >> 32);
}

int vdPmuProfilerTransportInit(
    struct vd_pmu_profiler_transport* transport)
{
    if(!transport || !vdPmuBackendReady())
        return VD_KERNEL_ERROR_PMU_PROFILER_DISABLED;
    if(transport_initialized(transport))
    {
        if(transport->state != VD_PMU_PROFILER_TRANSPORT_IDLE)
            return VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
        /* Initialization is deliberately idempotent.  In particular it must
         * never clear the real-event-per-boot latch or make an old handle
         * generation reachable again. */
        return 0;
    }
    zero_bytes(transport, sizeof(*transport));
    transport->initialization_cookie =
        VD_PMU_PROFILER_TRANSPORT_INITIALIZATION_COOKIE;
    transport->next_owner_token = 1;
    transport->next_generation = 1;
    transport->owner_pid = -1;
    transport->owner_thread = -1;
    return 0;
}

int vdPmuProfilerTransportGetInfo(
    const struct vd_pmu_profiler_transport* transport,
    struct vd_kernel_pmu_profiler_info* info)
{
    if(!transport_initialized(transport) || !vdPmuBackendReady())
        return VD_KERNEL_ERROR_PMU_PROFILER_DISABLED;
    if(!info_query_valid(info))
        return VD_KERNEL_ERROR_PMU_PROFILER_INVALID;
    zero_bytes(info, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION;
    info->capabilities = VD_KERNEL_PMU_PROFILER_CAP_EXACT_RESTORE |
        VD_KERNEL_PMU_PROFILER_CAP_LEASE_WATCHDOG |
        VD_KERNEL_PMU_PROFILER_CAP_SOFTWARE_INCREMENT;
    info->fixed_core = VD_KERNEL_PMU_PROFILER_FIXED_CORE;
    info->fixed_counter = VD_KERNEL_PMU_PROFILER_FIXED_COUNTER;
    info->min_lease_ms = VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS;
    info->max_lease_ms = VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS;
    info->event_codes[info->event_count++] =
        VD_KERNEL_PMU_PROFILER_EVENT_SOFTWARE_INCREMENT;
#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED
    info->capabilities |= VD_KERNEL_PMU_PROFILER_CAP_REAL_EVENTS |
        VD_KERNEL_PMU_PROFILER_CAP_SINGLE_REAL_EVENT_PER_BOOT;
    info->event_codes[info->event_count++] =
        VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS;
    info->event_codes[info->event_count++] =
        VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS;
    info->event_codes[info->event_count++] =
        VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT;
#endif
    return 0;
}

int vdPmuProfilerTransportOpen(
    struct vd_pmu_profiler_transport* transport,
    int32_t caller_pid,
    int32_t caller_thread,
    const struct vd_kernel_pmu_profiler_open_request* request,
    struct vd_kernel_pmu_profiler_handle* handle)
{
    struct vd_pmu_profiler_bridge_config bridge_config;
    struct vd_pmu_profiler_bridge_status status;
    struct vp_pmu_config pmu_config;
    const struct vp_pmu_provider* provider;
    uint64_t lease_token = 0;

    if(handle)
        zero_bytes(handle, sizeof(*handle));
    if(!transport_initialized(transport) || !vdPmuBackendReady())
        return VD_KERNEL_ERROR_PMU_PROFILER_DISABLED;
    if(caller_pid < 0 || caller_thread < 0 || !handle ||
       !open_request_valid(request))
        return VD_KERNEL_ERROR_PMU_PROFILER_INVALID;
    if(transport->state != VD_PMU_PROFILER_TRANSPORT_IDLE)
        return VD_KERNEL_ERROR_PMU_PROFILER_BUSY;
    if(!event_allowed(request->event_code))
        return VD_KERNEL_ERROR_PMU_PROFILER_UNSUPPORTED_EVENT;
    if(is_real_event(request->event_code) &&
       transport->real_event_attempted != 0)
        return VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED;

    transport->owner_pid = caller_pid;
    transport->owner_thread = caller_thread;
    transport->owner_token = take_nonzero(&transport->next_owner_token);
    transport->generation = take_nonzero(&transport->next_generation);
    transport->event_code = request->event_code;
    transport->lease_ms = request->lease_ms;
    if(is_real_event(request->event_code))
        transport->real_event_attempted = 1;

    zero_bytes(&bridge_config, sizeof(bridge_config));
    bridge_config.struct_size = sizeof(bridge_config);
    bridge_config.abi_version = VD_PMU_PROFILER_BRIDGE_ABI_VERSION;
    bridge_config.owner_pid = caller_pid;
    bridge_config.owner_token = transport->owner_token;
    bridge_config.core_id = VD_KERNEL_PMU_PROFILER_FIXED_CORE;
    bridge_config.lease_ms = request->lease_ms;
    if(is_real_event(request->event_code))
        bridge_config.flags =
            VD_PMU_PROFILER_BRIDGE_CONFIG_ALLOW_REAL_EVENTS;
    int result = vdPmuProfilerBridgeInit(&transport->bridge,
                                         &bridge_config);
    if(result != VP_RESULT_OK)
    {
        const int mapped = map_bridge_result(result, NULL);
        clear_session(transport);
        transport->last_result = mapped;
        return mapped;
    }

    provider = vdPmuProfilerBridgeGetProvider(&transport->bridge);
    if(!provider || provider->abi_version != VP_PMU_PROVIDER_ABI_VERSION ||
       provider->flags != VP_PMU_PROVIDER_FLAG_EXACT_RESTORE ||
       !provider->acquire || !provider->read || !provider->release)
    {
        clear_session(transport);
        transport->last_result = VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM;
        return transport->last_result;
    }

    zero_bytes(&pmu_config, sizeof(pmu_config));
    pmu_config.counter_mask = VD_PMU_PROFILER_BRIDGE_COUNTER_MASK;
    pmu_config.event_count = 1;
    pmu_config.event_codes[0] = request->event_code;
    result = provider->acquire(provider->user, &pmu_config, &lease_token);
    if(result != VP_RESULT_OK)
    {
        get_bridge_status(transport, &status);
        const int mapped = map_bridge_result(result, &status);
        if(status.orphan_restore_pending != 0 ||
           status.provider_lease_held != 0)
            transport->state =
                VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
        else
            clear_session(transport);
        transport->last_result = mapped;
        return mapped;
    }
    if(lease_token == 0)
    {
        transport->state = VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
        transport->last_result =
            VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
        return transport->last_result;
    }

    transport->lease_token = lease_token;
    transport->state = VD_PMU_PROFILER_TRANSPORT_ACTIVE;
    transport->last_result = 0;
    publish_handle(transport, handle);
    return 0;
}

int vdPmuProfilerTransportRead(
    struct vd_pmu_profiler_transport* transport,
    int32_t caller_pid,
    int32_t caller_thread,
    const struct vd_kernel_pmu_profiler_handle* handle,
    struct vd_kernel_pmu_profiler_sample* sample)
{
    struct vd_pmu_profiler_bridge_status status;
    struct vp_pmu_sample provider_sample;
    const struct vp_pmu_provider* provider;

    if(sample)
        zero_bytes(sample, sizeof(*sample));
    if(!transport_initialized(transport) || !vdPmuBackendReady())
        return VD_KERNEL_ERROR_PMU_PROFILER_DISABLED;
    if(!sample)
        return VD_KERNEL_ERROR_PMU_PROFILER_INVALID;
    if(!handle_valid(transport, caller_pid, caller_thread, handle))
        return VD_KERNEL_ERROR_PMU_PROFILER_OWNER;
    if(transport->state != VD_PMU_PROFILER_TRANSPORT_ACTIVE)
        return VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;

    provider = vdPmuProfilerBridgeGetProvider(&transport->bridge);
    if(!provider)
        return VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM;
    zero_bytes(&provider_sample, sizeof(provider_sample));
    int result = provider->read(provider->user, transport->lease_token,
                                &provider_sample);
    if(result != VP_RESULT_OK)
    {
        get_bridge_status(transport, &status);
        const int mapped = map_bridge_result(result, &status);
        if(status.auto_restored_waiting_release != 0)
        {
            const int release_result = provider->release(
                provider->user, transport->lease_token);
            if(release_result == VP_RESULT_OK)
                clear_session(transport);
            else
                transport->state =
                    VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
        }
        else if(status.session_state == VD_PMU_SESSION_RESTORE_PENDING ||
                status.orphan_restore_pending != 0)
        {
            transport->state =
                VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
        }
        transport->last_result = mapped;
        return mapped;
    }
    if(provider_sample.counter_mask !=
           VD_PMU_PROFILER_BRIDGE_COUNTER_MASK ||
       provider_sample.reserved != 0 || provider_sample.cycles != 0)
    {
        /* A provider contract violation cannot leave the lane live until the
         * ordinary lease timeout.  Attempt exact restoration immediately and
         * retain the obligation for the watchdog if that attempt is
         * ambiguous. */
        const int release_result = provider->release(
            provider->user, transport->lease_token);
        if(release_result == VP_RESULT_OK)
        {
            clear_session(transport);
            transport->last_result =
                VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM;
        }
        else
        {
            transport->state =
                VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
            transport->last_result =
                VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
        }
        return transport->last_result;
    }

    zero_bytes(sample, sizeof(*sample));
    sample->struct_size = sizeof(*sample);
    sample->abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION;
    sample->owner_token = transport->owner_token;
    sample->generation = transport->generation;
    sample->event_code = transport->event_code;
    sample->core_id = VD_KERNEL_PMU_PROFILER_FIXED_CORE;
    sample->physical_counter = VD_KERNEL_PMU_PROFILER_FIXED_COUNTER;
    sample->value = provider_sample.events[0];
    transport->last_result = 0;
    return 0;
}

int vdPmuProfilerTransportClose(
    struct vd_pmu_profiler_transport* transport,
    int32_t caller_pid,
    int32_t caller_thread,
    const struct vd_kernel_pmu_profiler_handle* handle)
{
    struct vd_pmu_profiler_bridge_status status;
    if(!transport_initialized(transport))
        return VD_KERNEL_ERROR_PMU_PROFILER_DISABLED;
    if(!handle_valid(transport, caller_pid, caller_thread, handle))
        return VD_KERNEL_ERROR_PMU_PROFILER_OWNER;

    const struct vp_pmu_provider* provider =
        vdPmuProfilerBridgeGetProvider(&transport->bridge);
    if(!provider)
        return VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM;
    int result = provider->release(provider->user,
                                   transport->lease_token);
    if(result == VP_RESULT_OK)
    {
        clear_session(transport);
        transport->last_result = 0;
        return 0;
    }
    get_bridge_status(transport, &status);
    transport->state = VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
    transport->last_result = map_bridge_result(result, &status);
    return VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
}

static int cleanup_once(struct vd_pmu_profiler_transport* transport)
{
    struct vd_pmu_profiler_bridge_status status;
    const struct vp_pmu_provider* provider =
        vdPmuProfilerBridgeGetProvider(&transport->bridge);
    if(!provider)
        return VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM;
    get_bridge_status(transport, &status);

    int result;
    if(status.orphan_restore_pending != 0)
        result = vdPmuProfilerBridgeRetryOrphanRestore(
            &transport->bridge);
    else
        result = vdPmuProfilerBridgeWatchdog(&transport->bridge);

    get_bridge_status(transport, &status);
    if(status.auto_restored_waiting_release != 0)
        result = provider->release(provider->user,
                                   transport->lease_token);
    if(result == VP_RESULT_OK &&
       transport->state == VD_PMU_PROFILER_TRANSPORT_ACTIVE &&
       status.session_state == VD_PMU_SESSION_ACTIVE)
        return 0;
    if(result == VP_RESULT_OK ||
       result == VD_PMU_SESSION_WATCHDOG_RESTORED)
    {
        get_bridge_status(transport, &status);
        if(status.session_state == VD_PMU_SESSION_EMPTY &&
           status.orphan_restore_pending == 0 &&
           status.auto_restored_waiting_release == 0 &&
           status.provider_lease_held == 0)
        {
            clear_session(transport);
            transport->last_result = 0;
            return 1;
        }
    }
    transport->state = VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
    transport->last_result = map_bridge_result(result, &status);
    return transport->last_result < 0 ? transport->last_result :
        VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
}

int vdPmuProfilerTransportWatchdog(
    struct vd_pmu_profiler_transport* transport)
{
    if(!transport_initialized(transport))
        return VD_KERNEL_ERROR_PMU_PROFILER_DISABLED;
    if(transport->state == VD_PMU_PROFILER_TRANSPORT_IDLE)
    {
        /* A failed first snapshot has no session/bridge lease to retain: the
         * session is still EMPTY and Open() correctly returns the transport
         * to IDLE.  PMSELR may nevertheless need exact restoration, or a
         * timed-out snapshot command may still need to be reaped.  Keep this
         * direct backend recovery strictly on the ownerless IDLE path; active
         * sessions continue through the lease-aware bridge below. */
        if(!vdPmuBackendRecoveryPending())
            return 0;
        const int result = vdPmuBackendRecover();
        if(result < 0 || vdPmuBackendRecoveryPending() ||
           !vdPmuBackendReady())
        {
            transport->last_result =
                VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
            return transport->last_result;
        }
        transport->last_result = 0;
        return 1;
    }
    return cleanup_once(transport);
}

int vdPmuProfilerTransportShutdown(
    struct vd_pmu_profiler_transport* transport)
{
    if(!transport_initialized(transport))
        return VD_KERNEL_ERROR_PMU_PROFILER_DISABLED;
    if(transport->state == VD_PMU_PROFILER_TRANSPORT_IDLE)
        return transport->real_event_attempted != 0 ?
            VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED : 0;
    if(transport->state == VD_PMU_PROFILER_TRANSPORT_ACTIVE)
    {
        const struct vp_pmu_provider* provider =
            vdPmuProfilerBridgeGetProvider(&transport->bridge);
        if(provider && provider->release(
               provider->user, transport->lease_token) == VP_RESULT_OK)
        {
            clear_session(transport);
            return transport->real_event_attempted != 0 ?
                VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED : 0;
        }
        transport->state = VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
    }
    const int result = cleanup_once(transport);
    if(result <= 0)
        return result;
    return transport->real_event_attempted != 0 ?
        VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED : 0;
}

#endif

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
    transport->exact_restore_proven = 0;
    transport->owner_identity_valid = 0;
    transport->owner_identity_release_uncertain = 0;
    zero_bytes(&transport->owner_identity,
               sizeof(transport->owner_identity));
    zero_bytes(&transport->bridge, sizeof(transport->bridge));
}

static int owner_backend_valid(
    const struct vd_pmu_profiler_owner_backend* backend)
{
    return backend && backend->capture && backend->query &&
        backend->release;
}

#if VD_PMU_PROFILER_SAFE_REARM_COMPILED
static int owner_identity_valid(
    const struct vd_pmu_profiler_owner_identity* identity,
    int32_t owner_pid, int32_t owner_thread)
{
    return identity && identity->process_id == owner_pid &&
        identity->thread_id == owner_thread &&
        identity->retained_thread_object != (uintptr_t)0 &&
        identity->thread_entry != (uintptr_t)0 &&
        identity->thread_stack != (uintptr_t)0 &&
        identity->thread_stack_size != 0;
}

static int capture_owner_identity(
    struct vd_pmu_profiler_transport* transport,
    int32_t owner_pid, int32_t owner_thread,
    struct vd_pmu_profiler_owner_identity* identity)
{
    zero_bytes(identity, sizeof(*identity));
    if(!owner_backend_valid(&transport->owner_backend))
        return VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM;
    const int result = transport->owner_backend.capture(
        transport->owner_backend.context, owner_pid, owner_thread,
        identity);
    if(result != 0 ||
       !owner_identity_valid(identity, owner_pid, owner_thread))
    {
        zero_bytes(identity, sizeof(*identity));
        return VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM;
    }
    return 1;
}
#endif

static int query_owner_status(
    struct vd_pmu_profiler_transport* transport)
{
    if(transport->owner_identity_valid == 0 ||
       transport->owner_identity_release_uncertain != 0 ||
       !owner_backend_valid(&transport->owner_backend))
        return VD_PMU_PROFILER_OWNER_UNKNOWN;
    const int result = transport->owner_backend.query(
        transport->owner_backend.context, transport->owner_pid,
        transport->owner_thread, &transport->owner_identity);
    if(result == VD_PMU_PROFILER_OWNER_QUARANTINE)
    {
        transport->owner_identity_release_uncertain = 1;
        return VD_PMU_PROFILER_OWNER_UNKNOWN;
    }
    if(result == VD_PMU_PROFILER_OWNER_ALIVE ||
       result == VD_PMU_PROFILER_OWNER_GONE)
        return result;
    return VD_PMU_PROFILER_OWNER_UNKNOWN;
}

static int release_owner_identity(
    struct vd_pmu_profiler_transport* transport)
{
    if(transport->owner_identity_valid == 0)
        return 0;
    if(transport->owner_identity_release_uncertain != 0 ||
       !owner_backend_valid(&transport->owner_backend))
    {
        transport->owner_identity_release_uncertain = 1;
        transport->state =
            VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER;
        transport->last_result =
            VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
        return transport->last_result;
    }

    const int result = transport->owner_backend.release(
        transport->owner_backend.context, transport->owner_pid,
        transport->owner_thread, &transport->owner_identity);
    if(result != 0)
    {
        /* The public GUID-release contract does not distinguish "retained"
         * from "released but reported late" on failure.  Never retry an
         * uncertain decrement: that could release someone else's reference.
         * Keep the plugin resident and quarantine all future leases. */
        transport->owner_identity_release_uncertain = 1;
        transport->state =
            VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER;
        transport->last_result =
            VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
        return transport->last_result;
    }
    transport->owner_identity_valid = 0;
    zero_bytes(&transport->owner_identity,
               sizeof(transport->owner_identity));
    return 0;
}

static int release_owner_and_clear(
    struct vd_pmu_profiler_transport* transport)
{
    const int result = release_owner_identity(transport);
    if(result < 0)
        return result;
    clear_session(transport);
    return 0;
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

static int exact_restoration_is_independently_proven(
    const struct vd_pmu_profiler_transport* transport)
{
    struct vd_pmu_profiler_bridge_status status;
    get_bridge_status(transport, &status);
    return status.initialized != 0 &&
        status.session_state == VD_PMU_SESSION_EMPTY &&
        status.orphan_restore_pending == 0 &&
        status.auto_restored_waiting_release == 0 &&
        status.provider_lease_held == 0 &&
        vdPmuBackendReady() && !vdPmuBackendRecoveryPending() &&
        !vdPmuBackendHasRestoreObligation();
}

enum rearm_terminal_proof {
    REARM_TERMINAL_NONE = 0,
    REARM_TERMINAL_EXPLICIT_CLOSE = 1,
    REARM_TERMINAL_OWNER_GONE = 2,
};

#if VD_PMU_PROFILER_SAFE_REARM_COMPILED
static int rearm_identity_matches(
    const struct vd_pmu_profiler_transport* transport,
    uint32_t owner_token, uint32_t generation)
{
    return owner_token != 0 && generation != 0 &&
        owner_token == transport->owner_token &&
        generation == transport->generation;
}
#endif

/* Called only after the bridge has released its provider token.  It performs
 * a second, independent check of the session/backend state before recording
 * exact restoration.  In safe-rearm builds a real event remains quarantined
 * until its exact owner/generation closes or the captured owner is proven
 * gone. */
static int finish_exact_restoration(
    struct vd_pmu_profiler_transport* transport,
    enum rearm_terminal_proof terminal_proof,
    uint32_t owner_token, uint32_t generation)
{
    if(!exact_restoration_is_independently_proven(transport))
    {
        transport->state =
            VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
        transport->last_result =
            VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
        return transport->last_result;
    }

    transport->exact_restore_proven = 1;
    const int real = is_real_event(transport->event_code);
#if VD_PMU_PROFILER_SAFE_REARM_COMPILED
    if(real)
    {
        const int identity_matches = rearm_identity_matches(
            transport, owner_token, generation);
        if((terminal_proof == REARM_TERMINAL_EXPLICIT_CLOSE &&
            identity_matches) ||
           (terminal_proof == REARM_TERMINAL_OWNER_GONE &&
            identity_matches && transport->owner_identity_valid != 0))
        {
            const int release_result = release_owner_identity(transport);
            if(release_result < 0)
                return release_result;
            transport->real_event_attempted = 0;
            ++transport->rearm_count;
            if(transport->rearm_count == 0)
                transport->rearm_count = 1;
            clear_session(transport);
            transport->last_result = 0;
            return 1;
        }

        /* Drop only the now-idle bridge.  Retain the kernel-generated owner,
         * generation, lease token, and captured identity so a later exact
         * Close or liveness proof can authorize re-arm. */
        zero_bytes(&transport->bridge, sizeof(transport->bridge));
        transport->state =
            VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER;
        transport->last_result = 0;
        return 1;
    }
#else
    (void)terminal_proof;
    (void)owner_token;
    (void)generation;
    (void)real;
#endif

    if(release_owner_and_clear(transport) < 0)
        return transport->last_result;
    transport->last_result = 0;
    return 1;
}

static int acknowledge_restored_owner(
    struct vd_pmu_profiler_transport* transport,
    enum rearm_terminal_proof terminal_proof,
    uint32_t owner_token, uint32_t generation)
{
#if VD_PMU_PROFILER_SAFE_REARM_COMPILED
    if(transport->state !=
           VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER ||
       transport->exact_restore_proven == 0 ||
       transport->owner_identity_release_uncertain != 0 ||
       !is_real_event(transport->event_code) ||
       !rearm_identity_matches(transport, owner_token, generation) ||
       vdPmuBackendRecoveryPending() ||
       vdPmuBackendHasRestoreObligation() || !vdPmuBackendReady())
        return VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
    if(terminal_proof != REARM_TERMINAL_EXPLICIT_CLOSE &&
       (terminal_proof != REARM_TERMINAL_OWNER_GONE ||
        transport->owner_identity_valid == 0))
        return VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;

    const int release_result = release_owner_identity(transport);
    if(release_result < 0)
        return release_result;
    transport->real_event_attempted = 0;
    ++transport->rearm_count;
    if(transport->rearm_count == 0)
        transport->rearm_count = 1;
    clear_session(transport);
    transport->last_result = 0;
    return 1;
#else
    (void)transport;
    (void)terminal_proof;
    (void)owner_token;
    (void)generation;
    return VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED;
#endif
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

int vdPmuProfilerTransportSetOwnerBackend(
    struct vd_pmu_profiler_transport* transport,
    const struct vd_pmu_profiler_owner_backend* backend)
{
    if(!transport_initialized(transport) ||
       !owner_backend_valid(backend))
        return VD_KERNEL_ERROR_PMU_PROFILER_INVALID;
    if(transport->state != VD_PMU_PROFILER_TRANSPORT_IDLE ||
       transport->real_event_attempted != 0 ||
       transport->owner_identity_release_uncertain != 0 ||
       vdPmuBackendRecoveryPending())
        return VD_KERNEL_ERROR_PMU_PROFILER_BUSY;
    transport->owner_backend = *backend;
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
    info->capabilities |= VD_KERNEL_PMU_PROFILER_CAP_REAL_EVENTS;
#if VD_PMU_PROFILER_SAFE_REARM_COMPILED
    info->capabilities |=
        VD_KERNEL_PMU_PROFILER_CAP_SAFE_POST_RESTORE_REARM;
#else
    info->capabilities |=
        VD_KERNEL_PMU_PROFILER_CAP_SINGLE_REAL_EVENT_PER_BOOT;
#endif
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
    struct vd_pmu_profiler_owner_identity owner_identity;
    uint64_t lease_token = 0;

    if(handle)
        zero_bytes(handle, sizeof(*handle));
    if(!transport_initialized(transport) || !vdPmuBackendReady())
        return VD_KERNEL_ERROR_PMU_PROFILER_DISABLED;
    if(caller_pid < 0 || caller_thread < 0 || !handle ||
       !open_request_valid(request))
        return VD_KERNEL_ERROR_PMU_PROFILER_INVALID;
    if(transport->state != VD_PMU_PROFILER_TRANSPORT_IDLE)
    {
        /* Open is already serialized by the kernel PMU lock.  Service one
         * bounded liveness/recovery pass here so an exited owner cannot be
         * starved indefinitely by an Open poll running at the same cadence
         * as the non-blocking background watchdog.  A live or unprovable
         * owner remains BUSY; any ambiguous cleanup fails closed. */
        const int recovery_result =
            vdPmuProfilerTransportWatchdog(transport);
        if(recovery_result < 0)
            return recovery_result;
        if(transport->state != VD_PMU_PROFILER_TRANSPORT_IDLE)
            return VD_KERNEL_ERROR_PMU_PROFILER_BUSY;
    }
    if(!event_allowed(request->event_code))
        return VD_KERNEL_ERROR_PMU_PROFILER_UNSUPPORTED_EVENT;
    if(is_real_event(request->event_code) &&
       transport->real_event_attempted != 0)
        return VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED;

    zero_bytes(&owner_identity, sizeof(owner_identity));
    int identity_result = 0;
#if VD_PMU_PROFILER_SAFE_REARM_COMPILED
    if(is_real_event(request->event_code))
        identity_result = capture_owner_identity(
            transport, caller_pid, caller_thread, &owner_identity);
#endif
    if(identity_result < 0)
    {
        transport->last_result = identity_result;
        return identity_result;
    }

    transport->owner_pid = caller_pid;
    transport->owner_thread = caller_thread;
    transport->owner_token = take_nonzero(&transport->next_owner_token);
    transport->generation = take_nonzero(&transport->next_generation);
    transport->event_code = request->event_code;
    transport->lease_ms = request->lease_ms;
    transport->exact_restore_proven = 0;
    transport->owner_identity_valid = identity_result > 0 ? 1u : 0u;
    transport->owner_identity = owner_identity;
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
        if(release_owner_and_clear(transport) < 0)
            return transport->last_result;
        transport->last_result = mapped;
        return mapped;
    }

    provider = vdPmuProfilerBridgeGetProvider(&transport->bridge);
    if(!provider || provider->abi_version != VP_PMU_PROVIDER_ABI_VERSION ||
       provider->flags != VP_PMU_PROVIDER_FLAG_EXACT_RESTORE ||
       !provider->acquire || !provider->read || !provider->release)
    {
        if(release_owner_and_clear(transport) < 0)
            return transport->last_result;
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
        else if(release_owner_and_clear(transport) < 0)
            return transport->last_result;
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
            {
                const int finish_result = finish_exact_restoration(
                    transport, REARM_TERMINAL_NONE,
                    transport->owner_token, transport->generation);
                if(finish_result < 0)
                    return finish_result;
            }
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
            const int finish_result = finish_exact_restoration(
                transport, REARM_TERMINAL_NONE,
                transport->owner_token, transport->generation);
            transport->last_result = finish_result < 0 ? finish_result :
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

    if(transport->state ==
       VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER)
    {
        const int result = acknowledge_restored_owner(
            transport, REARM_TERMINAL_EXPLICIT_CLOSE,
            handle->owner_token, handle->generation);
        return result > 0 ? 0 : result;
    }

    const struct vp_pmu_provider* provider =
        vdPmuProfilerBridgeGetProvider(&transport->bridge);
    if(!provider)
        return VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM;
    int result = provider->release(provider->user,
                                   transport->lease_token);
    if(result == VP_RESULT_OK)
    {
        result = finish_exact_restoration(
            transport, REARM_TERMINAL_EXPLICIT_CLOSE,
            handle->owner_token, handle->generation);
        return result > 0 ? 0 : result;
    }
    get_bridge_status(transport, &status);
    transport->state = VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
    transport->last_result = map_bridge_result(result, &status);
    return VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
}

static int cleanup_once(
    struct vd_pmu_profiler_transport* transport,
    enum rearm_terminal_proof terminal_proof)
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
            return finish_exact_restoration(
                transport, terminal_proof, transport->owner_token,
                transport->generation);
    }
    transport->state = VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
    transport->last_result = map_bridge_result(result, &status);
    return transport->last_result < 0 ? transport->last_result :
        VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
}

/* A liveness probe may discover that reference accounting itself is no
 * longer trustworthy (for example a UID/object mismatch or failed temporary
 * reference release).  Restore a live PMU lease immediately, but never query,
 * release, re-arm, or unload through that identity again. */
static int restore_for_owner_reference_quarantine(
    struct vd_pmu_profiler_transport* transport)
{
    if(transport->state == VD_PMU_PROFILER_TRANSPORT_ACTIVE)
    {
        const struct vp_pmu_provider* provider =
            vdPmuProfilerBridgeGetProvider(&transport->bridge);
        if(provider && provider->release(
               provider->user, transport->lease_token) == VP_RESULT_OK)
            (void)finish_exact_restoration(
                transport, REARM_TERMINAL_NONE,
                transport->owner_token, transport->generation);
        else
            transport->state =
                VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
    }
    if(transport->state == VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED)
        (void)cleanup_once(transport, REARM_TERMINAL_NONE);
    transport->last_result =
        VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
    return transport->last_result;
}

int vdPmuProfilerTransportWatchdog(
    struct vd_pmu_profiler_transport* transport)
{
    if(!transport_initialized(transport))
        return VD_KERNEL_ERROR_PMU_PROFILER_DISABLED;
    if(transport->owner_identity_release_uncertain != 0)
        return restore_for_owner_reference_quarantine(transport);
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

    const int owner_status = query_owner_status(transport);
    if(transport->owner_identity_release_uncertain != 0)
        return restore_for_owner_reference_quarantine(transport);
    if(transport->state ==
       VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER)
    {
        if(owner_status != VD_PMU_PROFILER_OWNER_GONE)
            return 0;
        const int result = acknowledge_restored_owner(
            transport, REARM_TERMINAL_OWNER_GONE,
            transport->owner_token, transport->generation);
        return result > 0 ? 1 : result;
    }

    if(owner_status == VD_PMU_PROFILER_OWNER_GONE &&
       transport->state == VD_PMU_PROFILER_TRANSPORT_ACTIVE)
    {
        const struct vp_pmu_provider* provider =
            vdPmuProfilerBridgeGetProvider(&transport->bridge);
        if(!provider || provider->release(
               provider->user, transport->lease_token) != VP_RESULT_OK)
        {
            transport->state =
                VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
            transport->last_result =
                VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED;
            return transport->last_result;
        }
        const int result = finish_exact_restoration(
            transport, REARM_TERMINAL_OWNER_GONE,
            transport->owner_token, transport->generation);
        return result > 0 ? 1 : result;
    }

    return cleanup_once(
        transport,
        owner_status == VD_PMU_PROFILER_OWNER_GONE ?
            REARM_TERMINAL_OWNER_GONE : REARM_TERMINAL_NONE);
}

int vdPmuProfilerTransportShutdown(
    struct vd_pmu_profiler_transport* transport)
{
    if(!transport_initialized(transport))
        return VD_KERNEL_ERROR_PMU_PROFILER_DISABLED;
    if(transport->owner_identity_release_uncertain != 0)
        return restore_for_owner_reference_quarantine(transport);
    if(transport->state == VD_PMU_PROFILER_TRANSPORT_IDLE)
        return transport->real_event_attempted != 0 ?
            VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED : 0;
    if(transport->state ==
       VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER)
    {
        const int owner_status = query_owner_status(transport);
        if(transport->owner_identity_release_uncertain != 0)
            return restore_for_owner_reference_quarantine(transport);
        if(owner_status == VD_PMU_PROFILER_OWNER_GONE)
        {
            const int rearm_result = acknowledge_restored_owner(
                transport, REARM_TERMINAL_OWNER_GONE,
                transport->owner_token, transport->generation);
            if(rearm_result > 0)
                return 0;
        }
        return VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED;
    }
    if(transport->state == VD_PMU_PROFILER_TRANSPORT_ACTIVE)
    {
        const struct vp_pmu_provider* provider =
            vdPmuProfilerBridgeGetProvider(&transport->bridge);
        if(provider && provider->release(
               provider->user, transport->lease_token) == VP_RESULT_OK)
        {
            const int finish_result = finish_exact_restoration(
                transport, REARM_TERMINAL_NONE,
                transport->owner_token, transport->generation);
            if(finish_result < 0)
                return finish_result;
            return transport->real_event_attempted != 0 ?
                VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED : 0;
        }
        transport->state = VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED;
    }
    const int result = cleanup_once(transport, REARM_TERMINAL_NONE);
    if(result <= 0)
        return result;
    return transport->real_event_attempted != 0 ?
        VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED : 0;
}

#endif

#include "pmu_session.h"

#include <stddef.h>

#define VD_PMU_PMCR_RESET_BITS ((UINT32_C(1) << 1) | (UINT32_C(1) << 2))
#define VD_PMU_EVENT_COUNTER_MASK                                      \
    ((UINT32_C(1) << VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS) -        \
     UINT32_C(1))
#define VD_PMU_IMPLEMENTED_COUNTER_MASK                                \
    (VD_PMU_EVENT_COUNTER_MASK | (UINT32_C(1) << 31))

struct vd_pmu_allowlisted_event {
    uint32_t event_id;
    uint32_t event_code;
};

static const struct vd_pmu_allowlisted_event g_allowlisted_events[] = {
    {VD_PMU_EVENT_ICACHE_MISS, UINT32_C(0x01)},
    {VD_PMU_EVENT_DCACHE_MISS, UINT32_C(0x03)},
    {VD_PMU_EVENT_DCACHE_ACCESS, UINT32_C(0x04)},
    {VD_PMU_EVENT_BRANCH_MISPREDICT, UINT32_C(0x10)},
    {VD_PMU_EVENT_PREDICTED_BRANCH, UINT32_C(0x12)},
    {VD_PMU_EVENT_MAIN_PIPE, UINT32_C(0x70)},
    {VD_PMU_EVENT_SECOND_PIPE, UINT32_C(0x71)},
    {VD_PMU_EVENT_LOAD_STORE_PIPE, UINT32_C(0x72)},
    {VD_PMU_EVENT_FPU_RENAME, UINT32_C(0x73)},
    {VD_PMU_EVENT_NEON_RENAME, UINT32_C(0x74)},
    {VD_PMU_EVENT_SOFTWARE_INCREMENT, UINT32_C(0x00)},
};

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

static int restored_owned_fields_equal(
    const struct vd_pmu_snapshot* current,
    const struct vd_pmu_snapshot* original,
    const struct vd_pmu_configuration* configuration)
{
    if(configuration->event_count != VD_PMU_SESSION_MAX_ACTIVE_EVENTS)
        return 0;
    const uint32_t counter = configuration->events[0].physical_counter;
    if(counter != VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS - 1u ||
       current->event_counter_count != original->event_counter_count)
        return 0;
    if((current->raw_pmcr & ~VD_PMU_PMCR_RESET_BITS) !=
           (original->raw_pmcr & ~VD_PMU_PMCR_RESET_BITS) ||
       (current->raw_pmcntenset & VD_PMU_IMPLEMENTED_COUNTER_MASK) !=
           (original->raw_pmcntenset & VD_PMU_IMPLEMENTED_COUNTER_MASK) ||
       (current->raw_pmovsr & VD_PMU_IMPLEMENTED_COUNTER_MASK) !=
           (original->raw_pmovsr & VD_PMU_IMPLEMENTED_COUNTER_MASK) ||
       current->raw_pmselr != original->raw_pmselr ||
       current->raw_pmccntr != original->raw_pmccntr ||
       current->raw_pmuserenr != original->raw_pmuserenr ||
       (current->raw_pmintenset & VD_PMU_IMPLEMENTED_COUNTER_MASK) !=
           (original->raw_pmintenset & VD_PMU_IMPLEMENTED_COUNTER_MASK))
        return 0;
    for(uint32_t i = 0; i < VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS; ++i)
        if(current->raw_pmxevtyper[i] != original->raw_pmxevtyper[i] ||
           current->raw_pmxevcntr[i] != original->raw_pmxevcntr[i])
            return 0;
    return 1;
}

static int owner_valid(const struct vd_pmu_session_owner* owner)
{
    return owner && owner->owner_pid >= 0 && owner->owner_token != 0 &&
           owner->core_id < VD_PMU_SESSION_PHYSICAL_CORE_COUNT;
}

static int owner_equal(const struct vd_pmu_session_owner* left,
                       const struct vd_pmu_session_owner* right)
{
    return left->owner_pid == right->owner_pid &&
           left->owner_token == right->owner_token &&
           left->core_id == right->core_id;
}

static int backend_valid(const struct vd_pmu_session_backend* backend)
{
    return backend && backend->snapshot && backend->configure &&
           backend->read && backend->restore && backend->now_ms;
}

static int backend_failure(int result)
{
    return result < 0 ? result : VD_PMU_SESSION_ERROR_BACKEND_CONTRACT;
}

static int request_valid(const struct vd_pmu_session_request* request)
{
    if(!request || request->struct_size != sizeof(*request) ||
       request->abi_version != VD_PMU_SESSION_ABI_VERSION ||
       request->lease_ms < VD_PMU_SESSION_MIN_LEASE_MS ||
       request->lease_ms > VD_PMU_SESSION_MAX_LEASE_MS ||
       (request->flags & ~VD_PMU_SESSION_ALLOWED_FLAGS) != 0 ||
       request->event_count != VD_PMU_SESSION_MAX_ACTIVE_EVENTS)
        return 0;
    for(uint32_t i = 0; i < 3u; ++i)
        if(request->reserved[i] != 0)
            return 0;
    for(uint32_t i = 0; i < VD_PMU_SESSION_MAX_EVENTS; ++i)
    {
        if(i >= request->event_count)
        {
            if(request->event_ids[i] != 0)
                return 0;
            continue;
        }
        if(request->event_ids[i] == 0)
            return 0;
        struct vd_pmu_event_metadata metadata;
        if(vdPmuSessionLookupEvent(request->event_ids[i], &metadata) < 0)
            return 0;
        for(uint32_t j = 0; j < i; ++j)
            if(request->event_ids[i] == request->event_ids[j])
                return 0;
    }
    return 1;
}

static int make_configuration(
    const struct vd_pmu_session_request* request,
    const struct vd_pmu_snapshot* snapshot,
    struct vd_pmu_configuration* configuration)
{
    struct vd_pmu_configuration local;
    zero_bytes(&local, sizeof(local));
    local.flags = request->flags;
    local.event_count = request->event_count;
    if((snapshot->raw_pmcntenset & VD_PMU_IMPLEMENTED_COUNTER_MASK) != 0 ||
       (snapshot->raw_pmintenset & VD_PMU_IMPLEMENTED_COUNTER_MASK) != 0 ||
       (snapshot->raw_pmovsr & VD_PMU_IMPLEMENTED_COUNTER_MASK) != 0)
        return VD_PMU_SESSION_ERROR_NOT_IDLE;

    if((snapshot->raw_pmcr & UINT32_C(1)) == 0)
    {
        local.control_flags = VD_PMU_CONFIGURATION_ENABLE_GLOBAL;
    }
    const uint32_t selected_counter =
        VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS - 1u;

    for(uint32_t i = 0; i < request->event_count; ++i)
    {
        int result = vdPmuSessionLookupEvent(request->event_ids[i],
                                             &local.events[i]);
        if(result < 0)
            return result;
        local.events[i].physical_counter = selected_counter;
    }
    copy_bytes(configuration, &local, sizeof(*configuration));
    return 0;
}

static void clear_lease(struct vd_pmu_session* session)
{
    uint32_t initialization_cookie = session->initialization_cookie;
    uint64_t next = session->next_lease_token;
    zero_bytes(session, sizeof(*session));
    session->initialization_cookie = initialization_cookie;
    session->next_lease_token = next ? next : UINT64_C(1);
}

static uint64_t take_lease_token(struct vd_pmu_session* session)
{
    uint64_t token = session->next_lease_token;
    if(token == 0)
        token = UINT64_C(1);
    session->next_lease_token = token + UINT64_C(1);
    if(session->next_lease_token == 0)
        session->next_lease_token = UINT64_C(1);
    session->lease_token = token;
    return token;
}

static uint64_t saturating_deadline(uint64_t now_ms, uint32_t lease_ms)
{
    uint64_t duration = (uint64_t)lease_ms;
    if(UINT64_MAX - now_ms < duration)
        return UINT64_MAX;
    return now_ms + duration;
}

static int restore_exact(struct vd_pmu_session* session)
{
    struct vd_pmu_snapshot verification;
    zero_bytes(&verification, sizeof(verification));
    session->state = VD_PMU_SESSION_RESTORE_PENDING;

    int result = session->backend.restore(session->backend.context,
                                          session->owner.core_id,
                                          &session->configuration,
                                          &session->original);
    if(result != 0)
    {
        session->last_backend_error = backend_failure(result);
        return VD_PMU_SESSION_ERROR_RESTORE_REQUIRED;
    }
    result = session->backend.snapshot(session->backend.context,
                                       session->owner.core_id,
                                       &verification);
    if(result != 0)
    {
        session->last_backend_error = backend_failure(result);
        return VD_PMU_SESSION_ERROR_RESTORE_REQUIRED;
    }
    if(!restored_owned_fields_equal(&verification, &session->original,
                                    &session->configuration))
    {
        session->last_backend_error = VD_PMU_SESSION_ERROR_RESTORE_REQUIRED;
        return VD_PMU_SESSION_ERROR_RESTORE_REQUIRED;
    }
    clear_lease(session);
    return 0;
}

static int owner_and_token_valid(
    const struct vd_pmu_session* session,
    const struct vd_pmu_session_owner* owner,
    uint64_t lease_token)
{
    if(!session ||
       session->initialization_cookie !=
           VD_PMU_SESSION_INITIALIZATION_COOKIE ||
       session->state == VD_PMU_SESSION_EMPTY)
        return VD_PMU_SESSION_ERROR_STATE;
    if(!owner_valid(owner) || lease_token == 0 ||
       lease_token != session->lease_token ||
       !owner_equal(owner, &session->owner))
        return VD_PMU_SESSION_ERROR_OWNER;
    return 0;
}

void vdPmuSessionInit(struct vd_pmu_session* session)
{
    if(!session)
        return;
    if(session->initialization_cookie ==
       VD_PMU_SESSION_INITIALIZATION_COOKIE)
        return;
    if(session->initialization_cookie != 0)
        return;
    zero_bytes(session, sizeof(*session));
    session->initialization_cookie = VD_PMU_SESSION_INITIALIZATION_COOKIE;
    session->next_lease_token = UINT64_C(1);
}

int vdPmuSessionLookupEvent(uint32_t event_id,
                            struct vd_pmu_event_metadata* metadata)
{
    if(!metadata)
        return VD_PMU_SESSION_ERROR_INVALID;
    for(size_t i = 0;
        i < sizeof(g_allowlisted_events) / sizeof(g_allowlisted_events[0]);
        ++i)
    {
        if(g_allowlisted_events[i].event_id == event_id)
        {
            struct vd_pmu_event_metadata local;
            zero_bytes(&local, sizeof(local));
            local.event_id = event_id;
            local.event_code = g_allowlisted_events[i].event_code;
            copy_bytes(metadata, &local, sizeof(*metadata));
            return 0;
        }
    }
    return VD_PMU_SESSION_ERROR_UNSUPPORTED_EVENT;
}

int vdPmuSessionAcquire(struct vd_pmu_session* session,
                        const struct vd_pmu_session_owner* owner,
                        const struct vd_pmu_session_request* request,
                        const struct vd_pmu_session_backend* backend,
                        uint64_t* lease_token)
{
    struct vd_pmu_configuration configuration;
    struct vd_pmu_snapshot original;
    uint64_t start_ms;

    if(!session ||
       session->initialization_cookie !=
           VD_PMU_SESSION_INITIALIZATION_COOKIE ||
       !owner_valid(owner) || !request_valid(request) ||
       !backend_valid(backend) || !lease_token)
        return VD_PMU_SESSION_ERROR_INVALID;
    if(session->state == VD_PMU_SESSION_RESTORE_PENDING)
        return VD_PMU_SESSION_ERROR_RESTORE_REQUIRED;
    if(session->state != VD_PMU_SESSION_EMPTY)
        return VD_PMU_SESSION_ERROR_BUSY;

    int result = backend->now_ms(backend->context, &start_ms);
    if(result != 0)
        return backend_failure(result);

    zero_bytes(&original, sizeof(original));
    result = backend->snapshot(backend->context, owner->core_id, &original);
    if(result != 0)
        return backend_failure(result);
    if(original.event_counter_count != VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS ||
       (original.raw_pmcr & VD_PMU_PMCR_RESET_BITS) != 0 ||
       ((original.raw_pmcr >> 11) & UINT32_C(0x1f)) !=
           original.event_counter_count ||
       original.raw_pmselr >= original.event_counter_count)
        return VD_PMU_SESSION_ERROR_INVALID;

    result = make_configuration(request, &original, &configuration);
    if(result != 0)
        return backend_failure(result);

    const uint64_t deadline_ms =
        saturating_deadline(start_ms, request->lease_ms);
    uint64_t before_configure_ms;
    result = backend->now_ms(backend->context, &before_configure_ms);
    if(result != 0)
        return backend_failure(result);
    if(before_configure_ms < start_ms)
        return VD_PMU_SESSION_ERROR_CLOCK;
    if(before_configure_ms >= deadline_ms)
        return VD_PMU_SESSION_ERROR_EXPIRED;

    copy_bytes(&session->owner, owner, sizeof(session->owner));
    copy_bytes(&session->configuration, &configuration,
               sizeof(session->configuration));
    copy_bytes(&session->original, &original, sizeof(session->original));
    copy_bytes(&session->backend, backend, sizeof(session->backend));
    session->acquired_ms = start_ms;
    session->deadline_ms = deadline_ms;
    session->last_backend_error = 0;
    take_lease_token(session);

    /* From this point configure may have changed hardware even when it fails.
     * Publish a restoration obligation before invoking it. */
    session->state = VD_PMU_SESSION_RESTORE_PENDING;
    result = session->backend.configure(session->backend.context,
                                        session->owner.core_id,
                                        &session->configuration);
    if(result != 0)
    {
        const int configure_error = backend_failure(result);
        session->last_backend_error = configure_error;
        const uint64_t pending_token = session->lease_token;
        result = restore_exact(session);
        if(result < 0)
            *lease_token = pending_token;
        return result < 0 ? result : configure_error;
    }

    uint64_t configured_ms;
    result = session->backend.now_ms(session->backend.context,
                                     &configured_ms);
    if(result != 0 || configured_ms < start_ms ||
       configured_ms >= session->deadline_ms)
    {
        const int time_error = result != 0 ? backend_failure(result) :
            (configured_ms < start_ms ? VD_PMU_SESSION_ERROR_CLOCK :
                                        VD_PMU_SESSION_ERROR_EXPIRED);
        session->last_backend_error = result != 0 ? time_error : 0;
        const uint64_t pending_token = session->lease_token;
        result = restore_exact(session);
        if(result < 0)
            *lease_token = pending_token;
        return result < 0 ? result : time_error;
    }

    session->state = VD_PMU_SESSION_ACTIVE;
    *lease_token = session->lease_token;
    return 0;
}

int vdPmuSessionRead(struct vd_pmu_session* session,
                     const struct vd_pmu_session_owner* owner,
                     uint64_t lease_token,
                     struct vd_pmu_reading* reading)
{
    if(!reading)
        return VD_PMU_SESSION_ERROR_INVALID;
    int result = owner_and_token_valid(session, owner, lease_token);
    if(result < 0)
        return result;
    if(session->state == VD_PMU_SESSION_RESTORE_PENDING)
        return VD_PMU_SESSION_ERROR_RESTORE_REQUIRED;
    if(session->state != VD_PMU_SESSION_ACTIVE)
        return VD_PMU_SESSION_ERROR_STATE;

    uint64_t now_ms;
    result = session->backend.now_ms(session->backend.context, &now_ms);
    if(result != 0)
    {
        const int clock_error = backend_failure(result);
        session->last_backend_error = clock_error;
        result = restore_exact(session);
        return result < 0 ? result : clock_error;
    }
    if(now_ms < session->acquired_ms)
    {
        result = restore_exact(session);
        return result < 0 ? result : VD_PMU_SESSION_ERROR_CLOCK;
    }
    if(now_ms >= session->deadline_ms)
    {
        result = restore_exact(session);
        return result < 0 ? result : VD_PMU_SESSION_ERROR_EXPIRED;
    }

    struct vd_pmu_counter_values values;
    zero_bytes(&values, sizeof(values));
    result = session->backend.read(session->backend.context,
                                   session->owner.core_id,
                                   &session->configuration, &values);
    if(result != 0)
    {
        const int read_error = backend_failure(result);
        session->last_backend_error = read_error;
        result = restore_exact(session);
        return result < 0 ? result : read_error;
    }

    uint64_t completed_ms;
    result = session->backend.now_ms(session->backend.context, &completed_ms);
    if(result != 0)
    {
        const int clock_error = backend_failure(result);
        session->last_backend_error = clock_error;
        result = restore_exact(session);
        return result < 0 ? result : clock_error;
    }
    if(completed_ms < now_ms || completed_ms < session->acquired_ms)
    {
        result = restore_exact(session);
        return result < 0 ? result : VD_PMU_SESSION_ERROR_CLOCK;
    }
    if(completed_ms >= session->deadline_ms)
    {
        result = restore_exact(session);
        return result < 0 ? result : VD_PMU_SESSION_ERROR_EXPIRED;
    }

    struct vd_pmu_reading local;
    zero_bytes(&local, sizeof(local));
    local.timestamp_ms = completed_ms;
    local.lease_token = session->lease_token;
    local.flags = session->configuration.flags;
    local.event_count = session->configuration.event_count;
    if((local.flags & VD_PMU_SESSION_FLAG_CYCLES) != 0)
        local.cycles = values.cycles;
    for(uint32_t i = 0; i < local.event_count; ++i)
    {
        copy_bytes(&local.events[i].metadata,
                   &session->configuration.events[i],
                   sizeof(local.events[i].metadata));
        local.events[i].value = values.events[i];
    }
    copy_bytes(reading, &local, sizeof(*reading));
    return 0;
}

int vdPmuSessionRestore(struct vd_pmu_session* session,
                        const struct vd_pmu_session_owner* owner,
                        uint64_t lease_token)
{
    int result = owner_and_token_valid(session, owner, lease_token);
    if(result < 0)
        return result;
    return restore_exact(session);
}

int vdPmuSessionWatchdog(struct vd_pmu_session* session)
{
    if(!session ||
       session->initialization_cookie !=
           VD_PMU_SESSION_INITIALIZATION_COOKIE)
        return VD_PMU_SESSION_ERROR_INVALID;
    if(session->state == VD_PMU_SESSION_EMPTY)
        return 0;
    if(session->state == VD_PMU_SESSION_RESTORE_PENDING)
    {
        int result = restore_exact(session);
        return result < 0 ? result : VD_PMU_SESSION_WATCHDOG_RESTORED;
    }
    if(session->state != VD_PMU_SESSION_ACTIVE)
        return VD_PMU_SESSION_ERROR_STATE;

    uint64_t now_ms;
    int result = session->backend.now_ms(session->backend.context, &now_ms);
    if(result != 0)
    {
        const int clock_error = backend_failure(result);
        session->last_backend_error = clock_error;
        result = restore_exact(session);
        return result < 0 ? result : clock_error;
    }
    if(now_ms < session->acquired_ms)
    {
        result = restore_exact(session);
        return result < 0 ? result : VD_PMU_SESSION_ERROR_CLOCK;
    }
    if(now_ms < session->deadline_ms)
        return 0;
    result = restore_exact(session);
    return result < 0 ? result : VD_PMU_SESSION_WATCHDOG_RESTORED;
}

int vdPmuSessionIsActive(const struct vd_pmu_session* session)
{
    return session &&
           session->initialization_cookie ==
               VD_PMU_SESSION_INITIALIZATION_COOKIE &&
           session->state != VD_PMU_SESSION_EMPTY;
}

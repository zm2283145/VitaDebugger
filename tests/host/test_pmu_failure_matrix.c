#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pmu_profiler_transport.h"
#include "vitaprofiler_tcp_vita.h"

#if !VD_PMU_PROFILER_REAL_EVENTS_COMPILED
#error "the PMU failure matrix requires the real-event host-test gate"
#endif

#if !VD_PMU_PROFILER_SAFE_REARM_COMPILED
#error "the PMU failure matrix requires the safe-rearm host-test gate"
#endif

/*
 * This is an integration fixture, not another register-backend unit test.
 * The existing backend/session suites prove the real fake-register restore
 * algorithm.  Here a deliberately small fake kernel keeps a complete PMU
 * image and exposes only the bridge/backend observations consumed by the
 * public transport.  That lets the test couple TCP failures to owner-bound
 * cleanup without adding test hooks to production sources.
 */

static int failures;

#define CHECK(condition, message)                                           \
    do {                                                                    \
        if(!(condition))                                                    \
        {                                                                   \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__);   \
            ++failures;                                                     \
        }                                                                   \
    } while(0)

struct fake_kernel
{
    struct vd_pmu_snapshot initial;
    struct vd_pmu_snapshot current;
    struct vd_pmu_snapshot saved;
    uint64_t next_lease_token;
    uint32_t saved_valid;
    uint32_t backend_ready;
    uint32_t recovery_pending;
    uint32_t restore_obligation;
    uint32_t hold_exact_restore_proof;
    uint32_t expire_on_watchdog;
    uint32_t release_failures;
    uint32_t acquire_calls;
    uint32_t release_calls;
    uint32_t restore_calls;
    uint32_t exact_restore_count;
};

static struct fake_kernel fake_kernel;

struct fake_owner
{
    struct vd_pmu_profiler_owner_identity identity;
    uint32_t process_alive;
    uint32_t thread_alive;
    uint32_t query_unknown;
    uint32_t capture_calls;
    uint32_t query_calls;
    uint32_t release_calls;
    uint32_t release_failures;
};

enum fake_send_mode
{
    FAKE_SEND_OK = 0,
    FAKE_SEND_DISCONNECT = 1,
    FAKE_SEND_ERROR = 2,
    FAKE_SEND_TIMEOUT = 3,
};

struct fake_socket
{
    uint64_t now_ms;
    enum fake_send_mode send_mode;
    int disconnect_error;
    int generic_error;
    uint32_t open_calls;
    uint32_t send_calls;
    uint32_t wait_calls;
    uint32_t shutdown_calls;
    uint32_t close_calls;
};

static void fill_snapshot(struct vd_pmu_snapshot* snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->event_counter_count =
        VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS;
    snapshot->raw_pmcr = UINT32_C(0x41002000);
    snapshot->raw_pmcntenset = 0;
    snapshot->raw_pmovsr = 0;
    snapshot->raw_pmselr = 2;
    snapshot->raw_pmccntr = UINT32_C(0x12345678);
    snapshot->raw_pmuserenr = 0;
    snapshot->raw_pmintenset = 0;
    for(uint32_t i = 0; i < VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS; ++i)
    {
        snapshot->raw_pmxevtyper[i] = UINT32_C(0x20) + i;
        snapshot->raw_pmxevcntr[i] = UINT32_C(0x1000) + i;
    }
}

static void reset_fake_kernel(void)
{
    memset(&fake_kernel, 0, sizeof(fake_kernel));
    fill_snapshot(&fake_kernel.initial);
    fake_kernel.current = fake_kernel.initial;
    fake_kernel.next_lease_token = UINT64_C(0x1020304050607080);
    fake_kernel.backend_ready = 1;
}

static int exact_registers_restored(void)
{
    return memcmp(&fake_kernel.current, &fake_kernel.initial,
                  sizeof(fake_kernel.current)) == 0;
}

static int fake_restore_hardware(struct vd_pmu_profiler_bridge* bridge,
                                 int auto_restored)
{
    ++fake_kernel.restore_calls;
    if(fake_kernel.saved_valid == 0)
        return VP_ERROR_RESTORE_REQUIRED;

    fake_kernel.current = fake_kernel.saved;
    if(memcmp(&fake_kernel.current, &fake_kernel.initial,
              sizeof(fake_kernel.current)) != 0)
    {
        fake_kernel.restore_obligation = 1;
        bridge->session.state = VD_PMU_SESSION_RESTORE_PENDING;
        bridge->last_kernel_error = VD_PMU_BACKEND_ERROR_RESTORE;
        return VP_ERROR_RESTORE_REQUIRED;
    }

    ++fake_kernel.exact_restore_count;
    bridge->session.state = VD_PMU_SESSION_EMPTY;
    bridge->last_kernel_error = 0;
    if(auto_restored)
    {
        bridge->auto_restored_token = bridge->active_token;
        bridge->provider_lease_held = 1;
    }
    else
    {
        bridge->auto_restored_token = 0;
        bridge->provider_lease_held = 0;
    }

    fake_kernel.restore_obligation =
        fake_kernel.hold_exact_restore_proof != 0;
    fake_kernel.recovery_pending =
        fake_kernel.hold_exact_restore_proof != 0;
    fake_kernel.saved_valid = 0;
    return VP_RESULT_OK;
}

static int fake_provider_acquire(void* user,
                                 const struct vp_pmu_config* config,
                                 uint64_t* lease_token)
{
    struct vd_pmu_profiler_bridge* bridge =
        (struct vd_pmu_profiler_bridge*)user;
    ++fake_kernel.acquire_calls;
    if(!config || !lease_token || config->counter_mask !=
           VD_PMU_PROFILER_BRIDGE_COUNTER_MASK ||
       config->event_count != 1 || fake_kernel.saved_valid != 0)
        return VP_ERROR_STATE;

    fake_kernel.saved = fake_kernel.current;
    fake_kernel.saved_valid = 1;
    fake_kernel.current.raw_pmselr =
        VD_KERNEL_PMU_PROFILER_FIXED_COUNTER;
    fake_kernel.current.raw_pmxevtyper[
        VD_KERNEL_PMU_PROFILER_FIXED_COUNTER] = config->event_codes[0];
    fake_kernel.current.raw_pmxevcntr[
        VD_KERNEL_PMU_PROFILER_FIXED_COUNTER] = 0;
    fake_kernel.current.raw_pmcntenset |=
        UINT32_C(1) << VD_KERNEL_PMU_PROFILER_FIXED_COUNTER;
    fake_kernel.current.raw_pmcr |= VD_PMU_CONFIGURATION_ENABLE_GLOBAL;

    ++fake_kernel.next_lease_token;
    if(fake_kernel.next_lease_token == 0)
        ++fake_kernel.next_lease_token;
    *lease_token = fake_kernel.next_lease_token;
    bridge->active_token = *lease_token;
    bridge->active_event_code = config->event_codes[0];
    bridge->provider_lease_held = 1;
    bridge->session.state = VD_PMU_SESSION_ACTIVE;
    return VP_RESULT_OK;
}

static int fake_provider_read(void* user, uint64_t lease_token,
                              struct vp_pmu_sample* sample)
{
    struct vd_pmu_profiler_bridge* bridge =
        (struct vd_pmu_profiler_bridge*)user;
    if(!sample || lease_token != bridge->active_token ||
       bridge->session.state != VD_PMU_SESSION_ACTIVE)
        return VP_ERROR_STATE;
    memset(sample, 0, sizeof(*sample));
    sample->counter_mask = VD_PMU_PROFILER_BRIDGE_COUNTER_MASK;
    sample->events[0] = UINT64_C(73);
    return VP_RESULT_OK;
}

static int fake_provider_release(void* user, uint64_t lease_token)
{
    struct vd_pmu_profiler_bridge* bridge =
        (struct vd_pmu_profiler_bridge*)user;
    ++fake_kernel.release_calls;
    if(lease_token != bridge->active_token)
        return VP_ERROR_STATE;
    if(bridge->auto_restored_token != 0)
    {
        bridge->auto_restored_token = 0;
        bridge->provider_lease_held = 0;
        bridge->active_token = 0;
        return VP_RESULT_OK;
    }
    if(fake_kernel.release_failures != 0)
    {
        --fake_kernel.release_failures;
        fake_kernel.restore_obligation = 1;
        fake_kernel.recovery_pending = 1;
        bridge->session.state = VD_PMU_SESSION_RESTORE_PENDING;
        bridge->last_kernel_error =
            VD_PMU_SESSION_ERROR_RESTORE_REQUIRED;
        return VP_ERROR_RESTORE_REQUIRED;
    }
    return fake_restore_hardware(bridge, 0);
}

int vdPmuProfilerBridgeInit(
    struct vd_pmu_profiler_bridge* bridge,
    const struct vd_pmu_profiler_bridge_config* config)
{
    if(!bridge || !config)
        return VP_ERROR_INVALID_ARGUMENT;
    memset(bridge, 0, sizeof(*bridge));
    bridge->initialization_cookie =
        VD_PMU_PROFILER_BRIDGE_INITIALIZATION_COOKIE;
    bridge->lease_ms = config->lease_ms;
    bridge->config_flags = config->flags;
    bridge->owner.owner_pid = config->owner_pid;
    bridge->owner.owner_token = config->owner_token;
    bridge->owner.core_id = config->core_id;
    bridge->session.state = VD_PMU_SESSION_EMPTY;
    bridge->provider.abi_version = VP_PMU_PROVIDER_ABI_VERSION;
    bridge->provider.flags = VP_PMU_PROVIDER_FLAG_EXACT_RESTORE;
    bridge->provider.acquire = fake_provider_acquire;
    bridge->provider.read = fake_provider_read;
    bridge->provider.release = fake_provider_release;
    bridge->provider.user = bridge;
    return VP_RESULT_OK;
}

const struct vp_pmu_provider* vdPmuProfilerBridgeGetProvider(
    const struct vd_pmu_profiler_bridge* bridge)
{
    if(!bridge || bridge->initialization_cookie !=
           VD_PMU_PROFILER_BRIDGE_INITIALIZATION_COOKIE)
        return NULL;
    return &bridge->provider;
}

int vdPmuProfilerBridgeGetStatus(
    const struct vd_pmu_profiler_bridge* bridge,
    struct vd_pmu_profiler_bridge_status* status)
{
    if(!bridge || !status)
        return VP_ERROR_INVALID_ARGUMENT;
    memset(status, 0, sizeof(*status));
    status->struct_size = sizeof(*status);
    status->abi_version = VD_PMU_PROFILER_BRIDGE_ABI_VERSION;
    status->initialized = bridge->initialization_cookie ==
        VD_PMU_PROFILER_BRIDGE_INITIALIZATION_COOKIE;
    status->session_state = (uint32_t)bridge->session.state;
    status->orphan_restore_pending = bridge->orphan_restore_pending;
    status->auto_restored_waiting_release =
        bridge->auto_restored_token != 0;
    status->last_kernel_error = bridge->last_kernel_error;
    status->provider_lease_held = bridge->provider_lease_held;
    status->active_event_code = bridge->active_event_code;
    return VP_RESULT_OK;
}

int vdPmuProfilerBridgeWatchdog(
    struct vd_pmu_profiler_bridge* bridge)
{
    if(!bridge)
        return VP_ERROR_INVALID_ARGUMENT;
    if(bridge->session.state == VD_PMU_SESSION_RESTORE_PENDING)
    {
        const int result = fake_restore_hardware(bridge, 0);
        return result == VP_RESULT_OK ?
            VD_PMU_SESSION_WATCHDOG_RESTORED : result;
    }
    if(bridge->session.state == VD_PMU_SESSION_ACTIVE &&
       fake_kernel.expire_on_watchdog != 0)
    {
        fake_kernel.expire_on_watchdog = 0;
        const int result = fake_restore_hardware(bridge, 1);
        return result == VP_RESULT_OK ?
            VD_PMU_SESSION_WATCHDOG_RESTORED : result;
    }
    return VP_RESULT_OK;
}

int vdPmuProfilerBridgeRetryOrphanRestore(
    struct vd_pmu_profiler_bridge* bridge)
{
    if(!bridge || bridge->orphan_restore_pending == 0)
        return VP_RESULT_OK;
    const int result = fake_restore_hardware(bridge, 0);
    if(result == VP_RESULT_OK)
        bridge->orphan_restore_pending = 0;
    return result;
}

int vdPmuBackendReady(void)
{
    return fake_kernel.backend_ready != 0;
}

int vdPmuBackendHasRestoreObligation(void)
{
    return fake_kernel.restore_obligation != 0;
}

int vdPmuBackendRecoveryPending(void)
{
    return fake_kernel.recovery_pending != 0;
}

int vdPmuBackendSnapshotIdle(
    uint32_t core_id, struct vd_pmu_snapshot* snapshot)
{
    if(core_id != VD_KERNEL_PMU_PROFILER_FIXED_CORE || !snapshot)
        return VD_PMU_BACKEND_ERROR_INVALID;
    if(!vdPmuBackendReady() || vdPmuBackendRecoveryPending() ||
       vdPmuBackendHasRestoreObligation())
        return VD_PMU_BACKEND_ERROR_BUSY;
    *snapshot = fake_kernel.current;
    return 0;
}

int vdPmuBackendRecover(void)
{
    if(fake_kernel.restore_obligation != 0 ||
       fake_kernel.recovery_pending != 0)
        return VD_PMU_BACKEND_ERROR_RESTORE;
    return 0;
}

static void fake_owner_init(struct fake_owner* owner)
{
    memset(owner, 0, sizeof(*owner));
    owner->process_alive = 1;
    owner->thread_alive = 1;
    owner->identity.retained_thread_object =
        (uintptr_t)UINT32_C(0x81001000);
    owner->identity.thread_entry =
        (uintptr_t)UINT32_C(0x81100000);
    owner->identity.thread_stack =
        (uintptr_t)UINT32_C(0x82000000);
    owner->identity.thread_stack_size = UINT32_C(0x4000);
    owner->identity.initial_priority = 0x40;
    owner->identity.initial_affinity = 1;
    owner->identity.thread_attributes = UINT32_C(0x10000000);
}

static int fake_owner_capture(
    void* context, int32_t owner_pid, int32_t owner_thread,
    struct vd_pmu_profiler_owner_identity* identity)
{
    struct fake_owner* owner = (struct fake_owner*)context;
    ++owner->capture_calls;
    *identity = owner->identity;
    identity->process_id = owner_pid;
    identity->thread_id = owner_thread;
    return 0;
}

static int fake_owner_query(
    void* context, int32_t owner_pid, int32_t owner_thread,
    const struct vd_pmu_profiler_owner_identity* identity)
{
    struct fake_owner* owner = (struct fake_owner*)context;
    ++owner->query_calls;
    if(owner->query_unknown != 0)
        return VD_PMU_PROFILER_OWNER_UNKNOWN;
    if(owner->process_alive == 0 || owner->thread_alive == 0)
        return VD_PMU_PROFILER_OWNER_GONE;
    struct vd_pmu_profiler_owner_identity expected = owner->identity;
    expected.process_id = owner_pid;
    expected.thread_id = owner_thread;
    return memcmp(&expected, identity, sizeof(expected)) == 0 ?
        VD_PMU_PROFILER_OWNER_ALIVE : VD_PMU_PROFILER_OWNER_UNKNOWN;
}

static int fake_owner_release(
    void* context, int32_t owner_pid, int32_t owner_thread,
    const struct vd_pmu_profiler_owner_identity* identity)
{
    struct fake_owner* owner = (struct fake_owner*)context;
    ++owner->release_calls;
    struct vd_pmu_profiler_owner_identity expected = owner->identity;
    expected.process_id = owner_pid;
    expected.thread_id = owner_thread;
    if(!identity || memcmp(&expected, identity, sizeof(expected)) != 0)
        return -1;
    if(owner->release_failures != 0)
    {
        --owner->release_failures;
        return -1;
    }
    return 0;
}

static struct vd_pmu_profiler_owner_backend owner_backend(
    struct fake_owner* owner)
{
    const struct vd_pmu_profiler_owner_backend backend = {
        .context = owner,
        .capture = fake_owner_capture,
        .query = fake_owner_query,
        .release = fake_owner_release,
        .release_terminal = fake_owner_release,
    };
    return backend;
}

static uint64_t fake_socket_now(void* context)
{
    return ((struct fake_socket*)context)->now_ms;
}

static int fake_socket_open(void* context, int* socket_out,
                            int* native_error)
{
    struct fake_socket* socket = (struct fake_socket*)context;
    ++socket->open_calls;
    *socket_out = 17;
    *native_error = 0;
    return VP_VITA_TCP_IO_OK;
}

static int fake_socket_connect_start(
    void* context, int socket,
    const struct vp_vita_tcp_endpoint* endpoint,
    int* native_error)
{
    (void)context;
    (void)socket;
    (void)endpoint;
    *native_error = 0;
    return VP_VITA_TCP_IO_OK;
}

static int fake_socket_connect_finish(void* context, int socket,
                                      int* native_error)
{
    (void)context;
    (void)socket;
    *native_error = 0;
    return VP_VITA_TCP_IO_OK;
}

static int fake_socket_wait(void* context, int socket,
                            uint32_t timeout_ms, int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)context;
    (void)socket;
    ++fake->wait_calls;
    fake->now_ms += timeout_ms;
    *native_error = 0;
    return VP_VITA_TCP_IO_TIMEOUT;
}

static int fake_socket_send(void* context, int socket,
                            const uint8_t* data, size_t size,
                            size_t* bytes_sent, int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)context;
    (void)socket;
    (void)data;
    ++fake->send_calls;
    *bytes_sent = 0;
    *native_error = 0;
    switch(fake->send_mode)
    {
        case FAKE_SEND_OK:
            *bytes_sent = size;
            return VP_VITA_TCP_IO_OK;
        case FAKE_SEND_DISCONNECT:
            *native_error = fake->disconnect_error;
            return VP_VITA_TCP_IO_ERROR;
        case FAKE_SEND_ERROR:
            *native_error = fake->generic_error;
            return VP_VITA_TCP_IO_ERROR;
        case FAKE_SEND_TIMEOUT:
            return VP_VITA_TCP_IO_WOULD_BLOCK;
        default:
            return VP_VITA_TCP_IO_ERROR;
    }
}

static int fake_socket_shutdown(void* context, int socket,
                                int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)context;
    (void)socket;
    ++fake->shutdown_calls;
    *native_error = 0;
    return VP_VITA_TCP_IO_OK;
}

static int fake_socket_close(void* context, int socket,
                             int* native_error)
{
    struct fake_socket* fake = (struct fake_socket*)context;
    (void)socket;
    ++fake->close_calls;
    *native_error = 0;
    return VP_VITA_TCP_IO_OK;
}

static void open_fake_socket(struct fake_socket* fake,
                             struct vp_vita_tcp_sink* sink,
                             enum fake_send_mode send_mode)
{
    struct vp_vita_tcp_sink_config config;
    memset(fake, 0, sizeof(*fake));
    memset(sink, 0, sizeof(*sink));
    fake->now_ms = 100;
    fake->send_mode = send_mode;
    fake->disconnect_error = -104;
    fake->generic_error = -5;
    vp_vita_tcp_sink_config_init(&config);
    config.endpoint.ipv4[0] = 192;
    config.endpoint.ipv4[1] = 0;
    config.endpoint.ipv4[2] = 2;
    config.endpoint.ipv4[3] = 1;
    config.endpoint.port = 18195;
    config.ops.now_ms = fake_socket_now;
    config.ops.open = fake_socket_open;
    config.ops.connect_start = fake_socket_connect_start;
    config.ops.connect_finish = fake_socket_connect_finish;
    config.ops.wait_writable = fake_socket_wait;
    config.ops.send = fake_socket_send;
    config.ops.shutdown_write = fake_socket_shutdown;
    config.ops.close = fake_socket_close;
    config.ops_user = fake;
    config.send_timeout_ms = 10;
    config.wait_slice_ms = 10;
    CHECK(vp_vita_tcp_sink_init(sink, &config) == VP_RESULT_OK &&
              vp_vita_tcp_sink_connect(sink) == VP_RESULT_OK,
          "fake TCP receiver connects");
}

static struct vd_kernel_pmu_profiler_open_request request_for(
    uint32_t event_code)
{
    struct vd_kernel_pmu_profiler_open_request request;
    memset(&request, 0, sizeof(request));
    request.struct_size = sizeof(request);
    request.abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION;
    request.event_code = event_code;
    request.lease_ms = VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS;
    request.flags = VD_KERNEL_PMU_PROFILER_OPEN_ACK_REAL_EVENT;
    return request;
}

static void start_transport(
    struct vd_pmu_profiler_transport* transport,
    struct fake_owner* owner,
    int32_t owner_pid, int32_t owner_thread,
    uint32_t event_code,
    struct vd_kernel_pmu_profiler_handle* handle)
{
    reset_fake_kernel();
    fake_owner_init(owner);
    memset(transport, 0, sizeof(*transport));
    const struct vd_pmu_profiler_owner_backend backend =
        owner_backend(owner);
    const struct vd_kernel_pmu_profiler_open_request request =
        request_for(event_code);
    CHECK(vdPmuProfilerTransportInit(transport) == 0 &&
              vdPmuProfilerTransportSetOwnerBackend(
                  transport, &backend) == 0 &&
              vdPmuProfilerTransportOpen(
                  transport, owner_pid, owner_thread,
                  &request, handle) == 0,
          "fake-kernel transport opens one real-event lease");
    CHECK(!exact_registers_restored() &&
              transport->state == VD_PMU_PROFILER_TRANSPORT_ACTIVE,
          "open mutates only while an owner-bound lease is active");
}

static int open_next_event(
    struct vd_pmu_profiler_transport* transport,
    int32_t owner_pid, int32_t owner_thread,
    uint32_t event_code,
    struct vd_kernel_pmu_profiler_handle* handle)
{
    const struct vd_kernel_pmu_profiler_open_request request =
        request_for(event_code);
    return vdPmuProfilerTransportOpen(
        transport, owner_pid, owner_thread, &request, handle);
}

static void test_disconnect_closes_and_rearms(void)
{
    static const uint8_t payload[] = { 1, 2, 3, 4 };
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_handle handle;
    struct fake_owner owner;
    struct fake_socket socket;
    struct vp_vita_tcp_sink sink;
    struct vp_vita_tcp_sink_stats stats;
    const int32_t owner_pid = 101;
    const int32_t owner_thread = 103;

    start_transport(&transport, &owner, owner_pid, owner_thread,
                    VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS, &handle);
    open_fake_socket(&socket, &sink, FAKE_SEND_DISCONNECT);
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
              VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == 0 &&
              stats.failure == VP_VITA_TCP_FAILURE_SEND &&
              stats.last_native_error == socket.disconnect_error &&
              socket.shutdown_calls == 1 && socket.close_calls == 1,
          "peer disconnect is a bounded send failure with closed socket");
    CHECK(transport.state == VD_PMU_PROFILER_TRANSPORT_ACTIVE &&
              !exact_registers_restored(),
          "TCP cleanup alone never claims PMU restoration");
    CHECK(vdPmuProfilerTransportClose(
              &transport, owner_pid, owner_thread, &handle) == 0 &&
              exact_registers_restored() &&
              fake_kernel.exact_restore_count == 1 &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              transport.real_event_attempted == 0 &&
              transport.rearm_count == 1,
          "authenticated disconnect cleanup restores exactly and re-arms");
    CHECK(open_next_event(
              &transport, owner_pid, owner_thread,
              VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS, &handle) == 0 &&
              vdPmuProfilerTransportClose(
                  &transport, owner_pid, owner_thread, &handle) == 0 &&
              transport.rearm_count == 2 && exact_registers_restored(),
          "a post-disconnect real event is accepted only after proven close");
}

static void test_send_timeout_then_process_exit(void)
{
    static const uint8_t payload[] = { 9, 8, 7 };
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_handle handle;
    struct fake_owner owner;
    struct fake_socket socket;
    struct vp_vita_tcp_sink sink;
    struct vp_vita_tcp_sink_stats stats;
    const int32_t owner_pid = 107;
    const int32_t owner_thread = 109;

    start_transport(&transport, &owner, owner_pid, owner_thread,
                    VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS, &handle);
    open_fake_socket(&socket, &sink, FAKE_SEND_TIMEOUT);
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
              VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == 0 &&
              stats.failure == VP_VITA_TCP_FAILURE_SEND_TIMEOUT &&
              socket.wait_calls == 1 && socket.close_calls == 1,
          "unwritable peer reaches the finite send deadline and closes");
    owner.process_alive = 0;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 1 &&
              owner.query_calls == 1 && owner.release_calls == 1 &&
              exact_registers_restored() &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              transport.real_event_attempted == 0 &&
              transport.rearm_count == 1,
          "proven process exit restores the abandoned lease and re-arms");
    CHECK(vdPmuProfilerTransportClose(
              &transport, owner_pid, owner_thread, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_OWNER,
          "process-exit recovery permanently retires the abandoned handle");
}

static void test_send_error_then_thread_exit(void)
{
    static const uint8_t payload[] = { 5, 4, 3 };
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_handle handle;
    struct fake_owner owner;
    struct fake_socket socket;
    struct vp_vita_tcp_sink sink;
    struct vp_vita_tcp_sink_stats stats;
    const int32_t owner_pid = 113;
    const int32_t owner_thread = 127;

    start_transport(&transport, &owner, owner_pid, owner_thread,
                    VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS, &handle);
    open_fake_socket(&socket, &sink, FAKE_SEND_ERROR);
    CHECK(vp_vita_tcp_sink_write(&sink, payload, sizeof(payload)) ==
              VP_ERROR_IO &&
              vp_vita_tcp_sink_get_stats(&sink, &stats) == 0 &&
              stats.failure == VP_VITA_TCP_FAILURE_SEND &&
              stats.last_native_error == socket.generic_error,
          "generic send error is retained as explicit TCP evidence");
    owner.thread_alive = 0;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 1 &&
              owner.query_calls == 1 && owner.release_calls == 1 &&
              exact_registers_restored() &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              transport.rearm_count == 1,
          "proven controller-thread exit restores and retires its lease");
}

static void test_conflict_and_exact_restore_boundary(void)
{
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_handle handle;
    struct vd_kernel_pmu_profiler_handle other_handle;
    struct vd_kernel_pmu_profiler_sample sample;
    struct fake_owner owner;
    const int32_t owner_pid = 131;
    const int32_t owner_thread = 137;

    start_transport(&transport, &owner, owner_pid, owner_thread,
                    VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT,
                    &handle);
    CHECK(open_next_event(
              &transport, owner_pid + 1, owner_thread + 1,
              VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS,
              &other_handle) == VD_KERNEL_ERROR_PMU_PROFILER_BUSY &&
              vdPmuProfilerTransportRead(
                  &transport, owner_pid + 1, owner_thread,
                  &handle, &sample) == VD_KERNEL_ERROR_PMU_PROFILER_OWNER &&
              vdPmuProfilerTransportClose(
                  &transport, owner_pid, owner_thread + 1,
                  &handle) == VD_KERNEL_ERROR_PMU_PROFILER_OWNER &&
              fake_kernel.restore_calls == 0 &&
              !exact_registers_restored(),
          "competing process/thread cannot read, close, or steal a lease");

    fake_kernel.hold_exact_restore_proof = 1;
    CHECK(vdPmuProfilerTransportClose(
              &transport, owner_pid, owner_thread, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
              exact_registers_restored() &&
              fake_kernel.restore_obligation != 0 &&
              transport.state ==
                  VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED &&
              transport.real_event_attempted == 1,
          "matching registers without independent backend proof cannot re-arm");
    CHECK(open_next_event(
              &transport, owner_pid, owner_thread,
              VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS,
              &other_handle) ==
                  VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
              vdPmuProfilerTransportWatchdog(&transport) ==
                  VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED,
          "pending exact-restore evidence fails Open closed and blocks watchdog retirement");

    fake_kernel.hold_exact_restore_proof = 0;
    fake_kernel.restore_obligation = 0;
    fake_kernel.recovery_pending = 0;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 1 &&
              transport.state ==
                  VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER &&
              transport.exact_restore_proven == 1 &&
              transport.real_event_attempted == 1,
          "independent exact-restore proof still awaits a terminal owner event");

    struct vd_kernel_pmu_profiler_handle wrong = handle;
    ++wrong.generation;
    CHECK(vdPmuProfilerTransportClose(
              &transport, owner_pid, owner_thread, &wrong) ==
              VD_KERNEL_ERROR_PMU_PROFILER_OWNER &&
              open_next_event(
                  &transport, owner_pid, owner_thread,
                  VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS,
                  &other_handle) == VD_KERNEL_ERROR_PMU_PROFILER_BUSY,
          "wrong generation cannot acknowledge restoration or permit re-arm");
    owner.query_unknown = 1;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 0 &&
              transport.state ==
                  VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER,
          "unknown owner status fails closed after exact restoration");
    owner.query_unknown = 0;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 0 &&
              vdPmuProfilerTransportClose(
                  &transport, owner_pid, owner_thread, &handle) == 0 &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              transport.real_event_attempted == 0 &&
              transport.rearm_count == 1,
          "only the exact owner/generation acknowledgement grants re-arm");
}

static void test_timeout_quarantine_and_gone_owner_acceptance(void)
{
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_handle handle;
    struct vd_kernel_pmu_profiler_handle next;
    struct fake_owner owner;
    const int32_t owner_pid = 139;
    const int32_t owner_thread = 149;

    start_transport(&transport, &owner, owner_pid, owner_thread,
                    VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS, &handle);
    owner.query_unknown = 1;
    fake_kernel.expire_on_watchdog = 1;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 1 &&
              exact_registers_restored() &&
              transport.state ==
                  VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER &&
              transport.exact_restore_proven == 1 &&
              transport.real_event_attempted == 1,
          "lease timeout restores exactly but cannot itself authorize re-arm");
    CHECK(open_next_event(
              &transport, owner_pid, owner_thread,
              VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS, &next) ==
              VD_KERNEL_ERROR_PMU_PROFILER_BUSY &&
              vdPmuProfilerTransportWatchdog(&transport) == 0,
          "unknown owner after timeout remains quarantined");
    owner.query_unknown = 0;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 0,
          "a still-live owner cannot be inferred dead from timeout");
    owner.identity.retained_thread_object += (uintptr_t)UINT32_C(0x1000);
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 0 &&
              transport.state ==
                  VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER &&
              transport.real_event_attempted == 1,
          "UID/object-identity collision is unknown and cannot authorize re-arm");
    owner.identity.retained_thread_object -= (uintptr_t)UINT32_C(0x1000);
    owner.thread_alive = 0;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 1 &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              transport.real_event_attempted == 0 &&
              transport.rearm_count == 1 && owner.release_calls == 1,
          "exact restore plus proven owner exit accepts safe re-arm");
    CHECK(open_next_event(
              &transport, owner_pid, owner_thread,
              VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS, &next) == 0,
          "next real event is admitted after both proofs");
    owner.thread_alive = 1;
    CHECK(vdPmuProfilerTransportClose(
              &transport, owner_pid, owner_thread, &next) == 0,
          "accepted follow-up lease also restores exactly");
}

static void test_gone_owner_cannot_override_restore_obligation(void)
{
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_handle handle;
    struct vd_kernel_pmu_profiler_handle next;
    struct fake_owner owner;
    const int32_t owner_pid = 163;
    const int32_t owner_thread = 167;

    start_transport(&transport, &owner, owner_pid, owner_thread,
                    VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT,
                    &handle);
    fake_kernel.hold_exact_restore_proof = 1;
    owner.process_alive = 0;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
              exact_registers_restored() &&
              fake_kernel.restore_obligation != 0 &&
              transport.state ==
                  VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED &&
              transport.real_event_attempted == 1 &&
              owner.release_calls == 0,
          "owner exit alone cannot override missing exact-restore proof");
    CHECK(open_next_event(
              &transport, owner_pid, owner_thread,
              VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS, &next) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
              vdPmuProfilerTransportWatchdog(&transport) ==
                  VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED,
          "gone owner fails Open closed while backend evidence is pending");

    fake_kernel.hold_exact_restore_proof = 0;
    fake_kernel.restore_obligation = 0;
    fake_kernel.recovery_pending = 0;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 1 &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              transport.real_event_attempted == 0 &&
              transport.rearm_count == 1 &&
              owner.release_calls == 1,
          "gone-owner re-arm waits for independent exact-restore evidence");
}

static void test_uncertain_owner_reference_permanently_refuses_rearm(void)
{
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_handle handle;
    struct vd_kernel_pmu_profiler_handle next;
    struct fake_owner owner;
    const int32_t owner_pid = 151;
    const int32_t owner_thread = 157;

    start_transport(&transport, &owner, owner_pid, owner_thread,
                    VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS, &handle);
    fake_kernel.expire_on_watchdog = 1;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 1 &&
              transport.state ==
                  VD_PMU_PROFILER_TRANSPORT_RESTORED_AWAITING_OWNER,
          "owner-reference failure fixture reaches restored quarantine");
    owner.release_failures = 1;
    CHECK(vdPmuProfilerTransportClose(
              &transport, owner_pid, owner_thread, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
              transport.owner_identity_release_uncertain == 1 &&
              transport.real_event_attempted == 1,
          "uncertain retained-owner release refuses re-arm");
    owner.process_alive = 0;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED,
          "uncertain identity release cannot become owner-exit proof");
    CHECK(vdPmuProfilerTransportClose(
              &transport, owner_pid, owner_thread, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED,
          "uncertain identity release cannot be acknowledged twice");
    CHECK(open_next_event(
              &transport, owner_pid, owner_thread,
              VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS, &next) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED,
          "uncertain identity release permanently fails new leases closed");
    CHECK(vdPmuProfilerTransportShutdown(&transport) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED,
          "uncertain identity release keeps the module resident");
    CHECK(owner.release_calls == 1,
          "uncertain retained-object release is never retried blindly");
    CHECK(exact_registers_restored(),
          "identity-reference quarantine does not undo exact PMU restoration");
}

int main(void)
{
    test_disconnect_closes_and_rearms();
    test_send_timeout_then_process_exit();
    test_send_error_then_thread_exit();
    test_conflict_and_exact_restore_boundary();
    test_timeout_quarantine_and_gone_owner_acceptance();
    test_gone_owner_cannot_override_restore_obligation();
    test_uncertain_owner_reference_permanently_refuses_rearm();

    if(failures != 0)
    {
        fprintf(stderr, "%d PMU failure-matrix check(s) failed\n",
                failures);
        return 1;
    }
    puts("PASS: PMU/TCP failure recovery and safe-rearm matrix");
    return 0;
}

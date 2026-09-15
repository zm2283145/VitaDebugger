#include "pmu_session.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MOCK_ERROR_NOW (-701)
#define MOCK_ERROR_SNAPSHOT (-702)
#define MOCK_ERROR_CONFIGURE (-703)
#define MOCK_ERROR_READ (-704)
#define MOCK_ERROR_RESTORE (-705)

static int failures;

static void check(int condition, const char* message)
{
    if(condition)
        return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

struct mock_backend {
    struct vd_pmu_snapshot hardware;
    struct vd_pmu_configuration last_configuration;
    struct vd_pmu_counter_values values;
    uint64_t now_ms;
    uint32_t configure_advance_ms;
    uint32_t read_advance_ms;
    uint32_t last_core;
    int now_calls;
    int snapshot_calls;
    int configure_calls;
    int read_calls;
    int restore_calls;
    int fail_now_call;
    int fail_snapshot_call;
    int fail_configure_call;
    int fail_read_call;
    int fail_restore_call;
    int positive_now_call;
    int positive_snapshot_call;
    int positive_configure_call;
    int positive_read_call;
    int positive_restore_call;
    int corrupt_snapshot_call;
};

static struct vd_pmu_snapshot original_state(void)
{
    struct vd_pmu_snapshot state;
    memset(&state, 0, sizeof(state));
    state.event_counter_count = VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS;
    /* PMCR.N == 6 exactly: available indices are 0 through 5. */
    state.raw_pmcr = 0x41003001u;
    state.raw_pmcntenset = 0u;
    state.raw_pmovsr = 0u;
    state.raw_pmselr = 4u;
    state.raw_pmccntr = 0x12345678u;
    state.raw_pmuserenr = 1u;
    state.raw_pmintenset = 0u;
    for(uint32_t i = 0; i < VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS; ++i)
    {
        state.raw_pmxevtyper[i] = 0x40u + i;
        state.raw_pmxevcntr[i] = 0x1000u + i;
    }
    return state;
}

static struct mock_backend make_mock(void)
{
    struct mock_backend mock;
    memset(&mock, 0, sizeof(mock));
    mock.hardware = original_state();
    mock.now_ms = 1000u;
    mock.values.cycles = 0xabcdef01u;
    mock.values.events[0] = 10u;
    mock.values.events[1] = 20u;
    mock.values.events[2] = 30u;
    mock.values.events[3] = 40u;
    return mock;
}

static int mock_now(void* context, uint64_t* now_ms)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    mock->now_calls++;
    if(mock->fail_now_call == mock->now_calls)
        return MOCK_ERROR_NOW;
    if(mock->positive_now_call == mock->now_calls)
        return 1;
    *now_ms = mock->now_ms;
    return 0;
}

static int mock_snapshot(void* context, uint32_t core_id,
                         struct vd_pmu_snapshot* snapshot)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    mock->snapshot_calls++;
    mock->last_core = core_id;
    if(mock->fail_snapshot_call == mock->snapshot_calls)
        return MOCK_ERROR_SNAPSHOT;
    if(mock->positive_snapshot_call == mock->snapshot_calls)
        return 1;
    *snapshot = mock->hardware;
    if(mock->corrupt_snapshot_call == mock->snapshot_calls)
        snapshot->raw_pmxevcntr[5] ^= 1u;
    return 0;
}

static int mock_configure(
    void* context, uint32_t core_id,
    const struct vd_pmu_configuration* configuration)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    mock->configure_calls++;
    mock->last_core = core_id;
    mock->last_configuration = *configuration;

    /* Deliberately mutate the selected lane before a possible injected
     * failure. Other lanes, PMCR reset commands, cycle state, IRQ ownership,
     * and W1C overflow state remain untouched. */
    if((configuration->control_flags &
        VD_PMU_CONFIGURATION_ENABLE_GLOBAL) != 0)
        mock->hardware.raw_pmcr |= UINT32_C(1);
    for(uint32_t i = 0; i < configuration->event_count; ++i)
    {
        const uint32_t counter = configuration->events[i].physical_counter;
        mock->hardware.raw_pmselr = counter;
        mock->hardware.raw_pmxevtyper[counter] =
            configuration->events[i].event_code;
        mock->hardware.raw_pmxevcntr[counter] = 0u;
        mock->hardware.raw_pmcntenset |= UINT32_C(1) << counter;
    }
    mock->now_ms += mock->configure_advance_ms;
    if(mock->fail_configure_call == mock->configure_calls)
        return MOCK_ERROR_CONFIGURE;
    if(mock->positive_configure_call == mock->configure_calls)
        return 1;
    return 0;
}

static int mock_read(void* context, uint32_t core_id,
                     const struct vd_pmu_configuration* configuration,
                     struct vd_pmu_counter_values* values)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    mock->read_calls++;
    mock->last_core = core_id;
    (void)configuration;
    if(mock->fail_read_call == mock->read_calls)
        return MOCK_ERROR_READ;
    if(mock->positive_read_call == mock->read_calls)
        return 1;
    *values = mock->values;
    mock->now_ms += mock->read_advance_ms;
    return 0;
}

static int mock_restore(void* context, uint32_t core_id,
                        const struct vd_pmu_configuration* configuration,
                        const struct vd_pmu_snapshot* snapshot)
{
    struct mock_backend* mock = (struct mock_backend*)context;
    mock->restore_calls++;
    mock->last_core = core_id;
    if(mock->fail_restore_call == mock->restore_calls)
        return MOCK_ERROR_RESTORE;
    if(mock->positive_restore_call == mock->restore_calls)
        return 1;
    const uint32_t counter = configuration->events[0].physical_counter;
    const uint32_t bit = UINT32_C(1) << counter;
    mock->hardware.raw_pmcntenset =
        (mock->hardware.raw_pmcntenset & ~bit) |
        (snapshot->raw_pmcntenset & bit);
    mock->hardware.raw_pmintenset =
        (mock->hardware.raw_pmintenset & ~bit) |
        (snapshot->raw_pmintenset & bit);
    mock->hardware.raw_pmovsr =
        (mock->hardware.raw_pmovsr & ~bit) | (snapshot->raw_pmovsr & bit);
    mock->hardware.raw_pmxevtyper[counter] =
        snapshot->raw_pmxevtyper[counter];
    mock->hardware.raw_pmxevcntr[counter] =
        snapshot->raw_pmxevcntr[counter];
    mock->hardware.raw_pmselr = snapshot->raw_pmselr;
    if((configuration->control_flags &
        VD_PMU_CONFIGURATION_ENABLE_GLOBAL) != 0)
    {
        mock->hardware.raw_pmcr =
            (mock->hardware.raw_pmcr & ~UINT32_C(1)) |
            (snapshot->raw_pmcr & UINT32_C(1));
    }
    return 0;
}

static struct vd_pmu_session_backend make_backend(struct mock_backend* mock)
{
    const struct vd_pmu_session_backend backend = {
        .context = mock,
        .snapshot = mock_snapshot,
        .configure = mock_configure,
        .read = mock_read,
        .restore = mock_restore,
        .now_ms = mock_now,
    };
    return backend;
}

static struct vd_pmu_session_owner owner(void)
{
    const struct vd_pmu_session_owner value = {
        .owner_pid = 0x1234,
        .owner_token = 0xa5a55a5au,
        .core_id = 2u,
    };
    return value;
}

static struct vd_pmu_session_request request(uint32_t lease_ms)
{
    struct vd_pmu_session_request value;
    memset(&value, 0, sizeof(value));
    value.struct_size = sizeof(value);
    value.abi_version = VD_PMU_SESSION_ABI_VERSION;
    value.lease_ms = lease_ms;
    value.flags = 0u;
    value.event_count = 1u;
    value.event_ids[0] = VD_PMU_EVENT_SOFTWARE_INCREMENT;
    return value;
}

static int callback_count(const struct mock_backend* mock)
{
    return mock->now_calls + mock->snapshot_calls + mock->configure_calls +
           mock->read_calls + mock->restore_calls;
}

static void test_allowlist(void)
{
    struct vd_pmu_event_metadata metadata;
    memset(&metadata, 0xa5, sizeof(metadata));
    check(vdPmuSessionLookupEvent(VD_PMU_EVENT_DCACHE_MISS, &metadata) == 0 &&
              metadata.event_id == VD_PMU_EVENT_DCACHE_MISS &&
              metadata.event_code == 0x03u &&
              metadata.physical_counter == 0u && metadata.reserved == 0u,
          "allowlist resolves stable event metadata");
    check(vdPmuSessionLookupEvent(VD_PMU_EVENT_SOFTWARE_INCREMENT,
                                  &metadata) == 0 &&
              metadata.event_code == 0u,
          "software increment is an explicit allowlisted gate event");

    const struct vd_pmu_event_metadata before = metadata;
    check(vdPmuSessionLookupEvent(0xfeedu, &metadata) ==
              VD_PMU_SESSION_ERROR_UNSUPPORTED_EVENT &&
              memcmp(&metadata, &before, sizeof(metadata)) == 0,
          "unknown event cannot inject raw PMU configuration metadata");
    check(vdPmuSessionLookupEvent(VD_PMU_EVENT_DCACHE_MISS, NULL) ==
              VD_PMU_SESSION_ERROR_INVALID,
          "allowlist rejects a missing destination");
}

static void expect_invalid_acquire(
    const struct vd_pmu_session_owner* test_owner,
    const struct vd_pmu_session_request* test_request,
    const struct vd_pmu_session_backend* backend,
    struct mock_backend* mock,
    const char* message)
{
    struct vd_pmu_session session = {0};
    vdPmuSessionInit(&session);
    const struct vd_pmu_session before = session;
    const int calls_before = callback_count(mock);
    uint64_t token = UINT64_C(0x1122334455667788);
    check(vdPmuSessionAcquire(&session, test_owner, test_request, backend,
                              &token) < 0 &&
              memcmp(&session, &before, sizeof(session)) == 0 &&
              callback_count(mock) == calls_before &&
              token == UINT64_C(0x1122334455667788),
          message);
}

static void test_invalid_requests_have_no_side_effects(void)
{
    struct mock_backend mock = make_mock();
    struct vd_pmu_session_backend backend = make_backend(&mock);
    struct vd_pmu_session_owner test_owner = owner();
    struct vd_pmu_session_request test_request = request(250u);

    test_request.lease_ms = 249u;
    expect_invalid_acquire(&test_owner, &test_request, &backend, &mock,
                           "lease shorter than 250 ms has zero side effects");
    test_request = request(5001u);
    expect_invalid_acquire(&test_owner, &test_request, &backend, &mock,
                           "lease longer than 5000 ms has zero side effects");
    test_request = request(250u);
    test_request.flags = 0x80000000u;
    expect_invalid_acquire(&test_owner, &test_request, &backend, &mock,
                           "unknown request flags have zero side effects");
    test_request = request(250u);
    test_request.flags = VD_PMU_SESSION_FLAG_CYCLES;
    expect_invalid_acquire(&test_owner, &test_request, &backend, &mock,
                           "cycle counter remains outside the first safe gate");
    test_request = request(250u);
    test_request.event_count = 2u;
    test_request.event_ids[1] = test_request.event_ids[0];
    expect_invalid_acquire(&test_owner, &test_request, &backend, &mock,
                           "duplicate events have zero side effects");
    test_request = request(250u);
    test_request.event_ids[0] = 0xfeedu;
    expect_invalid_acquire(&test_owner, &test_request, &backend, &mock,
                           "non-allowlisted event has zero backend effects");
    test_request = request(250u);
    test_request.event_ids[3] = VD_PMU_EVENT_MAIN_PIPE;
    expect_invalid_acquire(&test_owner, &test_request, &backend, &mock,
                           "nonzero unused event slot has zero side effects");
    test_request = request(250u);
    test_request.event_count = 0u;
    test_request.flags = 0u;
    test_request.event_ids[0] = 0u;
    test_request.event_ids[1] = 0u;
    expect_invalid_acquire(&test_owner, &test_request, &backend, &mock,
                           "empty counter request has zero side effects");
    test_request = request(250u);
    test_owner.core_id = VD_PMU_SESSION_PHYSICAL_CORE_COUNT;
    expect_invalid_acquire(&test_owner, &test_request, &backend, &mock,
                           "invalid core has zero side effects");
    test_owner = owner();
    backend.restore = NULL;
    expect_invalid_acquire(&test_owner, &test_request, &backend, &mock,
                           "incomplete backend has zero side effects");
}

static void test_success_owner_read_and_restore(void)
{
    struct vd_pmu_session session = {0};
    vdPmuSessionInit(&session);
    struct mock_backend mock = make_mock();
    const struct vd_pmu_snapshot original = mock.hardware;
    const struct vd_pmu_session_backend backend = make_backend(&mock);
    const struct vd_pmu_session_owner test_owner = owner();
    const struct vd_pmu_session_request test_request = request(5000u);
    uint64_t token = 0;

    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0 &&
              token != 0 && vdPmuSessionIsActive(&session) &&
              session.state == VD_PMU_SESSION_ACTIVE &&
              mock.last_core == test_owner.core_id &&
              mock.last_configuration.event_count == 1u &&
              mock.last_configuration.events[0].event_id ==
                  VD_PMU_EVENT_SOFTWARE_INCREMENT &&
              mock.last_configuration.events[0].event_code == 0x00u &&
              mock.last_configuration.events[0].physical_counter == 5u,
          "acquire binds one owner/core and selects clean counter 5");

    const int calls_before_busy = callback_count(&mock);
    uint64_t other_token = UINT64_C(0x55);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &other_token) == VD_PMU_SESSION_ERROR_BUSY &&
              callback_count(&mock) == calls_before_busy &&
              other_token == UINT64_C(0x55),
          "single active lease cannot be overwritten");

    struct vd_pmu_reading reading;
    memset(&reading, 0xa5, sizeof(reading));
    const struct vd_pmu_reading untouched = reading;
    struct vd_pmu_session_owner wrong_owner = test_owner;
    wrong_owner.owner_pid++;
    const int calls_before_wrong = callback_count(&mock);
    check(vdPmuSessionRead(&session, &wrong_owner, token, &reading) ==
              VD_PMU_SESSION_ERROR_OWNER &&
              callback_count(&mock) == calls_before_wrong &&
              memcmp(&reading, &untouched, sizeof(reading)) == 0,
          "read rejects the wrong owner before callbacks or output writes");
    check(vdPmuSessionRestore(&session, &test_owner, token + 1u) ==
              VD_PMU_SESSION_ERROR_OWNER &&
              callback_count(&mock) == calls_before_wrong,
          "restore rejects the wrong token before callbacks");

    check(vdPmuSessionRead(&session, &test_owner, token, &reading) == 0 &&
              reading.timestamp_ms == mock.now_ms &&
              reading.lease_token == token &&
              reading.flags == 0u && reading.cycles == 0u &&
              reading.event_count == 1u &&
              reading.events[0].metadata.event_id ==
                  VD_PMU_EVENT_SOFTWARE_INCREMENT &&
              reading.events[0].value == 10u,
          "read returns values paired with immutable event metadata");

    check(vdPmuSessionRestore(&session, &test_owner, token) == 0 &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "explicit restore verifies the exact original PMU snapshot");

    uint64_t next_token = 0;
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &next_token) == 0 &&
              next_token != token &&
              vdPmuSessionRestore(&session, &test_owner, next_token) == 0,
          "successive leases receive distinct tokens");
}

static void test_deadline_and_watchdog(void)
{
    struct vd_pmu_session session = {0};
    vdPmuSessionInit(&session);
    struct mock_backend mock = make_mock();
    const struct vd_pmu_snapshot original = mock.hardware;
    const struct vd_pmu_session_backend backend = make_backend(&mock);
    const struct vd_pmu_session_owner test_owner = owner();
    const struct vd_pmu_session_request test_request = request(250u);
    uint64_t token = 0;
    struct vd_pmu_reading reading;

    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "minimum lease duration is accepted");
    mock.now_ms = 1249u;
    check(vdPmuSessionWatchdog(&session) == 0 &&
              vdPmuSessionRead(&session, &test_owner, token, &reading) == 0,
          "lease remains readable immediately before its deadline");
    mock.now_ms = 1250u;
    check(vdPmuSessionRead(&session, &test_owner, token, &reading) ==
              VD_PMU_SESSION_ERROR_EXPIRED &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "read at deadline restores first and reports expiration");

    mock = make_mock();
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "watchdog expiration fixture acquired");
    mock.now_ms = 1250u;
    check(vdPmuSessionWatchdog(&session) ==
              VD_PMU_SESSION_WATCHDOG_RESTORED &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "watchdog restores an expired lease exactly");

    mock = make_mock();
    mock.configure_advance_ms = 250u;
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == VD_PMU_SESSION_ERROR_EXPIRED &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "configuration that consumes the lease is rolled back immediately");
}

static void test_acquire_faults(void)
{
    struct vd_pmu_session session = {0};
    struct mock_backend mock = make_mock();
    const struct vd_pmu_snapshot original = mock.hardware;
    const struct vd_pmu_session_backend backend = make_backend(&mock);
    const struct vd_pmu_session_owner test_owner = owner();
    const struct vd_pmu_session_request test_request = request(500u);
    uint64_t token;

    vdPmuSessionInit(&session);
    mock.fail_now_call = 1;
    token = UINT64_C(0x99);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == MOCK_ERROR_NOW &&
              !vdPmuSessionIsActive(&session) &&
              mock.snapshot_calls == 0 && mock.configure_calls == 0 &&
              token == UINT64_C(0x99),
          "initial clock failure cannot create a lease or touch PMU state");

    mock = make_mock();
    mock.fail_snapshot_call = 1;
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == MOCK_ERROR_SNAPSHOT &&
              !vdPmuSessionIsActive(&session) &&
              mock.configure_calls == 0 && mock.restore_calls == 0,
          "initial snapshot failure cannot configure PMU state");

    mock = make_mock();
    mock.fail_configure_call = 1;
    vdPmuSessionInit(&session);
    token = UINT64_C(0x99);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                               &token) == MOCK_ERROR_CONFIGURE &&
              !vdPmuSessionIsActive(&session) &&
              mock.restore_calls == 1 && mock.snapshot_calls == 2 &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0 &&
              token == UINT64_C(0x99),
          "partial configure failure rolls back and verifies exactly");

    mock = make_mock();
    mock.fail_configure_call = 1;
    mock.fail_restore_call = 1;
    vdPmuSessionInit(&session);
    token = UINT64_C(0x99);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                               &token) ==
              VD_PMU_SESSION_ERROR_RESTORE_REQUIRED &&
              session.state == VD_PMU_SESSION_RESTORE_PENDING &&
              vdPmuSessionIsActive(&session) &&
              token != 0 && token != UINT64_C(0x99),
          "failed configure rollback retains the restore obligation");
    const int calls_before_blocked_begin = callback_count(&mock);
    uint64_t blocked_token = UINT64_C(0x77);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &blocked_token) ==
              VD_PMU_SESSION_ERROR_RESTORE_REQUIRED &&
              callback_count(&mock) == calls_before_blocked_begin &&
              blocked_token == UINT64_C(0x77),
          "pending restoration blocks acquisition without force-forget");
    check(vdPmuSessionRestore(&session, &test_owner, token) == 0 &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "owner can retry and complete a failed acquire rollback");

    mock = make_mock();
    mock.fail_now_call = 3;
    vdPmuSessionInit(&session);
    token = UINT64_C(0x99);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                               &token) == MOCK_ERROR_NOW &&
              !vdPmuSessionIsActive(&session) &&
              mock.restore_calls == 1 &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0 &&
              token == UINT64_C(0x99),
          "post-configure clock failure restores before returning");

    mock = make_mock();
    mock.fail_configure_call = 1;
    mock.fail_snapshot_call = 2;
    vdPmuSessionInit(&session);
    token = UINT64_C(0x99);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                               &token) ==
              VD_PMU_SESSION_ERROR_RESTORE_REQUIRED &&
              session.state == VD_PMU_SESSION_RESTORE_PENDING &&
              token != 0 && token != UINT64_C(0x99) &&
              vdPmuSessionWatchdog(&session) ==
                  VD_PMU_SESSION_WATCHDOG_RESTORED &&
              !vdPmuSessionIsActive(&session),
          "failed rollback verification remains pending until a verified retry");
}

static void test_live_callback_faults(void)
{
    struct vd_pmu_session session = {0};
    struct mock_backend mock = make_mock();
    const struct vd_pmu_snapshot original = mock.hardware;
    const struct vd_pmu_session_backend backend = make_backend(&mock);
    const struct vd_pmu_session_owner test_owner = owner();
    const struct vd_pmu_session_request test_request = request(500u);
    uint64_t token;
    struct vd_pmu_reading reading;

    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "read fault fixture acquired");
    mock.fail_read_call = 1;
    memset(&reading, 0xa5, sizeof(reading));
    const struct vd_pmu_reading untouched = reading;
    check(vdPmuSessionRead(&session, &test_owner, token, &reading) ==
              MOCK_ERROR_READ &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&reading, &untouched, sizeof(reading)) == 0 &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "read callback failure withholds output and restores immediately");

    mock = make_mock();
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "restore fault fixture acquired");
    mock.fail_restore_call = 1;
    check(vdPmuSessionRestore(&session, &test_owner, token) ==
              VD_PMU_SESSION_ERROR_RESTORE_REQUIRED &&
              session.state == VD_PMU_SESSION_RESTORE_PENDING,
          "restore callback failure retains exact restoration metadata");
    const int reads_before_pending = mock.read_calls;
    check(vdPmuSessionRead(&session, &test_owner, token, &reading) ==
              VD_PMU_SESSION_ERROR_RESTORE_REQUIRED &&
              mock.read_calls == reads_before_pending &&
              vdPmuSessionWatchdog(&session) ==
                  VD_PMU_SESSION_WATCHDOG_RESTORED &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "pending state rejects reads and watchdog retries restoration");

    mock = make_mock();
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "restore verification fault fixture acquired");
    mock.corrupt_snapshot_call = 2;
    check(vdPmuSessionRestore(&session, &test_owner, token) ==
              VD_PMU_SESSION_ERROR_RESTORE_REQUIRED &&
              session.state == VD_PMU_SESSION_RESTORE_PENDING &&
              vdPmuSessionWatchdog(&session) ==
                  VD_PMU_SESSION_WATCHDOG_RESTORED,
          "mismatched restore verification cannot discard the obligation");

    mock = make_mock();
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "watchdog clock fault fixture acquired");
    mock.fail_now_call = 4;
    check(vdPmuSessionWatchdog(&session) == MOCK_ERROR_NOW &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "watchdog clock failure fails safe by restoring immediately");

    mock = make_mock();
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "read clock fault fixture acquired");
    mock.now_ms = 999u;
    check(vdPmuSessionRead(&session, &test_owner, token, &reading) ==
              VD_PMU_SESSION_ERROR_CLOCK &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "clock regression during read restores instead of extending a lease");

    mock = make_mock();
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "post-read clock fault fixture acquired");
    mock.fail_now_call = 5;
    check(vdPmuSessionRead(&session, &test_owner, token, &reading) ==
              MOCK_ERROR_NOW &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "post-read clock failure restores instead of returning stale data");

    mock = make_mock();
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "read-crosses-deadline fixture acquired");
    mock.read_advance_ms = test_request.lease_ms;
    check(vdPmuSessionRead(&session, &test_owner, token, &reading) ==
              VD_PMU_SESSION_ERROR_EXPIRED &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "read that crosses deadline restores and withholds its sample");
}

static void test_positive_backend_results_fail_closed(void)
{
    struct vd_pmu_session session = {0};
    struct mock_backend mock = make_mock();
    struct vd_pmu_session_backend backend = make_backend(&mock);
    const struct vd_pmu_session_owner test_owner = owner();
    const struct vd_pmu_session_request test_request = request(500u);
    uint64_t token = UINT64_C(0x99);
    struct vd_pmu_reading reading;

    vdPmuSessionInit(&session);
    mock.positive_now_call = 1;
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) ==
              VD_PMU_SESSION_ERROR_BACKEND_CONTRACT &&
              !vdPmuSessionIsActive(&session) &&
              token == UINT64_C(0x99),
          "positive clock result violates the backend contract");

    mock = make_mock();
    backend = make_backend(&mock);
    vdPmuSessionInit(&session);
    mock.positive_snapshot_call = 1;
    token = UINT64_C(0x99);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) ==
              VD_PMU_SESSION_ERROR_BACKEND_CONTRACT &&
              !vdPmuSessionIsActive(&session) &&
              token == UINT64_C(0x99),
          "positive initial snapshot result cannot create a lease");

    mock = make_mock();
    backend = make_backend(&mock);
    vdPmuSessionInit(&session);
    mock.positive_configure_call = 1;
    token = UINT64_C(0x99);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) ==
              VD_PMU_SESSION_ERROR_BACKEND_CONTRACT &&
              !vdPmuSessionIsActive(&session) &&
              mock.restore_calls == 1 && token == UINT64_C(0x99),
          "positive configure result triggers verified restoration");

    mock = make_mock();
    backend = make_backend(&mock);
    vdPmuSessionInit(&session);
    token = 0;
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "positive-read contract fixture acquired");
    mock.positive_read_call = 1;
    memset(&reading, 0xa5, sizeof(reading));
    const struct vd_pmu_reading untouched = reading;
    check(vdPmuSessionRead(&session, &test_owner, token, &reading) ==
              VD_PMU_SESSION_ERROR_BACKEND_CONTRACT &&
              !vdPmuSessionIsActive(&session) &&
              memcmp(&reading, &untouched, sizeof(reading)) == 0,
          "positive read result withholds output and restores immediately");

    mock = make_mock();
    backend = make_backend(&mock);
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "positive-restore contract fixture acquired");
    mock.positive_restore_call = 1;
    check(vdPmuSessionRestore(&session, &test_owner, token) ==
              VD_PMU_SESSION_ERROR_RESTORE_REQUIRED &&
              session.state == VD_PMU_SESSION_RESTORE_PENDING &&
              session.last_backend_error ==
                  VD_PMU_SESSION_ERROR_BACKEND_CONTRACT &&
              vdPmuSessionWatchdog(&session) ==
                  VD_PMU_SESSION_WATCHDOG_RESTORED,
          "positive restore result retains an obligation for retry");

    mock = make_mock();
    backend = make_backend(&mock);
    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "positive-verification contract fixture acquired");
    mock.positive_snapshot_call = 2;
    check(vdPmuSessionRestore(&session, &test_owner, token) ==
              VD_PMU_SESSION_ERROR_RESTORE_REQUIRED &&
              session.state == VD_PMU_SESSION_RESTORE_PENDING &&
              session.last_backend_error ==
                  VD_PMU_SESSION_ERROR_BACKEND_CONTRACT &&
              vdPmuSessionWatchdog(&session) ==
                  VD_PMU_SESSION_WATCHDOG_RESTORED,
          "positive restore-verification result cannot retire a lease");
}

static void test_snapshot_capacity_gate(void)
{
    struct vd_pmu_session session = {0};
    vdPmuSessionInit(&session);
    struct mock_backend mock = make_mock();
    mock.hardware.event_counter_count = 1u;
    const struct vd_pmu_session_backend backend = make_backend(&mock);
    const struct vd_pmu_session_owner test_owner = owner();
    const struct vd_pmu_session_request test_request = request(250u);
    uint64_t token = UINT64_C(0x42);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == VD_PMU_SESSION_ERROR_INVALID &&
              !vdPmuSessionIsActive(&session) &&
              mock.configure_calls == 0 && mock.restore_calls == 0 &&
              token == UINT64_C(0x42),
          "PMCR.N metadata mismatch is rejected before PMU mutation");

    mock = make_mock();
    vdPmuSessionInit(&session);
    mock.hardware.raw_pmcr |= (UINT32_C(1) << 1);
    token = UINT64_C(0x42);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                               &token) == VD_PMU_SESSION_ERROR_INVALID &&
              !vdPmuSessionIsActive(&session) &&
              mock.configure_calls == 0 && mock.restore_calls == 0 &&
              token == UINT64_C(0x42),
          "PMCR reset-command bits are rejected rather than retained/replayed");

    mock = make_mock();
    vdPmuSessionInit(&session);
    mock.hardware.raw_pmselr =
        VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS;
    token = UINT64_C(0x42);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == VD_PMU_SESSION_ERROR_INVALID &&
              !vdPmuSessionIsActive(&session) &&
              mock.configure_calls == 0 && mock.restore_calls == 0 &&
              token == UINT64_C(0x42),
          "out-of-range PMSELR is rejected before indexed PMU access");
}

static void test_global_idle_gate_and_fixed_lane(void)
{
    struct vd_pmu_session session = {0};
    vdPmuSessionInit(&session);
    struct mock_backend mock = make_mock();
    const struct vd_pmu_session_backend backend = make_backend(&mock);
    const struct vd_pmu_session_owner test_owner = owner();
    const struct vd_pmu_session_request test_request = request(250u);
    uint64_t token = UINT64_C(0x42);

    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0 &&
              mock.last_configuration.events[0].physical_counter == 5u &&
              vdPmuSessionRestore(&session, &test_owner, token) == 0,
          "globally idle six-counter PMU uses fixed lane 5");

    mock = make_mock();
    vdPmuSessionInit(&session);
    mock.hardware.raw_pmintenset |= UINT32_C(1) << 5;
    const int calls_before = callback_count(&mock);
    token = UINT64_C(0x42);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) ==
              VD_PMU_SESSION_ERROR_NOT_IDLE &&
              !vdPmuSessionIsActive(&session) &&
              mock.configure_calls == 0 && mock.restore_calls == 0 &&
              callback_count(&mock) == calls_before + 2 &&
              token == UINT64_C(0x42),
          "any programmed PMU interrupt blocks the first live gate");

    mock = make_mock();
    vdPmuSessionInit(&session);
    mock.hardware.raw_pmcntenset |= UINT32_C(1) << 0;
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == VD_PMU_SESSION_ERROR_NOT_IDLE &&
              mock.configure_calls == 0 && mock.restore_calls == 0,
          "any enabled programmable lane blocks acquisition");

    mock = make_mock();
    vdPmuSessionInit(&session);
    mock.hardware.raw_pmovsr |= UINT32_C(1) << 31;
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == VD_PMU_SESSION_ERROR_NOT_IDLE &&
              mock.configure_calls == 0 && mock.restore_calls == 0,
          "cycle overflow state blocks acquisition instead of using W1C");
}

static void test_external_pmu_activity_keeps_obligation(void)
{
    struct vd_pmu_session session = {0};
    vdPmuSessionInit(&session);
    struct mock_backend mock = make_mock();
    const struct vd_pmu_snapshot original = mock.hardware;
    const struct vd_pmu_session_backend backend = make_backend(&mock);
    const struct vd_pmu_session_owner test_owner = owner();
    const struct vd_pmu_session_request test_request = request(250u);
    uint64_t token = 0;

    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "external-conflict fixture acquired from globally idle state");
    mock.hardware.raw_pmxevcntr[0] += 77u;
    mock.hardware.raw_pmccntr += 99u;
    mock.hardware.raw_pmovsr |= UINT32_C(1) << 0;
    check(vdPmuSessionRestore(&session, &test_owner, token) ==
              VD_PMU_SESSION_ERROR_RESTORE_REQUIRED &&
              session.state == VD_PMU_SESSION_RESTORE_PENDING &&
              mock.hardware.raw_pmxevcntr[0] ==
                  original.raw_pmxevcntr[0] + 77u &&
              mock.hardware.raw_pmccntr == original.raw_pmccntr + 99u &&
              (mock.hardware.raw_pmovsr & 1u) != 0 &&
              mock.hardware.raw_pmxevcntr[5] ==
                  original.raw_pmxevcntr[5] &&
              (mock.hardware.raw_pmcntenset & (UINT32_C(1) << 5)) == 0,
          "external PMU activity is preserved but prevents false restoration");
    mock.hardware = original;
    check(vdPmuSessionWatchdog(&session) ==
              VD_PMU_SESSION_WATCHDOG_RESTORED &&
              !vdPmuSessionIsActive(&session),
          "obligation clears only after the external conflict disappears");
}

static void test_global_enable_is_derived_safely(void)
{
    struct vd_pmu_session session = {0};
    vdPmuSessionInit(&session);
    struct mock_backend mock = make_mock();
    mock.hardware.raw_pmcr &= ~UINT32_C(1);
    mock.hardware.raw_pmcntenset = 0u;
    const struct vd_pmu_snapshot original = mock.hardware;
    const struct vd_pmu_session_backend backend = make_backend(&mock);
    const struct vd_pmu_session_owner test_owner = owner();
    const struct vd_pmu_session_request test_request = request(250u);
    uint64_t token = 0;

    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0 &&
              mock.last_configuration.control_flags ==
                  VD_PMU_CONFIGURATION_ENABLE_GLOBAL &&
              (mock.hardware.raw_pmcr & 1u) != 0 &&
              vdPmuSessionRestore(&session, &test_owner, token) == 0 &&
              (mock.hardware.raw_pmcr & 1u) == 0 &&
              memcmp(&mock.hardware, &original, sizeof(original)) == 0,
          "global PMU enable is derived only when no other lane can start");

    mock = make_mock();
    vdPmuSessionInit(&session);
    mock.hardware.raw_pmcr &= ~UINT32_C(1);
    mock.hardware.raw_pmcntenset = UINT32_C(1) << 0;
    token = UINT64_C(0x42);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) ==
              VD_PMU_SESSION_ERROR_NOT_IDLE &&
              !vdPmuSessionIsActive(&session) &&
              mock.configure_calls == 0 && mock.restore_calls == 0 &&
              token == UINT64_C(0x42),
          "disabled global PMU with armed foreign lanes is rejected");
}

static void test_reinitialization_cannot_discard_obligation(void)
{
    struct vd_pmu_session session = {0};
    struct mock_backend mock = make_mock();
    const struct vd_pmu_session_backend backend = make_backend(&mock);
    const struct vd_pmu_session_owner test_owner = owner();
    const struct vd_pmu_session_request test_request = request(500u);
    uint64_t token = 0;

    vdPmuSessionInit(&session);
    check(vdPmuSessionAcquire(&session, &test_owner, &test_request, &backend,
                              &token) == 0,
          "reinitialization fixture acquired");
    const struct vd_pmu_session active = session;
    vdPmuSessionInit(&session);
    check(memcmp(&session, &active, sizeof(session)) == 0 &&
              vdPmuSessionIsActive(&session),
          "reinitialization cannot erase an active PMU lease");

    mock.fail_restore_call = 1;
    check(vdPmuSessionRestore(&session, &test_owner, token) ==
              VD_PMU_SESSION_ERROR_RESTORE_REQUIRED,
          "reinitialization fixture retains a restore obligation");
    const struct vd_pmu_session pending = session;
    vdPmuSessionInit(&session);
    check(memcmp(&session, &pending, sizeof(session)) == 0 &&
              session.state == VD_PMU_SESSION_RESTORE_PENDING &&
              vdPmuSessionWatchdog(&session) ==
                  VD_PMU_SESSION_WATCHDOG_RESTORED,
          "reinitialization cannot erase a pending PMU restoration");
}

int main(void)
{
    check(sizeof(struct vd_pmu_event_metadata) == 16u,
          "event metadata has a stable fixed-width layout");
    check(sizeof(struct vd_pmu_session_request) == 48u,
          "session request has a stable fixed-width layout");
    test_allowlist();
    test_invalid_requests_have_no_side_effects();
    test_success_owner_read_and_restore();
    test_deadline_and_watchdog();
    test_acquire_faults();
    test_live_callback_faults();
    test_positive_backend_results_fail_closed();
    test_snapshot_capacity_gate();
    test_global_idle_gate_and_fixed_lane();
    test_external_pmu_activity_keeps_obligation();
    test_global_enable_is_derived_safely();
    test_reinitialization_cannot_discard_obligation();
    if(failures)
        return 1;
    puts("PASS: bounded PMU lease and exact-restoration state machine");
    return 0;
}

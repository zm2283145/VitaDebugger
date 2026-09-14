#include "vitaprofiler_pmu_vita.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__);     \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

enum fake_operation {
    FAKE_GET_THREAD = 1,
    FAKE_RESET,
    FAKE_SELECT,
    FAKE_START,
    FAKE_STOP,
    FAKE_GET,
    FAKE_SET,
};

struct fake_call {
    uint32_t operation;
    int32_t thread_id;
    uint32_t counter;
    uint32_t value;
};

struct fake_sceperf {
    struct fake_call calls[64];
    uint32_t call_count;
    uint32_t fail_call[4];
    int fail_code[4];
    uint32_t values[VP_VITA_PMU_PHYSICAL_COUNTERS_USED];
    int32_t self_thread_id;
};

static int fake_record(struct fake_sceperf* fake, uint32_t operation,
                       int32_t thread_id, uint32_t counter, uint32_t value)
{
    uint32_t i;
    uint32_t call_number = fake->call_count + 1u;
    if (fake->call_count <
        (uint32_t)(sizeof(fake->calls) / sizeof(fake->calls[0]))) {
        struct fake_call* call = &fake->calls[fake->call_count];
        call->operation = operation;
        call->thread_id = thread_id;
        call->counter = counter;
        call->value = value;
    }
    ++fake->call_count;
    for (i = 0u; i < 4u; ++i) {
        if (fake->fail_call[i] == call_number)
            return fake->fail_code[i];
    }
    return 0;
}

static int32_t fake_get_thread_id(void* user)
{
    struct fake_sceperf* fake = (struct fake_sceperf*)user;
    int result = fake_record(fake, FAKE_GET_THREAD, 0, 0u, 0u);
    return result == 0 ? fake->self_thread_id : result;
}

static int fake_reset(void* user, int32_t thread_id)
{
    return fake_record((struct fake_sceperf*)user, FAKE_RESET, thread_id, 0u,
                       0u);
}

static int fake_select(void* user, int32_t thread_id, uint32_t counter,
                       uint8_t event_code)
{
    return fake_record((struct fake_sceperf*)user, FAKE_SELECT, thread_id,
                       counter, event_code);
}

static int fake_start(void* user, int32_t thread_id)
{
    return fake_record((struct fake_sceperf*)user, FAKE_START, thread_id, 0u,
                       0u);
}

static int fake_stop(void* user, int32_t thread_id)
{
    return fake_record((struct fake_sceperf*)user, FAKE_STOP, thread_id, 0u,
                       0u);
}

static int fake_get(void* user, int32_t thread_id, uint32_t counter,
                    uint32_t* value)
{
    struct fake_sceperf* fake = (struct fake_sceperf*)user;
    int result;
    if (counter >= VP_VITA_PMU_PHYSICAL_COUNTERS_USED)
        return -999;
    result = fake_record(fake, FAKE_GET, thread_id, counter, 0u);
    if (result == 0)
        *value = fake->values[counter];
    return result;
}

static int fake_set(void* user, int32_t thread_id, uint32_t counter,
                    uint32_t value)
{
    struct fake_sceperf* fake = (struct fake_sceperf*)user;
    int result;
    if (counter >= VP_VITA_PMU_PHYSICAL_COUNTERS_USED)
        return -999;
    result = fake_record(fake, FAKE_SET, thread_id, counter, value);
    if (result == 0)
        fake->values[counter] = value;
    return result;
}

static struct vp_vita_pmu_ops fake_ops(struct fake_sceperf* fake)
{
    struct vp_vita_pmu_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.get_thread_id = fake_get_thread_id;
    ops.reset = fake_reset;
    ops.select_event = fake_select;
    ops.start = fake_start;
    ops.stop = fake_stop;
    ops.get_counter = fake_get;
    ops.set_counter = fake_set;
    ops.user = fake;
    return ops;
}

static struct vp_pmu_config owned_config(void)
{
    struct vp_pmu_config config;
    memset(&config, 0, sizeof(config));
    config.counter_mask = VP_PMU_COUNTER_CYCLES | VP_PMU_COUNTER_EVENT0;
    config.event_count = 1u;
    config.event_codes[0] = 0x03u;
    config.flags = VP_PMU_CONFIG_FLAG_ALLOW_OWNED_RESET;
    return config;
}

static void test_explicit_ownership_and_mapping(void)
{
    struct fake_sceperf fake;
    struct fake_sceperf other_fake;
    struct vp_vita_pmu_ops ops;
    struct vp_vita_pmu_ops other_ops;
    struct vp_vita_pmu_owned owned;
    struct vp_vita_pmu_owned other;
    struct vp_pmu_session session;
    struct vp_pmu_session other_session;
    struct vp_pmu_config config = owned_config();
    struct vp_pmu_sample sample;

    memset(&fake, 0, sizeof(fake));
    memset(&other_fake, 0, sizeof(other_fake));
    memset(&owned, 0, sizeof(owned));
    memset(&other, 0, sizeof(other));
    memset(&session, 0, sizeof(session));
    memset(&other_session, 0, sizeof(other_session));
    fake.self_thread_id = 42;
    other_fake.self_thread_id = 43;
    ops = fake_ops(&fake);
    other_ops = fake_ops(&other_fake);

    CHECK(vp_vita_pmu_owned_init_with_ops(
              &owned, &ops, 0, VP_VITA_PMU_APPLICATION_OWNERSHIP_ACK) ==
              VP_RESULT_OK,
          "SELF is resolved once during owned-provider initialization");
    CHECK(fake.call_count == 1u &&
              fake.calls[0].operation == FAKE_GET_THREAD,
          "initialization resolves the target without touching PMU state");
    CHECK(vp_vita_pmu_owned_init_with_ops(&other, &other_ops, 43,
              VP_VITA_PMU_APPLICATION_OWNERSHIP_ACK) == VP_RESULT_OK,
          "second provider may initialize without claiming hardware");

    config.flags = 0u;
    CHECK(vp_pmu_session_begin(
              &session, vp_vita_pmu_owned_get_provider(&owned), &config) ==
              VP_ERROR_UNSUPPORTED && fake.call_count == 1u,
          "owned-reset provider requires per-session destructive opt-in");
    config.flags = VP_PMU_CONFIG_FLAG_ALLOW_OWNED_RESET;
    CHECK(vp_pmu_session_begin(
              &session, vp_vita_pmu_owned_get_provider(&owned), &config) ==
              VP_RESULT_OK,
          "explicitly owned thread begins through documented operations");
    CHECK(fake.call_count == 7u &&
              fake.calls[1].operation == FAKE_RESET &&
              fake.calls[2].operation == FAKE_SELECT &&
              fake.calls[2].counter == 0u &&
              fake.calls[2].value == VP_VITA_PMU_EVENT_CYCLE_COUNT &&
              fake.calls[3].operation == FAKE_SET &&
              fake.calls[4].operation == FAKE_SELECT &&
              fake.calls[4].counter == 1u &&
              fake.calls[4].value == 0x03u &&
              fake.calls[5].operation == FAKE_SET &&
              fake.calls[6].operation == FAKE_START,
          "cycles and one event map to programmable counters zero and one");
    CHECK(vp_pmu_session_begin(
              &other_session, vp_vita_pmu_owned_get_provider(&other),
              &config) == VP_ERROR_BUSY,
          "linked adapter lease prevents conflicting profilers");

    fake.values[0] = UINT32_C(0xf1234567);
    fake.values[1] = 99u;
    CHECK(vp_pmu_session_read(&session, &sample) == VP_RESULT_OK &&
              sample.cycles == UINT32_C(0xf1234567) &&
              sample.events[0] == 99u &&
              sample.counter_mask == config.counter_mask,
          "ScePerf's raw 32-bit samples are zero-extended without guessing wraps");
    CHECK(vp_pmu_session_end(&session) == VP_RESULT_OK &&
              fake.calls[fake.call_count - 2u].operation == FAKE_STOP &&
              fake.calls[fake.call_count - 1u].operation == FAKE_RESET,
          "release stops and resets application-owned PMU state");

    CHECK(vp_pmu_session_begin(
              &other_session, vp_vita_pmu_owned_get_provider(&other),
              &config) == VP_RESULT_OK &&
              vp_pmu_session_end(&other_session) == VP_RESULT_OK,
          "successful cleanup releases the linked adapter lease");
}

static void test_orphaned_acquire_cleanup(void)
{
    struct fake_sceperf fake;
    struct fake_sceperf other_fake;
    struct vp_vita_pmu_ops ops;
    struct vp_vita_pmu_ops other_ops;
    struct vp_vita_pmu_owned owned;
    struct vp_vita_pmu_owned other;
    struct vp_vita_pmu_owned_status status;
    struct vp_pmu_session session;
    struct vp_pmu_session other_session;
    struct vp_pmu_config config = owned_config();

    memset(&fake, 0, sizeof(fake));
    memset(&other_fake, 0, sizeof(other_fake));
    memset(&owned, 0, sizeof(owned));
    memset(&other, 0, sizeof(other));
    memset(&session, 0, sizeof(session));
    memset(&other_session, 0, sizeof(other_session));
    ops = fake_ops(&fake);
    other_ops = fake_ops(&other_fake);
    CHECK(vp_vita_pmu_owned_init_with_ops(&owned, &ops, 51,
              VP_VITA_PMU_APPLICATION_OWNERSHIP_ACK) == VP_RESULT_OK &&
              vp_vita_pmu_owned_init_with_ops(&other, &other_ops, 52,
              VP_VITA_PMU_APPLICATION_OWNERSHIP_ACK) == VP_RESULT_OK,
          "initialize orphan-cleanup fixtures");

    fake.fail_call[0] = 2u;
    fake.fail_code[0] = -100;
    fake.fail_call[1] = 4u;
    fake.fail_code[1] = -200;
    CHECK(vp_pmu_session_begin(
              &session, vp_vita_pmu_owned_get_provider(&owned), &config) ==
              VP_ERROR_RESTORE_REQUIRED,
          "partial acquire retains a failed cleanup obligation");
    CHECK(vp_vita_pmu_owned_get_status(&owned, &status) == VP_RESULT_OK &&
              status.active == 0u && status.cleanup_pending == 1u &&
              status.last_operation == VP_VITA_PMU_OPERATION_RESET &&
              status.last_platform_error == -200,
          "orphaned cleanup exposes the exact failing ScePerf operation");
    CHECK(vp_pmu_session_begin(
              &other_session, vp_vita_pmu_owned_get_provider(&other),
              &config) == VP_ERROR_BUSY,
          "failed acquire keeps the linked adapter lease blocked");
    CHECK(vp_vita_pmu_owned_retry_orphan_cleanup(&owned) == VP_RESULT_OK,
          "explicit orphan cleanup retries stop and reset");
    CHECK(vp_pmu_session_begin(
              &other_session, vp_vita_pmu_owned_get_provider(&other),
              &config) == VP_RESULT_OK &&
              vp_pmu_session_end(&other_session) == VP_RESULT_OK,
          "orphan cleanup releases the linked adapter lease");
}

static void test_release_retry_and_validation(void)
{
    struct fake_sceperf fake;
    struct vp_vita_pmu_ops ops;
    struct vp_vita_pmu_owned owned;
    struct vp_vita_pmu_owned_status owned_status;
    struct vp_pmu_session session;
    struct vp_pmu_session_status session_status;
    struct vp_pmu_config config = owned_config();
    const struct vp_pmu_provider* provider;
    uint64_t direct_token = 0u;

    memset(&fake, 0, sizeof(fake));
    memset(&owned, 0, sizeof(owned));
    memset(&session, 0, sizeof(session));
    ops = fake_ops(&fake);
    CHECK(vp_vita_pmu_owned_init_with_ops(
              &owned, &ops, 61, 0u) == VP_ERROR_INVALID_ARGUMENT,
          "provider initialization requires explicit ownership assertion");
    CHECK(vp_vita_pmu_owned_init_with_ops(&owned, &ops, 61,
              VP_VITA_PMU_APPLICATION_OWNERSHIP_ACK) == VP_RESULT_OK,
          "initialize release-retry fixture");
    provider = vp_vita_pmu_owned_get_provider(&owned);

    config.event_count = UINT32_MAX;
    CHECK(provider->acquire(provider->user, &config, &direct_token) ==
                  VP_ERROR_INVALID_ARGUMENT &&
              direct_token == 0u && fake.call_count == 0u,
          "direct provider callback bounds event count before array access");
    config = owned_config();
    config.reserved = 1u;
    CHECK(provider->acquire(provider->user, &config, &direct_token) ==
                  VP_ERROR_INVALID_ARGUMENT &&
              fake.call_count == 0u,
          "direct provider callback rejects reserved configuration data");
    config = owned_config();
    config.counter_mask = VP_PMU_COUNTER_CYCLES | VP_PMU_COUNTER_EVENT1;
    CHECK(provider->acquire(provider->user, &config, &direct_token) ==
                  VP_ERROR_INVALID_ARGUMENT &&
              fake.call_count == 0u,
          "direct provider callback rejects noncontiguous event lanes");

    config = owned_config();
    config.event_codes[0] = 0x100u;
    CHECK(vp_pmu_session_begin(
              &session, vp_vita_pmu_owned_get_provider(&owned), &config) ==
              VP_ERROR_INVALID_ARGUMENT && fake.call_count == 0u,
          "event codes outside ScePerf's uint8 range fail before mutation");
    config.event_codes[0] = 0x04u;
    CHECK(vp_pmu_session_begin(
              &session, vp_vita_pmu_owned_get_provider(&owned), &config) ==
              VP_RESULT_OK,
          "begin release-retry fixture");
    fake.fail_call[0] = fake.call_count + 1u;
    fake.fail_code[0] = -300;
    CHECK(vp_pmu_session_end(&session) == VP_ERROR_RESTORE_REQUIRED,
          "failed stop keeps the generic session retryable");
    CHECK(vp_pmu_session_get_status(&session, &session_status) ==
                  VP_RESULT_OK &&
              session_status.active == 1u &&
              session_status.restore_pending == 1u &&
              vp_vita_pmu_owned_get_status(&owned, &owned_status) ==
                  VP_RESULT_OK &&
              owned_status.active == 1u &&
              owned_status.cleanup_pending == 1u &&
              owned_status.last_operation == VP_VITA_PMU_OPERATION_STOP &&
              owned_status.last_platform_error == -300,
          "release failure preserves both portable and raw platform status");
    CHECK(vp_vita_pmu_owned_retry_orphan_cleanup(&owned) == VP_ERROR_BUSY,
          "active session cleanup cannot bypass its retained lease token");
    CHECK(vp_pmu_session_end(&session) == VP_RESULT_OK,
          "ordinary session end retries and completes owned cleanup");
}

int main(void)
{
    test_explicit_ownership_and_mapping();
    test_orphaned_acquire_cleanup();
    test_release_retry_and_validation();
    if (failures != 0) {
        fprintf(stderr, "%d Vita PMU adapter test(s) failed\n", failures);
        return 1;
    }
    puts("vitaprofiler Vita PMU adapter: all native tests passed");
    return 0;
}

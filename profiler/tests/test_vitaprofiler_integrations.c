#include "vitaprofiler_gpu.h"
#include "vitaprofiler_pmu.h"

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

struct fake_pmu {
    uint32_t acquire_calls;
    uint32_t read_calls;
    uint32_t release_calls;
    uint32_t fail_release;
    uint64_t observed_token;
};

static int fake_pmu_acquire(void* user, const struct vp_pmu_config* config,
                            uint64_t* lease_token)
{
    struct fake_pmu* fake = (struct fake_pmu*)user;
    ++fake->acquire_calls;
    if (config->counter_mask == 0u)
        return VP_ERROR_INVALID_ARGUMENT;
    *lease_token = UINT64_C(0x123456789abcdef0);
    return VP_RESULT_OK;
}

static int fake_pmu_read(void* user, uint64_t lease_token,
                         struct vp_pmu_sample* sample)
{
    struct fake_pmu* fake = (struct fake_pmu*)user;
    ++fake->read_calls;
    fake->observed_token = lease_token;
    sample->cycles = 1000u;
    sample->events[0] = 25u;
    sample->counter_mask = VP_PMU_COUNTER_CYCLES | VP_PMU_COUNTER_EVENT0;
    return VP_RESULT_OK;
}

static int fake_pmu_release(void* user, uint64_t lease_token)
{
    struct fake_pmu* fake = (struct fake_pmu*)user;
    ++fake->release_calls;
    fake->observed_token = lease_token;
    if (fake->fail_release != 0u) {
        fake->fail_release = 0u;
        return VP_ERROR_PLATFORM;
    }
    return VP_RESULT_OK;
}

static void test_guarded_pmu_provider(void)
{
    struct vp_pmu_session session;
    struct vp_pmu_session_status status;
    struct vp_pmu_sample sample;
    struct vp_pmu_config config;
    struct fake_pmu fake;
    struct vp_pmu_provider provider;

    memset(&session, 0, sizeof(session));
    memset(&config, 0, sizeof(config));
    config.counter_mask = VP_PMU_COUNTER_CYCLES | VP_PMU_COUNTER_EVENT0;
    config.event_count = 1u;
    config.event_codes[0] = 0x08u;
    CHECK(vp_pmu_session_begin(&session, NULL, &config) ==
              VP_ERROR_UNSUPPORTED,
          "missing PMU provider fails closed");

    memset(&fake, 0, sizeof(fake));
    memset(&provider, 0, sizeof(provider));
    provider.abi_version = VP_PMU_PROVIDER_ABI_VERSION;
    provider.flags = VP_PMU_PROVIDER_FLAG_EXACT_RESTORE;
    provider.acquire = fake_pmu_acquire;
    provider.read = fake_pmu_read;
    provider.release = fake_pmu_release;
    provider.user = &fake;
    config.flags = VP_PMU_CONFIG_FLAG_ALLOW_OWNED_RESET;
    CHECK(vp_pmu_session_begin(&session, &provider, &config) ==
              VP_ERROR_UNSUPPORTED,
          "exact-restore provider cannot satisfy owned-reset request");
    config.flags = 0u;
    CHECK(vp_pmu_session_begin(&session, &provider, &config) ==
                  VP_RESULT_OK &&
              fake.acquire_calls == 1u,
          "audited provider acquires a session");
    CHECK(vp_pmu_session_begin(&session, &provider, &config) ==
              VP_ERROR_BUSY,
          "active PMU lease cannot be overwritten");
    CHECK(vp_pmu_session_read(&session, &sample) == VP_RESULT_OK &&
              sample.cycles == 1000u && sample.events[0] == 25u &&
              fake.observed_token == UINT64_C(0x123456789abcdef0),
          "PMU read is scoped to the opaque lease");

    fake.fail_release = 1u;
    CHECK(vp_pmu_session_end(&session) == VP_ERROR_RESTORE_REQUIRED,
          "release failure retains restoration obligation");
    CHECK(vp_pmu_session_get_status(&session, &status) == VP_RESULT_OK &&
              status.active == 1u && status.restore_pending == 1u &&
              status.last_provider_error == VP_ERROR_PLATFORM,
          "restore-pending state is observable");
    CHECK(vp_pmu_session_read(&session, &sample) ==
              VP_ERROR_RESTORE_REQUIRED,
          "reads stop while restoration is unresolved");
    CHECK(vp_pmu_session_end(&session) == VP_RESULT_OK &&
              fake.release_calls == 2u &&
              vp_pmu_session_get_status(&session, &status) == VP_RESULT_OK &&
              status.active == 0u && status.restore_pending == 0u,
          "retry completes exact provider restoration");

    config.event_count = 0u;
    CHECK(vp_pmu_session_begin(&session, &provider, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "counter mask cannot request an unconfigured event lane");
}

struct fake_source {
    uint64_t now;
    uint32_t thread_id;
};

static uint64_t fake_clock(void* user)
{
    return ((struct fake_source*)user)->now;
}

static uint32_t fake_thread(void* user)
{
    return ((struct fake_source*)user)->thread_id;
}

static void test_cooperative_graphics_hooks(void)
{
    struct vp_context context;
    struct vp_slot slots[32];
    struct vp_config config;
    struct fake_source source = {100u, 5u};
    struct vp_name_dictionary names;
    struct vp_name_dictionary_config name_config;
    struct vp_name_entry entries[16];
    char text[512];
    struct vp_graphics_name_ids ids;
    struct vp_pmu_name_ids pmu_ids;
    struct vp_pmu_sample pmu_sample;
    struct vp_graphics_hooks hooks;
    struct vp_zone_scope scene;
    struct vp_event events[8];
    struct vp_name_view view;
    size_t count;

    memset(&config, 0, sizeof(config));
    config.slots = slots;
    config.capacity = 32u;
    config.clock = fake_clock;
    config.clock_user = &source;
    config.thread_id = fake_thread;
    config.thread_user = &source;
    CHECK(vp_init(&context, &config) == VP_RESULT_OK,
          "initialize graphics profiler context");
    memset(&name_config, 0, sizeof(name_config));
    name_config.entries = entries;
    name_config.entry_capacity = 16u;
    name_config.text = text;
    name_config.text_capacity = sizeof(text);
    CHECK(vp_name_dictionary_init(&names, &name_config) == VP_RESULT_OK &&
              vp_graphics_register_names(&names, &ids) == VP_RESULT_OK &&
              vp_name_dictionary_register(&names, "cpu.pmu.cycles",
                                          &pmu_ids.cycles) == VP_RESULT_OK &&
              vp_name_dictionary_register(&names, "cpu.pmu.l1_misses",
                                          &pmu_ids.events[0]) == VP_RESULT_OK &&
              vp_name_dictionary_seal(&names) == VP_RESULT_OK,
          "register and seal integration hook names");
    pmu_ids.events[1] = 0u;
    pmu_ids.events[2] = 0u;
    pmu_ids.events[3] = 0u;
    CHECK(vp_name_dictionary_lookup(
              &names, ids.zones[VP_GRAPHICS_ZONE_SCEGXM_SCENE], &view) ==
                  VP_RESULT_OK &&
              view.name_length == sizeof("scegxm.scene.cpu") - 1u &&
              memcmp(view.name, "scegxm.scene.cpu", view.name_length) == 0,
          "dictionary resolves SceGxm scene hook");
    CHECK(vp_graphics_hooks_init(&hooks, &context, &ids) == VP_RESULT_OK,
          "initialize header-independent graphics hooks");
    CHECK(vp_graphics_zone_begin(&hooks, VP_GRAPHICS_ZONE_SCEGXM_SCENE,
                                 &scene) == VP_RESULT_OK,
          "begin CPU-side SceGxm scene hook");
    source.now = 175u;
    CHECK(vp_graphics_zone_end(&hooks, &scene) == VP_RESULT_OK,
          "end CPU-side SceGxm scene hook");
    CHECK(vp_graphics_counter(&hooks,
                              VP_GRAPHICS_COUNTER_SCEGXM_DRAW_CALLS,
                              12) == VP_RESULT_OK,
          "record cooperative draw count");
    memset(&pmu_sample, 0, sizeof(pmu_sample));
    pmu_sample.counter_mask = VP_PMU_COUNTER_CYCLES | VP_PMU_COUNTER_EVENT0;
    pmu_sample.cycles = 1000u;
    pmu_sample.events[0] = 25u;
    CHECK(vp_pmu_record_sample(&context, &pmu_sample, &pmu_ids) ==
              VP_RESULT_OK,
          "record provider sample as named raw counters");
    count = vp_drain(&context, events, 8u);
    CHECK(count == 5u && events[0].type == VP_EVENT_ZONE_BEGIN &&
              events[1].type == VP_EVENT_ZONE_END &&
              events[1].value == 75 &&
              events[2].type == VP_EVENT_COUNTER &&
              events[2].value == 12 &&
              events[3].name_id == pmu_ids.cycles &&
              events[3].value == 1000 &&
              (events[3].flags & VP_EVENT_FLAG_RAW_VALUE) != 0u &&
              events[4].name_id == pmu_ids.events[0] &&
              events[4].value == 25,
          "integration hooks emit ordinary named CPU events");
    CHECK(vp_graphics_zone_begin(&hooks,
                                 (enum vp_graphics_zone)VP_GRAPHICS_ZONE_COUNT,
                                 &scene) == VP_ERROR_INVALID_ARGUMENT,
          "unknown graphics hook fails closed");
}

static void test_render96ex_vitagl_callsite_sequence(void)
{
    struct vp_context context;
    struct vp_slot slots[8];
    struct vp_config config;
    struct fake_source source = {100u, 7u};
    struct vp_name_dictionary dictionary;
    struct vp_name_dictionary_config dictionary_config;
    struct vp_name_entry entries[16];
    char text[512];
    struct vp_graphics_name_ids ids;
    struct vp_graphics_hooks hooks;
    struct vp_zone_scope frame;
    struct vp_zone_scope swap;
    struct vp_event events[8];
    size_t count;

    memset(&config, 0, sizeof(config));
    config.slots = slots;
    config.capacity = 8u;
    config.clock = fake_clock;
    config.clock_user = &source;
    config.thread_id = fake_thread;
    config.thread_user = &source;
    memset(&dictionary_config, 0, sizeof(dictionary_config));
    dictionary_config.entries = entries;
    dictionary_config.entry_capacity = 16u;
    dictionary_config.text = text;
    dictionary_config.text_capacity = sizeof(text);
    CHECK(vp_init(&context, &config) == VP_RESULT_OK &&
              vp_name_dictionary_init(&dictionary, &dictionary_config) ==
                  VP_RESULT_OK &&
              vp_graphics_register_names(&dictionary, &ids) ==
                  VP_RESULT_OK &&
              vp_name_dictionary_seal(&dictionary) == VP_RESULT_OK &&
              vp_graphics_hooks_init(&hooks, &context, &ids) ==
                  VP_RESULT_OK,
          "initialize the audited Render96EX call-site sequence");

    CHECK(vp_graphics_zone_begin(
              &hooks, VP_GRAPHICS_ZONE_VITAGL_FRAME, &frame) ==
                  VP_RESULT_OK,
          "begin one complete Render96EX game frame");
    source.now = 110u;
    CHECK(vp_graphics_counter(
              &hooks, VP_GRAPHICS_COUNTER_VITAGL_DRAW_CALLS, 23) ==
                  VP_RESULT_OK,
          "publish one aggregate VitaGL draw count");
    source.now = 120u;
    CHECK(vp_graphics_zone_begin(
              &hooks, VP_GRAPHICS_ZONE_VITAGL_SWAP_BUFFERS, &swap) ==
                  VP_RESULT_OK,
          "begin the application-owned VitaGL swap call");
    source.now = 135u;
    CHECK(vp_graphics_zone_end(&hooks, &swap) == VP_RESULT_OK,
          "end the application-owned VitaGL swap call");
    source.now = 150u;
    CHECK(vp_graphics_zone_end(&hooks, &frame) == VP_RESULT_OK,
          "end one complete Render96EX game frame");

    count = vp_drain(&context, events, 8u);
    CHECK(count == 5u && events[0].type == VP_EVENT_ZONE_BEGIN &&
              events[0].name_id ==
                  ids.zones[VP_GRAPHICS_ZONE_VITAGL_FRAME] &&
              events[1].type == VP_EVENT_COUNTER &&
              events[1].name_id ==
                  ids.counters[VP_GRAPHICS_COUNTER_VITAGL_DRAW_CALLS] &&
              events[1].value == 23 &&
              events[2].type == VP_EVENT_ZONE_BEGIN &&
              events[2].name_id ==
                  ids.zones[VP_GRAPHICS_ZONE_VITAGL_SWAP_BUFFERS] &&
              events[3].type == VP_EVENT_ZONE_END &&
              events[3].value == 15 &&
              events[4].type == VP_EVENT_ZONE_END &&
              events[4].value == 50,
          "audited frame/draw/swap hooks remain ordered and balanced");
    vp_name_dictionary_deinit(&dictionary);
    vp_deinit(&context);
}

int main(void)
{
    test_guarded_pmu_provider();
    test_cooperative_graphics_hooks();
    test_render96ex_vitagl_callsite_sequence();
    if (failures != 0) {
        fprintf(stderr, "%d profiler integration test(s) failed\n", failures);
        return 1;
    }
    puts("vitaprofiler integrations: all native tests passed");
    return 0;
}

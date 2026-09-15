#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pmu_backend_host_test.h"
#include "pmu_profiler_bridge.h"
#include "pmu_profiler_transport.h"

#if !defined(VD_TEST_EXPECT_REAL_EVENTS_COMPILED)
#error "bridge host test must declare its expected real-event gate mode"
#endif
#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED != \
    VD_TEST_EXPECT_REAL_EVENTS_COMPILED
#error "real-event compile gate does not match the requested host-test mode"
#endif

#define TEST_MIDR UINT32_C(0x412fc09a)
#define TEST_COUNTERS 6u
#define TEST_PMCR_N_SHIFT 11u
#define TEST_PMCR_E (UINT32_C(1) << 0)
#define TEST_PMCR_P (UINT32_C(1) << 1)
#define TEST_PMCR_C (UINT32_C(1) << 2)
#define TEST_PMCR_PERSISTENT_MASK UINT32_C(0x39)
#define TEST_EVENT_MASK UINT32_C(0x3f)
#define TEST_CYCLE_MASK (UINT32_C(1) << 31)
#define TEST_IMPLEMENTED_MASK (TEST_EVENT_MASK | TEST_CYCLE_MASK)
#define TEST_LANE 5u
#define TEST_LANE_MASK (UINT32_C(1) << TEST_LANE)

struct fake_pmu_state
{
    uint32_t midr;
    uint32_t mpidr;
    uint32_t pmcr;
    uint32_t counter_enable;
    uint32_t overflow;
    uint32_t selector;
    uint32_t cycle_count;
    uint32_t user_enable;
    uint32_t interrupt_enable;
    uint32_t event_type[TEST_COUNTERS];
    uint32_t event_count[TEST_COUNTERS];
};

struct fake_pmu
{
    struct fake_pmu_state state;
    uint32_t writes[VD_PMU_BACKEND_HOST_PMINTENCLR + 1u];
    uint32_t fail_next_lane_type_and_timeout_restore;
    uint32_t increment_real_event_on_enable;
    uint32_t fail_selector_write_ordinal;
};

static int failures;

#define CHECK(condition, message)                                         \
    do                                                                    \
    {                                                                     \
        if(!(condition))                                                  \
        {                                                                 \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            ++failures;                                                   \
            return;                                                       \
        }                                                                 \
    } while(0)

static void fake_init(struct fake_pmu* fake, uint32_t core)
{
    memset(fake, 0, sizeof(*fake));
    fake->state.midr = TEST_MIDR;
    fake->state.mpidr = core;
    fake->state.pmcr =
        (TEST_COUNTERS << TEST_PMCR_N_SHIFT) | UINT32_C(0x18);
    fake->state.selector = 2;
    fake->state.cycle_count = UINT32_C(0x89abcdef);
    for(uint32_t i = 0; i < TEST_COUNTERS; ++i)
    {
        fake->state.event_type[i] = UINT32_C(0x20) + i;
        fake->state.event_count[i] = UINT32_C(0x10203040) + i;
    }
}

static uint32_t fake_read(
    void* context, enum vd_pmu_backend_host_register reg)
{
    struct fake_pmu* fake = (struct fake_pmu*)context;
    const uint32_t selected = fake->state.selector & UINT32_C(0x1f);
    switch(reg)
    {
        case VD_PMU_BACKEND_HOST_MIDR:
            return fake->state.midr;
        case VD_PMU_BACKEND_HOST_MPIDR:
            return fake->state.mpidr;
        case VD_PMU_BACKEND_HOST_PMCR:
            return fake->state.pmcr;
        case VD_PMU_BACKEND_HOST_PMCNTENSET:
        case VD_PMU_BACKEND_HOST_PMCNTENCLR:
            return fake->state.counter_enable;
        case VD_PMU_BACKEND_HOST_PMOVSR:
            return fake->state.overflow;
        case VD_PMU_BACKEND_HOST_PMSELR:
            return fake->state.selector;
        case VD_PMU_BACKEND_HOST_PMCCNTR:
            return fake->state.cycle_count;
        case VD_PMU_BACKEND_HOST_PMUSERENR:
            return fake->state.user_enable;
        case VD_PMU_BACKEND_HOST_PMINTENSET:
        case VD_PMU_BACKEND_HOST_PMINTENCLR:
            return fake->state.interrupt_enable;
        case VD_PMU_BACKEND_HOST_PMXEVTYPER:
            return selected < TEST_COUNTERS ?
                fake->state.event_type[selected] : 0;
        case VD_PMU_BACKEND_HOST_PMXEVCNTR:
            return selected < TEST_COUNTERS ?
                fake->state.event_count[selected] : 0;
        case VD_PMU_BACKEND_HOST_PMSWINC:
        default:
            return 0;
    }
}

static void fake_write(void* context,
                       enum vd_pmu_backend_host_register reg,
                       uint32_t value)
{
    struct fake_pmu* fake = (struct fake_pmu*)context;
    const uint32_t selected = fake->state.selector & UINT32_C(0x1f);
    ++fake->writes[reg];
    switch(reg)
    {
        case VD_PMU_BACKEND_HOST_PMCR:
        {
            if((value & TEST_PMCR_P) != 0)
                memset(fake->state.event_count, 0,
                       sizeof(fake->state.event_count));
            if((value & TEST_PMCR_C) != 0)
                fake->state.cycle_count = 0;
            const uint32_t count_field = fake->state.pmcr &
                (UINT32_C(0x1f) << TEST_PMCR_N_SHIFT);
            fake->state.pmcr = count_field |
                (value & TEST_PMCR_PERSISTENT_MASK);
            break;
        }
        case VD_PMU_BACKEND_HOST_PMCNTENSET:
            fake->state.counter_enable |= value & TEST_IMPLEMENTED_MASK;
            if(fake->increment_real_event_on_enable != 0 &&
               (value & TEST_LANE_MASK) != 0 &&
               (fake->state.event_type[TEST_LANE] & UINT32_C(0xff)) != 0)
                fake->state.event_count[TEST_LANE] += 7;
            break;
        case VD_PMU_BACKEND_HOST_PMCNTENCLR:
            fake->state.counter_enable &=
                ~(value & TEST_IMPLEMENTED_MASK);
            break;
        case VD_PMU_BACKEND_HOST_PMOVSR:
            fake->state.overflow &= ~(value & TEST_IMPLEMENTED_MASK);
            break;
        case VD_PMU_BACKEND_HOST_PMSELR:
            if(fake->fail_selector_write_ordinal != 0 &&
               fake->writes[reg] == fake->fail_selector_write_ordinal)
                break;
            fake->state.selector = value;
            break;
        case VD_PMU_BACKEND_HOST_PMCCNTR:
            fake->state.cycle_count = value;
            break;
        case VD_PMU_BACKEND_HOST_PMXEVTYPER:
            if(selected < TEST_COUNTERS)
            {
                if(selected == TEST_LANE &&
                   fake->fail_next_lane_type_and_timeout_restore != 0)
                {
                    fake->fail_next_lane_type_and_timeout_restore = 0;
                    vdPmuBackendHostTestSetDispatchMode(
                        VD_PMU_BACKEND_HOST_DISPATCH_TIMEOUT);
                }
                else
                {
                    fake->state.event_type[selected] = value;
                }
            }
            break;
        case VD_PMU_BACKEND_HOST_PMXEVCNTR:
            if(selected < TEST_COUNTERS)
                fake->state.event_count[selected] = value;
            break;
        case VD_PMU_BACKEND_HOST_PMUSERENR:
            fake->state.user_enable = value;
            break;
        case VD_PMU_BACKEND_HOST_PMINTENSET:
            fake->state.interrupt_enable |= value & TEST_IMPLEMENTED_MASK;
            break;
        case VD_PMU_BACKEND_HOST_PMINTENCLR:
            fake->state.interrupt_enable &=
                ~(value & TEST_IMPLEMENTED_MASK);
            break;
        case VD_PMU_BACKEND_HOST_PMSWINC:
            if((fake->state.pmcr & TEST_PMCR_E) != 0)
                for(uint32_t lane = 0; lane < TEST_COUNTERS; ++lane)
                    if((value & (UINT32_C(1) << lane)) != 0 &&
                       (fake->state.counter_enable &
                        (UINT32_C(1) << lane)) != 0 &&
                       (fake->state.event_type[lane] & UINT32_C(0xff)) == 0)
                        ++fake->state.event_count[lane];
            break;
        case VD_PMU_BACKEND_HOST_MIDR:
        case VD_PMU_BACKEND_HOST_MPIDR:
        default:
            break;
    }
}

static void fake_barrier(void* context,
                         enum vd_pmu_backend_host_barrier barrier)
{
    (void)context;
    (void)barrier;
}

static void start_backend(struct fake_pmu* fake)
{
    const struct vd_pmu_backend_host_register_ops ops = {
        .context = fake,
        .read = fake_read,
        .write = fake_write,
        .barrier = fake_barrier,
    };
    if(vdPmuBackendHostTestInit(&ops) != 0)
    {
        fputs("FAIL: PMU host backend initialization\n", stderr);
        ++failures;
    }
}

static struct vd_pmu_profiler_bridge_config bridge_config(void)
{
    const struct vd_pmu_profiler_bridge_config config = {
        .struct_size = sizeof(struct vd_pmu_profiler_bridge_config),
        .abi_version = VD_PMU_PROFILER_BRIDGE_ABI_VERSION,
        .owner_pid = 7,
        .owner_token = UINT32_C(0x12345678),
        .core_id = 0,
        .lease_ms = VD_PMU_SESSION_MIN_LEASE_MS,
    };
    return config;
}

static struct vp_pmu_config event_config(uint32_t event_code)
{
    struct vp_pmu_config config;
    memset(&config, 0, sizeof(config));
    config.counter_mask = VP_PMU_COUNTER_EVENT0;
    config.event_count = 1;
    config.event_codes[0] = event_code;
    return config;
}

static struct vd_kernel_pmu_profiler_info transport_info_query(void)
{
    const struct vd_kernel_pmu_profiler_info info = {
        .struct_size = sizeof(struct vd_kernel_pmu_profiler_info),
        .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
    };
    return info;
}

static struct vd_kernel_pmu_profiler_open_request transport_request(
    uint32_t event_code)
{
    const struct vd_kernel_pmu_profiler_open_request request = {
        .struct_size =
            sizeof(struct vd_kernel_pmu_profiler_open_request),
        .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
        .event_code = event_code,
        .lease_ms = VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS,
        .flags = event_code ==
                     VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS ||
                 event_code ==
                     VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS ||
                 event_code ==
                     VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT ?
            VD_KERNEL_PMU_PROFILER_OPEN_ACK_REAL_EVENT : 0,
    };
    return request;
}

struct fake_profiler_source
{
    uint64_t now_us;
    uint32_t thread_id;
};

static uint64_t fake_profiler_clock(void* user)
{
    return ((struct fake_profiler_source*)user)->now_us;
}

static uint32_t fake_profiler_thread(void* user)
{
    return ((struct fake_profiler_source*)user)->thread_id;
}

static void test_disabled_backend_fails_closed(void)
{
    struct vd_pmu_profiler_bridge bridge;
    struct vd_pmu_profiler_bridge_config config = bridge_config();
    memset(&bridge, 0, sizeof(bridge));
    vdPmuBackendHostTestReset();
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) == VP_ERROR_PLATFORM,
          "bridge refuses an unavailable kernel backend");
    CHECK(vdPmuProfilerBridgeGetProvider(&bridge) == NULL,
          "failed initialization publishes no provider");
}

static void test_version_and_scope_validation(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_bridge bridge;
    struct vd_pmu_profiler_bridge_config config = bridge_config();
    fake_init(&fake, 0);
    start_backend(&fake);
    memset(&bridge, 0, sizeof(bridge));

    --config.abi_version;
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "bridge rejects an older configuration ABI");
    config = bridge_config();
    config.core_id = VD_PMU_BACKEND_APP_CORE_COUNT;
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "bridge rejects the reserved fourth core");
    config = bridge_config();
    config.flags = UINT32_C(1) << 31;
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "bridge rejects unknown feature acknowledgements");
#if !VD_PMU_PROFILER_REAL_EVENTS_COMPILED
    config = bridge_config();
    config.flags = VD_PMU_PROFILER_BRIDGE_CONFIG_ALLOW_REAL_EVENTS;
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "normal builds reject the real-event acknowledgement");
#endif
    config = bridge_config();
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) == VP_RESULT_OK,
          "versioned bridge initializes against the ready backend");
    const struct vp_pmu_provider* provider =
        vdPmuProfilerBridgeGetProvider(&bridge);
    CHECK(provider != NULL &&
              provider->abi_version == VP_PMU_PROVIDER_ABI_VERSION &&
              provider->flags == VP_PMU_PROVIDER_FLAG_EXACT_RESTORE,
          "bridge publishes only the exact-restore provider contract");
    vdPmuBackendHostTestReset();
}

static void test_named_vitaprofiler_pipeline(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_bridge bridge;
    struct vd_pmu_profiler_bridge_config bridge_settings = bridge_config();
    struct vp_pmu_session session;
    struct vp_pmu_config pmu_config;
    struct vp_pmu_name_ids pmu_names;
    struct vp_pmu_sample sample;
    struct vp_context profiler;
    struct vp_slot slots[4];
    struct fake_profiler_source source = {UINT64_C(123456), 9};
    struct vp_config profiler_config;
    struct vp_name_dictionary dictionary;
    struct vp_name_entry entries[4];
    char name_text[128];
    struct vp_name_dictionary_config dictionary_config;
    struct vp_name_view view;
    struct vp_event event;

    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    start_backend(&fake);
    memset(&bridge, 0, sizeof(bridge));
    memset(&session, 0, sizeof(session));
    memset(&profiler_config, 0, sizeof(profiler_config));
    profiler_config.slots = slots;
    profiler_config.capacity = 4;
    profiler_config.clock = fake_profiler_clock;
    profiler_config.clock_user = &source;
    profiler_config.thread_id = fake_profiler_thread;
    profiler_config.thread_user = &source;
    memset(&dictionary_config, 0, sizeof(dictionary_config));
    dictionary_config.entries = entries;
    dictionary_config.entry_capacity = 4;
    dictionary_config.text = name_text;
    dictionary_config.text_capacity = sizeof(name_text);

    CHECK(vdPmuProfilerBridgeInit(&bridge, &bridge_settings) ==
                  VP_RESULT_OK &&
              vp_init(&profiler, &profiler_config) == VP_RESULT_OK &&
              vp_name_dictionary_init(&dictionary, &dictionary_config) ==
                  VP_RESULT_OK,
          "named bridge path initializes without PMU access");
    CHECK(vdPmuProfilerBridgePrepareEvent(
              &bridge, &dictionary, UINT32_C(0x00), &pmu_config,
              &pmu_names) == VP_RESULT_OK &&
              pmu_config.counter_mask == VP_PMU_COUNTER_EVENT0 &&
              pmu_config.event_count == 1 &&
              pmu_config.event_codes[0] == UINT32_C(0x00) &&
              pmu_names.events[0] != 0 &&
              vp_name_dictionary_seal(&dictionary) == VP_RESULT_OK &&
              vp_name_dictionary_lookup(&dictionary, pmu_names.events[0],
                                        &view) == VP_RESULT_OK &&
              view.name_length ==
                  sizeof(VD_PMU_PROFILER_NAME_SOFTWARE_INCREMENT) - 1u &&
              memcmp(view.name,
                     VD_PMU_PROFILER_NAME_SOFTWARE_INCREMENT,
                     view.name_length) == 0,
          "bridge prepares a stable named VitaProfiler selection");
    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &pmu_config) == VP_RESULT_OK,
          "named selection starts through the exact-restore provider");
    for(uint32_t i = 0; i < 5; ++i)
        vdPmuBackendHostTestWriteSoftwareIncrement(TEST_LANE_MASK);
    CHECK(vp_pmu_session_read(&session, &sample) == VP_RESULT_OK &&
              sample.events[0] == 5 &&
              vp_pmu_record_sample(&profiler, &sample, &pmu_names) ==
                  VP_RESULT_OK &&
              vp_drain(&profiler, &event, 1) == 1 &&
              event.type == VP_EVENT_COUNTER &&
              event.name_id == pmu_names.events[0] && event.value == 5 &&
              (event.flags & VP_EVENT_FLAG_RAW_VALUE) != 0,
          "kernel sample reaches the ordinary named profiler event ring");
    CHECK(vp_pmu_session_end(&session) == VP_RESULT_OK &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "named provider path restores the complete PMU snapshot");
    vp_name_dictionary_deinit(&dictionary);
    vp_deinit(&profiler);
    vdPmuBackendHostTestReset();
}

static void test_allowlisted_sample_and_exact_release(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_bridge bridge;
    struct vd_pmu_profiler_bridge other_bridge;
    struct vp_pmu_session session;
    struct vp_pmu_session other_session;
    struct vd_pmu_profiler_bridge_config config = bridge_config();
    struct vd_pmu_profiler_bridge_config other_config = bridge_config();
    struct vp_pmu_config pmu_config = event_config(0);
    struct vp_pmu_sample sample;
    struct vd_pmu_profiler_bridge_status status;

    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    start_backend(&fake);
    memset(&bridge, 0, sizeof(bridge));
    memset(&other_bridge, 0, sizeof(other_bridge));
    memset(&session, 0, sizeof(session));
    memset(&other_session, 0, sizeof(other_session));
    other_config.owner_token ^= UINT32_C(0x00ff00ff);
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) == VP_RESULT_OK,
          "bridge initializes for a sampling lease");
    CHECK(vdPmuProfilerBridgeInit(&other_bridge, &other_config) ==
              VP_RESULT_OK,
          "a second bridge may initialize without claiming the PMU");

    struct vp_pmu_config invalid = event_config(UINT32_C(0x08));
    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &invalid) == VP_ERROR_UNSUPPORTED,
          "raw event values outside the kernel allowlist are rejected");
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0,
          "rejected events cause no PMU mutation");

    invalid = event_config(UINT32_C(0x03));
    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &invalid) == VP_ERROR_UNSUPPORTED,
          "allowlisted real events remain disabled before their live gate");
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0,
          "unpromoted allowlisted events cause no PMU mutation");

    invalid = event_config(0);
    invalid.counter_mask |= VP_PMU_COUNTER_CYCLES;
    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &invalid) == VP_ERROR_UNSUPPORTED,
          "the unproved cycle-counter path remains disabled");
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0,
          "rejected cycle requests cause no PMU mutation");

    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &pmu_config) == VP_RESULT_OK,
          "allowlisted event begins a bounded exact-restore lease");
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) == VP_ERROR_BUSY,
          "active bridge cannot be reinitialized");
    CHECK((fake.state.counter_enable & TEST_LANE_MASK) != 0 &&
              (fake.state.event_type[TEST_LANE] & UINT32_C(0xff)) == 0 &&
              fake.state.event_count[TEST_LANE] == 0,
          "bridge routes the event only to fixed lane five");
    CHECK(vp_pmu_session_begin(
              &other_session,
              vdPmuProfilerBridgeGetProvider(&other_bridge),
              &pmu_config) == VP_ERROR_BUSY,
          "linked-copy lease excludes a second bridge through verification");
    for(uint32_t i = 0; i < 3; ++i)
        vdPmuBackendHostTestWriteSoftwareIncrement(TEST_LANE_MASK);
    CHECK(vp_pmu_session_read(&session, &sample) == VP_RESULT_OK &&
              sample.counter_mask == VP_PMU_COUNTER_EVENT0 &&
              sample.cycles == 0 && sample.events[0] == 3,
          "provider translates the leased kernel reading");
    CHECK(vdPmuProfilerBridgeGetStatus(&bridge, &status) == VP_RESULT_OK &&
              status.session_state == VD_PMU_SESSION_ACTIVE &&
              status.active_event_id == VD_PMU_EVENT_SOFTWARE_INCREMENT &&
              status.active_event_code == 0,
          "status reports bounded metadata without raw PMU registers");
    CHECK(vp_pmu_session_end(&session) == VP_RESULT_OK,
          "normal provider release succeeds");
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0,
          "normal release restores the complete fake PMU snapshot");
    CHECK(vp_pmu_session_begin(
              &other_session,
              vdPmuProfilerBridgeGetProvider(&other_bridge),
              &pmu_config) == VP_RESULT_OK &&
              vp_pmu_session_end(&other_session) == VP_RESULT_OK,
          "second bridge can acquire only after verified release");
    vdPmuBackendHostTestReset();
}

#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED
static void test_double_gated_real_event_catalog(void)
{
    static const uint32_t event_codes[] = {
        UINT32_C(0x01), UINT32_C(0x03), UINT32_C(0x10),
    };
    static const char* const event_names[] = {
        VD_PMU_PROFILER_NAME_ICACHE_MISS,
        VD_PMU_PROFILER_NAME_DCACHE_MISS,
        VD_PMU_PROFILER_NAME_BRANCH_MISPREDICT,
    };
    struct fake_pmu fake;
    struct vd_pmu_profiler_bridge bridge;
    struct vd_pmu_profiler_bridge_config config = bridge_config();
    struct vd_pmu_profiler_bridge_status status;

    fake_init(&fake, 0);
    fake.increment_real_event_on_enable = 1;
    const struct fake_pmu_state before = fake.state;
    start_backend(&fake);
    memset(&bridge, 0, sizeof(bridge));
    config.flags = VD_PMU_PROFILER_BRIDGE_CONFIG_ALLOW_REAL_EVENTS;
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) == VP_RESULT_OK &&
              vdPmuProfilerBridgeGetStatus(&bridge, &status) ==
                  VP_RESULT_OK &&
              status.config_flags == config.flags,
          "experimental build still requires and reports runtime opt-in");

    for(size_t i = 0; i < sizeof(event_codes) / sizeof(event_codes[0]); ++i)
    {
        struct vp_pmu_session session;
        struct vp_pmu_config pmu_config;
        struct vp_pmu_name_ids pmu_names;
        struct vp_pmu_sample sample;
        struct vp_context profiler;
        struct vp_slot slots[2];
        struct fake_profiler_source source = {
            UINT64_C(200000) + (uint64_t)i, 11};
        struct vp_config profiler_config;
        struct vp_event profiler_event;
        struct vp_name_dictionary dictionary;
        struct vp_name_entry entries[2];
        char text[96];
        struct vp_name_dictionary_config dictionary_config;
        struct vp_name_view view;

        memset(&session, 0, sizeof(session));
        memset(&profiler_config, 0, sizeof(profiler_config));
        profiler_config.slots = slots;
        profiler_config.capacity = 2;
        profiler_config.clock = fake_profiler_clock;
        profiler_config.clock_user = &source;
        profiler_config.thread_id = fake_profiler_thread;
        profiler_config.thread_user = &source;
        memset(&dictionary_config, 0, sizeof(dictionary_config));
        dictionary_config.entries = entries;
        dictionary_config.entry_capacity = 2;
        dictionary_config.text = text;
        dictionary_config.text_capacity = sizeof(text);
        CHECK(vp_init(&profiler, &profiler_config) == VP_RESULT_OK &&
                  vp_name_dictionary_init(&dictionary, &dictionary_config) ==
                      VP_RESULT_OK &&
                  vdPmuProfilerBridgePrepareEvent(
                      &bridge, &dictionary, event_codes[i], &pmu_config,
                      &pmu_names) == VP_RESULT_OK &&
                  vp_name_dictionary_seal(&dictionary) == VP_RESULT_OK &&
                  vp_name_dictionary_lookup(
                      &dictionary, pmu_names.events[0], &view) ==
                      VP_RESULT_OK &&
                  view.name_length == strlen(event_names[i]) &&
                  memcmp(view.name, event_names[i], view.name_length) == 0,
              "real-event catalog resolves an exact stable name");
        CHECK(vp_pmu_session_begin(
                  &session, vdPmuProfilerBridgeGetProvider(&bridge),
                  &pmu_config) == VP_RESULT_OK &&
                  (fake.state.event_type[TEST_LANE] & UINT32_C(0xff)) ==
                      event_codes[i] &&
                  fake.state.event_count[TEST_LANE] == UINT32_C(7),
              "live real event config remains confined to fixed lane five");
        fake.state.event_count[TEST_LANE] = UINT32_C(100) + (uint32_t)i;
        CHECK(vp_pmu_session_read(&session, &sample) == VP_RESULT_OK &&
                  sample.counter_mask == VP_PMU_COUNTER_EVENT0 &&
                  sample.events[0] == UINT32_C(100) + (uint32_t)i &&
                  vp_pmu_record_sample(&profiler, &sample, &pmu_names) ==
                      VP_RESULT_OK &&
                  vp_drain(&profiler, &profiler_event, 1) == 1 &&
                  profiler_event.type == VP_EVENT_COUNTER &&
                  profiler_event.name_id == pmu_names.events[0] &&
                  profiler_event.value ==
                      (int64_t)(UINT32_C(100) + (uint32_t)i),
              "real-event reading reaches the named profiler ring");
        CHECK(vp_pmu_session_end(&session) == VP_RESULT_OK &&
                  memcmp(&fake.state, &before, sizeof(before)) == 0,
              "each real-event lease restores exact prior state");
        vp_name_dictionary_deinit(&dictionary);
        vp_deinit(&profiler);
    }

    struct vp_pmu_session rejected_session;
    struct vp_pmu_config rejected = event_config(UINT32_C(0x08));
    memset(&rejected_session, 0, sizeof(rejected_session));
    CHECK(vp_pmu_session_begin(
              &rejected_session, vdPmuProfilerBridgeGetProvider(&bridge),
              &rejected) == VP_ERROR_UNSUPPORTED &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "experimental opt-in still rejects events outside the small list");

    struct vp_pmu_session watchdog_session;
    struct vp_pmu_config watchdog_config = event_config(UINT32_C(0x10));
    memset(&watchdog_session, 0, sizeof(watchdog_session));
    CHECK(vp_pmu_session_begin(
              &watchdog_session,
              vdPmuProfilerBridgeGetProvider(&bridge),
              &watchdog_config) == VP_RESULT_OK,
          "real-event lease enters the common watchdog path");
    vdPmuBackendHostTestAdvanceTimeUs(
        (uint64_t)config.lease_ms * UINT64_C(1000));
    CHECK(vdPmuProfilerBridgeWatchdog(&bridge) ==
                  VD_PMU_SESSION_WATCHDOG_RESTORED &&
              memcmp(&fake.state, &before, sizeof(before)) == 0 &&
              vp_pmu_session_end(&watchdog_session) == VP_RESULT_OK,
          "real-event expiry restores exactly before outer release ack");
    vdPmuBackendHostTestReset();
}
#endif

static void test_watchdog_restores_and_requires_release_ack(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_bridge bridge;
    struct vp_pmu_session session;
    struct vp_pmu_session blocked_session;
    struct vd_pmu_profiler_bridge_config config = bridge_config();
    struct vp_pmu_config pmu_config = event_config(0);
    struct vp_pmu_sample sample;
    struct vd_pmu_profiler_bridge_status status;

    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    start_backend(&fake);
    memset(&bridge, 0, sizeof(bridge));
    memset(&session, 0, sizeof(session));
    memset(&blocked_session, 0, sizeof(blocked_session));
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) == VP_RESULT_OK &&
              vp_pmu_session_begin(
                  &session, vdPmuProfilerBridgeGetProvider(&bridge),
                  &pmu_config) == VP_RESULT_OK,
          "watchdog test starts one lease");
    vdPmuBackendHostTestAdvanceTimeUs(
        (uint64_t)config.lease_ms * UINT64_C(1000));
    CHECK(vdPmuProfilerBridgeWatchdog(&bridge) ==
              VD_PMU_SESSION_WATCHDOG_RESTORED,
          "watchdog expires and restores the bounded lease");
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0,
          "watchdog restores exact PMU state");
    CHECK(vdPmuProfilerBridgeGetStatus(&bridge, &status) == VP_RESULT_OK &&
              status.session_state == VD_PMU_SESSION_EMPTY &&
              status.auto_restored_waiting_release == 1,
          "auto-restored token remains reserved for outer release");
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) ==
              VP_ERROR_RESTORE_REQUIRED,
          "auto-restored bridge cannot be reinitialized before release ack");
    CHECK(vp_pmu_session_read(&session, &sample) == VP_ERROR_STATE,
          "expired outer session cannot sample restored hardware");
    CHECK(vp_pmu_session_begin(
              &blocked_session, vdPmuProfilerBridgeGetProvider(&bridge),
              &pmu_config) == VP_ERROR_BUSY,
          "new leases stay blocked until the outer owner acknowledges release");
    CHECK(vp_pmu_session_end(&session) == VP_RESULT_OK,
          "outer session acknowledges the already-restored token");
    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &pmu_config) == VP_RESULT_OK &&
              vp_pmu_session_end(&session) == VP_RESULT_OK,
          "a new lease is blocked until acknowledgement, then succeeds");
    vdPmuBackendHostTestReset();
}

static void test_release_failure_retains_restore_obligation(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_bridge bridge;
    struct vp_pmu_session session;
    struct vd_pmu_profiler_bridge_config config = bridge_config();
    struct vp_pmu_config pmu_config = event_config(0);
    struct vd_pmu_profiler_bridge_status status;

    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    start_backend(&fake);
    memset(&bridge, 0, sizeof(bridge));
    memset(&session, 0, sizeof(session));
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) == VP_RESULT_OK &&
              vp_pmu_session_begin(
                  &session, vdPmuProfilerBridgeGetProvider(&bridge),
                  &pmu_config) == VP_RESULT_OK,
          "restore retry test starts one lease");

    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_TIMEOUT);
    CHECK(vp_pmu_session_end(&session) == VP_ERROR_RESTORE_REQUIRED,
          "ambiguous release retains the provider lease");
    CHECK(vdPmuProfilerBridgeGetStatus(&bridge, &status) == VP_RESULT_OK &&
              status.session_state == VD_PMU_SESSION_RESTORE_PENDING &&
              status.last_provider_result == VP_ERROR_RESTORE_REQUIRED,
          "restore obligation is observable but not discardable");
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) ==
              VP_ERROR_RESTORE_REQUIRED,
          "pending ordinary restore cannot be discarded by reinitialization");

    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_INLINE);
    CHECK(vdPmuBackendHostTestCompleteInflight() == 0,
          "late kernel restore completes in the test harness");
    CHECK(vp_pmu_session_end(&session) == VP_RESULT_OK,
          "retrying the same outer release proves restoration");
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0 &&
              !vdPmuBackendHasRestoreObligation() && vdPmuBackendReady(),
          "release retry clears both session and backend obligations");
    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &pmu_config) == VP_RESULT_OK &&
              vp_pmu_session_end(&session) == VP_RESULT_OK,
          "late-restore recovery re-enables a fresh verified lease");
    vdPmuBackendHostTestReset();
}

static void test_failed_acquire_retains_orphan_restore(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_bridge bridge;
    struct vp_pmu_session session;
    struct vd_pmu_profiler_bridge_config config = bridge_config();
    struct vp_pmu_config pmu_config = event_config(0);
    struct vd_pmu_profiler_bridge_status status;

    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    start_backend(&fake);
    memset(&bridge, 0, sizeof(bridge));
    memset(&session, 0, sizeof(session));
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) == VP_RESULT_OK,
          "orphan restore test initializes the bridge");

    /* Corrupt the configure verification, then make its automatic restore
     * time out.  The outer session never receives the kernel lease token. */
    fake.fail_next_lane_type_and_timeout_restore = 1;
    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &pmu_config) == VP_ERROR_RESTORE_REQUIRED,
          "ambiguous failed acquire retains an internal restoration lease");
    CHECK(vdPmuProfilerBridgeGetStatus(&bridge, &status) == VP_RESULT_OK &&
              status.session_state == VD_PMU_SESSION_RESTORE_PENDING &&
              status.orphan_restore_pending == 1,
          "failed acquire exposes its orphan restoration obligation");
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) ==
              VP_ERROR_RESTORE_REQUIRED,
          "orphan restore cannot be discarded by reinitialization");
    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &pmu_config) == VP_ERROR_BUSY,
          "orphan obligation blocks every new provider lease");

    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_INLINE);
    CHECK(vdPmuBackendHostTestCompleteInflight() == 0,
          "late orphan restore completes in the kernel harness");
    CHECK(vdPmuProfilerBridgeRetryOrphanRestore(&bridge) == VP_RESULT_OK,
          "dedicated orphan retry verifies and clears exact restoration");
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0 &&
              !vdPmuBackendHasRestoreObligation(),
          "orphan retry restores the complete snapshot");
    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &pmu_config) == VP_RESULT_OK &&
              vp_pmu_session_end(&session) == VP_RESULT_OK,
          "provider accepts a new lease only after orphan recovery");
    vdPmuBackendHostTestReset();
}

static void test_busy_hardware_maps_to_stable_busy_result(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_bridge bridge;
    struct vp_pmu_session session;
    struct vd_pmu_profiler_bridge_config config = bridge_config();
    struct vp_pmu_config pmu_config = event_config(0);
    struct vd_pmu_profiler_bridge_status status;

    fake_init(&fake, 0);
    fake.state.counter_enable = UINT32_C(1) << 1;
    const struct fake_pmu_state busy_before = fake.state;
    start_backend(&fake);
    memset(&bridge, 0, sizeof(bridge));
    memset(&session, 0, sizeof(session));
    CHECK(vdPmuProfilerBridgeInit(&bridge, &config) == VP_RESULT_OK,
          "busy-hardware test initializes without touching PMU state");
    CHECK(vp_pmu_session_begin(
              &session, vdPmuProfilerBridgeGetProvider(&bridge),
              &pmu_config) == VP_ERROR_BUSY,
          "an existing PMU owner maps to stable provider busy");
    CHECK(vdPmuProfilerBridgeGetStatus(&bridge, &status) == VP_RESULT_OK &&
              status.last_kernel_error == VD_PMU_BACKEND_ERROR_NOT_IDLE &&
              status.last_provider_result == VP_ERROR_BUSY &&
              status.provider_lease_held == 0,
          "busy failure retains its raw cause and releases bridge ownership");
    CHECK(memcmp(&fake.state, &busy_before, sizeof(busy_before)) == 0,
          "busy PMU refusal performs no register writes");
    vdPmuBackendHostTestReset();
}

static void test_transport_version_owner_and_exact_release(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_info info = transport_info_query();
    struct vd_kernel_pmu_profiler_open_request request =
        transport_request(VD_KERNEL_PMU_PROFILER_EVENT_SOFTWARE_INCREMENT);
    struct vd_kernel_pmu_profiler_handle handle;
    struct vd_kernel_pmu_profiler_sample sample;
    const int32_t owner_pid = 31;
    const int32_t owner_thread = 47;

    fake_init(&fake, VD_KERNEL_PMU_PROFILER_FIXED_CORE);
    const struct fake_pmu_state before = fake.state;
    start_backend(&fake);
    memset(&transport, 0, sizeof(transport));
    CHECK(vdPmuProfilerTransportInit(&transport) == 0,
          "transport initializes only over a ready backend");
    CHECK(vdPmuProfilerTransportGetInfo(&transport, &info) == 0 &&
              info.struct_size == sizeof(info) &&
              info.abi_version == VD_KERNEL_PMU_PROFILER_ABI_VERSION &&
              info.fixed_core == VD_KERNEL_PMU_PROFILER_FIXED_CORE &&
              info.fixed_counter ==
                  VD_KERNEL_PMU_PROFILER_FIXED_COUNTER &&
              info.min_lease_ms ==
                  VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS &&
              info.max_lease_ms ==
                  VD_KERNEL_PMU_PROFILER_MAX_LEASE_MS &&
              info.event_count >= 1 && info.event_codes[0] == 0,
          "versioned transport info reports only fixed resources");

    info = transport_info_query();
    info.abi_version++;
    CHECK(vdPmuProfilerTransportGetInfo(&transport, &info) ==
              VD_KERNEL_ERROR_PMU_PROFILER_INVALID,
          "transport rejects a mismatched info ABI");
    info = transport_info_query();
    info.reserved[3] = 1;
    CHECK(vdPmuProfilerTransportGetInfo(&transport, &info) ==
              VD_KERNEL_ERROR_PMU_PROFILER_INVALID,
          "transport rejects nonzero negotiation residue");

    request.struct_size--;
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_INVALID,
          "transport rejects an inexact open-request size");
    request = transport_request(UINT32_C(0x08));
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_UNSUPPORTED_EVENT &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "transport rejects arbitrary event codes without PMU access");

    request = transport_request(0);
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) == 0 &&
              handle.struct_size == sizeof(handle) &&
              handle.abi_version ==
                  VD_KERNEL_PMU_PROFILER_ABI_VERSION &&
              handle.owner_token != 0 && handle.generation != 0 &&
              handle.event_code == 0 &&
              (fake.state.counter_enable & TEST_LANE_MASK) != 0 &&
              fake.state.event_type[TEST_LANE] == 0,
          "transport opens only fixed core 0 lane 5 with a kernel handle");

    struct vd_kernel_pmu_profiler_handle tampered = handle;
    tampered.event_code = UINT32_C(0x03);
    CHECK(vdPmuProfilerTransportRead(
              &transport, owner_pid, owner_thread, &tampered, &sample) ==
              VD_KERNEL_ERROR_PMU_PROFILER_OWNER,
          "complete handle tampering is rejected");
    CHECK(vdPmuProfilerTransportRead(
              &transport, owner_pid + 1, owner_thread, &handle, &sample) ==
              VD_KERNEL_ERROR_PMU_PROFILER_OWNER &&
              vdPmuProfilerTransportRead(
                  &transport, owner_pid, owner_thread + 1, &handle,
                  &sample) == VD_KERNEL_ERROR_PMU_PROFILER_OWNER,
          "transport binds the lease to exact process and controller thread");

    for(uint32_t i = 0; i < 9; ++i)
        vdPmuBackendHostTestWriteSoftwareIncrement(TEST_LANE_MASK);
    CHECK(vdPmuProfilerTransportRead(
              &transport, owner_pid, owner_thread, &handle, &sample) == 0 &&
              sample.struct_size == sizeof(sample) &&
              sample.abi_version ==
                  VD_KERNEL_PMU_PROFILER_ABI_VERSION &&
              sample.owner_token == handle.owner_token &&
              sample.generation == handle.generation &&
              sample.event_code == 0 &&
              sample.core_id == VD_KERNEL_PMU_PROFILER_FIXED_CORE &&
              sample.physical_counter ==
                  VD_KERNEL_PMU_PROFILER_FIXED_COUNTER &&
              sample.value == 9 && sample.flags == 0,
          "transport returns one bounded fixed-lane sample");
    CHECK(vdPmuProfilerTransportClose(
              &transport, owner_pid, owner_thread, &handle) == 0 &&
              memcmp(&fake.state, &before, sizeof(before)) == 0 &&
              !vdPmuBackendHasRestoreObligation(),
          "transport close proves exact complete-state restoration");
    CHECK(vdPmuProfilerTransportRead(
              &transport, owner_pid, owner_thread, &handle, &sample) ==
              VD_KERNEL_ERROR_PMU_PROFILER_OWNER,
          "a retired handle cannot be replayed");
    CHECK(vdPmuProfilerTransportShutdown(&transport) == 0,
          "software-only transport can shut down after exact restoration");
    vdPmuBackendHostTestReset();
}

static void test_transport_watchdog_and_orphan_cleanup(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_open_request request =
        transport_request(0);
    struct vd_kernel_pmu_profiler_handle handle;
    const int32_t owner_pid = 53;
    const int32_t owner_thread = 59;

    fake_init(&fake, VD_KERNEL_PMU_PROFILER_FIXED_CORE);
    const struct fake_pmu_state before = fake.state;
    start_backend(&fake);
    memset(&transport, 0, sizeof(transport));
    CHECK(vdPmuProfilerTransportInit(&transport) == 0 &&
              vdPmuProfilerTransportOpen(
                  &transport, owner_pid, owner_thread, &request,
                  &handle) == 0,
          "watchdog transport lease opens");
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 0,
          "watchdog leaves an unexpired session active");
    vdPmuBackendHostTestAdvanceTimeUs(
        ((uint64_t)VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS + 1u) * 1000u);
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 1 &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              memcmp(&fake.state, &before, sizeof(before)) == 0 &&
              !vdPmuBackendHasRestoreObligation(),
          "lease expiry restores and retires an abandoned owner");

    fake.fail_next_lane_type_and_timeout_restore = 1;
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
              transport.state ==
                  VD_PMU_PROFILER_TRANSPORT_CLEANUP_REQUIRED,
          "failed acquire retains an ownerless restoration obligation");
    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_INLINE);
    CHECK(vdPmuBackendHostTestCompleteInflight() == 0,
          "late orphan restore finishes in the fake kernel");
    const int orphan_watchdog =
        vdPmuProfilerTransportWatchdog(&transport);
    if(orphan_watchdog != 1)
    {
        struct vd_pmu_profiler_bridge_status orphan_status;
        memset(&orphan_status, 0, sizeof(orphan_status));
        (void)vdPmuProfilerBridgeGetStatus(&transport.bridge,
                                           &orphan_status);
        fprintf(stderr,
                "orphan diagnostic: result=%d state=%u last=%d "
                "session=%u orphan=%u auto=%u lease=%u backend=%d\n",
                orphan_watchdog, transport.state, transport.last_result,
                orphan_status.session_state,
                orphan_status.orphan_restore_pending,
                orphan_status.auto_restored_waiting_release,
                orphan_status.provider_lease_held,
                vdPmuBackendHasRestoreObligation());
    }
    CHECK(orphan_watchdog == 1,
          "transport watchdog retires the failed-open orphan");
    CHECK(transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE,
          "orphan cleanup returns the transport to idle");
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0,
          "orphan cleanup restores the complete PMU snapshot");
    CHECK(!vdPmuBackendHasRestoreObligation(),
          "orphan cleanup retires the backend restoration obligation");
    vdPmuBackendHostTestReset();
}

static void test_transport_watchdog_recovers_idle_snapshot_failure(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_transport transport;
#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED
    struct vd_kernel_pmu_profiler_open_request request =
        transport_request(VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS);
#else
    struct vd_kernel_pmu_profiler_open_request request =
        transport_request(VD_KERNEL_PMU_PROFILER_EVENT_SOFTWARE_INCREMENT);
#endif
    struct vd_kernel_pmu_profiler_handle handle;
    const int32_t owner_pid = 71;
    const int32_t owner_thread = 73;

    fake_init(&fake, VD_KERNEL_PMU_PROFILER_FIXED_CORE);
    const struct fake_pmu_state before = fake.state;
    start_backend(&fake);
    memset(&transport, 0, sizeof(transport));
    CHECK(vdPmuProfilerTransportInit(&transport) == 0,
          "idle-recovery transport initializes");

    /* Snapshot selects lane 5, then its attempt to restore the caller's
     * original selector is ignored.  There is no session lease yet, so the
     * transport returns to IDLE while the backend remains obligated. */
    fake.fail_selector_write_ordinal =
        fake.writes[VD_PMU_BACKEND_HOST_PMSELR] + 2u;
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              transport.owner_pid == -1 &&
              transport.owner_thread == -1 &&
              vdPmuBackendRecoveryPending() &&
              vdPmuBackendHasRestoreObligation() &&
              !vdPmuBackendReady() &&
              fake.state.selector == TEST_LANE,
          "failed first snapshot stays fail-closed with an ownerless obligation");

    /* Prove the live watchdog retains and retries the obligation instead of
     * clearing it after one failed restoration. */
    fake.fail_selector_write_ordinal =
        fake.writes[VD_PMU_BACKEND_HOST_PMSELR] + 1u;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              transport.owner_pid == -1 &&
              transport.owner_thread == -1 &&
              vdPmuBackendRecoveryPending() &&
              vdPmuBackendHasRestoreObligation() &&
              !vdPmuBackendReady() &&
              fake.state.selector == TEST_LANE,
          "failed idle watchdog recovery remains fail-closed and retryable");

    fake.fail_selector_write_ordinal = 0;
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 1 &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              !vdPmuBackendRecoveryPending() &&
              !vdPmuBackendHasRestoreObligation() &&
              vdPmuBackendReady() &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "idle watchdog proves exact selector recovery and re-enables backend");

#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED,
          "failed real-event snapshot still consumes the per-boot attempt");
    request = transport_request(
        VD_KERNEL_PMU_PROFILER_EVENT_SOFTWARE_INCREMENT);
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) == 0 &&
              vdPmuProfilerTransportClose(
                  &transport, owner_pid, owner_thread, &handle) == 0 &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "software session remains available after failed real-event recovery");
#else
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) == 0 &&
              vdPmuProfilerTransportClose(
                  &transport, owner_pid, owner_thread, &handle) == 0 &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "software session works after exact idle recovery");
#endif
    vdPmuBackendHostTestReset();
}

static void test_transport_watchdog_reaps_idle_snapshot_timeout(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_open_request request =
        transport_request(VD_KERNEL_PMU_PROFILER_EVENT_SOFTWARE_INCREMENT);
    struct vd_kernel_pmu_profiler_handle handle;
    const int32_t owner_pid = 79;
    const int32_t owner_thread = 83;

    fake_init(&fake, VD_KERNEL_PMU_PROFILER_FIXED_CORE);
    const struct fake_pmu_state before = fake.state;
    start_backend(&fake);
    memset(&transport, 0, sizeof(transport));
    CHECK(vdPmuProfilerTransportInit(&transport) == 0,
          "snapshot-timeout transport initializes");

    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_TIMEOUT);
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              vdPmuBackendHostTestIsInflight() &&
              vdPmuBackendRecoveryPending() &&
              !vdPmuBackendHasRestoreObligation() &&
              !vdPmuBackendReady(),
          "timed-out first snapshot remains visible to idle watchdog");

    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_INLINE);
    CHECK(vdPmuBackendHostTestCompleteInflight() == 0 &&
              vdPmuBackendHostTestIsInflight() &&
              vdPmuBackendRecoveryPending(),
          "late snapshot completion remains pending until watchdog reap");
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 1 &&
              !vdPmuBackendHostTestIsInflight() &&
              !vdPmuBackendRecoveryPending() &&
              vdPmuBackendReady() &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "idle watchdog reaps late snapshot and proves exact recovery");
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) == 0 &&
              vdPmuProfilerTransportClose(
                  &transport, owner_pid, owner_thread, &handle) == 0 &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "software session works after a late snapshot is reaped");
    vdPmuBackendHostTestReset();
}

static void test_transport_watchdog_keeps_negative_late_snapshot_disabled(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_open_request request =
        transport_request(VD_KERNEL_PMU_PROFILER_EVENT_SOFTWARE_INCREMENT);
    struct vd_kernel_pmu_profiler_handle handle;

    fake_init(&fake, VD_KERNEL_PMU_PROFILER_FIXED_CORE);
    start_backend(&fake);
    memset(&transport, 0, sizeof(transport));
    CHECK(vdPmuProfilerTransportInit(&transport) == 0,
          "negative late-snapshot transport initializes");

    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_TIMEOUT);
    CHECK(vdPmuProfilerTransportOpen(
              &transport, 89, 97, &request, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_PLATFORM &&
              vdPmuBackendHostTestIsInflight() &&
              !vdPmuBackendReady(),
          "negative late-snapshot fixture times out ownerless");

    /* The worker later proves it ran on an unexpected core/CPU identity and
     * completed without publishing any exact-restore record.  Reaping that
     * negative result must not turn readiness back on merely because the
     * obligation flags are empty. */
    fake.state.midr = 0;
    CHECK(vdPmuBackendHostTestCompleteInflight() ==
              VD_PMU_BACKEND_ERROR_CORE &&
              vdPmuBackendRecoveryPending(),
          "late snapshot records a completed negative command");
    CHECK(vdPmuProfilerTransportWatchdog(&transport) ==
              VD_KERNEL_ERROR_PMU_PROFILER_RESTORE_REQUIRED &&
              transport.state == VD_PMU_PROFILER_TRANSPORT_IDLE &&
              !vdPmuBackendHostTestIsInflight() &&
              !vdPmuBackendRecoveryPending() &&
              !vdPmuBackendHasRestoreObligation() &&
              !vdPmuBackendReady(),
          "negative late snapshot is reaped but backend stays fail-closed");
    CHECK(vdPmuProfilerTransportWatchdog(&transport) == 0 &&
              !vdPmuBackendReady(),
          "idle watchdog never blindly re-enables an unproved backend");
    fake.state.midr = TEST_MIDR;
    CHECK(vdPmuBackendRecover() == 0 && !vdPmuBackendReady(),
          "second direct recovery cannot re-enable an unproved backend");
    vdPmuBackendHostTestReset();
}

static void test_transport_real_event_gate(void)
{
    struct fake_pmu fake;
    struct vd_pmu_profiler_transport transport;
    struct vd_kernel_pmu_profiler_open_request request =
        transport_request(VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS);
    struct vd_kernel_pmu_profiler_handle handle;
    struct vd_kernel_pmu_profiler_sample sample;
    const int32_t owner_pid = 61;
    const int32_t owner_thread = 67;

    fake_init(&fake, VD_KERNEL_PMU_PROFILER_FIXED_CORE);
    const struct fake_pmu_state before = fake.state;
    fake.increment_real_event_on_enable = 1;
    start_backend(&fake);
    memset(&transport, 0, sizeof(transport));
    CHECK(vdPmuProfilerTransportInit(&transport) == 0,
          "real-event transport initializes without PMU access");
#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) == 0,
          "double-gated transport admits event 0x01");
    CHECK(vdPmuProfilerTransportRead(
              &transport, owner_pid, owner_thread, &handle, &sample) == 0 &&
              sample.value == 7 &&
              vdPmuProfilerTransportClose(
                  &transport, owner_pid, owner_thread, &handle) == 0 &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "real-event sample closes with exact restoration");
    request = transport_request(
        VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS);
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "a second real-event attempt is locked out until reboot");
    CHECK(vdPmuProfilerTransportInit(&transport) == 0 &&
              vdPmuProfilerTransportOpen(
                  &transport, owner_pid, owner_thread, &request,
                  &handle) == VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED,
          "idempotent reinitialization cannot clear the per-boot latch");
    CHECK(vdPmuProfilerTransportShutdown(&transport) ==
              VD_KERNEL_ERROR_PMU_PROFILER_REBOOT_REQUIRED,
          "module unload cannot bypass the per-boot latch");
#else
    (void)sample;
    CHECK(vdPmuProfilerTransportOpen(
              &transport, owner_pid, owner_thread, &request, &handle) ==
              VD_KERNEL_ERROR_PMU_PROFILER_UNSUPPORTED_EVENT &&
              memcmp(&fake.state, &before, sizeof(before)) == 0,
          "ordinary transport build rejects every real event without writes");
#endif
    vdPmuBackendHostTestReset();
}

int main(void)
{
    test_disabled_backend_fails_closed();
    test_version_and_scope_validation();
    test_named_vitaprofiler_pipeline();
    test_allowlisted_sample_and_exact_release();
#if VD_PMU_PROFILER_REAL_EVENTS_COMPILED
    test_double_gated_real_event_catalog();
#endif
    test_watchdog_restores_and_requires_release_ack();
    test_release_failure_retains_restore_obligation();
    test_failed_acquire_retains_orphan_restore();
    test_busy_hardware_maps_to_stable_busy_result();
    test_transport_version_owner_and_exact_release();
    test_transport_watchdog_and_orphan_cleanup();
    test_transport_watchdog_recovers_idle_snapshot_failure();
    test_transport_watchdog_reaps_idle_snapshot_timeout();
    test_transport_watchdog_keeps_negative_late_snapshot_disabled();
    test_transport_real_event_gate();
    vdPmuBackendHostTestReset();

    if(failures != 0)
    {
        fprintf(stderr, "%d PMU profiler bridge test(s) failed\n",
                failures);
        return 1;
    }
    puts("PASS: lease-protected PMU profiler bridge semantics");
    return 0;
}

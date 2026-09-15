#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pmu_backend_host_test.h"

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

struct fake_pmu_state {
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

struct fake_pmu {
    struct fake_pmu_state state;
    uint32_t writes[VD_PMU_BACKEND_HOST_PMINTENCLR + 1u];
    uint32_t last_write[VD_PMU_BACKEND_HOST_PMINTENCLR + 1u];
    uint32_t isb_count;
    uint32_t dsb_count;
    uint32_t pmcr_event_reset_commands;
    uint32_t pmcr_cycle_reset_commands;
    uint32_t fail_selector_write_ordinal;
};

static int failures;

#define CHECK(condition)                                                   \
    do {                                                                   \
        if(!(condition)) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                           \
            ++failures;                                                    \
            return;                                                        \
        }                                                                  \
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
        fake->state.event_type[i] = UINT32_C(0x10) + i;
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
        case VD_PMU_BACKEND_HOST_PMXEVTYPER:
            return selected < TEST_COUNTERS ?
                fake->state.event_type[selected] : 0;
        case VD_PMU_BACKEND_HOST_PMXEVCNTR:
            return selected < TEST_COUNTERS ?
                fake->state.event_count[selected] : 0;
        case VD_PMU_BACKEND_HOST_PMUSERENR:
            return fake->state.user_enable;
        case VD_PMU_BACKEND_HOST_PMINTENSET:
        case VD_PMU_BACKEND_HOST_PMINTENCLR:
            return fake->state.interrupt_enable;
        case VD_PMU_BACKEND_HOST_PMSWINC:
        default:
            return 0;
    }
}

static void fake_increment_event(struct fake_pmu* fake, uint32_t lane)
{
    const uint32_t prior = fake->state.event_count[lane];
    fake->state.event_count[lane] = prior + UINT32_C(1);
    if(prior == UINT32_MAX)
        fake->state.overflow |= UINT32_C(1) << lane;
}

static void fake_write(void* context,
                       enum vd_pmu_backend_host_register reg,
                       uint32_t value)
{
    struct fake_pmu* fake = (struct fake_pmu*)context;
    const uint32_t selected = fake->state.selector & UINT32_C(0x1f);
    ++fake->writes[reg];
    fake->last_write[reg] = value;

    switch(reg)
    {
        case VD_PMU_BACKEND_HOST_PMCR:
        {
            if((value & TEST_PMCR_P) != 0)
            {
                ++fake->pmcr_event_reset_commands;
                memset(fake->state.event_count, 0,
                       sizeof(fake->state.event_count));
            }
            if((value & TEST_PMCR_C) != 0)
            {
                ++fake->pmcr_cycle_reset_commands;
                fake->state.cycle_count = 0;
            }
            const uint32_t count_field = fake->state.pmcr &
                (UINT32_C(0x1f) << TEST_PMCR_N_SHIFT);
            fake->state.pmcr = count_field |
                (value & TEST_PMCR_PERSISTENT_MASK);
            break;
        }
        case VD_PMU_BACKEND_HOST_PMCNTENSET:
            fake->state.counter_enable |= value & TEST_IMPLEMENTED_MASK;
            break;
        case VD_PMU_BACKEND_HOST_PMCNTENCLR:
            fake->state.counter_enable &=
                ~(value & TEST_IMPLEMENTED_MASK);
            break;
        case VD_PMU_BACKEND_HOST_PMOVSR:
            fake->state.overflow &= ~(value & TEST_IMPLEMENTED_MASK);
            break;
        case VD_PMU_BACKEND_HOST_PMSWINC:
            if((fake->state.pmcr & TEST_PMCR_E) != 0)
                for(uint32_t lane = 0; lane < TEST_COUNTERS; ++lane)
                    if((value & (UINT32_C(1) << lane)) != 0 &&
                       (fake->state.counter_enable &
                        (UINT32_C(1) << lane)) != 0 &&
                       (fake->state.event_type[lane] & UINT32_C(0xff)) == 0)
                        fake_increment_event(fake, lane);
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
                fake->state.event_type[selected] = value;
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
        case VD_PMU_BACKEND_HOST_MIDR:
        case VD_PMU_BACKEND_HOST_MPIDR:
        default:
            break;
    }
}

static void fake_barrier(void* context,
                         enum vd_pmu_backend_host_barrier barrier)
{
    struct fake_pmu* fake = (struct fake_pmu*)context;
    if(barrier == VD_PMU_BACKEND_HOST_ISB)
        ++fake->isb_count;
    else if(barrier == VD_PMU_BACKEND_HOST_DSB)
        ++fake->dsb_count;
}

static struct vd_pmu_session_backend start_backend(struct fake_pmu* fake)
{
    const struct vd_pmu_backend_host_register_ops ops = {
        .context = fake,
        .read = fake_read,
        .write = fake_write,
        .barrier = fake_barrier,
    };
    struct vd_pmu_session_backend backend;
    memset(&backend, 0, sizeof(backend));
    if(vdPmuBackendHostTestInit(&ops) != 0 ||
       vdPmuBackendMakeSessionBackend(&backend) != 0)
    {
        fprintf(stderr, "FAIL: could not initialize PMU backend harness\n");
        ++failures;
    }
    return backend;
}

static struct vd_pmu_configuration make_configuration(
    const struct vd_pmu_snapshot* original)
{
    struct vd_pmu_configuration configuration;
    memset(&configuration, 0, sizeof(configuration));
    configuration.event_count = 1;
    if((original->raw_pmcr & TEST_PMCR_E) == 0)
        configuration.control_flags = VD_PMU_CONFIGURATION_ENABLE_GLOBAL;
    configuration.events[0].event_id =
        VD_PMU_EVENT_SOFTWARE_INCREMENT;
    configuration.events[0].event_code = 0;
    configuration.events[0].physical_counter = TEST_LANE;
    return configuration;
}

static void test_register_wrappers_preserve_arm_semantics(void)
{
    struct fake_pmu fake;
    fake_init(&fake, 0);
    (void)start_backend(&fake);

    vdPmuBackendHostTestWriteCounterEnableSet(
        (UINT32_C(1) << 1) | TEST_LANE_MASK);
    CHECK(fake.state.counter_enable ==
          ((UINT32_C(1) << 1) | TEST_LANE_MASK));
    vdPmuBackendHostTestWriteCounterEnableClear(UINT32_C(1) << 1);
    CHECK(fake.state.counter_enable == TEST_LANE_MASK);

    /* Model PMINTENSET as W1S, then exercise the backend's W1C path. */
    fake_write(&fake, VD_PMU_BACKEND_HOST_PMINTENSET,
               (UINT32_C(1) << 1) | TEST_LANE_MASK);
    vdPmuBackendHostTestWriteInterruptEnableClear(TEST_LANE_MASK);
    CHECK(fake.state.interrupt_enable == (UINT32_C(1) << 1));
    CHECK(fake.writes[VD_PMU_BACKEND_HOST_PMINTENCLR] == 1);

    fake.state.overflow = (UINT32_C(1) << 1) | TEST_LANE_MASK;
    vdPmuBackendHostTestWriteOverflowClear(TEST_LANE_MASK);
    CHECK(fake.state.overflow == (UINT32_C(1) << 1));

    const uint32_t lane_zero_before = fake.state.event_count[0];
    const uint32_t lane_five_before = fake.state.event_count[TEST_LANE];
    const uint32_t cycle_before = fake.state.cycle_count;
    vdPmuBackendHostTestWritePmcr(
        TEST_PMCR_E | TEST_PMCR_P | TEST_PMCR_C | UINT32_C(0x38));
    CHECK((fake.last_write[VD_PMU_BACKEND_HOST_PMCR] &
           (TEST_PMCR_P | TEST_PMCR_C)) == 0);
    CHECK(fake.pmcr_event_reset_commands == 0);
    CHECK(fake.pmcr_cycle_reset_commands == 0);
    CHECK(fake.state.event_count[0] == lane_zero_before);
    CHECK(fake.state.event_count[TEST_LANE] == lane_five_before);
    CHECK(fake.state.cycle_count == cycle_before);

    vdPmuBackendHostTestWriteSelector(TEST_LANE);
    vdPmuBackendHostTestWriteEventType(UINT32_C(0x1ff));
    vdPmuBackendHostTestWriteEventCount(UINT32_MAX);
    vdPmuBackendHostTestWriteSoftwareIncrement(TEST_LANE_MASK);
    CHECK(fake.state.event_type[TEST_LANE] == UINT32_C(0xff));
    CHECK(fake.state.event_count[TEST_LANE] == UINT32_MAX);

    vdPmuBackendHostTestWriteEventType(0);
    vdPmuBackendHostTestWriteSoftwareIncrement(TEST_LANE_MASK);
    CHECK(fake.state.event_count[TEST_LANE] == 0);
    CHECK((fake.state.overflow & TEST_LANE_MASK) != 0);
    CHECK(fake.state.event_count[0] == lane_zero_before);
}

static void test_self_test_restores_exact_architectural_state(void)
{
    struct fake_pmu fake;
    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    (void)start_backend(&fake);

    struct vd_pmu_backend_test_result result;
    memset(&result, 0, sizeof(result));
    CHECK(vdPmuBackendRunSelfTest(0, &result) == 0);
    CHECK(result.stage == VD_PMU_BACKEND_TEST_COMPLETE);
    CHECK(result.operation_result == 0);
    CHECK(result.restore_result == 0);
    CHECK(result.observed_count == VD_PMU_BACKEND_TEST_INCREMENT_COUNT);
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0);
    CHECK(fake.writes[VD_PMU_BACKEND_HOST_PMSWINC] ==
          VD_PMU_BACKEND_TEST_INCREMENT_COUNT);
    CHECK(fake.writes[VD_PMU_BACKEND_HOST_PMCNTENSET] == 1);
    CHECK(fake.writes[VD_PMU_BACKEND_HOST_PMCNTENCLR] >= 2);
    CHECK(fake.writes[VD_PMU_BACKEND_HOST_PMINTENSET] == 0);
    CHECK(fake.writes[VD_PMU_BACKEND_HOST_PMINTENCLR] >= 2);
    CHECK(fake.pmcr_event_reset_commands == 0);
    CHECK(fake.pmcr_cycle_reset_commands == 0);
    CHECK(!vdPmuBackendHasRestoreObligation());
    CHECK(!vdPmuBackendHostTestIsActive());
}

static void test_owned_overflow_is_cleared_and_restored(void)
{
    struct fake_pmu fake;
    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    struct vd_pmu_session_backend backend = start_backend(&fake);
    struct vd_pmu_snapshot original;
    struct vd_pmu_counter_values values;

    CHECK(backend.snapshot(backend.context, 0, &original) == 0);
    const struct vd_pmu_configuration configuration =
        make_configuration(&original);
    CHECK(backend.configure(backend.context, 0, &configuration) == 0);

    vdPmuBackendHostTestWriteSelector(TEST_LANE);
    vdPmuBackendHostTestWriteEventCount(UINT32_MAX);
    vdPmuBackendHostTestWriteSelector(original.raw_pmselr);
    vdPmuBackendHostTestWriteSoftwareIncrement(TEST_LANE_MASK);
    CHECK((fake.state.overflow & TEST_LANE_MASK) != 0);
    CHECK(backend.read(backend.context, 0, &configuration, &values) ==
          VD_PMU_BACKEND_ERROR_CONFLICT);
    CHECK(backend.restore(backend.context, 0, &configuration, &original) ==
          0);
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0);
    CHECK(fake.writes[VD_PMU_BACKEND_HOST_PMOVSR] == 1);
    CHECK(fake.last_write[VD_PMU_BACKEND_HOST_PMOVSR] == TEST_LANE_MASK);
}

static void test_selector_restore_failure_retains_obligation(void)
{
    struct fake_pmu fake;
    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    struct vd_pmu_session_backend backend = start_backend(&fake);
    struct vd_pmu_snapshot snapshot;

    fake.fail_selector_write_ordinal =
        fake.writes[VD_PMU_BACKEND_HOST_PMSELR] + 2u;
    CHECK(backend.snapshot(backend.context, 0, &snapshot) ==
          VD_PMU_BACKEND_ERROR_RESTORE);
    CHECK(vdPmuBackendHostTestSelectorRestorePending());
    CHECK(vdPmuBackendHasRestoreObligation());
    CHECK(!vdPmuBackendReady());
    CHECK(fake.state.selector == TEST_LANE);

    fake.fail_selector_write_ordinal = 0;
    CHECK(vdPmuBackendRecover() == 0);
    CHECK(!vdPmuBackendHostTestSelectorRestorePending());
    CHECK(!vdPmuBackendHasRestoreObligation());
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0);
}

static void test_external_conflict_preserves_restore_obligation(void)
{
    struct fake_pmu fake;
    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    struct vd_pmu_session_backend backend = start_backend(&fake);
    struct vd_pmu_snapshot original;

    CHECK(backend.snapshot(backend.context, 0, &original) == 0);
    const struct vd_pmu_configuration configuration =
        make_configuration(&original);
    CHECK(backend.configure(backend.context, 0, &configuration) == 0);

    vdPmuBackendHostTestWriteCounterEnableSet(UINT32_C(1) << 1);
    CHECK(backend.restore(backend.context, 0, &configuration, &original) ==
          VD_PMU_BACKEND_ERROR_CONFLICT);
    CHECK(vdPmuBackendHasRestoreObligation());
    CHECK((fake.state.counter_enable & TEST_LANE_MASK) == 0);
    CHECK((fake.state.counter_enable & (UINT32_C(1) << 1)) != 0);

    vdPmuBackendHostTestWriteCounterEnableClear(UINT32_C(1) << 1);
    CHECK(vdPmuBackendRecover() == 0);
    CHECK(!vdPmuBackendHasRestoreObligation());
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0);
}

static void test_timeout_can_complete_late_and_be_reaped(void)
{
    struct fake_pmu fake;
    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    struct vd_pmu_session_backend backend = start_backend(&fake);
    struct vd_pmu_snapshot original;

    CHECK(backend.snapshot(backend.context, 0, &original) == 0);
    const struct vd_pmu_configuration configuration =
        make_configuration(&original);
    CHECK(backend.configure(backend.context, 0, &configuration) == 0);

    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_TIMEOUT);
    CHECK(backend.restore(backend.context, 0, &configuration, &original) ==
          VD_PMU_BACKEND_ERROR_TIMEOUT);
    CHECK(vdPmuBackendHostTestIsInflight());
    CHECK(vdPmuBackendHasRestoreObligation());
    CHECK(!vdPmuBackendReady());

    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_INLINE);
    CHECK(vdPmuBackendHostTestCompleteInflight() == 0);
    CHECK(vdPmuBackendHostTestIsInflight());
    CHECK(!vdPmuBackendHasRestoreObligation());
    CHECK(vdPmuBackendRecover() == 0);
    CHECK(!vdPmuBackendHostTestIsInflight());
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0);
}

static void test_signal_failure_has_provable_no_write_restore(void)
{
    struct fake_pmu fake;
    fake_init(&fake, 0);
    const struct fake_pmu_state before = fake.state;
    struct vd_pmu_session_backend backend = start_backend(&fake);
    struct vd_pmu_snapshot original;

    CHECK(backend.snapshot(backend.context, 0, &original) == 0);
    const struct vd_pmu_configuration configuration =
        make_configuration(&original);
    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_SIGNAL_FAILURE);
    CHECK(backend.configure(backend.context, 0, &configuration) ==
          VD_PMU_BACKEND_ERROR_CORE);
    CHECK(!vdPmuBackendHostTestIsInflight());
    CHECK(!vdPmuBackendHasRestoreObligation());
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0);

    vdPmuBackendHostTestSetDispatchMode(
        VD_PMU_BACKEND_HOST_DISPATCH_INLINE);
    CHECK(backend.restore(backend.context, 0, &configuration, &original) ==
          0);
    CHECK(!vdPmuBackendHasRestoreObligation());
    CHECK(memcmp(&fake.state, &before, sizeof(before)) == 0);
}

static void test_not_idle_and_preconfigure_conflict_are_non_mutating(void)
{
    struct fake_pmu fake;
    fake_init(&fake, 0);
    fake.state.counter_enable = UINT32_C(1) << 1;
    const struct fake_pmu_state busy_before = fake.state;
    struct vd_pmu_session_backend backend = start_backend(&fake);
    struct vd_pmu_snapshot original;
    CHECK(backend.snapshot(backend.context, 0, &original) ==
          VD_PMU_BACKEND_ERROR_NOT_IDLE);
    CHECK(memcmp(&fake.state, &busy_before, sizeof(busy_before)) == 0);
    CHECK(fake.writes[VD_PMU_BACKEND_HOST_PMSELR] == 0);
    CHECK(!vdPmuBackendHasRestoreObligation());

    fake_init(&fake, 0);
    backend = start_backend(&fake);
    CHECK(backend.snapshot(backend.context, 0, &original) == 0);
    const struct vd_pmu_configuration configuration =
        make_configuration(&original);
    ++fake.state.event_count[TEST_LANE];
    CHECK(backend.configure(backend.context, 0, &configuration) ==
          VD_PMU_BACKEND_ERROR_CONFLICT);
    CHECK(!vdPmuBackendHasRestoreObligation());
    CHECK(!vdPmuBackendHostTestIsActive());
}

int main(void)
{
    test_register_wrappers_preserve_arm_semantics();
    test_self_test_restores_exact_architectural_state();
    test_owned_overflow_is_cleared_and_restored();
    test_selector_restore_failure_retains_obligation();
    test_external_conflict_preserves_restore_obligation();
    test_timeout_can_complete_late_and_be_reaped();
    test_signal_failure_has_provable_no_write_restore();
    test_not_idle_and_preconfigure_conflict_are_non_mutating();
    vdPmuBackendHostTestReset();

    if(failures != 0)
    {
        fprintf(stderr, "%d PMU backend semantic test(s) failed\n", failures);
        return 1;
    }
    puts("PASS: Cortex-A9 PMU backend register and recovery semantics");
    return 0;
}

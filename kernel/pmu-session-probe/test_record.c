#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vitadebug_pmu_probe.h"

#define CHECK(condition)                                                   \
    do {                                                                   \
        if(!(condition))                                                   \
        {                                                                  \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
            return 1;                                                      \
        }                                                                  \
    } while(0)

static void finish_record(struct vd_pmu_probe_record* record)
{
    record->magic = VD_PMU_PROBE_RECORD_MAGIC;
    record->version = VD_PMU_PROBE_RECORD_VERSION;
    record->size = sizeof(*record);
    record->checksum = 0;
    record->checksum = vdPmuProbeChecksum(record);
}

static struct vd_pmu_probe_record pass_record(
    uint32_t revision, uint32_t sequence, uint32_t core,
    uint32_t passed_core_mask)
{
    struct vd_pmu_probe_record record;
    memset(&record, 0, sizeof(record));
    record.revision = revision;
    record.sequence = sequence;
    record.state = VD_PMU_PROBE_STATE_COMPLETE;
    record.flags = VD_PMU_PROBE_FLAG_HAS_RESULT |
                   VD_PMU_PROBE_FLAG_RESTORE_PROVEN |
                   VD_PMU_PROBE_FLAG_BACKEND_READY;
    record.module_start_result = 0;
    record.recovery_result = VD_PMU_PROBE_RESULT_NOT_RUN;
    record.caller_pid = 0x123;
    record.requested_core = core;
    record.backend_ready = 1;
    record.passed_core_mask = passed_core_mask;
    record.test.struct_size = sizeof(record.test);
    record.test.abi_version = VD_PMU_BACKEND_ABI_VERSION;
    record.test.core_id = core;
    record.test.stage = VD_PMU_BACKEND_TEST_COMPLETE;
    record.test.increment_count = VD_PMU_BACKEND_TEST_INCREMENT_COUNT;
    record.test.observed_count = VD_PMU_BACKEND_TEST_INCREMENT_COUNT;
    record.test.before.event_counter_count =
        VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS;
    record.test.before.raw_pmcr =
        VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS << 11;
    record.test.before.raw_pmselr = 2;
    record.test.before.raw_pmxevtyper[5] = 0x71;
    record.test.before.raw_pmxevcntr[5] = 0x12345678;
    record.test.after = record.test.before;
    finish_record(&record);
    return record;
}

int main(void)
{
    CHECK(sizeof(struct vd_pmu_backend_test_result) == 200);
    CHECK(sizeof(struct vd_pmu_probe_record) == VD_PMU_PROBE_RECORD_SIZE);
    CHECK(sizeof(struct vd_pmu_probe_status) == VD_PMU_PROBE_STATUS_SIZE);

    struct vd_pmu_probe_record first = pass_record(10, 3, 0, 1);
    CHECK(vdPmuProbeRecordValid(&first));
    CHECK(vdPmuProbeRecordPassed(&first));
    CHECK(vdPmuProbeRestorationProven(&first.test));
    CHECK(vdPmuProbeTestPassed(&first.test));

    struct vd_pmu_probe_record second = pass_record(11, 4, 1, 3);
    CHECK(vdPmuProbeSelectLatest(1, 1, &first, 1, 1, &second) == 1);
    CHECK(vdPmuProbeSelectLatest(1, 1, &second, 1, 1, &first) == 0);
    CHECK(vdPmuProbeSelectLatest(1, 1, &first, 0, 0, &second) == 0);
    CHECK(vdPmuProbeSelectLatest(0, 0, &first, 0, 0, &second) ==
          VD_PMU_PROBE_SELECT_EMPTY);

    second.revision = first.revision;
    finish_record(&second);
    CHECK(vdPmuProbeSelectLatest(1, 1, &first, 1, 1, &second) ==
          VD_PMU_PROBE_SELECT_CONFLICT);

    struct vd_pmu_probe_record wrap_old =
        pass_record(UINT32_MAX - 1u, 5, 0, 1);
    struct vd_pmu_probe_record wrap_new = pass_record(2, 6, 0, 1);
    CHECK(vdPmuProbeRevisionNewer(wrap_new.revision, wrap_old.revision));
    CHECK(vdPmuProbeSelectLatest(1, 1, &wrap_old, 1, 1, &wrap_new) == 1);

    struct vd_pmu_probe_record corrupt = first;
    ((uint8_t*)&corrupt)[sizeof(corrupt) - 1u] ^= UINT8_C(0x80);
    CHECK(!vdPmuProbeRecordValid(&corrupt));
    CHECK(vdPmuProbeSelectLatest(
              1, vdPmuProbeRecordValid(&first), &first,
              1, vdPmuProbeRecordValid(&corrupt), &corrupt) ==
          VD_PMU_PROBE_SELECT_INVALID);
    CHECK(vdPmuProbeSelectLatest(
              1, vdPmuProbeRecordValid(&corrupt), &corrupt,
              1, vdPmuProbeRecordValid(&first), &first) ==
          VD_PMU_PROBE_SELECT_INVALID);

    struct vd_pmu_probe_record incomplete;
    memset(&incomplete, 0, sizeof(incomplete));
    incomplete.revision = 12;
    incomplete.sequence = 5;
    incomplete.state = VD_PMU_PROBE_STATE_KERNEL_ENTERED;
    incomplete.flags = VD_PMU_PROBE_FLAG_BACKEND_READY;
    incomplete.syscall_result = VD_PMU_PROBE_RESULT_NOT_RUN;
    incomplete.recovery_result = VD_PMU_PROBE_RESULT_NOT_RUN;
    incomplete.caller_pid = 0x123;
    incomplete.backend_ready = 1;
    finish_record(&incomplete);
    CHECK(vdPmuProbeRecordValid(&incomplete));
    CHECK(!vdPmuProbeRecordPassed(&incomplete));

    struct vd_pmu_probe_record restore = first;
    restore.revision++;
    restore.state = VD_PMU_PROBE_STATE_RESTORE_REQUIRED;
    restore.flags &= ~VD_PMU_PROBE_FLAG_RESTORE_PROVEN;
    restore.flags |= VD_PMU_PROBE_FLAG_RUNTIME_OBLIGATION;
    restore.restore_obligation = 1;
    restore.syscall_result = VD_PMU_BACKEND_ERROR_RESTORE;
    restore.test.operation_result = VD_PMU_BACKEND_ERROR_RESTORE;
    restore.test.restore_result = VD_PMU_BACKEND_ERROR_RESTORE;
    finish_record(&restore);
    CHECK(vdPmuProbeRecordValid(&restore));
    CHECK(!vdPmuProbeRecordPassed(&restore));

    struct vd_pmu_probe_record bad_relationship = restore;
    bad_relationship.restore_obligation = 0;
    finish_record(&bad_relationship);
    CHECK(!vdPmuProbeRecordValid(&bad_relationship));

    struct vd_pmu_probe_record bad_core = first;
    bad_core.test.core_id = 2;
    finish_record(&bad_core);
    CHECK(!vdPmuProbeRecordValid(&bad_core));

    struct vd_pmu_probe_record bad_reserved = first;
    bad_reserved.reserved[4] = 1;
    finish_record(&bad_reserved);
    CHECK(!vdPmuProbeRecordValid(&bad_reserved));

    struct vd_pmu_probe_record changed_after = first;
    changed_after.test.after.raw_pmselr ^= 1;
    finish_record(&changed_after);
    CHECK(vdPmuProbeRecordValid(&changed_after));
    CHECK(!vdPmuProbeRestorationProven(&changed_after.test));
    CHECK(!vdPmuProbeRecordPassed(&changed_after));

    puts("PASS: PMU probe two-slot journal and result validation");
    return 0;
}

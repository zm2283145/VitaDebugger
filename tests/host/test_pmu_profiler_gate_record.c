#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "journal.h"

static int failures;

#define CHECK(condition, message)                                      \
    do                                                                 \
    {                                                                  \
        if(!(condition))                                               \
        {                                                              \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            ++failures;                                                \
        }                                                              \
    } while(0)

static void seal_record(struct vd_pmu_profiler_gate_record* record)
{
    record->magic = VD_PMU_PROFILER_GATE_RECORD_MAGIC;
    record->version = VD_PMU_PROFILER_GATE_RECORD_VERSION;
    record->size = sizeof(*record);
    record->checksum = 0;
    record->checksum = vdPmuProfilerGateChecksum(record);
}

static struct vd_pmu_profiler_gate_record attempted_record(void)
{
    struct vd_pmu_profiler_gate_record record;
    memset(&record, 0, sizeof(record));
    record.revision = 1;
    record.state = VD_PMU_PROFILER_GATE_ATTEMPTED;
    record.event_code = UINT32_C(0x01);
    record.info_result = 0;
    record.affinity_result = 0;
    record.open_result = VD_PMU_PROFILER_GATE_RESULT_NOT_RUN;
    record.read_result = VD_PMU_PROFILER_GATE_RESULT_NOT_RUN;
    record.close_result = VD_PMU_PROFILER_GATE_RESULT_NOT_RUN;
    record.affinity_restore_result =
        VD_PMU_PROFILER_GATE_RESULT_NOT_RUN;
    seal_record(&record);
    return record;
}

static struct vd_pmu_profiler_gate_record passing_record(void)
{
    struct vd_pmu_profiler_gate_record record;
    memset(&record, 0, sizeof(record));
    record.revision = 2;
    record.state = VD_PMU_PROFILER_GATE_COMPLETE;
    record.event_code = UINT32_C(0x01);
    record.flags = VD_PMU_PROFILER_GATE_FLAG_RESTORE_PROVEN |
        VD_PMU_PROFILER_GATE_FLAG_SAMPLE_VALID |
        VD_PMU_PROFILER_GATE_FLAG_PASS;
    record.info_result = 0;
    record.affinity_result = 0;
    record.open_result = 0;
    record.read_result = 0;
    record.close_result = 0;
    record.affinity_restore_result = 0;
    record.owner_token = 1;
    record.generation = 1;
    record.value_low = 7;
    record.core_id = 0;
    record.physical_counter = 5;
    seal_record(&record);
    return record;
}

int main(void)
{
    struct vd_pmu_profiler_gate_record attempted = attempted_record();
    struct vd_pmu_profiler_gate_record passed = passing_record();
    CHECK(vdPmuProfilerGateRecordValid(&attempted),
          "synced attempted record validates");
    CHECK(vdPmuProfilerGateRecordValid(&passed) &&
              vdPmuProfilerGateRecordPassed(&passed),
          "complete exact-restore record validates as passed");

    struct vd_pmu_profiler_gate_record corrupt = attempted;
    corrupt.event_code ^= 1u;
    CHECK(!vdPmuProfilerGateRecordValid(&corrupt),
          "checksum mismatch is rejected");
    corrupt = attempted;
    corrupt.reserved[39] = 1;
    seal_record(&corrupt);
    CHECK(!vdPmuProfilerGateRecordValid(&corrupt),
          "nonzero reserved data is rejected");
    corrupt = passed;
    corrupt.close_result = -1;
    seal_record(&corrupt);
    CHECK(!vdPmuProfilerGateRecordValid(&corrupt),
          "pass cannot claim a failed close");

    CHECK(vdPmuProfilerGateSelectLatest(
              0, 0, NULL, 0, 0, NULL) ==
              VD_PMU_PROFILER_GATE_SELECT_EMPTY,
          "two absent slots select empty");
    CHECK(vdPmuProfilerGateSelectLatest(
              1, 1, &attempted, 1, 1, &passed) == 1,
          "newer completion record wins");
    passed.revision = attempted.revision;
    seal_record(&passed);
    CHECK(vdPmuProfilerGateSelectLatest(
              1, 1, &attempted, 1, 1, &passed) ==
              VD_PMU_PROFILER_GATE_SELECT_CONFLICT,
          "equal revisions fail closed as a conflict");
    CHECK(vdPmuProfilerGateSelectLatest(
              1, 0, &attempted, 0, 0, NULL) ==
              VD_PMU_PROFILER_GATE_SELECT_INVALID,
          "a present corrupt slot cannot be hidden");

    if(failures != 0)
    {
        fprintf(stderr, "%d PMU gate journal test(s) failed\n", failures);
        return 1;
    }
    puts("PASS: PMU profiler gate journal validation");
    return 0;
}

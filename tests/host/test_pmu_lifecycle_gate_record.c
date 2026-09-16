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

static void seal(struct vd_pmu_lifecycle_record* record)
{
    record->magic = VD_PMU_LIFECYCLE_RECORD_MAGIC;
    record->version = VD_PMU_LIFECYCLE_RECORD_VERSION;
    record->size = sizeof(*record);
    record->checksum = 0;
    record->checksum = vdPmuLifecycleChecksum(record);
}

static struct vd_pmu_lifecycle_record base_record(void)
{
    struct vd_pmu_lifecycle_record record;
    memset(&record, 0, sizeof(record));
    int32_t* result = &record.info_result;
    for(size_t i = 0; i < 31; ++i)
        result[i] = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    record.journal_result = 0;
    return record;
}

int main(void)
{
    struct vd_pmu_lifecycle_record records[VD_PMU_LIFECYCLE_SLOT_COUNT];
    int present[VD_PMU_LIFECYCLE_SLOT_COUNT] = {0};
    int valid[VD_PMU_LIFECYCLE_SLOT_COUNT] = {0};

    records[0] = base_record();
    records[0].revision = 1;
    records[0].state = VD_PMU_LIFECYCLE_ATTEMPTED;
    seal(&records[0]);
    CHECK(vdPmuLifecycleRecordValid(&records[0]),
          "initial attempted journal validates");

    records[1] = records[0];
    records[1].revision = 2;
    records[1].state = VD_PMU_LIFECYCLE_PROCESS_EXIT_ATTEMPTED;
    records[1].flags = VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS;
    seal(&records[1]);
    CHECK(vdPmuLifecycleRecordValid(&records[1]),
          "process-exit attempted journal validates");

    records[2] = records[1];
    records[2].revision = 3;
    records[2].state = VD_PMU_LIFECYCLE_PROCESS_EXIT_ARMED;
    records[2].flags |= VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_ARMED;
    records[2].process_open_result = 0;
    records[2].process_read_result = 0;
    records[2].process_owner_token = 11;
    records[2].process_generation = 12;
    seal(&records[2]);
    CHECK(vdPmuLifecycleRecordValid(&records[2]),
          "durable process-exit armed journal validates");

    records[3] = records[2];
    records[3].revision = 4;
    records[3].state = VD_PMU_LIFECYCLE_RESUME_ATTEMPTED;
    records[3].resume_open_result = VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    seal(&records[3]);
    CHECK(vdPmuLifecycleRecordValid(&records[3]),
          "resume attempt journal validates");

    records[4] = records[3];
    records[4].revision = 5;
    records[4].state = VD_PMU_LIFECYCLE_COMPLETE;
    records[4].flags |= VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_REARM |
        VD_PMU_LIFECYCLE_FLAG_PASS;
    records[4].resume_open_result = 0;
    records[4].resume_read_result = 0;
    records[4].resume_close_result = 0;
    records[4].resume_owner_token = 21;
    records[4].resume_generation = 22;
    seal(&records[4]);
    CHECK(vdPmuLifecycleRecordValid(&records[4]),
          "complete two-launch lifecycle journal validates");

    struct vd_pmu_lifecycle_record corrupt = records[4];
    corrupt.flags ^= VD_PMU_LIFECYCLE_FLAG_PASS;
    CHECK(!vdPmuLifecycleRecordValid(&corrupt),
          "checksum mismatch is rejected");
    corrupt = records[4];
    corrupt.reserved[61] = 1;
    seal(&corrupt);
    CHECK(!vdPmuLifecycleRecordValid(&corrupt),
          "nonzero reserved data is rejected");

    CHECK(vdPmuLifecycleSelectLatest(
              present, valid, records, VD_PMU_LIFECYCLE_SLOT_COUNT) ==
              VD_PMU_LIFECYCLE_SELECT_EMPTY,
          "empty slot set is ready");
    for(size_t i = 0; i < VD_PMU_LIFECYCLE_SLOT_COUNT; ++i)
    {
        present[i] = 1;
        valid[i] = 1;
    }
    CHECK(vdPmuLifecycleSelectLatest(
              present, valid, records, VD_PMU_LIFECYCLE_SLOT_COUNT) == 4,
          "highest unique revision is selected");
    records[4].revision = records[3].revision;
    seal(&records[4]);
    CHECK(vdPmuLifecycleSelectLatest(
              present, valid, records, VD_PMU_LIFECYCLE_SLOT_COUNT) ==
              VD_PMU_LIFECYCLE_SELECT_CONFLICT,
          "duplicate revisions fail closed");
    valid[2] = 0;
    CHECK(vdPmuLifecycleSelectLatest(
              present, valid, records, VD_PMU_LIFECYCLE_SLOT_COUNT) ==
              VD_PMU_LIFECYCLE_SELECT_INVALID,
          "a present corrupt slot cannot be ignored");

    if(failures != 0)
    {
        fprintf(stderr, "%d PMU lifecycle journal test(s) failed\n",
                failures);
        return 1;
    }
    puts("PASS: PMU lifecycle gate journal validation");
    return 0;
}

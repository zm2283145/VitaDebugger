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

static void seal(struct vd_pmu_thread_exit_record* record)
{
    record->magic = VD_PMU_THREAD_EXIT_RECORD_MAGIC;
    record->version = VD_PMU_THREAD_EXIT_RECORD_VERSION;
    record->size = sizeof(*record);
    record->checksum = 0;
    record->checksum = vdPmuThreadExitChecksum(record);
}

static struct vd_pmu_thread_exit_record base_record(void)
{
    struct vd_pmu_thread_exit_record record;
    memset(&record, 0, sizeof(record));
    int32_t* result = &record.info_result;
    for(size_t i = 0; i < 14; ++i)
        result[i] = VD_PMU_THREAD_EXIT_RESULT_NOT_RUN;
    record.journal_result = 0;
    return record;
}

int main(void)
{
    struct vd_pmu_thread_exit_record
        records[VD_PMU_THREAD_EXIT_SLOT_COUNT];
    int present[VD_PMU_THREAD_EXIT_SLOT_COUNT] = {0};
    int valid[VD_PMU_THREAD_EXIT_SLOT_COUNT] = {0};

    records[0] = base_record();
    records[0].revision = 1;
    records[0].state = VD_PMU_THREAD_EXIT_ATTEMPTED;
    seal(&records[0]);
    CHECK(vdPmuThreadExitRecordValid(&records[0]),
          "initial attempted journal validates");

    records[1] = records[0];
    records[1].revision = 2;
    records[1].state = VD_PMU_THREAD_EXIT_COMPLETE;
    records[1].flags = VD_PMU_THREAD_EXIT_PASS_FLAGS;
    records[1].capabilities =
        VD_PMU_THREAD_EXIT_REQUIRED_CAPABILITIES |
        VD_KERNEL_PMU_PROFILER_CAP_SOFTWARE_INCREMENT;
    records[1].info_result = 0;
    records[1].affinity_result = 0;
    records[1].owner_create_result = 10;
    records[1].owner_start_result = 0;
    records[1].owner_affinity_result = 0;
    records[1].owner_open_result = 0;
    records[1].owner_read_result = 0;
    records[1].owner_wait_result = 0;
    records[1].post_open_result = 0;
    records[1].post_read_result = 0;
    records[1].post_close_result = 0;
    records[1].owner_delete_result = 0;
    records[1].affinity_restore_result = 0;
    records[1].owner_token = 11;
    records[1].owner_generation = 12;
    records[1].post_owner_token = 21;
    records[1].post_generation = 22;
    records[1].rearm_elapsed_us = UINT64_C(100000);
    records[1].owner_open_attempt_us = UINT64_C(1000000);
    records[1].post_open_success_us = UINT64_C(1500000);
    records[1].lease_to_rearm_elapsed_us = UINT64_C(500000);
    seal(&records[1]);
    CHECK(vdPmuThreadExitRecordValid(&records[1]),
          "complete thread-exit journal validates");

    struct vd_pmu_thread_exit_record corrupt = records[1];
    corrupt.flags ^= VD_PMU_THREAD_EXIT_FLAG_PASS;
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "checksum mismatch is rejected");
    corrupt = records[1];
    corrupt.reserved[23] = 1;
    seal(&corrupt);
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "nonzero reserved data is rejected");
    corrupt = records[1];
    corrupt.post_close_result = VD_PMU_THREAD_EXIT_RESULT_NOT_RUN;
    seal(&corrupt);
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "incomplete post-rearm close is rejected");
    corrupt = records[1];
    corrupt.info_result = VD_PMU_THREAD_EXIT_RESULT_NOT_RUN;
    seal(&corrupt);
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "complete record requires successful PMU discovery");
    corrupt = records[1];
    corrupt.affinity_result = -1;
    seal(&corrupt);
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "complete record requires successful affinity pinning");
    corrupt = records[1];
    corrupt.capabilities &=
        ~VD_KERNEL_PMU_PROFILER_CAP_SAFE_POST_RESTORE_REARM;
    seal(&corrupt);
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "complete record requires safe-rearm capability");
    corrupt = records[1];
    corrupt.capabilities |=
        VD_KERNEL_PMU_PROFILER_CAP_SINGLE_REAL_EVENT_PER_BOOT;
    seal(&corrupt);
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "boot-latched real-event transport cannot prove rearm");
    corrupt = records[1];
    corrupt.owner_token = 0;
    seal(&corrupt);
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "missing owner identity is rejected");
    corrupt = records[1];
    corrupt.rearm_elapsed_us =
        VD_PMU_THREAD_EXIT_REARM_DEADLINE_US + UINT64_C(1);
    seal(&corrupt);
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "late timeout-based rearm cannot validate as a pass");
    corrupt = records[1];
    corrupt.post_owner_token = corrupt.owner_token;
    corrupt.post_generation = corrupt.owner_generation;
    seal(&corrupt);
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "complete record requires a fresh owner identity");
    corrupt = records[1];
    corrupt.post_open_success_us = corrupt.owner_open_attempt_us +
        VD_PMU_THREAD_EXIT_OWNER_LEASE_US;
    corrupt.lease_to_rearm_elapsed_us =
        VD_PMU_THREAD_EXIT_OWNER_LEASE_US;
    seal(&corrupt);
    CHECK(!vdPmuThreadExitRecordValid(&corrupt),
          "rearm at ordinary lease expiry cannot validate as a pass");

    CHECK(vdPmuThreadExitSelectLatest(
              present, valid, records, VD_PMU_THREAD_EXIT_SLOT_COUNT) ==
              VD_PMU_THREAD_EXIT_SELECT_EMPTY,
          "empty slot set is ready");
    present[0] = 1;
    present[1] = 1;
    valid[0] = 1;
    valid[1] = 1;
    CHECK(vdPmuThreadExitSelectLatest(
              present, valid, records, VD_PMU_THREAD_EXIT_SLOT_COUNT) == 1,
          "highest unique revision is selected");
    records[1].revision = records[0].revision;
    seal(&records[1]);
    CHECK(vdPmuThreadExitSelectLatest(
              present, valid, records, VD_PMU_THREAD_EXIT_SLOT_COUNT) ==
              VD_PMU_THREAD_EXIT_SELECT_CONFLICT,
          "duplicate revisions fail closed");
    valid[1] = 0;
    CHECK(vdPmuThreadExitSelectLatest(
              present, valid, records, VD_PMU_THREAD_EXIT_SLOT_COUNT) ==
              VD_PMU_THREAD_EXIT_SELECT_INVALID,
          "a present corrupt slot cannot be ignored");

    struct vd_pmu_thread_exit_record failed = base_record();
    failed.revision = 2;
    failed.state = VD_PMU_THREAD_EXIT_FAILED;
    failed.flags = VD_PMU_THREAD_EXIT_FLAG_OWNER_SAMPLE;
    seal(&failed);
    CHECK(vdPmuThreadExitRecordValid(&failed),
          "failure evidence without PASS validates");
    failed.flags |= VD_PMU_THREAD_EXIT_FLAG_PASS;
    seal(&failed);
    CHECK(!vdPmuThreadExitRecordValid(&failed),
          "failed state cannot claim PASS");

    if(failures != 0)
    {
        fprintf(stderr, "%d PMU thread-exit journal test(s) failed\n",
                failures);
        return 1;
    }
    puts("PASS: PMU thread-exit gate journal validation");
    return 0;
}

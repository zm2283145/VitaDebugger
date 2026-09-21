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

static void seal(struct vd_pmu_cleanup_record* record)
{
    record->magic = VD_PMU_CLEANUP_RECORD_MAGIC;
    record->version = VD_PMU_CLEANUP_RECORD_VERSION;
    record->size = sizeof(*record);
    record->checksum = 0;
    record->checksum = vdPmuCleanupChecksum(record);
}

static struct vd_pmu_cleanup_record attempted(uint32_t stage)
{
    struct vd_pmu_cleanup_record record;
    memset(&record, 0, sizeof(record));
    record.revision = 1;
    record.state = VD_PMU_CLEANUP_ATTEMPTED;
    record.stage = stage;
    record.info_result = 0;
    record.capabilities = VD_PMU_CLEANUP_REQUIRED_CAPABILITIES;
    record.baseline_status_result = 0;
    record.restored_status_result = VD_PMU_CLEANUP_RESULT_NOT_RUN;
    record.final_status_result = VD_PMU_CLEANUP_RESULT_NOT_RUN;
    record.baseline.struct_size = sizeof(record.baseline);
    record.baseline.abi_version =
        VD_KERNEL_PMU_PROFILER_STATUS_ABI_VERSION;
    record.baseline.owner_pid = -1;
    record.baseline.owner_thread = -1;
    record.baseline.backend_ready = 1;
    record.baseline.snapshot.event_counter_count = 6;
    for(size_t i = 0; i < 24; ++i)
        record.results[i] = VD_PMU_CLEANUP_RESULT_NOT_RUN;
    seal(&record);
    return record;
}

static void make_complete(struct vd_pmu_cleanup_record* record)
{
    record->revision =
        record->stage >= VD_PMU_CLEANUP_STAGE_NORMAL_EXIT ? 3 : 2;
    record->state = VD_PMU_CLEANUP_COMPLETE;
    record->flags = VD_PMU_CLEANUP_COMPLETE_FLAGS;
    record->restored_status_result = 0;
    record->final_status_result = 0;
    record->restored = record->baseline;
    record->final = record->baseline;
    if(record->stage == VD_PMU_CLEANUP_STAGE_NORMAL_EXIT)
    {
        ++record->restored.active_process_normal_exit_cleanup_count;
        ++record->final.active_process_normal_exit_cleanup_count;
    }
    else if(record->stage == VD_PMU_CLEANUP_STAGE_ABRUPT_EXIT)
    {
        ++record->restored.active_process_kill_cleanup_count;
        ++record->final.active_process_kill_cleanup_count;
    }
    record->final.rearm_count = record->baseline.rearm_count + 1;
    seal(record);
}

static void set_sample(
    struct vd_kernel_pmu_profiler_sample* sample,
    uint32_t owner_token, uint32_t generation)
{
    sample->struct_size = sizeof(*sample);
    sample->abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION;
    sample->owner_token = owner_token;
    sample->generation = generation;
    sample->event_code =
        VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT;
    sample->core_id = VD_KERNEL_PMU_PROFILER_FIXED_CORE;
    sample->physical_counter =
        VD_KERNEL_PMU_PROFILER_FIXED_COUNTER;
}

static void set_disconnect_evidence(
    struct vd_pmu_cleanup_record* record)
{
    static const size_t success_results[] = {
        GATE_RESULT_OPEN,
        GATE_RESULT_READ,
        GATE_RESULT_CLOSE,
        GATE_RESULT_NET_START,
        GATE_RESULT_NET_CONNECT,
        GATE_RESULT_NET_PRELUDE,
        GATE_RESULT_NET_CLOSE,
        GATE_RESULT_NET_STOP,
        GATE_RESULT_POST_DISCONNECT_READ,
    };
    for(size_t i = 0;
        i < sizeof(success_results) / sizeof(success_results[0]); ++i)
        record->results[success_results[i]] = 0;
    record->results[GATE_RESULT_NET_FAILURE] =
        VD_PMU_CLEANUP_EXPECTED_NET_FAILURE;
    record->handles[0].owner_token = 3;
    record->handles[0].generation = 4;
    set_sample(&record->samples[0], 3, 4);
    set_sample(&record->samples[2], 3, 4);
}

int main(void)
{
    struct vd_pmu_cleanup_record
        records[VD_PMU_CLEANUP_SLOT_COUNT];
    int present[VD_PMU_CLEANUP_SLOT_COUNT] = {0};
    int valid[VD_PMU_CLEANUP_SLOT_COUNT] = {0};

    records[0] = attempted(VD_PMU_CLEANUP_STAGE_CONFLICT);
    CHECK(vdPmuCleanupRecordValid(&records[0]),
          "durable pre-mutation attempt validates");
    records[1] = records[0];
    make_complete(&records[1]);
    CHECK(vdPmuCleanupRecordValid(&records[1]),
          "exact-restoration completion validates");

    struct vd_pmu_cleanup_record disconnect =
        attempted(VD_PMU_CLEANUP_STAGE_DISCONNECT);
    make_complete(&disconnect);
    set_disconnect_evidence(&disconnect);
    seal(&disconnect);
    CHECK(vdPmuCleanupRecordValid(&disconnect),
          "disconnect completion preserves complete execution evidence");

    static const size_t required_success_results[] = {
        GATE_RESULT_OPEN,
        GATE_RESULT_READ,
        GATE_RESULT_NET_START,
        GATE_RESULT_NET_CONNECT,
        GATE_RESULT_NET_PRELUDE,
    };
    for(size_t i = 0;
        i < sizeof(required_success_results) /
                sizeof(required_success_results[0]); ++i)
    {
        struct vd_pmu_cleanup_record invalid = disconnect;
        invalid.results[required_success_results[i]] =
            VD_PMU_CLEANUP_RESULT_NOT_RUN;
        seal(&invalid);
        CHECK(!vdPmuCleanupRecordValid(&invalid),
              "disconnect rejects a required result left NOT_RUN");
        invalid = disconnect;
        invalid.results[required_success_results[i]] = -1;
        seal(&invalid);
        CHECK(!vdPmuCleanupRecordValid(&invalid),
              "disconnect rejects a failed required result");
    }

    struct vd_pmu_cleanup_record invalid = disconnect;
    invalid.samples[0].owner_token = 5;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect rejects initial sample owner mismatch");
    invalid = disconnect;
    invalid.samples[0].generation = 5;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect rejects initial sample generation mismatch");
    invalid = disconnect;
    invalid.samples[0].event_code =
        VD_KERNEL_PMU_PROFILER_EVENT_DCACHE_MISS;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect rejects initial sample event mismatch");
    invalid = disconnect;
    invalid.samples[0].core_id =
        VD_KERNEL_PMU_PROFILER_FIXED_CORE + 1;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect rejects initial sample core mismatch");
    invalid = disconnect;
    invalid.samples[0].physical_counter =
        VD_KERNEL_PMU_PROFILER_FIXED_COUNTER - 1;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect rejects initial sample counter mismatch");

    invalid = disconnect;
    invalid.samples[2].generation = 5;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect completion rejects an unauthenticated post-error sample");
    invalid = disconnect;
    invalid.handles[0].owner_token = 0;
    invalid.samples[0].owner_token = 0;
    invalid.samples[2].owner_token = 0;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect completion rejects zero sample identity");
    invalid = disconnect;
    invalid.results[GATE_RESULT_NET_FAILURE] = 0;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect completion requires the exact socket I/O error");
    invalid = disconnect;
    invalid.results[GATE_RESULT_CLOSE] = -1;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect completion requires authenticated PMU close");
    invalid = disconnect;
    invalid.results[GATE_RESULT_NET_CLOSE] = -1;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect completion requires socket close");
    invalid = disconnect;
    invalid.results[GATE_RESULT_NET_STOP] = -1;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect completion requires network cleanup");

    records[2] = attempted(VD_PMU_CLEANUP_STAGE_ABRUPT_EXIT);
    records[2].revision = 2;
    records[2].state = VD_PMU_CLEANUP_ARMED;
    records[2].flags = VD_PMU_CLEANUP_FLAG_OWNER_ARMED;
    records[2].results[0] = 0;
    records[2].results[1] = 0;
    records[2].handles[0].owner_token = 1;
    records[2].handles[0].generation = 2;
    seal(&records[2]);
    CHECK(vdPmuCleanupRecordValid(&records[2]),
          "durable abrupt-exit armed record validates");
    make_complete(&records[2]);
    CHECK(vdPmuCleanupRecordValid(&records[2]),
          "post-kill same-boot completion validates");
    struct vd_pmu_cleanup_record corrupt = records[2];
    --corrupt.restored.active_process_kill_cleanup_count;
    ++corrupt.restored.active_process_normal_exit_cleanup_count;
    seal(&corrupt);
    CHECK(!vdPmuCleanupRecordValid(&corrupt),
          "stage 5 cannot substitute normal-exit cleanup for kill cleanup");

    corrupt = records[2];
    corrupt.final.snapshot.raw_pmcr ^= 1;
    CHECK(!vdPmuCleanupRecordValid(&corrupt),
          "checksum corruption is rejected");
    corrupt = records[2];
    corrupt.final.snapshot.raw_pmcr ^= 1;
    seal(&corrupt);
    CHECK(!vdPmuCleanupRecordValid(&corrupt),
          "baseline/final PMU mismatch is rejected");
    corrupt = records[2];
    corrupt.final.owner_identity_release_uncertain = 1;
    seal(&corrupt);
    CHECK(!vdPmuCleanupRecordValid(&corrupt),
          "release uncertainty is rejected");

    CHECK(vdPmuCleanupSelectLatest(
              present, valid, records,
              VD_PMU_CLEANUP_SLOT_COUNT) ==
              VD_PMU_CLEANUP_SELECT_EMPTY,
          "empty stage is runnable");
    records[0] = attempted(VD_PMU_CLEANUP_STAGE_ABRUPT_EXIT);
    records[1] = records[0];
    records[1].revision = 2;
    records[1].state = VD_PMU_CLEANUP_ARMED;
    records[1].flags = VD_PMU_CLEANUP_FLAG_OWNER_ARMED;
    records[1].results[0] = 0;
    records[1].results[1] = 0;
    records[1].handles[0].owner_token = 1;
    records[1].handles[0].generation = 2;
    seal(&records[1]);
    records[2] = records[1];
    make_complete(&records[2]);
    for(size_t i = 0; i < VD_PMU_CLEANUP_SLOT_COUNT; ++i)
    {
        present[i] = 1;
        valid[i] = 1;
    }
    CHECK(vdPmuCleanupSelectLatest(
              present, valid, records,
              VD_PMU_CLEANUP_SLOT_COUNT) == 2,
          "highest unique stage revision is selected");
    records[2].revision = 2;
    seal(&records[2]);
    CHECK(vdPmuCleanupSelectLatest(
              present, valid, records,
              VD_PMU_CLEANUP_SLOT_COUNT) ==
              VD_PMU_CLEANUP_SELECT_CONFLICT,
          "duplicate revisions fail closed");
    valid[1] = 0;
    CHECK(vdPmuCleanupSelectLatest(
              present, valid, records,
              VD_PMU_CLEANUP_SLOT_COUNT) ==
              VD_PMU_CLEANUP_SELECT_INVALID,
          "present corrupt evidence cannot be ignored");

    if(failures != 0)
    {
        fprintf(stderr, "%d PMU cleanup journal test(s) failed\n",
                failures);
        return 1;
    }
    puts("PASS: PMU cleanup gate journal validation");
    return 0;
}

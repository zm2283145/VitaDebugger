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

static void set_sample(
    struct vd_kernel_pmu_profiler_sample* sample,
    uint32_t owner_token, uint32_t generation, uint32_t event_code)
{
    sample->struct_size = sizeof(*sample);
    sample->abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION;
    sample->owner_token = owner_token;
    sample->generation = generation;
    sample->event_code = event_code;
    sample->core_id = VD_KERNEL_PMU_PROFILER_FIXED_CORE;
    sample->physical_counter =
        VD_KERNEL_PMU_PROFILER_FIXED_COUNTER;
}

static void set_conflict_evidence(
    struct vd_pmu_cleanup_record* record, int32_t busy_result)
{
    record->results[GATE_RESULT_OPEN] = 0;
    record->results[GATE_RESULT_READ] = 0;
    record->results[GATE_RESULT_CLOSE] = 0;
    record->results[GATE_RESULT_AUX_CREATE] = 1;
    record->results[GATE_RESULT_AUX_START] = 0;
    record->results[GATE_RESULT_AUX_WAIT] = 0;
    record->results[GATE_RESULT_AUX_DELETE] = 0;
    record->results[GATE_RESULT_AUX_ACTION] = busy_result;
    record->results[GATE_RESULT_AUX_CLEANUP] =
        VD_PMU_CLEANUP_RESULT_NOT_RUN;
    record->handles[0].owner_token = 1;
    record->handles[0].generation = 1;
    set_sample(
        &record->samples[0], 1, 1,
        VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS);
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
    record->results[GATE_RESULT_REARM_OPEN] = 0;
    record->results[GATE_RESULT_REARM_READ] = 0;
    record->results[GATE_RESULT_REARM_CLOSE] = 0;
    record->handles[1].owner_token = 2;
    record->handles[1].generation = 2;
    set_sample(
        &record->samples[1], 2, 2,
        VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS);
    record->rearm_elapsed_us = 1000;
    if(record->stage == VD_PMU_CLEANUP_STAGE_CONFLICT)
        set_conflict_evidence(
            record, VD_KERNEL_ERROR_PMU_PROFILER_BUSY);
    else if(record->stage == VD_PMU_CLEANUP_STAGE_NORMAL_EXIT)
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
    set_sample(
        &record->samples[0], 3, 4,
        VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT);
    set_sample(
        &record->samples[2], 3, 4,
        VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT);
}

int main(void)
{
    struct vd_pmu_cleanup_record
        records[VD_PMU_CLEANUP_SLOT_COUNT];
    int present[VD_PMU_CLEANUP_SLOT_COUNT] = {0};
    int valid[VD_PMU_CLEANUP_SLOT_COUNT] = {0};

    CHECK(offsetof(struct vd_pmu_cleanup_record, results) ==
              VD_PMU_CLEANUP_RESULTS_OFFSET,
          "results offset matches the durable ABI");
    CHECK(offsetof(struct vd_pmu_cleanup_record, handles) ==
              VD_PMU_CLEANUP_HANDLES_OFFSET,
          "handles offset matches the durable ABI");
    CHECK(offsetof(struct vd_pmu_cleanup_record, samples) ==
              VD_PMU_CLEANUP_SAMPLES_OFFSET,
          "samples offset includes four-byte ABI alignment padding");
    CHECK(offsetof(struct vd_pmu_cleanup_record, rearm_elapsed_us) ==
              VD_PMU_CLEANUP_REARM_ELAPSED_OFFSET,
          "re-arm duration offset matches the durable ABI");
    CHECK(offsetof(struct vd_pmu_cleanup_record, baseline) ==
              VD_PMU_CLEANUP_BASELINE_OFFSET &&
          offsetof(struct vd_pmu_cleanup_record, restored) ==
              VD_PMU_CLEANUP_RESTORED_OFFSET &&
          offsetof(struct vd_pmu_cleanup_record, final) ==
              VD_PMU_CLEANUP_FINAL_OFFSET &&
          offsetof(struct vd_pmu_cleanup_record, reserved) ==
              VD_PMU_CLEANUP_RESERVED_OFFSET,
          "status and reserved offsets match the durable ABI");

    records[0] = attempted(VD_PMU_CLEANUP_STAGE_CONFLICT);
    CHECK(vdPmuCleanupRecordValid(&records[0]),
          "durable pre-mutation attempt validates");
    records[1] = records[0];
    make_complete(&records[1]);
    CHECK(vdPmuCleanupRecordValid(&records[1]),
          "exact-restoration completion validates");
    CHECK(vdPmuCleanupConflictActionPassed(&records[1]),
          "raw host busy result admits the bounded re-arm path");

    struct vd_pmu_cleanup_record encoded_busy = records[1];
    encoded_busy.results[GATE_RESULT_AUX_ACTION] =
        VD_PMU_CLEANUP_VITA_SYSCALL_BUSY_RESULT;
    seal(&encoded_busy);
    CHECK(vdPmuCleanupResultIsBusy(
              VD_PMU_CLEANUP_VITA_SYSCALL_BUSY_RESULT),
          "exact Vita syscall-encoded busy result is recognized");
    CHECK(vdPmuCleanupConflictActionPassed(&encoded_busy),
          "encoded device busy result admits the bounded re-arm path");
    CHECK(vdPmuCleanupRecordValid(&encoded_busy),
          "encoded device busy completion validates");

    static const int32_t not_busy_results[] = {
        VD_KERNEL_ERROR_PMU_PROFILER_INVALID,
        VD_KERNEL_ERROR_PMU_PROFILER_OWNER,
        (int32_t)UINT32_C(0xbfffffd5),
        (int32_t)UINT32_C(0xbfffffd7),
        (int32_t)UINT32_C(0x80020008),
    };
    for(size_t i = 0;
        i < sizeof(not_busy_results) / sizeof(not_busy_results[0]); ++i)
    {
        struct vd_pmu_cleanup_record wrong_busy = records[1];
        wrong_busy.results[GATE_RESULT_AUX_ACTION] =
            not_busy_results[i];
        seal(&wrong_busy);
        CHECK(!vdPmuCleanupResultIsBusy(not_busy_results[i]),
              "neighboring and unrelated errors are not busy");
        CHECK(!vdPmuCleanupConflictActionPassed(&wrong_busy),
              "non-busy result cannot admit re-arm");
        CHECK(!vdPmuCleanupRecordValid(&wrong_busy),
              "non-busy stage-1 completion is rejected");
    }

    struct vd_pmu_cleanup_record missing_rearm = records[1];
    missing_rearm.results[GATE_RESULT_REARM_OPEN] =
        VD_PMU_CLEANUP_RESULT_NOT_RUN;
    seal(&missing_rearm);
    CHECK(!vdPmuCleanupRecordValid(&missing_rearm),
          "completion requires the bounded re-arm operation");
    missing_rearm = records[1];
    missing_rearm.samples[1].physical_counter =
        VD_KERNEL_PMU_PROFILER_FIXED_COUNTER - 1;
    seal(&missing_rearm);
    CHECK(!vdPmuCleanupRecordValid(&missing_rearm),
          "completion requires authenticated re-arm sample metadata");
    missing_rearm = records[1];
    missing_rearm.rearm_elapsed_us =
        VD_PMU_CLEANUP_REARM_DEADLINE_US + 1;
    seal(&missing_rearm);
    CHECK(!vdPmuCleanupRecordValid(&missing_rearm),
          "completion rejects re-arm beyond the bounded deadline");

    static const uint8_t captured_stage1_sample[48] = {
        0x30, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    struct vd_pmu_cleanup_record captured = encoded_busy;
    memcpy(
        &captured.samples[0], captured_stage1_sample,
        sizeof(captured_stage1_sample));
    seal(&captured);
    CHECK(captured.samples[0].struct_size == 48 &&
          captured.samples[0].abi_version == 1 &&
          captured.samples[0].owner_token == 1 &&
          captured.samples[0].generation == 1 &&
          captured.samples[0].event_code == 1 &&
          captured.samples[0].core_id == 0 &&
          captured.samples[0].physical_counter == 5 &&
          captured.samples[0].value == 49,
          "captured hardware sample matches the C ABI at offset 288");
    CHECK(vdPmuCleanupRecordValid(&captured),
          "captured hardware sample validates in a complete stage-1 record");

    struct vd_pmu_cleanup_record unaligned_sample = captured;
    ((uint8_t*)&unaligned_sample)[
        VD_PMU_CLEANUP_SAMPLE_PADDING_OFFSET] = 0x30;
    seal(&unaligned_sample);
    CHECK(!vdPmuCleanupRecordValid(&unaligned_sample),
          "nonzero legacy offset-284 sample padding is rejected");

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
    invalid.samples[0].struct_size =
        sizeof(invalid.samples[0]) - 1;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect rejects initial sample size mismatch");
    invalid = disconnect;
    invalid.samples[0].abi_version =
        VD_KERNEL_PMU_PROFILER_ABI_VERSION + 1;
    seal(&invalid);
    CHECK(!vdPmuCleanupRecordValid(&invalid),
          "disconnect rejects initial sample ABI mismatch");
    invalid = disconnect;
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

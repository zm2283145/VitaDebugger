#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_pmu_profiler.h"

#define VD_PMU_CLEANUP_RECORD_MAGIC UINT32_C(0x56504347)
#define VD_PMU_CLEANUP_RECORD_VERSION 2u
#define VD_PMU_CLEANUP_RECORD_SIZE 1024u
#define VD_PMU_CLEANUP_RESULT_NOT_RUN ((int32_t)-799)
#define VD_PMU_CLEANUP_SLOT_COUNT 3u
#define VD_PMU_CLEANUP_EXPECTED_NET_FAILURE ((int32_t)-13)
#define VD_PMU_CLEANUP_VITA_SYSCALL_BUSY_RESULT \
    ((int32_t)UINT32_C(0xbfffffd6))
#define VD_PMU_CLEANUP_REARM_DEADLINE_US UINT64_C(2000000)

enum gate_result_index {
    GATE_RESULT_OPEN = 0,
    GATE_RESULT_READ,
    GATE_RESULT_CLOSE,
    GATE_RESULT_AUX_CREATE,
    GATE_RESULT_AUX_START,
    GATE_RESULT_AUX_WAIT,
    GATE_RESULT_AUX_DELETE,
    GATE_RESULT_AUX_ACTION,
    GATE_RESULT_AUX_CLEANUP,
    GATE_RESULT_REARM_OPEN,
    GATE_RESULT_REARM_READ,
    GATE_RESULT_REARM_CLOSE,
    GATE_RESULT_AFFINITY,
    GATE_RESULT_AFFINITY_RESTORE,
    GATE_RESULT_NET_START,
    GATE_RESULT_NET_CONNECT,
    GATE_RESULT_NET_PRELUDE,
    GATE_RESULT_NET_FAILURE,
    GATE_RESULT_NET_CLOSE,
    GATE_RESULT_NET_STOP,
    GATE_RESULT_EXIT_RETURN,
    GATE_RESULT_POST_DISCONNECT_READ,
};

enum vd_pmu_cleanup_stage {
    VD_PMU_CLEANUP_STAGE_CONFLICT = 1,
    VD_PMU_CLEANUP_STAGE_TIMEOUT = 2,
    VD_PMU_CLEANUP_STAGE_DISCONNECT = 3,
    VD_PMU_CLEANUP_STAGE_NORMAL_EXIT = 4,
    VD_PMU_CLEANUP_STAGE_ABRUPT_EXIT = 5,
};

enum vd_pmu_cleanup_record_state {
    VD_PMU_CLEANUP_ATTEMPTED = 1,
    VD_PMU_CLEANUP_ARMED = 2,
    VD_PMU_CLEANUP_COMPLETE = 3,
    VD_PMU_CLEANUP_FAILED = 4,
};

#define VD_PMU_CLEANUP_FLAG_ACTION_PROVEN (UINT32_C(1) << 0)
#define VD_PMU_CLEANUP_FLAG_RESTORED (UINT32_C(1) << 1)
#define VD_PMU_CLEANUP_FLAG_REARMED (UINT32_C(1) << 2)
#define VD_PMU_CLEANUP_FLAG_OWNER_ARMED (UINT32_C(1) << 3)
#define VD_PMU_CLEANUP_FLAG_PASS (UINT32_C(1) << 4)
#define VD_PMU_CLEANUP_KNOWN_FLAGS \
    (VD_PMU_CLEANUP_FLAG_ACTION_PROVEN | \
     VD_PMU_CLEANUP_FLAG_RESTORED | \
     VD_PMU_CLEANUP_FLAG_REARMED | \
     VD_PMU_CLEANUP_FLAG_OWNER_ARMED | \
     VD_PMU_CLEANUP_FLAG_PASS)
#define VD_PMU_CLEANUP_COMPLETE_FLAGS \
    (VD_PMU_CLEANUP_FLAG_ACTION_PROVEN | \
     VD_PMU_CLEANUP_FLAG_RESTORED | \
     VD_PMU_CLEANUP_FLAG_REARMED | \
     VD_PMU_CLEANUP_FLAG_PASS)

#define VD_PMU_CLEANUP_REQUIRED_CAPABILITIES \
    (VD_KERNEL_PMU_PROFILER_CAP_EXACT_RESTORE | \
     VD_KERNEL_PMU_PROFILER_CAP_LEASE_WATCHDOG | \
     VD_KERNEL_PMU_PROFILER_CAP_REAL_EVENTS | \
     VD_KERNEL_PMU_PROFILER_CAP_SAFE_POST_RESTORE_REARM | \
     VD_KERNEL_PMU_PROFILER_CAP_PROCESS_EXIT_CLEANUP)

struct vd_pmu_cleanup_record {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    uint32_t revision;
    uint32_t state;
    uint32_t stage;
    uint32_t flags;
    uint64_t started_us;
    uint64_t finished_us;

    int32_t info_result;
    uint32_t capabilities;
    int32_t baseline_status_result;
    int32_t restored_status_result;
    int32_t final_status_result;
    int32_t results[24];
    struct vd_kernel_pmu_profiler_handle handles[3];
    struct vd_kernel_pmu_profiler_sample samples[3];
    uint64_t rearm_elapsed_us;
    struct vd_kernel_pmu_profiler_status baseline;
    struct vd_kernel_pmu_profiler_status restored;
    struct vd_kernel_pmu_profiler_status final;
    uint32_t reserved[7];
};

#define VD_PMU_CLEANUP_RESULTS_OFFSET 68u
#define VD_PMU_CLEANUP_HANDLES_OFFSET 164u
#define VD_PMU_CLEANUP_HANDLE_SIZE 40u
#define VD_PMU_CLEANUP_SAMPLE_PADDING_OFFSET 284u
#define VD_PMU_CLEANUP_SAMPLES_OFFSET 288u
#define VD_PMU_CLEANUP_SAMPLE_SIZE 48u
#define VD_PMU_CLEANUP_REARM_ELAPSED_OFFSET 432u
#define VD_PMU_CLEANUP_BASELINE_OFFSET 440u
#define VD_PMU_CLEANUP_RESTORED_OFFSET 624u
#define VD_PMU_CLEANUP_FINAL_OFFSET 808u
#define VD_PMU_CLEANUP_RESERVED_OFFSET 992u

typedef char vd_pmu_cleanup_record_size_must_be_1024[
    sizeof(struct vd_pmu_cleanup_record) ==
        VD_PMU_CLEANUP_RECORD_SIZE ? 1 : -1];
typedef char vd_pmu_cleanup_results_offset_must_be_68[
    offsetof(struct vd_pmu_cleanup_record, results) ==
        VD_PMU_CLEANUP_RESULTS_OFFSET ? 1 : -1];
typedef char vd_pmu_cleanup_handles_offset_must_be_164[
    offsetof(struct vd_pmu_cleanup_record, handles) ==
        VD_PMU_CLEANUP_HANDLES_OFFSET ? 1 : -1];
typedef char vd_pmu_cleanup_handle_size_must_be_40[
    sizeof(struct vd_kernel_pmu_profiler_handle) ==
        VD_PMU_CLEANUP_HANDLE_SIZE ? 1 : -1];
typedef char vd_pmu_cleanup_sample_padding_must_start_at_284[
    VD_PMU_CLEANUP_HANDLES_OFFSET +
        VD_PMU_CLEANUP_SLOT_COUNT * VD_PMU_CLEANUP_HANDLE_SIZE ==
        VD_PMU_CLEANUP_SAMPLE_PADDING_OFFSET ? 1 : -1];
typedef char vd_pmu_cleanup_samples_offset_must_be_288[
    offsetof(struct vd_pmu_cleanup_record, samples) ==
        VD_PMU_CLEANUP_SAMPLES_OFFSET ? 1 : -1];
typedef char vd_pmu_cleanup_sample_size_must_be_48[
    sizeof(struct vd_kernel_pmu_profiler_sample) ==
        VD_PMU_CLEANUP_SAMPLE_SIZE ? 1 : -1];
typedef char vd_pmu_cleanup_rearm_offset_must_be_432[
    offsetof(struct vd_pmu_cleanup_record, rearm_elapsed_us) ==
        VD_PMU_CLEANUP_REARM_ELAPSED_OFFSET ? 1 : -1];
typedef char vd_pmu_cleanup_baseline_offset_must_be_440[
    offsetof(struct vd_pmu_cleanup_record, baseline) ==
        VD_PMU_CLEANUP_BASELINE_OFFSET ? 1 : -1];
typedef char vd_pmu_cleanup_restored_offset_must_be_624[
    offsetof(struct vd_pmu_cleanup_record, restored) ==
        VD_PMU_CLEANUP_RESTORED_OFFSET ? 1 : -1];
typedef char vd_pmu_cleanup_final_offset_must_be_808[
    offsetof(struct vd_pmu_cleanup_record, final) ==
        VD_PMU_CLEANUP_FINAL_OFFSET ? 1 : -1];
typedef char vd_pmu_cleanup_reserved_offset_must_be_992[
    offsetof(struct vd_pmu_cleanup_record, reserved) ==
        VD_PMU_CLEANUP_RESERVED_OFFSET ? 1 : -1];

static inline uint32_t vdPmuCleanupChecksum(
    const struct vd_pmu_cleanup_record* record)
{
    const uint8_t* bytes = (const uint8_t*)record;
    const size_t checksum_start =
        offsetof(struct vd_pmu_cleanup_record, checksum);
    const size_t checksum_end =
        checksum_start + sizeof(record->checksum);
    uint32_t hash = UINT32_C(2166136261);
    for(size_t i = 0; i < sizeof(*record); ++i)
    {
        const uint8_t value =
            i >= checksum_start && i < checksum_end ? 0 : bytes[i];
        hash ^= value;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static inline int vdPmuCleanupSnapshotEqual(
    const struct vd_kernel_pmu_profiler_snapshot* left,
    const struct vd_kernel_pmu_profiler_snapshot* right)
{
    if(!left || !right)
        return 0;
    const uint8_t* lhs = (const uint8_t*)left;
    const uint8_t* rhs = (const uint8_t*)right;
    for(size_t i = 0; i < sizeof(*left); ++i)
        if(lhs[i] != rhs[i])
            return 0;
    return 1;
}

static inline int vdPmuCleanupStatusIdle(
    const struct vd_kernel_pmu_profiler_status* status)
{
    return status &&
        status->struct_size == sizeof(*status) &&
        status->abi_version ==
            VD_KERNEL_PMU_PROFILER_STATUS_ABI_VERSION &&
        status->transport_state == 0 &&
        status->owner_pid == -1 &&
        status->owner_thread == -1 &&
        status->owner_token == 0 &&
        status->generation == 0 &&
        status->owner_identity_valid == 0 &&
        status->owner_identity_release_uncertain == 0 &&
        status->owner_process_terminal_kind ==
            VD_KERNEL_PMU_PROFILER_TERMINAL_NONE &&
        status->owner_terminal_reference_released == 0 &&
        status->backend_ready == 1 &&
        status->backend_recovery_pending == 0 &&
        status->backend_restore_obligation == 0 &&
        status->snapshot_result == 0 &&
        status->snapshot.event_counter_count == 6;
}

static inline int vdPmuCleanupSampleMatchesHandle(
    const struct vd_kernel_pmu_profiler_sample* sample,
    const struct vd_kernel_pmu_profiler_handle* handle,
    uint32_t event_code)
{
    return sample && handle &&
        handle->owner_token != 0 &&
        handle->generation != 0 &&
        sample->struct_size == sizeof(*sample) &&
        sample->abi_version == VD_KERNEL_PMU_PROFILER_ABI_VERSION &&
        sample->owner_token == handle->owner_token &&
        sample->generation == handle->generation &&
        sample->event_code == event_code &&
        sample->core_id == VD_KERNEL_PMU_PROFILER_FIXED_CORE &&
        sample->physical_counter ==
            VD_KERNEL_PMU_PROFILER_FIXED_COUNTER;
}

static inline int vdPmuCleanupResultIsBusy(int32_t result)
{
    /*
     * Vita converts the provider-local -42 return to 0xBFFFFFD6 while
     * crossing the user/kernel syscall boundary. Accept only that exact
     * observed encoding and the raw host-model value.
     */
    return result == VD_KERNEL_ERROR_PMU_PROFILER_BUSY ||
        result == VD_PMU_CLEANUP_VITA_SYSCALL_BUSY_RESULT;
}

static inline int vdPmuCleanupConflictActionPassed(
    const struct vd_pmu_cleanup_record* record)
{
    return record &&
        record->results[GATE_RESULT_OPEN] == 0 &&
        record->results[GATE_RESULT_READ] == 0 &&
        vdPmuCleanupSampleMatchesHandle(
            &record->samples[0], &record->handles[0],
            VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS) &&
        record->results[GATE_RESULT_AUX_CREATE] >= 0 &&
        record->results[GATE_RESULT_AUX_START] >= 0 &&
        record->results[GATE_RESULT_AUX_WAIT] >= 0 &&
        record->results[GATE_RESULT_AUX_DELETE] >= 0 &&
        vdPmuCleanupResultIsBusy(
            record->results[GATE_RESULT_AUX_ACTION]) &&
        record->results[GATE_RESULT_AUX_CLEANUP] ==
            VD_PMU_CLEANUP_RESULT_NOT_RUN &&
        record->results[GATE_RESULT_CLOSE] == 0;
}

static inline int vdPmuCleanupRearmPassed(
    const struct vd_pmu_cleanup_record* record)
{
    return record &&
        record->results[GATE_RESULT_REARM_OPEN] == 0 &&
        record->results[GATE_RESULT_REARM_READ] == 0 &&
        record->results[GATE_RESULT_REARM_CLOSE] == 0 &&
        vdPmuCleanupSampleMatchesHandle(
            &record->samples[1], &record->handles[1],
            VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS) &&
        record->rearm_elapsed_us <=
            VD_PMU_CLEANUP_REARM_DEADLINE_US;
}

static inline int vdPmuCleanupStageValid(uint32_t stage)
{
    return stage >= VD_PMU_CLEANUP_STAGE_CONFLICT &&
        stage <= VD_PMU_CLEANUP_STAGE_ABRUPT_EXIT;
}

static inline int vdPmuCleanupRecordValid(
    const struct vd_pmu_cleanup_record* record)
{
    if(!record ||
       record->magic != VD_PMU_CLEANUP_RECORD_MAGIC ||
       record->version != VD_PMU_CLEANUP_RECORD_VERSION ||
       record->size != sizeof(*record) ||
       !vdPmuCleanupStageValid(record->stage) ||
       record->revision == 0 || record->revision > 3 ||
       record->state < VD_PMU_CLEANUP_ATTEMPTED ||
       record->state > VD_PMU_CLEANUP_FAILED ||
       (record->flags & ~VD_PMU_CLEANUP_KNOWN_FLAGS) != 0 ||
       record->checksum != vdPmuCleanupChecksum(record))
        return 0;
    const uint8_t* bytes = (const uint8_t*)record;
    for(size_t i = VD_PMU_CLEANUP_SAMPLE_PADDING_OFFSET;
        i < VD_PMU_CLEANUP_SAMPLES_OFFSET; ++i)
        if(bytes[i] != 0)
            return 0;
    for(size_t i = 0;
        i < sizeof(record->reserved) / sizeof(record->reserved[0]); ++i)
        if(record->reserved[i] != 0)
            return 0;
    if(record->info_result != 0 ||
       (record->capabilities &
        VD_PMU_CLEANUP_REQUIRED_CAPABILITIES) !=
           VD_PMU_CLEANUP_REQUIRED_CAPABILITIES ||
       record->baseline_status_result != 0 ||
       !vdPmuCleanupStatusIdle(&record->baseline))
        return 0;
    if(record->state == VD_PMU_CLEANUP_ATTEMPTED)
        return record->revision == 1 && record->flags == 0 &&
            record->restored_status_result ==
                VD_PMU_CLEANUP_RESULT_NOT_RUN &&
            record->final_status_result ==
                VD_PMU_CLEANUP_RESULT_NOT_RUN;
    if(record->state == VD_PMU_CLEANUP_ARMED)
        return record->revision == 2 &&
            (record->stage == VD_PMU_CLEANUP_STAGE_NORMAL_EXIT ||
             record->stage == VD_PMU_CLEANUP_STAGE_ABRUPT_EXIT) &&
            record->flags == VD_PMU_CLEANUP_FLAG_OWNER_ARMED &&
            record->results[0] == 0 && record->results[1] == 0 &&
            record->handles[0].owner_token != 0 &&
            record->handles[0].generation != 0;
    if(record->state == VD_PMU_CLEANUP_COMPLETE)
        return record->revision ==
                (record->stage >= VD_PMU_CLEANUP_STAGE_NORMAL_EXIT ?
                     3u : 2u) &&
            record->flags == VD_PMU_CLEANUP_COMPLETE_FLAGS &&
            record->restored_status_result == 0 &&
            record->final_status_result == 0 &&
            vdPmuCleanupStatusIdle(&record->restored) &&
            vdPmuCleanupStatusIdle(&record->final) &&
            record->final.rearm_count > record->baseline.rearm_count &&
            vdPmuCleanupRearmPassed(record) &&
            (record->stage != VD_PMU_CLEANUP_STAGE_CONFLICT ||
             vdPmuCleanupConflictActionPassed(record)) &&
            (record->stage != VD_PMU_CLEANUP_STAGE_DISCONNECT ||
             (record->results[GATE_RESULT_OPEN] == 0 &&
              record->results[GATE_RESULT_READ] == 0 &&
              record->results[GATE_RESULT_NET_START] == 0 &&
              record->results[GATE_RESULT_NET_CONNECT] == 0 &&
              record->results[GATE_RESULT_NET_PRELUDE] == 0 &&
              record->results[GATE_RESULT_NET_FAILURE] ==
                      VD_PMU_CLEANUP_EXPECTED_NET_FAILURE &&
              record->results[GATE_RESULT_CLOSE] == 0 &&
              record->results[GATE_RESULT_NET_CLOSE] == 0 &&
              record->results[GATE_RESULT_NET_STOP] == 0 &&
              record->results[GATE_RESULT_POST_DISCONNECT_READ] == 0 &&
              vdPmuCleanupSampleMatchesHandle(
                  &record->samples[0], &record->handles[0],
                  VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT) &&
              vdPmuCleanupSampleMatchesHandle(
                  &record->samples[2], &record->handles[0],
                  VD_KERNEL_PMU_PROFILER_EVENT_BRANCH_MISPREDICT))) &&
            (record->stage != VD_PMU_CLEANUP_STAGE_NORMAL_EXIT ||
             (record->restored.active_process_normal_exit_cleanup_count >
                  record->baseline.active_process_normal_exit_cleanup_count &&
              record->restored.active_process_kill_cleanup_count ==
                  record->baseline.active_process_kill_cleanup_count &&
              record->final.active_process_normal_exit_cleanup_count ==
                  record->restored.active_process_normal_exit_cleanup_count &&
              record->final.active_process_kill_cleanup_count ==
                  record->restored.active_process_kill_cleanup_count)) &&
            (record->stage != VD_PMU_CLEANUP_STAGE_ABRUPT_EXIT ||
             (record->restored.active_process_kill_cleanup_count >
                  record->baseline.active_process_kill_cleanup_count &&
              record->restored.active_process_normal_exit_cleanup_count ==
                  record->baseline.active_process_normal_exit_cleanup_count &&
              record->final.active_process_normal_exit_cleanup_count ==
                  record->restored.active_process_normal_exit_cleanup_count &&
              record->final.active_process_kill_cleanup_count ==
                  record->restored.active_process_kill_cleanup_count)) &&
            vdPmuCleanupSnapshotEqual(
                &record->baseline.snapshot,
                &record->restored.snapshot) &&
            vdPmuCleanupSnapshotEqual(
                &record->baseline.snapshot, &record->final.snapshot);
    return (record->flags & VD_PMU_CLEANUP_FLAG_PASS) == 0;
}

#define VD_PMU_CLEANUP_SELECT_EMPTY (-810)
#define VD_PMU_CLEANUP_SELECT_CONFLICT (-811)
#define VD_PMU_CLEANUP_SELECT_INVALID (-812)

static inline int vdPmuCleanupSelectLatest(
    const int* present, const int* valid,
    const struct vd_pmu_cleanup_record* records, size_t count)
{
    if(!present || !valid || !records || count == 0 ||
       count > VD_PMU_CLEANUP_SLOT_COUNT)
        return VD_PMU_CLEANUP_SELECT_INVALID;
    int selected = VD_PMU_CLEANUP_SELECT_EMPTY;
    for(size_t i = 0; i < count; ++i)
    {
        if((present[i] != 0 && present[i] != 1) ||
           (valid[i] != 0 && valid[i] != 1) ||
           (!present[i] && valid[i]) ||
           (present[i] && !valid[i]))
            return VD_PMU_CLEANUP_SELECT_INVALID;
        if(!present[i])
            continue;
        if(selected >= 0 &&
           records[i].stage != records[selected].stage)
            return VD_PMU_CLEANUP_SELECT_INVALID;
        if(selected >= 0 &&
           records[selected].revision == records[i].revision)
            return VD_PMU_CLEANUP_SELECT_CONFLICT;
        if(selected < 0 ||
           (int32_t)(records[i].revision -
                     records[selected].revision) > 0)
            selected = (int)i;
    }
    return selected;
}

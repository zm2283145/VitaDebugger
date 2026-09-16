#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_pmu_profiler.h"

#define VD_PMU_THREAD_EXIT_RECORD_MAGIC UINT32_C(0x56505447)
#define VD_PMU_THREAD_EXIT_RECORD_VERSION 1u
#define VD_PMU_THREAD_EXIT_RECORD_SIZE 256u
#define VD_PMU_THREAD_EXIT_RESULT_NOT_RUN ((int32_t)-699)
#define VD_PMU_THREAD_EXIT_SLOT_COUNT 2u
#define VD_PMU_THREAD_EXIT_REARM_DEADLINE_US UINT64_C(2000000)
#define VD_PMU_THREAD_EXIT_OWNER_LEASE_US UINT64_C(5000000)
#define VD_PMU_THREAD_EXIT_REQUIRED_CAPABILITIES \
    (VD_KERNEL_PMU_PROFILER_CAP_EXACT_RESTORE | \
     VD_KERNEL_PMU_PROFILER_CAP_LEASE_WATCHDOG | \
     VD_KERNEL_PMU_PROFILER_CAP_REAL_EVENTS | \
     VD_KERNEL_PMU_PROFILER_CAP_SAFE_POST_RESTORE_REARM)

#define VD_PMU_THREAD_EXIT_RECORD_A_PATH \
    "ux0:data/VitaDebugger/pmu-thread-exit-v1-a.bin"
#define VD_PMU_THREAD_EXIT_RECORD_B_PATH \
    "ux0:data/VitaDebugger/pmu-thread-exit-v1-b.bin"

enum vd_pmu_thread_exit_record_state {
    VD_PMU_THREAD_EXIT_ATTEMPTED = 1,
    VD_PMU_THREAD_EXIT_COMPLETE = 2,
    VD_PMU_THREAD_EXIT_FAILED = 3,
};

#define VD_PMU_THREAD_EXIT_FLAG_OWNER_SAMPLE (UINT32_C(1) << 0)
#define VD_PMU_THREAD_EXIT_FLAG_REARM_SAMPLE (UINT32_C(1) << 1)
#define VD_PMU_THREAD_EXIT_FLAG_PASS (UINT32_C(1) << 2)
#define VD_PMU_THREAD_EXIT_PASS_FLAGS \
    (VD_PMU_THREAD_EXIT_FLAG_OWNER_SAMPLE | \
     VD_PMU_THREAD_EXIT_FLAG_REARM_SAMPLE | \
     VD_PMU_THREAD_EXIT_FLAG_PASS)
#define VD_PMU_THREAD_EXIT_KNOWN_FLAGS VD_PMU_THREAD_EXIT_PASS_FLAGS

struct vd_pmu_thread_exit_record {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    uint32_t revision;
    uint32_t state;
    uint32_t flags;
    uint32_t capabilities;
    uint64_t timestamp_us;

    int32_t info_result;
    int32_t affinity_result;
    int32_t owner_create_result;
    int32_t owner_start_result;
    int32_t owner_affinity_result;
    int32_t owner_open_result;
    int32_t owner_read_result;
    int32_t owner_wait_result;
    int32_t post_open_result;
    int32_t post_read_result;
    int32_t post_close_result;
    int32_t owner_delete_result;
    int32_t affinity_restore_result;
    int32_t journal_result;

    uint32_t owner_token;
    uint32_t owner_generation;
    uint32_t post_owner_token;
    uint32_t post_generation;

    uint64_t owner_value;
    uint64_t post_value;
    uint64_t rearm_elapsed_us;
    uint64_t owner_open_attempt_us;
    uint64_t post_open_success_us;
    uint64_t lease_to_rearm_elapsed_us;
    uint32_t reserved[24];
};

typedef char vd_pmu_thread_exit_record_size_must_be_256[
    sizeof(struct vd_pmu_thread_exit_record) ==
        VD_PMU_THREAD_EXIT_RECORD_SIZE ? 1 : -1];

static inline uint32_t vdPmuThreadExitChecksum(
    const struct vd_pmu_thread_exit_record* record)
{
    const uint8_t* bytes = (const uint8_t*)record;
    const size_t checksum_start =
        offsetof(struct vd_pmu_thread_exit_record, checksum);
    const size_t checksum_end = checksum_start + sizeof(record->checksum);
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

static inline int vdPmuThreadExitRecordValid(
    const struct vd_pmu_thread_exit_record* record)
{
    if(!record || record->magic != VD_PMU_THREAD_EXIT_RECORD_MAGIC ||
       record->version != VD_PMU_THREAD_EXIT_RECORD_VERSION ||
       record->size != sizeof(*record) || record->revision == 0 ||
       record->state < VD_PMU_THREAD_EXIT_ATTEMPTED ||
       record->state > VD_PMU_THREAD_EXIT_FAILED ||
       (record->flags & ~VD_PMU_THREAD_EXIT_KNOWN_FLAGS) != 0 ||
       record->journal_result != 0 ||
       record->checksum != vdPmuThreadExitChecksum(record))
        return 0;
    for(size_t i = 0;
        i < sizeof(record->reserved) / sizeof(record->reserved[0]); ++i)
        if(record->reserved[i] != 0)
            return 0;

    if(record->state == VD_PMU_THREAD_EXIT_ATTEMPTED)
        return record->revision == 1 && record->flags == 0 &&
            record->owner_open_result ==
                VD_PMU_THREAD_EXIT_RESULT_NOT_RUN &&
            record->post_open_result ==
                VD_PMU_THREAD_EXIT_RESULT_NOT_RUN;
    if(record->state == VD_PMU_THREAD_EXIT_COMPLETE)
        return record->revision == 2 &&
            record->flags == VD_PMU_THREAD_EXIT_PASS_FLAGS &&
            record->info_result == 0 &&
            record->affinity_result >= 0 &&
            (record->capabilities &
             VD_PMU_THREAD_EXIT_REQUIRED_CAPABILITIES) ==
                VD_PMU_THREAD_EXIT_REQUIRED_CAPABILITIES &&
            (record->capabilities &
             VD_KERNEL_PMU_PROFILER_CAP_SINGLE_REAL_EVENT_PER_BOOT) == 0 &&
            record->owner_create_result >= 0 &&
            record->owner_start_result >= 0 &&
            record->owner_affinity_result >= 0 &&
            record->owner_open_result == 0 &&
            record->owner_read_result == 0 &&
            record->owner_wait_result >= 0 &&
            record->post_open_result == 0 &&
            record->post_read_result == 0 &&
            record->post_close_result == 0 &&
            record->owner_delete_result >= 0 &&
            record->affinity_restore_result >= 0 &&
            record->rearm_elapsed_us <=
                VD_PMU_THREAD_EXIT_REARM_DEADLINE_US &&
            record->owner_open_attempt_us != 0 &&
            record->post_open_success_us >=
                record->owner_open_attempt_us &&
            record->lease_to_rearm_elapsed_us ==
                record->post_open_success_us -
                    record->owner_open_attempt_us &&
            record->lease_to_rearm_elapsed_us <
                VD_PMU_THREAD_EXIT_OWNER_LEASE_US &&
            record->owner_token != 0 && record->owner_generation != 0 &&
            record->post_owner_token != 0 &&
            record->post_generation != 0 &&
            (record->post_owner_token != record->owner_token ||
             record->post_generation != record->owner_generation);
    return record->state == VD_PMU_THREAD_EXIT_FAILED &&
        record->revision == 2 &&
        (record->flags & VD_PMU_THREAD_EXIT_FLAG_PASS) == 0;
}

#define VD_PMU_THREAD_EXIT_SELECT_EMPTY (-710)
#define VD_PMU_THREAD_EXIT_SELECT_CONFLICT (-711)
#define VD_PMU_THREAD_EXIT_SELECT_INVALID (-712)

static inline int vdPmuThreadExitSelectLatest(
    const int* present, const int* valid,
    const struct vd_pmu_thread_exit_record* records, size_t count)
{
    if(!present || !valid || !records || count == 0 ||
       count > VD_PMU_THREAD_EXIT_SLOT_COUNT)
        return VD_PMU_THREAD_EXIT_SELECT_INVALID;
    int selected = VD_PMU_THREAD_EXIT_SELECT_EMPTY;
    for(size_t i = 0; i < count; ++i)
    {
        if((present[i] != 0 && present[i] != 1) ||
           (valid[i] != 0 && valid[i] != 1) ||
           (!present[i] && valid[i]) || (present[i] && !valid[i]))
            return VD_PMU_THREAD_EXIT_SELECT_INVALID;
        if(!present[i])
            continue;
        if(selected >= 0 &&
           records[selected].revision == records[i].revision)
            return VD_PMU_THREAD_EXIT_SELECT_CONFLICT;
        if(selected < 0 ||
           (int32_t)(records[i].revision -
                     records[selected].revision) > 0)
            selected = (int)i;
    }
    return selected;
}

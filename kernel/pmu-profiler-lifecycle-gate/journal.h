#pragma once

#include <stddef.h>
#include <stdint.h>

#define VD_PMU_LIFECYCLE_RECORD_MAGIC UINT32_C(0x56504c47)
#define VD_PMU_LIFECYCLE_RECORD_VERSION 1u
#define VD_PMU_LIFECYCLE_RECORD_SIZE 512u
#define VD_PMU_LIFECYCLE_RESULT_NOT_RUN ((int32_t)-599)
#define VD_PMU_LIFECYCLE_SLOT_COUNT 5u

#define VD_PMU_LIFECYCLE_RECORD_A_PATH \
    "ux0:data/VitaDebugger/pmu-lifecycle-v1-a.bin"
#define VD_PMU_LIFECYCLE_RECORD_B_PATH \
    "ux0:data/VitaDebugger/pmu-lifecycle-v1-b.bin"
#define VD_PMU_LIFECYCLE_RECORD_C_PATH \
    "ux0:data/VitaDebugger/pmu-lifecycle-v1-c.bin"
#define VD_PMU_LIFECYCLE_RECORD_D_PATH \
    "ux0:data/VitaDebugger/pmu-lifecycle-v1-d.bin"
#define VD_PMU_LIFECYCLE_RECORD_E_PATH \
    "ux0:data/VitaDebugger/pmu-lifecycle-v1-e.bin"

enum vd_pmu_lifecycle_record_state {
    VD_PMU_LIFECYCLE_ATTEMPTED = 1,
    VD_PMU_LIFECYCLE_PROCESS_EXIT_ATTEMPTED = 2,
    VD_PMU_LIFECYCLE_PROCESS_EXIT_ARMED = 3,
    VD_PMU_LIFECYCLE_RESUME_ATTEMPTED = 4,
    VD_PMU_LIFECYCLE_COMPLETE = 5,
    VD_PMU_LIFECYCLE_FAILED = 6,
};

#define VD_PMU_LIFECYCLE_FLAG_EXPLICIT_CLOSE (UINT32_C(1) << 0)
#define VD_PMU_LIFECYCLE_FLAG_OWNERSHIP_CONFLICT (UINT32_C(1) << 1)
#define VD_PMU_LIFECYCLE_FLAG_TIMEOUT_WATCHDOG (UINT32_C(1) << 2)
#define VD_PMU_LIFECYCLE_FLAG_THREAD_EXIT (UINT32_C(1) << 3)
#define VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_ARMED (UINT32_C(1) << 4)
#define VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_REARM (UINT32_C(1) << 5)
#define VD_PMU_LIFECYCLE_FLAG_PASS (UINT32_C(1) << 6)
#define VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS \
    (VD_PMU_LIFECYCLE_FLAG_EXPLICIT_CLOSE | \
     VD_PMU_LIFECYCLE_FLAG_OWNERSHIP_CONFLICT | \
     VD_PMU_LIFECYCLE_FLAG_TIMEOUT_WATCHDOG | \
     VD_PMU_LIFECYCLE_FLAG_THREAD_EXIT)
#define VD_PMU_LIFECYCLE_KNOWN_FLAGS \
    (VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS | \
     VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_ARMED | \
     VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_REARM | \
     VD_PMU_LIFECYCLE_FLAG_PASS)

struct vd_pmu_lifecycle_record {
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
    int32_t explicit_open_result;
    int32_t explicit_read_result;
    int32_t explicit_close_result;
    int32_t contender_create_result;
    int32_t contender_start_result;
    int32_t contender_open_result;
    int32_t contender_cleanup_result;
    int32_t contender_wait_result;
    int32_t contender_delete_result;
    int32_t timeout_open_result;
    int32_t timeout_read_result;
    int32_t timeout_close_result;
    int32_t exit_owner_create_result;
    int32_t exit_owner_start_result;
    int32_t exit_owner_affinity_result;
    int32_t exit_owner_open_result;
    int32_t exit_owner_read_result;
    int32_t exit_owner_wait_result;
    int32_t exit_owner_delete_result;
    int32_t post_exit_open_result;
    int32_t post_exit_read_result;
    int32_t post_exit_close_result;
    int32_t process_open_result;
    int32_t process_read_result;
    int32_t process_cleanup_result;
    int32_t resume_open_result;
    int32_t resume_read_result;
    int32_t resume_close_result;
    int32_t affinity_restore_result;
    int32_t journal_result;

    uint32_t explicit_owner_token;
    uint32_t explicit_generation;
    uint32_t timeout_owner_token;
    uint32_t timeout_generation;
    uint32_t exit_owner_token;
    uint32_t exit_generation;
    uint32_t post_exit_owner_token;
    uint32_t post_exit_generation;
    uint32_t process_owner_token;
    uint32_t process_generation;
    uint32_t resume_owner_token;
    uint32_t resume_generation;

    uint64_t explicit_value;
    uint64_t timeout_value;
    uint64_t exit_value;
    uint64_t post_exit_value;
    uint64_t process_value;
    uint64_t resume_value;
    uint32_t reserved[62];
};

typedef char vd_pmu_lifecycle_record_size_must_be_512[
    sizeof(struct vd_pmu_lifecycle_record) ==
        VD_PMU_LIFECYCLE_RECORD_SIZE ? 1 : -1];

static inline uint32_t vdPmuLifecycleChecksum(
    const struct vd_pmu_lifecycle_record* record)
{
    const uint8_t* bytes = (const uint8_t*)record;
    const size_t checksum_start =
        offsetof(struct vd_pmu_lifecycle_record, checksum);
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

static inline int vdPmuLifecycleRecordValid(
    const struct vd_pmu_lifecycle_record* record)
{
    if(!record || record->magic != VD_PMU_LIFECYCLE_RECORD_MAGIC ||
       record->version != VD_PMU_LIFECYCLE_RECORD_VERSION ||
       record->size != sizeof(*record) || record->revision == 0 ||
       record->state < VD_PMU_LIFECYCLE_ATTEMPTED ||
       record->state > VD_PMU_LIFECYCLE_FAILED ||
       (record->flags & ~VD_PMU_LIFECYCLE_KNOWN_FLAGS) != 0 ||
       record->journal_result != 0 ||
       record->checksum != vdPmuLifecycleChecksum(record))
        return 0;
    for(size_t i = 0;
        i < sizeof(record->reserved) / sizeof(record->reserved[0]); ++i)
        if(record->reserved[i] != 0)
            return 0;

    if(record->state == VD_PMU_LIFECYCLE_ATTEMPTED)
        return record->revision == 1 && record->flags == 0;
    if(record->state == VD_PMU_LIFECYCLE_PROCESS_EXIT_ATTEMPTED)
        return record->revision == 2 &&
            (record->flags & VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS) ==
                VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS &&
            record->process_open_result ==
                VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    if(record->state == VD_PMU_LIFECYCLE_PROCESS_EXIT_ARMED)
        return record->revision == 3 &&
            (record->flags & (VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS |
                              VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_ARMED)) ==
                (VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS |
                 VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_ARMED) &&
            record->process_open_result == 0 &&
            record->process_read_result == 0 &&
            record->process_owner_token != 0 &&
            record->process_generation != 0;
    if(record->state == VD_PMU_LIFECYCLE_RESUME_ATTEMPTED)
        return record->revision == 4 &&
            (record->flags & VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_ARMED) != 0 &&
            record->resume_open_result == VD_PMU_LIFECYCLE_RESULT_NOT_RUN;
    if(record->state == VD_PMU_LIFECYCLE_COMPLETE)
        return record->revision == 5 &&
            (record->flags & (VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS |
                              VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_ARMED |
                              VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_REARM |
                              VD_PMU_LIFECYCLE_FLAG_PASS)) ==
                (VD_PMU_LIFECYCLE_FIRST_RUN_FLAGS |
                 VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_ARMED |
                 VD_PMU_LIFECYCLE_FLAG_PROCESS_EXIT_REARM |
                 VD_PMU_LIFECYCLE_FLAG_PASS) &&
            record->resume_open_result == 0 &&
            record->resume_read_result == 0 &&
            record->resume_close_result == 0 &&
            record->resume_owner_token != 0 &&
            record->resume_generation != 0;
    return record->state == VD_PMU_LIFECYCLE_FAILED &&
        record->revision >= 2 && record->revision <= 5 &&
        (record->flags & VD_PMU_LIFECYCLE_FLAG_PASS) == 0;
}

#define VD_PMU_LIFECYCLE_SELECT_EMPTY (-610)
#define VD_PMU_LIFECYCLE_SELECT_CONFLICT (-611)
#define VD_PMU_LIFECYCLE_SELECT_INVALID (-612)

static inline int vdPmuLifecycleSelectLatest(
    const int* present, const int* valid,
    const struct vd_pmu_lifecycle_record* records, size_t count)
{
    if(!present || !valid || !records || count == 0 ||
       count > VD_PMU_LIFECYCLE_SLOT_COUNT)
        return VD_PMU_LIFECYCLE_SELECT_INVALID;
    int selected = VD_PMU_LIFECYCLE_SELECT_EMPTY;
    for(size_t i = 0; i < count; ++i)
    {
        if((present[i] != 0 && present[i] != 1) ||
           (valid[i] != 0 && valid[i] != 1) ||
           (!present[i] && valid[i]) || (present[i] && !valid[i]))
            return VD_PMU_LIFECYCLE_SELECT_INVALID;
        if(!present[i])
            continue;
        if(selected >= 0 &&
           records[selected].revision == records[i].revision)
            return VD_PMU_LIFECYCLE_SELECT_CONFLICT;
        if(selected < 0 || (int32_t)(records[i].revision -
                                     records[selected].revision) > 0)
            selected = (int)i;
    }
    return selected;
}

#pragma once

#include <stddef.h>
#include <stdint.h>

#define VD_PMU_PROFILER_GATE_RECORD_MAGIC UINT32_C(0x56504754)
#define VD_PMU_PROFILER_GATE_RECORD_VERSION 1u
#define VD_PMU_PROFILER_GATE_RECORD_SIZE 256u
#define VD_PMU_PROFILER_GATE_RESULT_NOT_RUN ((int32_t)-599)

#define VD_PMU_PROFILER_GATE_RECORD_A_PATH \
    "ux0:data/VitaDebugger/pmu-profiler-gate-v1-a.bin"
#define VD_PMU_PROFILER_GATE_RECORD_B_PATH \
    "ux0:data/VitaDebugger/pmu-profiler-gate-v1-b.bin"

enum vd_pmu_profiler_gate_record_state {
    VD_PMU_PROFILER_GATE_ATTEMPTED = 1,
    VD_PMU_PROFILER_GATE_COMPLETE = 2,
    VD_PMU_PROFILER_GATE_RESTORE_REQUIRED = 3,
};

#define VD_PMU_PROFILER_GATE_FLAG_RESTORE_PROVEN (UINT32_C(1) << 0)
#define VD_PMU_PROFILER_GATE_FLAG_SAMPLE_VALID (UINT32_C(1) << 1)
#define VD_PMU_PROFILER_GATE_FLAG_PASS (UINT32_C(1) << 2)
#define VD_PMU_PROFILER_GATE_KNOWN_FLAGS \
    (VD_PMU_PROFILER_GATE_FLAG_RESTORE_PROVEN | \
     VD_PMU_PROFILER_GATE_FLAG_SAMPLE_VALID | \
     VD_PMU_PROFILER_GATE_FLAG_PASS)

struct vd_pmu_profiler_gate_record {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    uint32_t revision;
    uint32_t state;
    uint32_t event_code;
    uint32_t flags;
    uint64_t timestamp_us;
    int32_t info_result;
    int32_t affinity_result;
    int32_t open_result;
    int32_t read_result;
    int32_t close_result;
    int32_t affinity_restore_result;
    int32_t journal_result;
    int32_t reserved_result;
    uint32_t owner_token;
    uint32_t generation;
    uint32_t value_low;
    uint32_t value_high;
    uint32_t core_id;
    uint32_t physical_counter;
    uint32_t reserved[40];
};

typedef char vd_pmu_profiler_gate_record_size_must_be_256[
    sizeof(struct vd_pmu_profiler_gate_record) ==
        VD_PMU_PROFILER_GATE_RECORD_SIZE ? 1 : -1];

static inline uint32_t vdPmuProfilerGateChecksum(
    const struct vd_pmu_profiler_gate_record* record)
{
    const uint8_t* bytes = (const uint8_t*)record;
    const size_t checksum_start =
        offsetof(struct vd_pmu_profiler_gate_record, checksum);
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

static inline int vdPmuProfilerGateEventValid(uint32_t event_code)
{
    return event_code == UINT32_C(0x01) ||
        event_code == UINT32_C(0x03) ||
        event_code == UINT32_C(0x10);
}

static inline int vdPmuProfilerGateRecordValid(
    const struct vd_pmu_profiler_gate_record* record)
{
    if(!record || record->magic != VD_PMU_PROFILER_GATE_RECORD_MAGIC ||
       record->version != VD_PMU_PROFILER_GATE_RECORD_VERSION ||
       record->size != sizeof(*record) || record->revision == 0 ||
       record->state < VD_PMU_PROFILER_GATE_ATTEMPTED ||
       record->state > VD_PMU_PROFILER_GATE_RESTORE_REQUIRED ||
       !vdPmuProfilerGateEventValid(record->event_code) ||
       (record->flags & ~VD_PMU_PROFILER_GATE_KNOWN_FLAGS) != 0 ||
       record->journal_result != 0 || record->reserved_result != 0 ||
       record->checksum != vdPmuProfilerGateChecksum(record))
        return 0;
    for(size_t i = 0;
        i < sizeof(record->reserved) / sizeof(record->reserved[0]); ++i)
        if(record->reserved[i] != 0)
            return 0;

    if(record->state == VD_PMU_PROFILER_GATE_ATTEMPTED)
        return record->flags == 0 &&
            record->open_result == VD_PMU_PROFILER_GATE_RESULT_NOT_RUN &&
            record->read_result == VD_PMU_PROFILER_GATE_RESULT_NOT_RUN &&
            record->close_result == VD_PMU_PROFILER_GATE_RESULT_NOT_RUN &&
            record->affinity_restore_result ==
                VD_PMU_PROFILER_GATE_RESULT_NOT_RUN &&
            record->owner_token == 0 && record->generation == 0 &&
            record->value_low == 0 && record->value_high == 0 &&
            record->core_id == 0 && record->physical_counter == 0;
    if(record->state == VD_PMU_PROFILER_GATE_RESTORE_REQUIRED)
        return record->flags == 0 &&
            (record->open_result < 0 || record->close_result < 0);
    if(record->open_result != 0 || record->close_result != 0 ||
       (record->flags & VD_PMU_PROFILER_GATE_FLAG_RESTORE_PROVEN) == 0)
        return 0;
    if((record->flags & VD_PMU_PROFILER_GATE_FLAG_SAMPLE_VALID) != 0 &&
       (record->read_result != 0 || record->owner_token == 0 ||
        record->generation == 0 || record->core_id != 0 ||
        record->physical_counter != 5))
        return 0;
    if((record->flags & VD_PMU_PROFILER_GATE_FLAG_PASS) != 0 &&
       ((record->flags & VD_PMU_PROFILER_GATE_FLAG_SAMPLE_VALID) == 0 ||
        record->info_result != 0 || record->affinity_result < 0 ||
        record->open_result != 0 || record->read_result != 0 ||
        record->affinity_restore_result < 0))
        return 0;
    return 1;
}

static inline int vdPmuProfilerGateRecordPassed(
    const struct vd_pmu_profiler_gate_record* record)
{
    return vdPmuProfilerGateRecordValid(record) &&
        record->state == VD_PMU_PROFILER_GATE_COMPLETE &&
        (record->flags & (VD_PMU_PROFILER_GATE_FLAG_RESTORE_PROVEN |
                          VD_PMU_PROFILER_GATE_FLAG_SAMPLE_VALID |
                          VD_PMU_PROFILER_GATE_FLAG_PASS)) ==
            (VD_PMU_PROFILER_GATE_FLAG_RESTORE_PROVEN |
             VD_PMU_PROFILER_GATE_FLAG_SAMPLE_VALID |
             VD_PMU_PROFILER_GATE_FLAG_PASS);
}

static inline int vdPmuProfilerGateRevisionNewer(
    uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) > 0;
}

#define VD_PMU_PROFILER_GATE_SELECT_EMPTY (-510)
#define VD_PMU_PROFILER_GATE_SELECT_CONFLICT (-511)
#define VD_PMU_PROFILER_GATE_SELECT_INVALID (-512)

static inline int vdPmuProfilerGateSelectLatest(
    int present_a, int valid_a,
    const struct vd_pmu_profiler_gate_record* a,
    int present_b, int valid_b,
    const struct vd_pmu_profiler_gate_record* b)
{
    if((present_a != 0 && present_a != 1) ||
       (present_b != 0 && present_b != 1) ||
       (valid_a != 0 && valid_a != 1) ||
       (valid_b != 0 && valid_b != 1) ||
       (!present_a && valid_a) || (!present_b && valid_b) ||
       (present_a && (!valid_a || !a)) ||
       (present_b && (!valid_b || !b)))
        return VD_PMU_PROFILER_GATE_SELECT_INVALID;
    if(!present_a && !present_b)
        return VD_PMU_PROFILER_GATE_SELECT_EMPTY;
    if(present_a && present_b && a->revision == b->revision)
        return VD_PMU_PROFILER_GATE_SELECT_CONFLICT;
    if(present_b && (!present_a ||
       vdPmuProfilerGateRevisionNewer(b->revision, a->revision)))
        return 1;
    return 0;
}

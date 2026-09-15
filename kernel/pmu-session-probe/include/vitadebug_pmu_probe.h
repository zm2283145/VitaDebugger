#ifndef VITADEBUG_PMU_PROBE_H
#define VITADEBUG_PMU_PROBE_H

#include <stddef.h>
#include <stdint.h>

#include "pmu_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VD_PMU_PROBE_ABI_VERSION UINT32_C(1)
#define VD_PMU_PROBE_RECORD_MAGIC UINT32_C(0x56504d55)
#define VD_PMU_PROBE_RECORD_VERSION UINT32_C(1)
#define VD_PMU_PROBE_RECORD_SIZE UINT32_C(512)
#define VD_PMU_PROBE_STATUS_SIZE UINT32_C(544)

#define VD_PMU_PROBE_RECORD_A_PATH \
    "ux0:data/VitaDebugger/pmu-session-gate-v1-a.bin"
#define VD_PMU_PROBE_RECORD_B_PATH \
    "ux0:data/VitaDebugger/pmu-session-gate-v1-b.bin"

#define VD_PMU_PROBE_ERROR_INVALID (-300)
#define VD_PMU_PROBE_ERROR_DISABLED (-301)
#define VD_PMU_PROBE_ERROR_BUSY (-302)
#define VD_PMU_PROBE_ERROR_JOURNAL (-303)
#define VD_PMU_PROBE_ERROR_LOCKED (-304)
#define VD_PMU_PROBE_ERROR_CORE_ORDER (-305)
#define VD_PMU_PROBE_ERROR_RESTORE_REQUIRED (-306)
#define VD_PMU_PROBE_ERROR_NO_RUNTIME_RESTORE (-307)
#define VD_PMU_PROBE_ERROR_REVISION (-308)

#define VD_PMU_PROBE_RESULT_NOT_RUN (-399)

#define VD_PMU_PROBE_JOURNAL_OK 0
#define VD_PMU_PROBE_JOURNAL_EMPTY 1
#define VD_PMU_PROBE_JOURNAL_CORRUPT (-1)
#define VD_PMU_PROBE_JOURNAL_IO (-2)
#define VD_PMU_PROBE_JOURNAL_CONFLICT (-3)

enum vd_pmu_probe_record_state {
    VD_PMU_PROBE_STATE_ATTEMPTED = 1,
    VD_PMU_PROBE_STATE_KERNEL_ENTERED = 2,
    VD_PMU_PROBE_STATE_RECOVERY_STARTED = 3,
    VD_PMU_PROBE_STATE_COMPLETE = 4,
    VD_PMU_PROBE_STATE_RESTORE_REQUIRED = 5,
};

enum vd_pmu_probe_record_flags {
    VD_PMU_PROBE_FLAG_HAS_RESULT = UINT32_C(1) << 0,
    VD_PMU_PROBE_FLAG_RESTORE_PROVEN = UINT32_C(1) << 1,
    VD_PMU_PROBE_FLAG_RUNTIME_OBLIGATION = UINT32_C(1) << 2,
    VD_PMU_PROBE_FLAG_BACKEND_READY = UINT32_C(1) << 3,
    VD_PMU_PROBE_FLAG_RECOVERY_ATTEMPTED = UINT32_C(1) << 4,
    VD_PMU_PROBE_FLAG_JOURNAL_WRITE_FAILED = UINT32_C(1) << 5,
};

#define VD_PMU_PROBE_KNOWN_FLAGS \
    (VD_PMU_PROBE_FLAG_HAS_RESULT | \
     VD_PMU_PROBE_FLAG_RESTORE_PROVEN | \
     VD_PMU_PROBE_FLAG_RUNTIME_OBLIGATION | \
     VD_PMU_PROBE_FLAG_BACKEND_READY | \
     VD_PMU_PROBE_FLAG_RECOVERY_ATTEMPTED | \
     VD_PMU_PROBE_FLAG_JOURNAL_WRITE_FAILED)

/*
 * Each transition is written to the older of two alternating files and
 * synchronised before the next potentially mutating step. A failed record is
 * intentionally a latch: collect both files before clearing them manually.
 */
struct vd_pmu_probe_record {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    uint32_t revision;
    uint32_t sequence;
    uint32_t state;
    uint32_t flags;
    int32_t syscall_result;
    int32_t journal_result;
    int32_t module_start_result;
    int32_t recovery_result;
    int32_t caller_pid;
    uint32_t requested_core;
    uint32_t backend_ready;
    uint32_t restore_obligation;
    uint32_t passed_core_mask;
    uint32_t reserved0;
    uint64_t timestamp_us;
    struct vd_pmu_backend_test_result test;
    uint32_t reserved[58];
};

struct vd_pmu_probe_status {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t module_start_result;
    int32_t journal_result;
    uint32_t backend_ready;
    uint32_t restore_obligation;
    uint32_t locked;
    uint32_t reserved;
    struct vd_pmu_probe_record latest;
};

#if defined(__cplusplus)
static_assert(sizeof(struct vd_pmu_backend_test_result) == 200,
              "PMU backend test-result ABI changed");
static_assert(sizeof(struct vd_pmu_probe_record) == VD_PMU_PROBE_RECORD_SIZE,
              "PMU probe journal ABI changed");
static_assert(sizeof(struct vd_pmu_probe_status) == VD_PMU_PROBE_STATUS_SIZE,
              "PMU probe status ABI changed");
#else
_Static_assert(sizeof(struct vd_pmu_backend_test_result) == 200,
               "PMU backend test-result ABI changed");
_Static_assert(sizeof(struct vd_pmu_probe_record) == VD_PMU_PROBE_RECORD_SIZE,
               "PMU probe journal ABI changed");
_Static_assert(sizeof(struct vd_pmu_probe_status) == VD_PMU_PROBE_STATUS_SIZE,
               "PMU probe status ABI changed");
#endif

static inline uint32_t vdPmuProbeChecksum(
    const struct vd_pmu_probe_record* record)
{
    const uint8_t* bytes = (const uint8_t*)record;
    const size_t checksum_start =
        offsetof(struct vd_pmu_probe_record, checksum);
    const size_t checksum_end = checksum_start + sizeof(record->checksum);
    uint32_t hash = UINT32_C(2166136261);

    for(size_t i = 0; i < sizeof(*record); ++i)
    {
        const uint8_t value =
            (i >= checksum_start && i < checksum_end) ? 0 : bytes[i];
        hash ^= value;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static inline int vdPmuProbeSnapshotEqual(
    const struct vd_pmu_snapshot* left,
    const struct vd_pmu_snapshot* right)
{
    const uint8_t* lhs = (const uint8_t*)left;
    const uint8_t* rhs = (const uint8_t*)right;

    if(!left || !right)
        return 0;
    for(size_t i = 0; i < sizeof(*left); ++i)
        if(lhs[i] != rhs[i])
            return 0;
    return 1;
}

static inline int vdPmuProbeRestorationProven(
    const struct vd_pmu_backend_test_result* test)
{
    return test &&
           test->struct_size == sizeof(*test) &&
           test->abi_version == VD_PMU_BACKEND_ABI_VERSION &&
           test->restore_result == 0 &&
           test->before.event_counter_count ==
               VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS &&
           test->after.event_counter_count ==
               VD_PMU_SESSION_PHYSICAL_EVENT_COUNTERS &&
           vdPmuProbeSnapshotEqual(&test->before, &test->after);
}

static inline int vdPmuProbeTestPassed(
    const struct vd_pmu_backend_test_result* test)
{
    return test && vdPmuProbeRestorationProven(test) &&
           test->stage == VD_PMU_BACKEND_TEST_COMPLETE &&
           test->operation_result == 0 &&
           test->observed_count == VD_PMU_BACKEND_TEST_INCREMENT_COUNT &&
           test->increment_count == VD_PMU_BACKEND_TEST_INCREMENT_COUNT;
}

static inline int vdPmuProbeRecordValid(
    const struct vd_pmu_probe_record* record)
{
    if(!record || record->magic != VD_PMU_PROBE_RECORD_MAGIC ||
       record->version != VD_PMU_PROBE_RECORD_VERSION ||
       record->size != sizeof(*record) || record->revision == 0 ||
       record->sequence == 0 ||
       record->state < VD_PMU_PROBE_STATE_ATTEMPTED ||
       record->state > VD_PMU_PROBE_STATE_RESTORE_REQUIRED ||
       (record->flags & ~VD_PMU_PROBE_KNOWN_FLAGS) != 0 ||
       record->requested_core >= VD_PMU_BACKEND_APP_CORE_COUNT ||
       record->backend_ready > 1 || record->restore_obligation > 1 ||
       (record->passed_core_mask & ~UINT32_C(0x7)) != 0 ||
       record->reserved0 != 0 ||
       record->checksum != vdPmuProbeChecksum(record))
        return 0;
    for(size_t i = 0;
        i < sizeof(record->reserved) / sizeof(record->reserved[0]); ++i)
        if(record->reserved[i] != 0)
            return 0;
    if(((record->flags & VD_PMU_PROBE_FLAG_RUNTIME_OBLIGATION) != 0) !=
       (record->restore_obligation != 0))
        return 0;
    if(record->state == VD_PMU_PROBE_STATE_COMPLETE &&
       record->restore_obligation != 0)
        return 0;
    if(record->state == VD_PMU_PROBE_STATE_RESTORE_REQUIRED &&
       record->restore_obligation == 0)
        return 0;
    if((record->flags & VD_PMU_PROBE_FLAG_HAS_RESULT) != 0 &&
       (record->test.struct_size != sizeof(record->test) ||
        record->test.abi_version != VD_PMU_BACKEND_ABI_VERSION ||
        record->test.core_id != record->requested_core ||
        record->test.increment_count !=
            VD_PMU_BACKEND_TEST_INCREMENT_COUNT))
        return 0;
    return 1;
}

static inline int vdPmuProbeRevisionNewer(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) > 0;
}

#define VD_PMU_PROBE_SELECT_EMPTY (-1)
#define VD_PMU_PROBE_SELECT_CONFLICT (-2)
#define VD_PMU_PROBE_SELECT_INVALID (-3)

/*
 * Returns the newest slot (0/1), EMPTY when both slots are absent, CONFLICT
 * on an equal-revision pair, or INVALID when any present slot is invalid.
 * Keeping presence separate from validity prevents a valid older record from
 * hiding a torn, corrupt, or unreadable sibling.
 */
static inline int vdPmuProbeSelectLatest(
    int present_a, int valid_a, const struct vd_pmu_probe_record* a,
    int present_b, int valid_b, const struct vd_pmu_probe_record* b)
{
    if((present_a != 0 && present_a != 1) ||
       (present_b != 0 && present_b != 1) ||
       (valid_a != 0 && valid_a != 1) ||
       (valid_b != 0 && valid_b != 1) ||
       (!present_a && valid_a) || (!present_b && valid_b) ||
       (present_a && (!valid_a || !a)) ||
       (present_b && (!valid_b || !b)))
        return VD_PMU_PROBE_SELECT_INVALID;
    if(!present_a && !present_b)
        return VD_PMU_PROBE_SELECT_EMPTY;
    if(present_a && present_b && a->revision == b->revision)
        return VD_PMU_PROBE_SELECT_CONFLICT;
    if(present_b && (!present_a ||
                  vdPmuProbeRevisionNewer(b->revision, a->revision)))
        return 1;
    return 0;
}

static inline int vdPmuProbeRecordPassed(
    const struct vd_pmu_probe_record* record)
{
    return vdPmuProbeRecordValid(record) &&
           record->state == VD_PMU_PROBE_STATE_COMPLETE &&
           record->syscall_result == 0 &&
           record->restore_obligation == 0 &&
           (record->flags & VD_PMU_PROBE_FLAG_HAS_RESULT) != 0 &&
           (record->flags & VD_PMU_PROBE_FLAG_RESTORE_PROVEN) != 0 &&
           vdPmuProbeTestPassed(&record->test);
}

int vdPmuProbeRunSelfTest(
    uint32_t core_id, struct vd_pmu_backend_test_result* result);
int vdPmuProbeGetStatus(struct vd_pmu_probe_status* status);
int vdPmuProbeRecover(void);

#ifdef __cplusplus
}
#endif

#endif

#ifndef VD_DIPSW_DBGVCR_RECORD_H
#define VD_DIPSW_DBGVCR_RECORD_H

#include <stddef.h>
#include <stdint.h>

#define VD_DIPSW_DBGVCR_MAGIC UINT32_C(0x56444456)
#define VD_DIPSW_DBGVCR_VERSION UINT32_C(1)
#define VD_DIPSW_DBGVCR_RECORD_SIZE UINT32_C(192)

#define VD_DIPSW_DBGVCR_RECORD_A_PATH \
    "ux0:data/VitaDebugger/dipsw-dbgvcr-v1-a.bin"
#define VD_DIPSW_DBGVCR_RECORD_B_PATH \
    "ux0:data/VitaDebugger/dipsw-dbgvcr-v1-b.bin"

#define VD_DIPSW_RECONFIG_BIT UINT32_C(203)
#define VD_DIPSW_HW_DEBUG_BIT UINT32_C(228)
#define VD_DIPSW_DEBUG_INFO_INDEX UINT32_C(6)
#define VD_DIPSW_SYSTEM_INFO_INDEX UINT32_C(7)
#define VD_DIPSW_RECONFIG_MASK UINT32_C(0x00000800)
#define VD_DIPSW_HW_DEBUG_MASK UINT32_C(0x00000010)

#if defined(__cplusplus)
static_assert(VD_DIPSW_RECONFIG_BIT / 32u == VD_DIPSW_DEBUG_INFO_INDEX,
              "bit 203 word mapping changed");
static_assert(VD_DIPSW_RECONFIG_BIT % 32u == 11u,
              "bit 203 position changed");
static_assert((UINT32_C(1) << (VD_DIPSW_RECONFIG_BIT % 32u)) ==
                  VD_DIPSW_RECONFIG_MASK,
              "bit 203 mask changed");
static_assert(VD_DIPSW_HW_DEBUG_BIT / 32u == VD_DIPSW_SYSTEM_INFO_INDEX,
              "bit 228 word mapping changed");
static_assert(VD_DIPSW_HW_DEBUG_BIT % 32u == 4u,
              "bit 228 position changed");
static_assert((UINT32_C(1) << (VD_DIPSW_HW_DEBUG_BIT % 32u)) ==
                  VD_DIPSW_HW_DEBUG_MASK,
              "bit 228 mask changed");
#else
_Static_assert(VD_DIPSW_RECONFIG_BIT / 32u == VD_DIPSW_DEBUG_INFO_INDEX,
               "bit 203 word mapping changed");
_Static_assert(VD_DIPSW_RECONFIG_BIT % 32u == 11u,
               "bit 203 position changed");
_Static_assert((UINT32_C(1) << (VD_DIPSW_RECONFIG_BIT % 32u)) ==
                   VD_DIPSW_RECONFIG_MASK,
               "bit 203 mask changed");
_Static_assert(VD_DIPSW_HW_DEBUG_BIT / 32u == VD_DIPSW_SYSTEM_INFO_INDEX,
               "bit 228 word mapping changed");
_Static_assert(VD_DIPSW_HW_DEBUG_BIT % 32u == 4u,
               "bit 228 position changed");
_Static_assert((UINT32_C(1) << (VD_DIPSW_HW_DEBUG_BIT % 32u)) ==
                   VD_DIPSW_HW_DEBUG_MASK,
               "bit 228 mask changed");
#endif

enum vd_dipsw_dbgvcr_state {
    VD_DIPSW_DBGVCR_STATE_ATTEMPTED = 1,
    VD_DIPSW_DBGVCR_STATE_KERNEL_ENTERED = 2,
    VD_DIPSW_DBGVCR_STATE_ORIGINAL_CAPTURED = 3,
    VD_DIPSW_DBGVCR_STATE_READ_PENDING = 4,
    VD_DIPSW_DBGVCR_STATE_COMPLETE = 5,
};

enum vd_dipsw_dbgvcr_result {
    VD_DIPSW_DBGVCR_OK = 0,
    VD_DIPSW_DBGVCR_NOT_RUN = -100,
    VD_DIPSW_DBGVCR_ERROR_BEFORE_CHECK_203 = -1,
    VD_DIPSW_DBGVCR_ERROR_BEFORE_CHECK_228 = -2,
    VD_DIPSW_DBGVCR_ERROR_BEFORE_MISMATCH_203 = -3,
    VD_DIPSW_DBGVCR_ERROR_BEFORE_MISMATCH_228 = -4,
    VD_DIPSW_DBGVCR_ERROR_RECONFIG_DISABLED = -5,
    VD_DIPSW_DBGVCR_ERROR_228_ALREADY_SET = -6,
    VD_DIPSW_DBGVCR_ERROR_BASELINE_UNSTABLE = -7,
    VD_DIPSW_DBGVCR_ERROR_SET_CHECK_228 = -8,
    VD_DIPSW_DBGVCR_ERROR_SET_SYSTEM_DELTA = -9,
    VD_DIPSW_DBGVCR_ERROR_CORE_MIGRATED = -10,
    VD_DIPSW_DBGVCR_ERROR_UNSAFE_CORE = -11,
    VD_DIPSW_DBGVCR_ERROR_RESTORE_CP_CHANGED = -12,
    VD_DIPSW_DBGVCR_ERROR_RESTORE_CHECK_203 = -13,
    VD_DIPSW_DBGVCR_ERROR_RESTORE_CHECK_228 = -14,
    VD_DIPSW_DBGVCR_ERROR_RESTORE_DEBUG_CHANGED = -15,
    VD_DIPSW_DBGVCR_ERROR_RESTORE_SYSTEM_CHANGED = -16,
    VD_DIPSW_DBGVCR_ERROR_JOURNAL_ORIGINAL = -17,
    VD_DIPSW_DBGVCR_ERROR_JOURNAL_READ_PENDING = -18,
};

enum vd_dipsw_dbgvcr_flags {
    VD_DIPSW_DBGVCR_FLAG_BEFORE_203_VALID = UINT32_C(1) << 0,
    VD_DIPSW_DBGVCR_FLAG_BEFORE_228_VALID = UINT32_C(1) << 1,
    VD_DIPSW_DBGVCR_FLAG_BEFORE_203_MATCH = UINT32_C(1) << 2,
    VD_DIPSW_DBGVCR_FLAG_BEFORE_228_MATCH = UINT32_C(1) << 3,
    VD_DIPSW_DBGVCR_FLAG_BEFORE_203_CLEAR = UINT32_C(1) << 4,
    VD_DIPSW_DBGVCR_FLAG_BEFORE_228_CLEAR = UINT32_C(1) << 5,
    VD_DIPSW_DBGVCR_FLAG_BASELINE_STABLE = UINT32_C(1) << 6,
    VD_DIPSW_DBGVCR_FLAG_SET_228_ONE = UINT32_C(1) << 7,
    VD_DIPSW_DBGVCR_FLAG_SET_SYSTEM_EXACT = UINT32_C(1) << 8,
    VD_DIPSW_DBGVCR_FLAG_IRQ_GUARDED = UINT32_C(1) << 9,
    VD_DIPSW_DBGVCR_FLAG_CORE_MATCH = UINT32_C(1) << 10,
    VD_DIPSW_DBGVCR_FLAG_CORE_ALLOWED = UINT32_C(1) << 11,
    VD_DIPSW_DBGVCR_FLAG_READ_RETURNED = UINT32_C(1) << 12,
    VD_DIPSW_DBGVCR_FLAG_RESTORE_CP_EXACT = UINT32_C(1) << 13,
    VD_DIPSW_DBGVCR_FLAG_RESTORE_203_EXACT = UINT32_C(1) << 14,
    VD_DIPSW_DBGVCR_FLAG_RESTORE_228_EXACT = UINT32_C(1) << 15,
    VD_DIPSW_DBGVCR_FLAG_RESTORE_DEBUG_EXACT = UINT32_C(1) << 16,
    VD_DIPSW_DBGVCR_FLAG_RESTORE_SYSTEM_EXACT = UINT32_C(1) << 17,
    VD_DIPSW_DBGVCR_FLAGS_COMPLETE = UINT32_C(0x0003FFFF),
};

/*
 * Alternating records form a crash journal. READ_PENDING is durable before
 * Set(228); no file I/O occurs while bit 228 is high. If the CP14 read does
 * not return, READ_PENDING remains the newest durable state and a reboot is
 * mandatory before any further work.
 */
struct vd_dipsw_dbgvcr_record {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    uint32_t revision;
    uint32_t sequence;
    uint32_t state;
    int32_t result;
    int32_t restore_result;
    uint32_t flags;
    uint32_t before_cp_build_version;
    uint32_t before_debug;
    uint32_t before_system;
    int32_t before_check_203;
    int32_t before_check_228;
    uint32_t confirm_cp_build_version;
    uint32_t confirm_debug;
    uint32_t confirm_system;
    int32_t confirm_check_203;
    int32_t confirm_check_228;
    uint32_t after_set_system;
    int32_t after_set_check_228;
    uint32_t dbgvcr_value;
    uint32_t after_restore_cp_build_version;
    uint32_t after_restore_debug;
    uint32_t after_restore_system;
    int32_t after_restore_check_203;
    int32_t after_restore_check_228;
    uint32_t core_before;
    uint32_t core_read;
    uint32_t hazard_armed;
    uint32_t set_call_count;
    uint32_t dbgvcr_read_count;
    uint32_t clear_call_count;
    int32_t journal_error;
    uint32_t reserved[13];
};

#if defined(__cplusplus)
static_assert(sizeof(struct vd_dipsw_dbgvcr_record) ==
                  VD_DIPSW_DBGVCR_RECORD_SIZE,
              "VitaDebugger DIP/DBGVCR record ABI changed");
#else
_Static_assert(sizeof(struct vd_dipsw_dbgvcr_record) ==
                   VD_DIPSW_DBGVCR_RECORD_SIZE,
               "VitaDebugger DIP/DBGVCR record ABI changed");
#endif

static inline uint32_t vd_dipsw_dbgvcr_checksum(
    const struct vd_dipsw_dbgvcr_record* record)
{
    const uint8_t* bytes = (const uint8_t*)record;
    const size_t checksum_start =
        offsetof(struct vd_dipsw_dbgvcr_record, checksum);
    const size_t checksum_end = checksum_start + sizeof(record->checksum);
    uint32_t hash = UINT32_C(2166136261);
    size_t i;

    for(i = 0; i < sizeof(*record); ++i)
    {
        const uint8_t value =
            (i >= checksum_start && i < checksum_end) ? 0 : bytes[i];
        hash ^= value;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static inline int vd_dipsw_dbgvcr_record_valid(
    const struct vd_dipsw_dbgvcr_record* record)
{
    return record->magic == VD_DIPSW_DBGVCR_MAGIC &&
           record->version == VD_DIPSW_DBGVCR_VERSION &&
           record->size == (uint32_t)sizeof(*record) &&
           record->checksum == vd_dipsw_dbgvcr_checksum(record);
}

static inline int vd_dipsw_dbgvcr_revision_newer(uint32_t left,
                                                  uint32_t right)
{
    return (int32_t)(left - right) > 0;
}

static inline int vd_dipsw_dbgvcr_boolean(int32_t value)
{
    return value == 0 || value == 1;
}

static inline void vd_dipsw_dbgvcr_set_first_error(
    struct vd_dipsw_dbgvcr_record* record, int32_t error)
{
    if(record->result == VD_DIPSW_DBGVCR_OK)
        record->result = error;
}

static inline void vd_dipsw_dbgvcr_set_first_restore_error(
    struct vd_dipsw_dbgvcr_record* record, int32_t error)
{
    if(record->restore_result == VD_DIPSW_DBGVCR_OK)
        record->restore_result = error;
}

static inline void vd_dipsw_dbgvcr_validate_before(
    struct vd_dipsw_dbgvcr_record* record)
{
    const uint32_t word_203 =
        (record->before_debug & VD_DIPSW_RECONFIG_MASK) != 0;
    const uint32_t word_228 =
        (record->before_system & VD_DIPSW_HW_DEBUG_MASK) != 0;

    record->result = VD_DIPSW_DBGVCR_OK;
    record->restore_result = VD_DIPSW_DBGVCR_NOT_RUN;
    record->flags = 0;
    if(vd_dipsw_dbgvcr_boolean(record->before_check_203))
        record->flags |= VD_DIPSW_DBGVCR_FLAG_BEFORE_203_VALID;
    else
        vd_dipsw_dbgvcr_set_first_error(
            record, VD_DIPSW_DBGVCR_ERROR_BEFORE_CHECK_203);
    if(vd_dipsw_dbgvcr_boolean(record->before_check_228))
        record->flags |= VD_DIPSW_DBGVCR_FLAG_BEFORE_228_VALID;
    else
        vd_dipsw_dbgvcr_set_first_error(
            record, VD_DIPSW_DBGVCR_ERROR_BEFORE_CHECK_228);
    if(vd_dipsw_dbgvcr_boolean(record->before_check_203) &&
       (uint32_t)record->before_check_203 == word_203)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_BEFORE_203_MATCH;
    else
        vd_dipsw_dbgvcr_set_first_error(
            record, VD_DIPSW_DBGVCR_ERROR_BEFORE_MISMATCH_203);
    if(vd_dipsw_dbgvcr_boolean(record->before_check_228) &&
       (uint32_t)record->before_check_228 == word_228)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_BEFORE_228_MATCH;
    else
        vd_dipsw_dbgvcr_set_first_error(
            record, VD_DIPSW_DBGVCR_ERROR_BEFORE_MISMATCH_228);
    if(record->before_check_203 == 0 && word_203 == 0)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_BEFORE_203_CLEAR;
    else
        vd_dipsw_dbgvcr_set_first_error(
            record, VD_DIPSW_DBGVCR_ERROR_RECONFIG_DISABLED);
    if(record->before_check_228 == 0 && word_228 == 0)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_BEFORE_228_CLEAR;
    else
        vd_dipsw_dbgvcr_set_first_error(
            record, VD_DIPSW_DBGVCR_ERROR_228_ALREADY_SET);
}

static inline void vd_dipsw_dbgvcr_validate_confirm(
    struct vd_dipsw_dbgvcr_record* record)
{
    if(record->confirm_cp_build_version ==
           record->before_cp_build_version &&
       record->confirm_debug == record->before_debug &&
       record->confirm_system == record->before_system &&
       record->confirm_check_203 == record->before_check_203 &&
       record->confirm_check_228 == record->before_check_228)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_BASELINE_STABLE;
    else
        vd_dipsw_dbgvcr_set_first_error(
            record, VD_DIPSW_DBGVCR_ERROR_BASELINE_UNSTABLE);
}

static inline void vd_dipsw_dbgvcr_validate_after_set(
    struct vd_dipsw_dbgvcr_record* record)
{
    if(record->after_set_check_228 == 1)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_SET_228_ONE;
    else
        vd_dipsw_dbgvcr_set_first_error(
            record, VD_DIPSW_DBGVCR_ERROR_SET_CHECK_228);
    if(record->after_set_system ==
       (record->before_system | VD_DIPSW_HW_DEBUG_MASK))
        record->flags |= VD_DIPSW_DBGVCR_FLAG_SET_SYSTEM_EXACT;
    else
        vd_dipsw_dbgvcr_set_first_error(
            record, VD_DIPSW_DBGVCR_ERROR_SET_SYSTEM_DELTA);
}

static inline void vd_dipsw_dbgvcr_validate_after_restore(
    struct vd_dipsw_dbgvcr_record* record)
{
    record->restore_result = VD_DIPSW_DBGVCR_OK;
    if(record->after_restore_cp_build_version ==
       record->before_cp_build_version)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_RESTORE_CP_EXACT;
    else
        vd_dipsw_dbgvcr_set_first_restore_error(
            record, VD_DIPSW_DBGVCR_ERROR_RESTORE_CP_CHANGED);
    if(vd_dipsw_dbgvcr_boolean(record->after_restore_check_203) &&
       record->after_restore_check_203 == record->before_check_203)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_RESTORE_203_EXACT;
    else
        vd_dipsw_dbgvcr_set_first_restore_error(
            record, VD_DIPSW_DBGVCR_ERROR_RESTORE_CHECK_203);
    if(vd_dipsw_dbgvcr_boolean(record->after_restore_check_228) &&
       record->after_restore_check_228 == record->before_check_228)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_RESTORE_228_EXACT;
    else
        vd_dipsw_dbgvcr_set_first_restore_error(
            record, VD_DIPSW_DBGVCR_ERROR_RESTORE_CHECK_228);
    if(record->after_restore_debug == record->before_debug)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_RESTORE_DEBUG_EXACT;
    else
        vd_dipsw_dbgvcr_set_first_restore_error(
            record, VD_DIPSW_DBGVCR_ERROR_RESTORE_DEBUG_CHANGED);
    if(record->after_restore_system == record->before_system)
        record->flags |= VD_DIPSW_DBGVCR_FLAG_RESTORE_SYSTEM_EXACT;
    else
        vd_dipsw_dbgvcr_set_first_restore_error(
            record, VD_DIPSW_DBGVCR_ERROR_RESTORE_SYSTEM_CHANGED);
}

#endif

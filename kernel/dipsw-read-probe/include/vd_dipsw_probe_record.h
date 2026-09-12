#ifndef VD_DIPSW_PROBE_RECORD_H
#define VD_DIPSW_PROBE_RECORD_H

#include <stddef.h>
#include <stdint.h>

#define VD_DIPSW_PROBE_MAGIC UINT32_C(0x56444450)
#define VD_DIPSW_PROBE_VERSION UINT32_C(1)
#define VD_DIPSW_PROBE_RECORD_SIZE UINT32_C(128)

#define VD_DIPSW_PROBE_RECORD_A_PATH \
    "ux0:data/VitaDebugger/dipsw-read-v1-a.bin"
#define VD_DIPSW_PROBE_RECORD_B_PATH \
    "ux0:data/VitaDebugger/dipsw-read-v1-b.bin"

#define VD_DIPSW_RECONFIG_BIT UINT32_C(203)
#define VD_DIPSW_HW_DEBUG_BIT UINT32_C(228)
#define VD_DIPSW_DEBUG_INFO_INDEX UINT32_C(6)
#define VD_DIPSW_SYSTEM_INFO_INDEX UINT32_C(7)
#define VD_DIPSW_RECONFIG_MASK UINT32_C(0x00000800)
#define VD_DIPSW_HW_DEBUG_MASK UINT32_C(0x00000010)

#if defined(__cplusplus)
static_assert(VD_DIPSW_RECONFIG_BIT / 32u == VD_DIPSW_DEBUG_INFO_INDEX,
              "DIP 203 word mapping changed");
static_assert(VD_DIPSW_RECONFIG_BIT % 32u == 11u,
              "DIP 203 bit mapping changed");
static_assert((UINT32_C(1) << (VD_DIPSW_RECONFIG_BIT % 32u)) ==
                  VD_DIPSW_RECONFIG_MASK,
              "DIP 203 mask changed");
static_assert(VD_DIPSW_HW_DEBUG_BIT / 32u == VD_DIPSW_SYSTEM_INFO_INDEX,
              "DIP 228 word mapping changed");
static_assert(VD_DIPSW_HW_DEBUG_BIT % 32u == 4u,
              "DIP 228 bit mapping changed");
static_assert((UINT32_C(1) << (VD_DIPSW_HW_DEBUG_BIT % 32u)) ==
                  VD_DIPSW_HW_DEBUG_MASK,
              "DIP 228 mask changed");
#else
_Static_assert(VD_DIPSW_RECONFIG_BIT / 32u == VD_DIPSW_DEBUG_INFO_INDEX,
               "DIP 203 word mapping changed");
_Static_assert(VD_DIPSW_RECONFIG_BIT % 32u == 11u,
               "DIP 203 bit mapping changed");
_Static_assert((UINT32_C(1) << (VD_DIPSW_RECONFIG_BIT % 32u)) ==
                   VD_DIPSW_RECONFIG_MASK,
               "DIP 203 mask changed");
_Static_assert(VD_DIPSW_HW_DEBUG_BIT / 32u == VD_DIPSW_SYSTEM_INFO_INDEX,
               "DIP 228 word mapping changed");
_Static_assert(VD_DIPSW_HW_DEBUG_BIT % 32u == 4u,
               "DIP 228 bit mapping changed");
_Static_assert((UINT32_C(1) << (VD_DIPSW_HW_DEBUG_BIT % 32u)) ==
                   VD_DIPSW_HW_DEBUG_MASK,
               "DIP 228 mask changed");
#endif

enum vd_dipsw_probe_state {
    VD_DIPSW_PROBE_STATE_ATTEMPTED = 1,
    VD_DIPSW_PROBE_STATE_KERNEL_ENTERED = 2,
    VD_DIPSW_PROBE_STATE_COMPLETE = 3,
};

enum vd_dipsw_probe_result {
    VD_DIPSW_PROBE_OK = 0,
    VD_DIPSW_PROBE_NOT_RUN = -100,
    VD_DIPSW_PROBE_ERROR_CHECK_203 = -1,
    VD_DIPSW_PROBE_ERROR_CHECK_228 = -2,
    VD_DIPSW_PROBE_ERROR_MISMATCH_203 = -3,
    VD_DIPSW_PROBE_ERROR_MISMATCH_228 = -4,
};

enum vd_dipsw_probe_flags {
    VD_DIPSW_PROBE_FLAG_CHECK_203_VALID = UINT32_C(1) << 0,
    VD_DIPSW_PROBE_FLAG_CHECK_228_VALID = UINT32_C(1) << 1,
    VD_DIPSW_PROBE_FLAG_CHECK_203_MATCH = UINT32_C(1) << 2,
    VD_DIPSW_PROBE_FLAG_CHECK_228_MATCH = UINT32_C(1) << 3,
    VD_DIPSW_PROBE_FLAGS_COMPLETE =
        VD_DIPSW_PROBE_FLAG_CHECK_203_VALID |
        VD_DIPSW_PROBE_FLAG_CHECK_228_VALID |
        VD_DIPSW_PROBE_FLAG_CHECK_203_MATCH |
        VD_DIPSW_PROBE_FLAG_CHECK_228_MATCH,
};

/*
 * Two alternating copies form a small lifecycle journal. checksum is FNV-1a
 * over the complete record with the checksum field treated as zero.
 */
struct vd_dipsw_probe_record {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    uint32_t revision;
    uint32_t sequence;
    uint32_t state;
    int32_t result;
    uint32_t flags;
    uint32_t cp_build_version;
    uint32_t debug_control;
    uint32_t system_control;
    int32_t check_203;
    int32_t check_228;
    uint32_t core_id;
    uint32_t reserved[17];
};

#if defined(__cplusplus)
static_assert(sizeof(struct vd_dipsw_probe_record) ==
                  VD_DIPSW_PROBE_RECORD_SIZE,
              "VitaDebugger DIP-switch probe record ABI changed");
#else
_Static_assert(sizeof(struct vd_dipsw_probe_record) ==
                   VD_DIPSW_PROBE_RECORD_SIZE,
               "VitaDebugger DIP-switch probe record ABI changed");
#endif

static inline uint32_t vd_dipsw_probe_checksum(
    const struct vd_dipsw_probe_record* record)
{
    const uint8_t* bytes = (const uint8_t*)record;
    const size_t checksum_start =
        offsetof(struct vd_dipsw_probe_record, checksum);
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

static inline int vd_dipsw_probe_record_valid(
    const struct vd_dipsw_probe_record* record)
{
    return record->magic == VD_DIPSW_PROBE_MAGIC &&
           record->version == VD_DIPSW_PROBE_VERSION &&
           record->size == (uint32_t)sizeof(*record) &&
           record->checksum == vd_dipsw_probe_checksum(record);
}

static inline int vd_dipsw_probe_revision_newer(uint32_t left,
                                                 uint32_t right)
{
    return (int32_t)(left - right) > 0;
}

static inline int vd_dipsw_probe_check_boolean(int32_t value)
{
    return value == 0 || value == 1;
}

static inline void vd_dipsw_probe_validate_readback(
    struct vd_dipsw_probe_record* record)
{
    const uint32_t bit_203_from_word =
        (record->debug_control & VD_DIPSW_RECONFIG_MASK) != 0;
    const uint32_t bit_228_from_word =
        (record->system_control & VD_DIPSW_HW_DEBUG_MASK) != 0;

    record->result = VD_DIPSW_PROBE_OK;
    record->flags = 0;

    if(vd_dipsw_probe_check_boolean(record->check_203))
        record->flags |= VD_DIPSW_PROBE_FLAG_CHECK_203_VALID;
    else
        record->result = VD_DIPSW_PROBE_ERROR_CHECK_203;

    if(vd_dipsw_probe_check_boolean(record->check_228))
        record->flags |= VD_DIPSW_PROBE_FLAG_CHECK_228_VALID;
    else if(record->result == VD_DIPSW_PROBE_OK)
        record->result = VD_DIPSW_PROBE_ERROR_CHECK_228;

    if(vd_dipsw_probe_check_boolean(record->check_203) &&
       (uint32_t)record->check_203 == bit_203_from_word)
        record->flags |= VD_DIPSW_PROBE_FLAG_CHECK_203_MATCH;
    else if(record->result == VD_DIPSW_PROBE_OK)
        record->result = VD_DIPSW_PROBE_ERROR_MISMATCH_203;

    if(vd_dipsw_probe_check_boolean(record->check_228) &&
       (uint32_t)record->check_228 == bit_228_from_word)
        record->flags |= VD_DIPSW_PROBE_FLAG_CHECK_228_MATCH;
    else if(record->result == VD_DIPSW_PROBE_OK)
        record->result = VD_DIPSW_PROBE_ERROR_MISMATCH_228;
}

#endif

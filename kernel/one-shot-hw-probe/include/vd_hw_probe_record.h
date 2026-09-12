#ifndef VD_HW_PROBE_RECORD_H
#define VD_HW_PROBE_RECORD_H

#include <stddef.h>
#include <stdint.h>

#define VD_HW_PROBE_MAGIC UINT32_C(0x56444850)
#define VD_HW_PROBE_VERSION UINT32_C(1)
#define VD_HW_PROBE_RECORD_SIZE UINT32_C(132)

#define VD_HW_PROBE_RESULT_PATH \
    "ux0:data/VitaDebugger/hw-disabled-roundtrip-v1.bin"
#define VD_HW_PROBE_TEMP_PATH \
    "ux0:data/VitaDebugger/hw-disabled-roundtrip-v1.tmp"

enum vd_hw_probe_stage {
    VD_HW_PROBE_STAGE_ENTERED = 1,
    VD_HW_PROBE_STAGE_SNAPSHOT = 2,
    VD_HW_PROBE_STAGE_TEST_WRITTEN = 3,
    VD_HW_PROBE_STAGE_RESTORED = 4,
    VD_HW_PROBE_STAGE_COMPLETE = 5,
};

enum vd_hw_probe_result {
    VD_HW_PROBE_OK = 0,
    VD_HW_PROBE_NOT_RUN = -100,
    VD_HW_PROBE_ERROR_WRONG_CORE = -1,
    VD_HW_PROBE_ERROR_WRONG_CPU = -2,
    VD_HW_PROBE_ERROR_NO_COMPARATORS = -3,
    VD_HW_PROBE_ERROR_DEBUG_ACTIVE = -4,
    VD_HW_PROBE_ERROR_COMPARATOR_BUSY = -5,
    VD_HW_PROBE_ERROR_CODEC = -6,
    VD_HW_PROBE_ERROR_BREAK_CONTROL_READBACK = -7,
    VD_HW_PROBE_ERROR_BREAK_VALUE_READBACK = -8,
    VD_HW_PROBE_ERROR_WATCH_CONTROL_READBACK = -9,
    VD_HW_PROBE_ERROR_WATCH_VALUE_READBACK = -10,
    VD_HW_PROBE_ERROR_BREAK_RESTORE = -11,
    VD_HW_PROBE_ERROR_WATCH_RESTORE = -12,
    VD_HW_PROBE_ERROR_VECTOR_CATCH_ACTIVE = -13,
    VD_HW_PROBE_ERROR_JOURNAL = -14,
};

enum vd_hw_probe_flags {
    VD_HW_PROBE_FLAG_SNAPSHOT_VALID = UINT32_C(1) << 0,
    VD_HW_PROBE_FLAG_BREAK_CONTROL_WRITTEN = UINT32_C(1) << 1,
    VD_HW_PROBE_FLAG_BREAK_VALUE_WRITTEN = UINT32_C(1) << 2,
    VD_HW_PROBE_FLAG_WATCH_CONTROL_WRITTEN = UINT32_C(1) << 3,
    VD_HW_PROBE_FLAG_WATCH_VALUE_WRITTEN = UINT32_C(1) << 4,
    VD_HW_PROBE_FLAG_RESTORE_ATTEMPTED = UINT32_C(1) << 5,
    VD_HW_PROBE_FLAG_RESTORE_VERIFIED = UINT32_C(1) << 6,
    VD_HW_PROBE_FLAG_AUTO_UNLOAD_SAFE = UINT32_C(1) << 7,
};

/*
 * Fixed-width, versioned record shared by the kernel probe and loader UI.
 * checksum is FNV-1a over the complete record with checksum set to zero.
 */
struct vd_hw_probe_record {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    int32_t result;
    uint32_t stage;
    uint32_t flags;
    uint32_t core_id;
    uint32_t raw_midr;
    uint32_t raw_didr;
    uint32_t raw_dscr;
    uint32_t raw_dbgvcr;
    uint32_t breakpoint_count;
    uint32_t watchpoint_count;
    uint32_t context_breakpoint_count;
    uint32_t original_bvr0;
    uint32_t original_bcr0;
    uint32_t original_wvr0;
    uint32_t original_wcr0;
    uint32_t test_bvr0;
    uint32_t test_bcr0;
    uint32_t test_wvr0;
    uint32_t test_wcr0;
    uint32_t readback_bvr0;
    uint32_t readback_bcr0;
    uint32_t readback_wvr0;
    uint32_t readback_wcr0;
    uint32_t restored_bvr0;
    uint32_t restored_bcr0;
    uint32_t restored_wvr0;
    uint32_t restored_wcr0;
    int32_t initial_journal_result;
    /* Zero is predeclared before the final atomic commit. */
    int32_t final_journal_result;
};

#if defined(__cplusplus)
static_assert(sizeof(struct vd_hw_probe_record) == VD_HW_PROBE_RECORD_SIZE,
              "VitaDebugger hardware probe record ABI changed");
#else
_Static_assert(sizeof(struct vd_hw_probe_record) == VD_HW_PROBE_RECORD_SIZE,
               "VitaDebugger hardware probe record ABI changed");
#endif

static inline uint32_t vd_hw_probe_record_checksum(
    const struct vd_hw_probe_record* record)
{
    const uint8_t* bytes = (const uint8_t*)record;
    const size_t checksum_start =
        offsetof(struct vd_hw_probe_record, checksum);
    const size_t checksum_end = checksum_start + sizeof(record->checksum);
    uint32_t hash = UINT32_C(2166136261);
    for(size_t i = 0; i < sizeof(*record); ++i)
    {
        uint8_t value = (i >= checksum_start && i < checksum_end) ? 0 :
            bytes[i];
        hash ^= value;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

#endif

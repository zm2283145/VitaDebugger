#ifndef VD_READ_LADDER_RECORD_H
#define VD_READ_LADDER_RECORD_H

#include <stddef.h>
#include <stdint.h>

#define VD_READ_LADDER_MAGIC UINT32_C(0x56444c52)
#define VD_READ_LADDER_VERSION UINT32_C(1)
#define VD_READ_LADDER_RECORD_SIZE UINT32_C(64)

#define VD_READ_LADDER_RECORD_A_PATH \
    "ux0:data/VitaDebugger/hw-read-ladder-v1-a.bin"
#define VD_READ_LADDER_RECORD_B_PATH \
    "ux0:data/VitaDebugger/hw-read-ladder-v1-b.bin"

enum vd_read_ladder_step {
    VD_READ_STEP_NONE = 0,
    VD_READ_STEP_KERNEL_LIFECYCLE = 1,
    VD_READ_STEP_CPU_ID = 2,
    VD_READ_STEP_MIDR = 3,
    VD_READ_STEP_DIDR = 4,
    VD_READ_STEP_DSCR = 5,
    VD_READ_STEP_DBGVCR = 6,
    VD_READ_STEP_BCR0 = 7,
    VD_READ_STEP_BVR0 = 8,
    VD_READ_STEP_WCR0 = 9,
    VD_READ_STEP_WVR0 = 10,
    VD_READ_STEP_FIRST = VD_READ_STEP_KERNEL_LIFECYCLE,
    VD_READ_STEP_LAST = VD_READ_STEP_WVR0,
};

enum vd_read_ladder_state {
    VD_READ_STATE_ATTEMPTED = 1,
    VD_READ_STATE_KERNEL_ENTERED = 2,
    VD_READ_STATE_COMPLETE = 3,
};

enum vd_read_ladder_result {
    VD_READ_RESULT_OK = 0,
    VD_READ_RESULT_NOT_RUN = -100,
    VD_READ_RESULT_BAD_RECORD = -1,
    VD_READ_RESULT_BAD_STEP = -2,
    VD_READ_RESULT_PREOP_JOURNAL = -3,
    VD_READ_RESULT_POSTOP_JOURNAL = -4,
    VD_READ_RESULT_CORE_MIGRATED = -5,
    VD_READ_RESULT_UNSAFE_CORE = -6,
};

enum vd_read_ladder_flags {
    VD_READ_FLAG_VALUE_VALID = UINT32_C(1) << 0,
    VD_READ_FLAG_CP14 = UINT32_C(1) << 1,
    VD_READ_FLAG_GENERAL_REGS_ONLY = UINT32_C(1) << 2,
    VD_READ_FLAG_IRQ_GUARDED = UINT32_C(1) << 3,
    VD_READ_FLAG_CORE_MATCH = UINT32_C(1) << 4,
};

/*
 * Two alternating copies of this record form a tiny crash journal. The valid
 * copy with the newest revision wins. checksum is FNV-1a over the complete
 * record with the checksum field treated as zero.
 */
struct vd_read_ladder_record {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    uint32_t revision;
    uint32_t sequence;
    uint32_t step;
    uint32_t state;
    int32_t result;
    uint32_t value;
    uint32_t flags;
    uint32_t core_id;
    uint32_t reserved[4];
};

#if defined(__cplusplus)
static_assert(sizeof(struct vd_read_ladder_record) ==
                  VD_READ_LADDER_RECORD_SIZE,
              "VitaDebugger read ladder record ABI changed");
#else
_Static_assert(sizeof(struct vd_read_ladder_record) ==
                   VD_READ_LADDER_RECORD_SIZE,
               "VitaDebugger read ladder record ABI changed");
#endif

static inline uint32_t vd_read_ladder_checksum(
    const struct vd_read_ladder_record* record)
{
    const uint8_t* bytes = (const uint8_t*)record;
    const size_t checksum_start =
        offsetof(struct vd_read_ladder_record, checksum);
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

static inline int vd_read_ladder_record_valid(
    const struct vd_read_ladder_record* record)
{
    return record->magic == VD_READ_LADDER_MAGIC &&
           record->version == VD_READ_LADDER_VERSION &&
           record->size == (uint32_t)sizeof(*record) &&
           record->checksum == vd_read_ladder_checksum(record);
}

static inline int vd_read_ladder_revision_newer(uint32_t left,
                                                 uint32_t right)
{
    return (int32_t)(left - right) > 0;
}

#endif

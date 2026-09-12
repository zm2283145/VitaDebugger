#ifndef VD_ARMV7_DEBUG_CODEC_H
#define VD_ARMV7_DEBUG_CODEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cortex-A9 has two context-aware breakpoint register pairs. VitaDebugger
 * reserves BRP5 as the context comparator and links address comparators to it.
 */
#define VD_ARMV7_CONTEXT_BRP_INDEX 5u

enum vd_armv7_debug_result {
    VD_ARMV7_DEBUG_OK = 0,
    VD_ARMV7_DEBUG_INVALID_ARGUMENT = -1,
    VD_ARMV7_DEBUG_INVALID_LENGTH = -2,
    VD_ARMV7_DEBUG_INVALID_ALIGNMENT = -3,
    VD_ARMV7_DEBUG_CROSSES_WORD = -4,
    VD_ARMV7_DEBUG_INVALID_ACCESS = -5,
};

enum vd_armv7_watch_access {
    VD_ARMV7_WATCH_READ = 1,
    VD_ARMV7_WATCH_WRITE = 2,
    VD_ARMV7_WATCH_ACCESS = 3,
};

struct vd_armv7_debug_encoding {
    uint32_t value;
    uint32_t control;
};

/*
 * Encode a user-mode instruction breakpoint linked to context BRP5.
 * Instruction length must be 2 (Thumb) or 4 (ARM), and the selected bytes
 * must form one of the Cortex-A9's supported, naturally aligned BAS patterns.
 */
int vd_armv7_encode_linked_exec(
    uint32_t address,
    uint32_t length,
    struct vd_armv7_debug_encoding* encoding);

/* Encode BRP5 as an enabled linked Context ID comparator. */
int vd_armv7_encode_linked_context(
    uint32_t context_id,
    struct vd_armv7_debug_encoding* encoding);

/*
 * Encode an initial user-mode watchpoint linked to context BRP5. Supported
 * lengths are 1, 2, and 4 bytes, wholly contained in one aligned word.
 */
int vd_armv7_encode_linked_watch(
    uint32_t address,
    uint32_t length,
    enum vd_armv7_watch_access access,
    struct vd_armv7_debug_encoding* encoding);

/* Extract the ARM short-descriptor Fault Status code: FSR bits [10,3:0]. */
uint32_t vd_armv7_fsr_status(uint32_t fsr);

/* Return nonzero only for architectural Debug event status 0x02. */
int vd_armv7_fsr_is_debug_event(uint32_t fsr);

/*
 * Test whether address is in [range_start, range_start + range_length).
 * Subtraction is used after the lower-bound check so a range ending beyond
 * UINT32_MAX cannot wrap around and match low addresses.
 */
int vd_armv7_range_contains(
    uint32_t range_start,
    uint32_t range_length,
    uint32_t address);

#ifdef __cplusplus
}
#endif

#endif

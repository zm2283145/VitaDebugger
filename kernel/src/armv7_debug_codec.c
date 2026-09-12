#include "vd_armv7_debug_codec.h"

#define VD_ARMV7_WORD_MASK UINT32_C(0x3)

#define VD_ARMV7_CTRL_ENABLE UINT32_C(0x1)
#define VD_ARMV7_CTRL_USER (UINT32_C(0x2) << 1)
#define VD_ARMV7_CTRL_ANY (UINT32_C(0x3) << 1)
#define VD_ARMV7_CTRL_BAS_SHIFT 5u
#define VD_ARMV7_CTRL_LBN_SHIFT 16u

#define VD_ARMV7_BCR_M_SHIFT 20u
#define VD_ARMV7_BCR_M_LINKED_ADDRESS UINT32_C(0x1)
#define VD_ARMV7_BCR_M_LINKED_CONTEXT UINT32_C(0x3)

#define VD_ARMV7_WCR_LSC_SHIFT 3u
#define VD_ARMV7_WCR_LINK (UINT32_C(0x1) << 20)

#define VD_ARMV7_FSR_LOW_STATUS_MASK UINT32_C(0xf)
#define VD_ARMV7_FSR_HIGH_STATUS_BIT UINT32_C(0x400)
#define VD_ARMV7_FSR_HIGH_STATUS_SHIFT 6u
#define VD_ARMV7_FSR_DEBUG_EVENT UINT32_C(0x2)

static uint32_t vd_armv7_contiguous_bas(uint32_t offset, uint32_t length)
{
    return ((UINT32_C(1) << length) - UINT32_C(1)) << offset;
}

int vd_armv7_encode_linked_exec(
    uint32_t address,
    uint32_t length,
    struct vd_armv7_debug_encoding* encoding)
{
    uint32_t offset;
    uint32_t bas;

    if(encoding == 0)
        return VD_ARMV7_DEBUG_INVALID_ARGUMENT;
    encoding->value = 0;
    encoding->control = 0;
    if(length != UINT32_C(2) && length != UINT32_C(4))
        return VD_ARMV7_DEBUG_INVALID_LENGTH;

    offset = address & VD_ARMV7_WORD_MASK;
    if((address & (length - UINT32_C(1))) != 0)
        return VD_ARMV7_DEBUG_INVALID_ALIGNMENT;
    if(length > UINT32_C(4) - offset)
        return VD_ARMV7_DEBUG_CROSSES_WORD;

    bas = vd_armv7_contiguous_bas(offset, length);
    /* Cortex-A9 instruction BAS accepts only 0011, 1100, or 1111. */
    if(bas != UINT32_C(0x3) && bas != UINT32_C(0xc) &&
       bas != UINT32_C(0xf))
        return VD_ARMV7_DEBUG_INVALID_ALIGNMENT;

    encoding->value = address & ~VD_ARMV7_WORD_MASK;
    encoding->control =
        (VD_ARMV7_BCR_M_LINKED_ADDRESS << VD_ARMV7_BCR_M_SHIFT) |
        (VD_ARMV7_CONTEXT_BRP_INDEX << VD_ARMV7_CTRL_LBN_SHIFT) |
        (bas << VD_ARMV7_CTRL_BAS_SHIFT) |
        VD_ARMV7_CTRL_USER |
        VD_ARMV7_CTRL_ENABLE;
    return VD_ARMV7_DEBUG_OK;
}

int vd_armv7_encode_linked_context(
    uint32_t context_id,
    struct vd_armv7_debug_encoding* encoding)
{
    if(encoding == 0)
        return VD_ARMV7_DEBUG_INVALID_ARGUMENT;

    encoding->value = 0;
    encoding->control = 0;
    encoding->value = context_id;
    encoding->control =
        (VD_ARMV7_BCR_M_LINKED_CONTEXT << VD_ARMV7_BCR_M_SHIFT) |
        (UINT32_C(0xf) << VD_ARMV7_CTRL_BAS_SHIFT) |
        VD_ARMV7_CTRL_ANY |
        VD_ARMV7_CTRL_ENABLE;
    return VD_ARMV7_DEBUG_OK;
}

int vd_armv7_encode_linked_watch(
    uint32_t address,
    uint32_t length,
    enum vd_armv7_watch_access access,
    struct vd_armv7_debug_encoding* encoding)
{
    uint32_t offset;
    uint32_t bas;

    if(encoding == 0)
        return VD_ARMV7_DEBUG_INVALID_ARGUMENT;
    encoding->value = 0;
    encoding->control = 0;
    if(length != UINT32_C(1) && length != UINT32_C(2) &&
       length != UINT32_C(4))
        return VD_ARMV7_DEBUG_INVALID_LENGTH;
    if(access != VD_ARMV7_WATCH_READ &&
       access != VD_ARMV7_WATCH_WRITE &&
       access != VD_ARMV7_WATCH_ACCESS)
        return VD_ARMV7_DEBUG_INVALID_ACCESS;

    offset = address & VD_ARMV7_WORD_MASK;
    if(length > UINT32_C(4) - offset)
        return VD_ARMV7_DEBUG_CROSSES_WORD;

    bas = vd_armv7_contiguous_bas(offset, length);
    encoding->value = address & ~VD_ARMV7_WORD_MASK;
    encoding->control =
        VD_ARMV7_WCR_LINK |
        (VD_ARMV7_CONTEXT_BRP_INDEX << VD_ARMV7_CTRL_LBN_SHIFT) |
        (bas << VD_ARMV7_CTRL_BAS_SHIFT) |
        ((uint32_t)access << VD_ARMV7_WCR_LSC_SHIFT) |
        VD_ARMV7_CTRL_USER |
        VD_ARMV7_CTRL_ENABLE;
    return VD_ARMV7_DEBUG_OK;
}

uint32_t vd_armv7_fsr_status(uint32_t fsr)
{
    return ((fsr & VD_ARMV7_FSR_HIGH_STATUS_BIT) >>
            VD_ARMV7_FSR_HIGH_STATUS_SHIFT) |
           (fsr & VD_ARMV7_FSR_LOW_STATUS_MASK);
}

int vd_armv7_fsr_is_debug_event(uint32_t fsr)
{
    return vd_armv7_fsr_status(fsr) == VD_ARMV7_FSR_DEBUG_EVENT;
}

int vd_armv7_range_contains(
    uint32_t range_start,
    uint32_t range_length,
    uint32_t address)
{
    if(range_length == 0 || address < range_start)
        return 0;
    return address - range_start < range_length;
}

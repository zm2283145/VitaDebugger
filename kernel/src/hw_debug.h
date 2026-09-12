#pragma once

#include <stdint.h>
#include <psp2/types.h>

#include "vitadebug_kernel.h"

#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG

int vdHwDebugStart(void);
int vdHwDebugStop(void);
int vdHwDebugReady(void);
void vdHwDebugWatchdog(uint64_t now_us);
int vdHwDebugRecover(SceUID caller_pid, unsigned int token);

int vdHwDebugAcquire(
    SceUID caller_pid,
    uint32_t context_id,
    unsigned int lease_ms,
    struct vd_kernel_hw_session_result* result);
int vdHwDebugRenew(
    SceUID caller_pid,
    uint32_t context_id,
    unsigned int token,
    unsigned int lease_ms);
int vdHwDebugUpdate(
    SceUID caller_pid,
    uint32_t context_id,
    unsigned int token,
    const struct vd_kernel_hw_point_request* request);
int vdHwDebugRelease(SceUID caller_pid, unsigned int token);

#endif

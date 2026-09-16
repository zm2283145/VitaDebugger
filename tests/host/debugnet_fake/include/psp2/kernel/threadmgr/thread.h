#pragma once

#include <psp2/types.h>

#define SCE_KERNEL_THREAD_EVENT_TYPE_START 0x00000004
#define SCE_KERNEL_THREAD_EVENT_TYPE_EXIT 0x00000008

typedef int (*SceKernelThreadEntry)(SceSize args, void* argp);
typedef int (*SceKernelThreadEventHandler)(SceInt32 type, SceUID thread_id,
                                           SceInt32 arg, void* common);

SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry,
                             int priority, SceSize stack_size,
                             unsigned int attributes, int cpu_affinity,
                             const void* option);
int sceKernelStartThread(SceUID thread, SceSize args, void* argp);
int sceKernelWaitThreadEnd(SceUID thread, int* status, SceUInt* timeout);
int sceKernelDeleteThread(SceUID thread);
SceUID sceKernelGetThreadId(void);
int sceKernelDelayThread(SceUInt delay_us);
SceInt64 sceKernelGetSystemTimeWide(void);
SceUID sceKernelRegisterThreadEventHandler(
    const char* name, SceUID thread, SceInt32 event_mask,
    SceKernelThreadEventHandler handler, void* common);
SceUID sceKernelUnregisterThreadEventHandler(SceUID event);

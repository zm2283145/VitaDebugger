#pragma once

#include <psp2/types.h>

SceUID sceKernelCreateSema(const char* name, SceUInt attr, int initial,
                           int maximum, void* option);
int sceKernelDeleteSema(SceUID semaphore);
int sceKernelSignalSema(SceUID semaphore, int signal);
int sceKernelWaitSema(SceUID semaphore, int signal, SceUInt* timeout);

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/types.h>
#include <stdint.h>

#include "control.h"

#define UVDB_ASLR_WORKER_STACK (16u * 1024u)
#define UVDB_ASLR_STOP_TIMEOUT_US UINT32_C(2000000)

static struct uvdb_aslr_fixture_control* fixture_control;
static SceUID fixture_worker = -1;

/*
 * This function is deliberately global, non-inlined, and retained.  Its
 * source location is the user-SUPRX half of the live ASLR symbol gate.
 */
__attribute__((noinline, noclone, used, visibility("default")))
uint32_t uvdb_aslr_suprx_breakpoint(uint32_t sequence)
{
    uint32_t value = sequence ^ UVDB_ASLR_SUPRX_RESULT_XOR;
    __asm__ volatile("" : "+r"(value) :: "memory");
    return value;
}

static int fixture_worker_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;

    struct uvdb_aslr_fixture_control* control = fixture_control;
    if(!control)
        return -1;

    __atomic_store_n(&control->ready, UVDB_ASLR_FIXTURE_READY_MAGIC,
                     __ATOMIC_RELEASE);
    while(!__atomic_load_n(&control->shutdown, __ATOMIC_ACQUIRE))
    {
        uint32_t request = __atomic_load_n(&control->request,
                                            __ATOMIC_ACQUIRE);
        uint32_t acknowledged =
            __atomic_load_n(&control->acknowledged, __ATOMIC_ACQUIRE);
        if(request != 0 && request != acknowledged)
        {
            uint32_t result = uvdb_aslr_suprx_breakpoint(request);
            __atomic_store_n(&control->result, result, __ATOMIC_RELEASE);
            __atomic_store_n(&control->acknowledged, request,
                             __ATOMIC_RELEASE);
        }
        sceKernelDelayThread(1000);
    }
    return 0;
}

int module_start(SceSize args, void* argp)
{
    if(args != sizeof(struct uvdb_aslr_fixture_args) || !argp ||
       fixture_worker >= 0)
        return SCE_KERNEL_START_FAILED;

    const struct uvdb_aslr_fixture_args* input = argp;
    struct uvdb_aslr_fixture_control* control = input->control;
    if(input->abi_version != UVDB_ASLR_FIXTURE_ABI_VERSION ||
       input->size != sizeof(*input) || !control ||
       ((uintptr_t)control & (sizeof(uint32_t) - 1u)) != 0 ||
       control->abi_version != UVDB_ASLR_FIXTURE_ABI_VERSION ||
       control->size != sizeof(*control))
        return SCE_KERNEL_START_FAILED;

    fixture_control = control;
    __atomic_store_n(&control->ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&control->acknowledged, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&control->result, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&control->shutdown, 0, __ATOMIC_RELEASE);

    SceUID worker = sceKernelCreateThread(
        "UVDB ASLR fixture", fixture_worker_main, 0x10000100,
        UVDB_ASLR_WORKER_STACK, 0,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, NULL);
    if(worker < 0)
    {
        __atomic_store_n(&control->ready, UVDB_ASLR_FIXTURE_FAILED_MAGIC,
                         __ATOMIC_RELEASE);
        fixture_control = NULL;
        return SCE_KERNEL_START_FAILED;
    }

    fixture_worker = worker;
    if(sceKernelStartThread(worker, 0, NULL) < 0)
    {
        sceKernelDeleteThread(worker);
        fixture_worker = -1;
        __atomic_store_n(&control->ready, UVDB_ASLR_FIXTURE_FAILED_MAGIC,
                         __ATOMIC_RELEASE);
        fixture_control = NULL;
        return SCE_KERNEL_START_FAILED;
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void* argp)
{
    (void)args;
    (void)argp;

    struct uvdb_aslr_fixture_control* control = fixture_control;
    if(control)
        __atomic_store_n(&control->shutdown, 1, __ATOMIC_RELEASE);

    if(fixture_worker >= 0)
    {
        int status = -1;
        SceUInt timeout = UVDB_ASLR_STOP_TIMEOUT_US;
        if(sceKernelWaitThreadEnd(fixture_worker, &status, &timeout) < 0 ||
           sceKernelDeleteThread(fixture_worker) < 0)
            return SCE_KERNEL_STOP_FAIL;
        fixture_worker = -1;
    }
    fixture_control = NULL;
    return SCE_KERNEL_STOP_SUCCESS;
}

int _start(SceSize args, void* argp)
    __attribute__((weak, alias("module_start")));

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/threadmgr/debugger.h>
#include <psp2kern/kernel/threadmgr/misc.h>

#include "vitadebug_kernel.h"

int _start(SceSize args, void* argp)
    __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    return SCE_KERNEL_STOP_SUCCESS;
}

int vdKernelGetStatus(struct vd_kernel_status* status)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);

    const struct vd_kernel_status kernel_status = {
        .abi_version = VD_KERNEL_ABI_VERSION,
        .capabilities = VD_KERNEL_CAP_THREAD_LIST,
        .max_threads = VD_KERNEL_MAX_THREADS,
        .reserved = 0,
    };
    int result = ksceKernelMemcpyKernelToUser(status, &kernel_status,
                                               sizeof(kernel_status));

    EXIT_SYSCALL(syscall_state);
    return result;
}

int vdKernelGetThreadList(SceUID* ids, int capacity, int* copied_count,
                          int* total_count)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);

    int result = -1;
    if(capacity < 0 || capacity > VD_KERNEL_MAX_THREADS ||
       !copied_count || !total_count || (capacity > 0 && !ids))
        goto finish;

    SceUID kernel_ids[VD_KERNEL_MAX_THREADS];
    SceUID user_ids[VD_KERNEL_MAX_THREADS];
    int kernel_copied = 0;
    SceUID caller_pid = ksceKernelGetProcessId();
    int total = ksceKernelGetThreadIdList(caller_pid,
                                          capacity ? kernel_ids : NULL,
                                          capacity,
                                          &kernel_copied);
    if(total < 0)
    {
        result = total;
        goto finish;
    }

    int user_copied = 0;
    for(int i = 0; i < kernel_copied; ++i)
    {
        SceUID user_id = ksceKernelGetUserThreadId(kernel_ids[i]);
        if(user_id >= 0)
            user_ids[user_copied++] = user_id;
    }

    if(user_copied > 0)
    {
        result = ksceKernelMemcpyKernelToUser(ids, user_ids,
                                               sizeof(user_ids[0]) * user_copied);
        if(result < 0)
            goto finish;
    }
    result = ksceKernelMemcpyKernelToUser(copied_count, &user_copied,
                                           sizeof(user_copied));
    if(result < 0)
        goto finish;
    result = ksceKernelMemcpyKernelToUser(total_count, &total, sizeof(total));

finish:
    EXIT_SYSCALL(syscall_state);
    return result;
}

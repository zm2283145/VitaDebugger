#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/threadmgr/debugger.h>
#include <psp2kern/kernel/threadmgr/misc.h>
#include <psp2kern/kernel/threadmgr/thread.h>

#include "vitadebug_kernel.h"

#define VD_SUSPEND_STATUS 0x1002
#define VD_MIN_LEASE_MS 250u
#define VD_MAX_LEASE_MS 5000u

struct vd_stop_session {
    SceUID pid;
    unsigned int token;
    SceUID controller_thread;
    SceUID exempt_thread;
    SceUID suspended[VD_KERNEL_MAX_THREADS];
    int suspended_count;
    uint64_t deadline_us;
    int active;
};

static struct vd_stop_session stop_session;
static volatile int session_lock;
static volatile int watchdog_stop;
static SceUID watchdog_thread = -1;
static unsigned int next_token = 1;

static void lock_sessions(void)
{
    for(;;)
    {
        int expected = 0;
        if(__atomic_compare_exchange_n(&session_lock, &expected, 1, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return;
    }
}

static void unlock_sessions(void)
{
    __atomic_store_n(&session_lock, 0, __ATOMIC_SEQ_CST);
}

static int resume_session_locked(void)
{
    int first_error = 0;
    for(int i = stop_session.suspended_count - 1; i >= 0; --i)
    {
        SceUID thread = stop_session.suspended[i];
        int result = ksceKernelDebugResumeThread(thread, VD_SUSPEND_STATUS);
        if(result < 0)
        {
            int recovery = ksceKernelChangeThreadSuspendStatus(thread, 2);
            if(recovery < 0 && first_error == 0)
                first_error = result;
        }
    }
    stop_session.active = 0;
    stop_session.pid = -1;
    stop_session.token = 0;
    stop_session.controller_thread = -1;
    stop_session.exempt_thread = -1;
    stop_session.suspended_count = 0;
    stop_session.deadline_us = 0;
    return first_error;
}

static int watchdog_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    while(!__atomic_load_n(&watchdog_stop, __ATOMIC_SEQ_CST))
    {
        if(__atomic_load_n(&stop_session.active, __ATOMIC_SEQ_CST))
        {
            uint64_t now = (uint64_t)ksceKernelGetSystemTimeWide();
            lock_sessions();
            if(stop_session.active && now >= stop_session.deadline_us)
                resume_session_locked();
            unlock_sessions();
        }
        ksceKernelDelayThread(20000);
    }
    return 0;
}

int _start(SceSize args, void* argp)
    __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    watchdog_stop = 0;
    watchdog_thread = ksceKernelCreateThread("vitadebug watchdog",
                                             watchdog_main, 0x40,
                                             16 * 1024, 0, 0, NULL);
    if(watchdog_thread < 0 ||
       ksceKernelStartThread(watchdog_thread, 0, NULL) < 0)
    {
        if(watchdog_thread >= 0)
            ksceKernelDeleteThread(watchdog_thread);
        watchdog_thread = -1;
        return SCE_KERNEL_START_FAILED;
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    __atomic_store_n(&watchdog_stop, 1, __ATOMIC_SEQ_CST);
    lock_sessions();
    if(stop_session.active)
        resume_session_locked();
    unlock_sessions();
    if(watchdog_thread >= 0)
    {
        int status = 0;
        ksceKernelWaitThreadEnd(watchdog_thread, &status, NULL);
        ksceKernelDeleteThread(watchdog_thread);
        watchdog_thread = -1;
    }
    return SCE_KERNEL_STOP_SUCCESS;
}

int vdKernelGetStatus(struct vd_kernel_status* status)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);

    const struct vd_kernel_status kernel_status = {
        .abi_version = VD_KERNEL_ABI_VERSION,
        .capabilities = VD_KERNEL_CAP_THREAD_LIST |
                        VD_KERNEL_CAP_THREAD_CONTROL |
                        VD_KERNEL_CAP_THREAD_REGISTERS |
                        VD_KERNEL_CAP_STOP_RECONCILE |
                        VD_KERNEL_CAP_PROBE_SUSPEND,
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

static SceUID find_caller_thread_guid(SceUID user_thread)
{
    SceUID kernel_ids[VD_KERNEL_MAX_THREADS];
    int copied = 0;
    SceUID caller_pid = ksceKernelGetProcessId();
    int result = ksceKernelGetThreadIdList(caller_pid, kernel_ids,
                                           VD_KERNEL_MAX_THREADS, &copied);
    if(result < 0)
        return result;

    for(int i = 0; i < copied; ++i)
        if(ksceKernelGetUserThreadId(kernel_ids[i]) == user_thread)
            return kernel_ids[i];
    return -1;
}

int vdKernelProbeSuspendThread(
    SceUID target_user_thread,
    unsigned int duration_us,
    struct vd_kernel_probe_suspend_result* probe_result)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);

    int result = -1;
    if(!probe_result || duration_us < 100000 || duration_us > 500000)
        goto finish;

    struct vd_kernel_probe_suspend_result kernel_result = {
        .suspend_result = -1,
        .state_while_suspended = -1,
        .resume_result = -1,
        .state_after_resume = -1,
        .recovery_result = -1,
    };
    SceUID target_guid = find_caller_thread_guid(target_user_thread);
    SceUID caller_guid = ksceKernelGetThreadId();
    if(target_guid < 0 || target_guid == caller_guid)
        goto copy_result;

    kernel_result.suspend_result =
        ksceKernelDebugSuspendThread(target_guid, VD_SUSPEND_STATUS);
    if(kernel_result.suspend_result < 0)
        goto copy_result;

    kernel_result.state_while_suspended =
        ksceKernelIsThreadDebugSuspended(target_guid);
    ksceKernelDelayThread(duration_us);
    kernel_result.resume_result =
        ksceKernelDebugResumeThread(target_guid, VD_SUSPEND_STATUS);
    if(kernel_result.resume_result < 0)
        kernel_result.recovery_result =
            ksceKernelChangeThreadSuspendStatus(target_guid, 2);
    kernel_result.state_after_resume =
        ksceKernelIsThreadDebugSuspended(target_guid);

copy_result:
    result = ksceKernelMemcpyKernelToUser(probe_result, &kernel_result,
                                          sizeof(kernel_result));

finish:
    EXIT_SYSCALL(syscall_state);
    return result;
}

static int valid_lease(unsigned int lease_ms)
{
    return lease_ms >= VD_MIN_LEASE_MS && lease_ms <= VD_MAX_LEASE_MS;
}

int vdKernelBeginStop(unsigned int lease_ms, SceUID exempt_user_thread,
                      struct vd_kernel_stop_result* stop_result)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);

    if(!stop_result || !valid_lease(lease_ms))
    {
        EXIT_SYSCALL(syscall_state);
        return -1;
    }

    struct vd_kernel_stop_result kernel_result = {
        .token = 0,
        .suspended_count = 0,
        .already_suspended_count = 0,
        .failed_thread = -1,
        .failure_code = 0,
    };
    SceUID caller_pid = ksceKernelGetProcessId();
    SceUID caller_thread = ksceKernelGetThreadId();
    SceUID exempt_thread = -1;
    if(exempt_user_thread >= 0)
    {
        exempt_thread = find_caller_thread_guid(exempt_user_thread);
        if(exempt_thread < 0)
        {
            kernel_result.failure_code = -3;
            goto copy_result;
        }
    }
    SceUID threads[VD_KERNEL_MAX_THREADS];
    int copied = 0;
    int total = ksceKernelGetThreadIdList(caller_pid, threads,
                                          VD_KERNEL_MAX_THREADS, &copied);
    if(total < 0 || total > VD_KERNEL_MAX_THREADS)
    {
        kernel_result.failure_code = total < 0 ? total : -4;
        goto copy_result;
    }

    lock_sessions();
    if(stop_session.active)
    {
        kernel_result.failure_code = -2;
        unlock_sessions();
        goto copy_result;
    }

    stop_session.pid = caller_pid;
    stop_session.controller_thread = caller_thread;
    stop_session.exempt_thread = exempt_thread;
    stop_session.suspended_count = 0;
    for(int i = 0; i < copied; ++i)
    {
        if(threads[i] == caller_thread || threads[i] == exempt_thread)
            continue;
        int state = ksceKernelIsThreadDebugSuspended(threads[i]);
        if(state > 0)
        {
            kernel_result.already_suspended_count++;
            continue;
        }
        int result = ksceKernelDebugSuspendThread(threads[i],
                                                  VD_SUSPEND_STATUS);
        if(result < 0)
        {
            kernel_result.failed_thread = ksceKernelGetUserThreadId(threads[i]);
            kernel_result.failure_code = result;
            resume_session_locked();
            unlock_sessions();
            goto copy_result;
        }
        stop_session.suspended[stop_session.suspended_count++] = threads[i];
    }

    unsigned int token = next_token++;
    if(token == 0)
        token = next_token++;
    stop_session.token = token;
    stop_session.deadline_us = (uint64_t)ksceKernelGetSystemTimeWide() +
                               (uint64_t)lease_ms * 1000u;
    stop_session.active = 1;
    kernel_result.token = token;
    kernel_result.suspended_count = stop_session.suspended_count;
    unlock_sessions();

copy_result:;
    int copy_result = ksceKernelMemcpyKernelToUser(stop_result, &kernel_result,
                                                   sizeof(kernel_result));
    if(copy_result < 0 && kernel_result.token != 0)
    {
        lock_sessions();
        if(stop_session.active && stop_session.pid == caller_pid &&
           stop_session.token == kernel_result.token)
            resume_session_locked();
        unlock_sessions();
    }
    EXIT_SYSCALL(syscall_state);
    return copy_result < 0 ? copy_result : kernel_result.failure_code;
}

int vdKernelRenewStop(unsigned int token, unsigned int lease_ms)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    if(!token || !valid_lease(lease_ms))
    {
        EXIT_SYSCALL(syscall_state);
        return -1;
    }

    int result = -5;
    SceUID caller_pid = ksceKernelGetProcessId();
    lock_sessions();
    if(stop_session.active && stop_session.pid == caller_pid &&
       stop_session.token == token)
    {
        SceUID threads[VD_KERNEL_MAX_THREADS];
        int copied = 0;
        int total = ksceKernelGetThreadIdList(caller_pid, threads,
                                              VD_KERNEL_MAX_THREADS, &copied);
        if(total < 0 || total > VD_KERNEL_MAX_THREADS)
            result = total < 0 ? total : -4;
        else
        {
            int original_count = stop_session.suspended_count;
            result = 0;
            for(int i = 0; i < copied; ++i)
            {
                SceUID thread = threads[i];
                if(thread == stop_session.controller_thread ||
                   thread == stop_session.exempt_thread)
                    continue;

                int owned = 0;
                for(int j = 0; j < stop_session.suspended_count; ++j)
                    if(stop_session.suspended[j] == thread)
                    {
                        owned = 1;
                        break;
                    }
                if(owned || ksceKernelIsThreadDebugSuspended(thread) > 0)
                    continue;

                result = ksceKernelDebugSuspendThread(thread,
                                                       VD_SUSPEND_STATUS);
                if(result < 0)
                    break;
                stop_session.suspended[stop_session.suspended_count++] = thread;
            }

            if(result < 0)
            {
                while(stop_session.suspended_count > original_count)
                {
                    SceUID thread = stop_session.suspended[
                        --stop_session.suspended_count];
                    int resume = ksceKernelDebugResumeThread(
                        thread, VD_SUSPEND_STATUS);
                    if(resume < 0)
                        ksceKernelChangeThreadSuspendStatus(thread, 2);
                }
            }
            else
                stop_session.deadline_us =
                    (uint64_t)ksceKernelGetSystemTimeWide() +
                    (uint64_t)lease_ms * 1000u;
        }
    }
    unlock_sessions();
    EXIT_SYSCALL(syscall_state);
    return result;
}

int vdKernelEndStop(unsigned int token, int* resumed_count)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    if(!token || !resumed_count)
    {
        EXIT_SYSCALL(syscall_state);
        return -1;
    }

    int result = -5;
    int resumed = 0;
    SceUID caller_pid = ksceKernelGetProcessId();
    lock_sessions();
    if(stop_session.active && stop_session.pid == caller_pid &&
       stop_session.token == token)
    {
        resumed = stop_session.suspended_count;
        result = resume_session_locked();
    }
    unlock_sessions();
    if(result >= 0)
        result = ksceKernelMemcpyKernelToUser(resumed_count, &resumed,
                                               sizeof(resumed));
    EXIT_SYSCALL(syscall_state);
    return result;
}

int vdKernelGetThreadRegisters(unsigned int token, SceUID target_user_thread,
                               struct vd_thread_registers* registers)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    if(!token || !registers)
    {
        EXIT_SYSCALL(syscall_state);
        return -1;
    }

    int result = -5;
    SceUID caller_pid = ksceKernelGetProcessId();
    lock_sessions();
    if(stop_session.active && stop_session.pid == caller_pid &&
       stop_session.token == token)
    {
        SceUID target_guid = -1;
        for(int i = 0; i < stop_session.suspended_count; ++i)
        {
            SceUID candidate = stop_session.suspended[i];
            if(ksceKernelGetUserThreadId(candidate) == target_user_thread)
            {
                target_guid = candidate;
                break;
            }
        }
        if(target_guid >= 0)
        {
            SceThreadCpuRegisters kernel_registers;
            result = ksceKernelGetThreadCpuRegisters(target_guid,
                                                      &kernel_registers);
            if(result >= 0)
                result = ksceKernelMemcpyKernelToUser(registers,
                                                       &kernel_registers,
                                                       sizeof(*registers));
        }
        else
        {
            result = -3;
        }
    }
    unlock_sessions();
    EXIT_SYSCALL(syscall_state);
    return result;
}

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/threadmgr/debugger.h>
#include <psp2kern/kernel/threadmgr/misc.h>
#include <psp2kern/kernel/threadmgr/thread.h>
#include <psp2/kernel/error.h>

#include "vitadebug_kernel.h"
#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
#include "hw_debug.h"
#endif

#define VD_SUSPEND_STATUS 0x1002
#define VD_MIN_LEASE_MS 250u
#define VD_MAX_LEASE_MS 5000u
#define VD_WATCHDOG_STOP_TIMEOUT_US 500000u
#define VD_SESSION_LOCK_RETRY_US 1000u
#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_VFP_SNAPSHOT
#define VD_VFP_PREFIX_GUARD_WORDS 64
#define VD_VFP_SUFFIX_GUARD_WORDS 1024
#define VD_VFP_GUARD_VALUE 0x56465047u

struct vd_vfp_scratch {
    unsigned int prefix_guard[VD_VFP_PREFIX_GUARD_WORDS];
    uint64_t d[VD_KERNEL_VFP_D_REGISTER_COUNT];
    unsigned int suffix_guard[VD_VFP_SUFFIX_GUARD_WORDS];
};

// The undocumented callee's output size is not declared by VitaSDK. Keep the
// candidate buffer out of the small syscall stack and surround D0-D31 with a
// full page of trailing canaries. The session lock serializes all access.
static struct vd_vfp_scratch vfp_scratch __attribute__((aligned(64)));
#endif

struct vd_stop_session {
    SceUID pid;
    unsigned int token;
    SceUID controller_thread;
    SceUID exempt_thread;
    SceUID suspended[VD_KERNEL_MAX_THREADS];
    int suspended_count;
    uint64_t deadline_us;
    int active;
    int hardware_mutation;
    int hardware_restore_pending;
    unsigned int hardware_token;
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
        ksceKernelDelayThread(VD_SESSION_LOCK_RETRY_US);
    }
}

static int try_lock_sessions(void)
{
    int expected = 0;
    return __atomic_compare_exchange_n(&session_lock, &expected, 1, 0,
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST);
}

static void unlock_sessions(void)
{
    __atomic_store_n(&session_lock, 0, __ATOMIC_SEQ_CST);
}

static int thread_list_contains(const SceUID* threads, int count,
                                SceUID thread)
{
    for(int i = 0; i < count; ++i)
        if(threads[i] == thread)
            return 1;
    return 0;
}

static int resume_session_locked(void)
{
    if(stop_session.hardware_mutation ||
       stop_session.hardware_restore_pending)
        return VD_KERNEL_ERROR_HW_BUSY;

    SceUID current_threads[VD_KERNEL_MAX_THREADS];
    SceUID retry_threads[VD_KERNEL_MAX_THREADS];
    int current_count = 0;
    int retry_count = 0;
    int total = ksceKernelGetThreadIdList(
        stop_session.pid, current_threads, VD_KERNEL_MAX_THREADS,
        &current_count);
    int membership_known = total >= 0 &&
                           total <= VD_KERNEL_MAX_THREADS &&
                           current_count >= 0 &&
                           current_count <= VD_KERNEL_MAX_THREADS;
    int first_error = 0;
    for(int i = stop_session.suspended_count - 1; i >= 0; --i)
    {
        SceUID thread = stop_session.suspended[i];
        int result = ksceKernelDebugResumeThread(thread, VD_SUSPEND_STATUS);
        if(result < 0)
        {
            int recovery = ksceKernelChangeThreadSuspendStatus(thread, 2);
            if(recovery < 0)
            {
                int still_present = !membership_known ||
                    thread_list_contains(current_threads, current_count,
                                         thread);
                int suspend_state = ksceKernelIsThreadDebugSuspended(thread);
                if(still_present && suspend_state != 0)
                {
                    retry_threads[retry_count++] = thread;
                    if(first_error == 0)
                        first_error = result;
                }
            }
        }
    }

    if(retry_count > 0)
    {
        for(int i = 0; i < retry_count; ++i)
            stop_session.suspended[i] = retry_threads[i];
        stop_session.suspended_count = retry_count;
        stop_session.active = 1;
        /* Keep the lease expired so the watchdog retries on its next tick. */
        stop_session.deadline_us = 0;
        return first_error;
    }

    stop_session.active = 0;
    stop_session.pid = -1;
    stop_session.token = 0;
    stop_session.controller_thread = -1;
    stop_session.exempt_thread = -1;
    stop_session.suspended_count = 0;
    stop_session.deadline_us = 0;
    stop_session.hardware_mutation = 0;
    stop_session.hardware_restore_pending = 0;
    stop_session.hardware_token = 0;
    return first_error;
}

static int watchdog_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    while(!__atomic_load_n(&watchdog_stop, __ATOMIC_SEQ_CST))
    {
        uint64_t now = (uint64_t)ksceKernelGetSystemTimeWide();
#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
        SceUID recovery_pid = -1;
        unsigned int recovery_stop_token = 0;
        unsigned int recovery_hw_token = 0;

        if(!try_lock_sessions())
        {
            ksceKernelDelayThread(20000);
            continue;
        }
        if(stop_session.active && stop_session.hardware_restore_pending)
        {
            recovery_pid = stop_session.pid;
            recovery_stop_token = stop_session.token;
            recovery_hw_token = stop_session.hardware_token;
        }
        unlock_sessions();

        int recovery_needed = recovery_pid >= 0 &&
                              recovery_stop_token != 0 &&
                              recovery_hw_token != 0;
        int hardware_recovered = 0;
        if(recovery_needed)
            hardware_recovered = vdHwDebugRecover(recovery_pid,
                                                   recovery_hw_token) >= 0;
        else
            vdHwDebugWatchdog(now);
#endif
        if(try_lock_sessions())
        {
#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
            if(recovery_needed && hardware_recovered &&
               stop_session.active && stop_session.hardware_restore_pending &&
               stop_session.pid == recovery_pid &&
               stop_session.token == recovery_stop_token &&
               stop_session.hardware_token == recovery_hw_token)
            {
                stop_session.hardware_restore_pending = 0;
                stop_session.hardware_token = 0;
            }
#endif
            if(stop_session.active && !stop_session.hardware_mutation &&
               !stop_session.hardware_restore_pending &&
               now >= stop_session.deadline_us)
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
#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
    if(vdHwDebugStart() < 0)
        return SCE_KERNEL_START_FAILED;
#endif
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
#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
        if(vdHwDebugStop() < 0)
        {
            /* Keep code resident if a worker could not be proven stopped. */
            return SCE_KERNEL_START_SUCCESS;
        }
#endif
        return SCE_KERNEL_START_FAILED;
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
    /* Keep the watchdog alive if exact hardware-state restoration fails. */
    if(vdHwDebugStop() < 0)
        return SCE_KERNEL_STOP_FAIL;
#endif
    int resume_result = 0;
    lock_sessions();
#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
    /* vdHwDebugStop proved that every local-core snapshot was restored. */
    stop_session.hardware_mutation = 0;
    stop_session.hardware_restore_pending = 0;
    stop_session.hardware_token = 0;
#endif
    if(stop_session.active)
        resume_result = resume_session_locked();
    unlock_sessions();
    if(resume_result < 0)
        return SCE_KERNEL_STOP_FAIL;
    /*
     * Do not retire the lease watchdog until every stopped application thread
     * has been released.  A concurrent hardware mutation can make the first
     * unload attempt fail with HW_BUSY; in that case the resident plugin must
     * retain its recovery thread for the caller's next cleanup attempt.
     */
    __atomic_store_n(&watchdog_stop, 1, __ATOMIC_SEQ_CST);
    if(watchdog_thread >= 0)
    {
        int status = 0;
        SceUInt timeout_us = VD_WATCHDOG_STOP_TIMEOUT_US;
        int result = ksceKernelWaitThreadEnd(watchdog_thread, &status,
                                              &timeout_us);
        if(result < 0)
            return SCE_KERNEL_STOP_FAIL;
        result = ksceKernelDeleteThread(watchdog_thread);
        if(result < 0)
            return SCE_KERNEL_STOP_FAIL;
        watchdog_thread = -1;
    }
    return SCE_KERNEL_STOP_SUCCESS;
}

int vdKernelGetStatus(struct vd_kernel_status* status)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);

    unsigned int capabilities = VD_KERNEL_CAP_THREAD_LIST |
                                VD_KERNEL_CAP_THREAD_CONTROL |
                                VD_KERNEL_CAP_THREAD_REGISTERS |
                                VD_KERNEL_CAP_STOP_RECONCILE |
                                VD_KERNEL_CAP_HW_DEBUG_DISCOVERY |
                                VD_KERNEL_CAP_PROBE_SUSPEND;
#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_VFP_SNAPSHOT
    capabilities |= VD_KERNEL_CAP_THREAD_VFP_REGISTERS;
#endif
#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
    if(vdHwDebugReady())
        capabilities |= VD_KERNEL_CAP_HW_BREAKPOINT |
                        VD_KERNEL_CAP_HW_WATCHPOINT;
#endif
    const struct vd_kernel_status kernel_status = {
        .abi_version = VD_KERNEL_ABI_VERSION,
        .capabilities = capabilities,
        .max_threads = VD_KERNEL_MAX_THREADS,
        .reserved = 0,
    };
    int result = ksceKernelMemcpyKernelToUser(status, &kernel_status,
                                               sizeof(kernel_status));

    EXIT_SYSCALL(syscall_state);
    return result;
}

int vdKernelGetHardwareDebugInfo(struct vd_kernel_hw_debug_info* info)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    if(!info)
    {
        EXIT_SYSCALL(syscall_state);
        return -1;
    }

    unsigned int didr;
    __asm__ volatile("mrc p14, 0, %0, c0, c0, 0" : "=r"(didr));
    const struct vd_kernel_hw_debug_info kernel_info = {
        .raw_didr = didr,
        .breakpoint_count = ((didr >> 24) & 0xf) + 1,
        .watchpoint_count = ((didr >> 28) & 0xf) + 1,
        .context_breakpoint_count = ((didr >> 20) & 0xf) + 1,
    };
    int result = ksceKernelMemcpyKernelToUser(info, &kernel_info,
                                               sizeof(kernel_info));
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
    stop_session.hardware_mutation = 0;
    stop_session.hardware_restore_pending = 0;
    stop_session.hardware_token = 0;
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
            int retained_count = 0;
            for(int i = 0; i < stop_session.suspended_count; ++i)
            {
                SceUID owned_thread = stop_session.suspended[i];
                if(thread_list_contains(threads, copied, owned_thread) &&
                   ksceKernelIsThreadDebugSuspended(owned_thread) > 0)
                    stop_session.suspended[retained_count++] = owned_thread;
            }
            stop_session.suspended_count = retained_count;
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

                if(stop_session.suspended_count >= VD_KERNEL_MAX_THREADS)
                {
                    result = -4;
                    break;
                }
                result = ksceKernelDebugSuspendThread(thread,
                                                       VD_SUSPEND_STATUS);
                if(result < 0)
                    break;
                stop_session.suspended[stop_session.suspended_count++] = thread;
            }

            if(result < 0)
            {
                int appended_count = stop_session.suspended_count;
                int retained_after_rollback = original_count;
                for(int i = original_count; i < appended_count; ++i)
                {
                    SceUID thread = stop_session.suspended[i];
                    int resume = ksceKernelDebugResumeThread(
                        thread, VD_SUSPEND_STATUS);
                    int recovery = resume < 0 ?
                        ksceKernelChangeThreadSuspendStatus(thread, 2) : 0;
                    if(resume < 0 && recovery < 0 &&
                       ksceKernelIsThreadDebugSuspended(thread) != 0)
                        stop_session.suspended[retained_after_rollback++] =
                            thread;
                }
                stop_session.suspended_count = retained_after_rollback;
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

static SceUID find_session_thread_locked(SceUID target_user_thread)
{
    if(target_user_thread < 0)
        return -1;
    for(int i = 0; i < stop_session.suspended_count; ++i)
    {
        SceUID candidate = stop_session.suspended[i];
        SceUID candidate_user = ksceKernelGetUserThreadId(candidate);
        if(candidate_user >= 0 && candidate_user == target_user_thread)
            return candidate;
    }
    return -1;
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
        SceUID target_guid = find_session_thread_locked(target_user_thread);
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

int vdKernelGetThreadVfpRegisters(
    unsigned int token,
    SceUID target_user_thread,
    struct vd_thread_vfp_registers* registers)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    if(!token || !registers)
    {
        EXIT_SYSCALL(syscall_state);
        return -1;
    }

#ifndef VD_KERNEL_ENABLE_EXPERIMENTAL_VFP_SNAPSHOT
    (void)target_user_thread;
    EXIT_SYSCALL(syscall_state);
    return VD_KERNEL_ERROR_VFP_DISABLED;
#else

    int result = -5;
    SceUID caller_pid = ksceKernelGetProcessId();
    lock_sessions();
    if(stop_session.active && stop_session.pid == caller_pid &&
       stop_session.token == token)
    {
        SceUID target_guid = find_session_thread_locked(target_user_thread);
        if(target_guid >= 0)
        {
            // VitaSDK intentionally leaves this API's output type undocumented.
            // Keep guards on both sides so a layout other than D0-D31 is
            // detected and rejected without exposing unknown bytes to user mode.
            for(int i = 0; i < VD_VFP_PREFIX_GUARD_WORDS; ++i)
                vfp_scratch.prefix_guard[i] = VD_VFP_GUARD_VALUE;
            for(int i = 0; i < VD_KERNEL_VFP_D_REGISTER_COUNT; ++i)
                vfp_scratch.d[i] = 0;
            for(int i = 0; i < VD_VFP_SUFFIX_GUARD_WORDS; ++i)
                vfp_scratch.suffix_guard[i] = VD_VFP_GUARD_VALUE;

            int raw_vfp_result =
                ksceKernelGetVfpRegisterForDebugger(target_guid,
                                                     vfp_scratch.d);
            int guard_changed = 0;
            for(int i = 0; i < VD_VFP_PREFIX_GUARD_WORDS; ++i)
                if(vfp_scratch.prefix_guard[i] != VD_VFP_GUARD_VALUE)
                    guard_changed = 1;
            for(int i = 0; i < VD_VFP_SUFFIX_GUARD_WORDS; ++i)
                if(vfp_scratch.suffix_guard[i] != VD_VFP_GUARD_VALUE)
                    guard_changed = 1;

            /* Retail 3.65 returns a permission error for valid suspended
             * application threads that have no readable VFP bank, while a
             * thread actively using VFP succeeds. Normalize only the two
             * exact API results below and only after proving the undocumented
             * call did not cross either guard. */
            if(guard_changed)
                result = VD_KERNEL_ERROR_VFP_GUARD;
            else if(raw_vfp_result ==
                        (int)SCE_KERNEL_ERROR_CAN_NOT_USE_VFP ||
                    raw_vfp_result ==
                        (int)SCE_KERNEL_ERROR_ILLEGAL_PERMISSION)
                result = VD_KERNEL_ERROR_VFP_CONTEXT_UNAVAILABLE;
            else
                result = raw_vfp_result;

            SceThreadCpuRegisters cpu_registers;
            if(result >= 0)
                result = ksceKernelGetThreadCpuRegisters(target_guid,
                                                          &cpu_registers);
            if(result >= 0)
            {
                struct vd_thread_vfp_registers kernel_registers;
                kernel_registers.layout_version =
                    VD_KERNEL_VFP_LAYOUT_D32_V1;
                kernel_registers.d_register_count =
                    VD_KERNEL_VFP_D_REGISTER_COUNT;
                for(int i = 0; i < VD_KERNEL_VFP_D_REGISTER_COUNT; ++i)
                    kernel_registers.d[i] = vfp_scratch.d[i];
                for(int i = 0; i < 2; ++i)
                    kernel_registers.fpscr_entry[i] =
                        cpu_registers.entry[i].fpscr;
                result = ksceKernelMemcpyKernelToUser(registers,
                                                       &kernel_registers,
                                                       sizeof(kernel_registers));
            }
        }
        else
        {
            result = -3;
        }
    }
    unlock_sessions();
    EXIT_SYSCALL(syscall_state);
    return result;
#endif
}

#ifdef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
static unsigned int read_full_context_id(void)
{
    unsigned int context_id;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 1" : "=r"(context_id));
    return context_id;
}

static int stop_session_is_current_locked(SceUID caller_pid,
                                          unsigned int stop_token,
                                          uint64_t now_us)
{
    SceUID threads[VD_KERNEL_MAX_THREADS];
    int copied = 0;
    int total;

    if(!stop_session.active || stop_session.pid != caller_pid ||
       stop_session.token != stop_token ||
       now_us >= stop_session.deadline_us ||
       stop_session.hardware_mutation ||
       stop_session.hardware_restore_pending ||
       stop_session.exempt_thread >= 0 ||
       stop_session.controller_thread != ksceKernelGetThreadId())
        return 0;

    total = ksceKernelGetThreadIdList(caller_pid, threads,
                                      VD_KERNEL_MAX_THREADS, &copied);
    if(total < 0 || total > VD_KERNEL_MAX_THREADS)
        return 0;
    for(int i = 0; i < copied; ++i)
    {
        if(threads[i] == stop_session.controller_thread ||
           threads[i] == stop_session.exempt_thread)
            continue;
        if(ksceKernelIsThreadDebugSuspended(threads[i]) <= 0)
            return 0;
    }
    return 1;
}
#endif

int vdKernelAcquireHardwareDebug(
    unsigned int lease_ms,
    struct vd_kernel_hw_session_result* session_result)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);

#ifndef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
    (void)lease_ms;
    (void)session_result;
    EXIT_SYSCALL(syscall_state);
    return VD_KERNEL_ERROR_HW_DISABLED;
#else
    if(!session_result || !valid_lease(lease_ms))
    {
        EXIT_SYSCALL(syscall_state);
        return VD_KERNEL_ERROR_HW_INVALID;
    }

    SceUID caller_pid = ksceKernelGetProcessId();
    unsigned int context_id = read_full_context_id();
    struct vd_kernel_hw_session_result kernel_result = {0};
    int result = vdHwDebugAcquire(caller_pid, context_id, lease_ms,
                                  &kernel_result);
    int copy_result = ksceKernelMemcpyKernelToUser(session_result,
                                                   &kernel_result,
                                                   sizeof(kernel_result));
    if(copy_result < 0 && kernel_result.token != 0)
        vdHwDebugRelease(caller_pid, kernel_result.token);

    EXIT_SYSCALL(syscall_state);
    return copy_result < 0 ? copy_result : result;
#endif
}

int vdKernelRenewHardwareDebug(unsigned int token, unsigned int lease_ms)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);

#ifndef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
    (void)token;
    (void)lease_ms;
    EXIT_SYSCALL(syscall_state);
    return VD_KERNEL_ERROR_HW_DISABLED;
#else
    if(token == 0 || !valid_lease(lease_ms))
    {
        EXIT_SYSCALL(syscall_state);
        return VD_KERNEL_ERROR_HW_INVALID;
    }
    int result = vdHwDebugRenew(ksceKernelGetProcessId(),
                                read_full_context_id(), token, lease_ms);
    EXIT_SYSCALL(syscall_state);
    return result;
#endif
}

int vdKernelUpdateHardwarePoint(
    unsigned int token,
    unsigned int stop_token,
    const struct vd_kernel_hw_point_request* request)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);

#ifndef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
    (void)token;
    (void)stop_token;
    (void)request;
    EXIT_SYSCALL(syscall_state);
    return VD_KERNEL_ERROR_HW_DISABLED;
#else
    struct vd_kernel_hw_point_request kernel_request;
    if(token == 0 || stop_token == 0 || !request)
    {
        EXIT_SYSCALL(syscall_state);
        return VD_KERNEL_ERROR_HW_INVALID;
    }
    int result = ksceKernelMemcpyUserToKernel(&kernel_request, request,
                                               sizeof(kernel_request));
    if(result < 0)
    {
        EXIT_SYSCALL(syscall_state);
        return result;
    }

    SceUID caller_pid = ksceKernelGetProcessId();
    unsigned int context_id = read_full_context_id();
    uint64_t now;
    lock_sessions();
    now = (uint64_t)ksceKernelGetSystemTimeWide();
    if(!stop_session_is_current_locked(caller_pid, stop_token, now))
    {
        unlock_sessions();
        EXIT_SYSCALL(syscall_state);
        return VD_KERNEL_ERROR_HW_STOP_REQUIRED;
    }
    stop_session.hardware_mutation = 1;
    stop_session.hardware_restore_pending = 0;
    stop_session.hardware_token = token;
    unlock_sessions();
    /*
     * The in-flight flag keeps EndStop and lease cleanup from resuming the
     * target while the three pinned workers program their local registers.
     * Do not hold the raw stop-session spin lock across this sleeping dispatch.
     */
    result = vdHwDebugUpdate(caller_pid, context_id, token,
                             &kernel_request);

    now = (uint64_t)ksceKernelGetSystemTimeWide();
    int resume_result = 0;
    lock_sessions();
    if(stop_session.active && stop_session.pid == caller_pid &&
       stop_session.token == stop_token && stop_session.hardware_mutation &&
       stop_session.hardware_token == token)
    {
        stop_session.hardware_mutation = 0;
        if(result == VD_KERNEL_ERROR_HW_RESTORE)
            stop_session.hardware_restore_pending = 1;
        else
        {
            stop_session.hardware_restore_pending = 0;
            stop_session.hardware_token = 0;
        }
        if(!stop_session.hardware_restore_pending &&
           now >= stop_session.deadline_us)
            resume_result = resume_session_locked();
    }
    unlock_sessions();
    if(result >= 0 && resume_result < 0)
        result = resume_result;
    EXIT_SYSCALL(syscall_state);
    return result;
#endif
}

int vdKernelReleaseHardwareDebug(unsigned int token)
{
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);

#ifndef VD_KERNEL_ENABLE_EXPERIMENTAL_HW_DEBUG
    (void)token;
    EXIT_SYSCALL(syscall_state);
    return VD_KERNEL_ERROR_HW_DISABLED;
#else
    if(token == 0)
    {
        EXIT_SYSCALL(syscall_state);
        return VD_KERNEL_ERROR_HW_INVALID;
    }
    SceUID caller_pid = ksceKernelGetProcessId();
    int result = vdHwDebugRelease(caller_pid, token);
    if(result >= 0)
    {
        uint64_t now = (uint64_t)ksceKernelGetSystemTimeWide();
        lock_sessions();
        if(stop_session.active && stop_session.pid == caller_pid &&
           stop_session.hardware_restore_pending &&
           stop_session.hardware_token == token)
        {
            stop_session.hardware_restore_pending = 0;
            stop_session.hardware_token = 0;
            if(now >= stop_session.deadline_us)
                result = resume_session_locked();
        }
        unlock_sessions();
    }
    EXIT_SYSCALL(syscall_state);
    return result;
#endif
}

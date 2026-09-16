#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/net/net_syscalls.h>
#include "uvdb.h"

#define UVDB_DEBUGNET_QUEUE_SLOTS 64
#define UVDB_DEBUGNET_MESSAGE_MAX 1024
#define UVDB_DEBUGNET_THREAD_STACK (32 * 1024)
#define UVDB_DEBUGNET_WRITER_TIMEOUT_US 500000
#define UVDB_DEBUGNET_STOP_TIMEOUT_US 2000000
#define UVDB_DEBUGNET_EXIT_LOCK_TIMEOUT_US 4000000
#define UVDB_DEBUGNET_GATE_CLOSED 0x80000000u
#define UVDB_DEBUGNET_GATE_GENERATION 0x7fffff00u
#define UVDB_DEBUGNET_GATE_WRITERS 0x000000ffu
#define UVDB_DEBUGNET_SOCKET_INVALID (-1)
#define UVDB_DEBUGNET_SOCKET_CLOSING (-2)

enum uvdb_debugnet_lifecycle {
    UVDB_DEBUGNET_STOPPED = 0,
    UVDB_DEBUGNET_STARTING,
    UVDB_DEBUGNET_RUNNING,
    UVDB_DEBUGNET_STOPPING,
    UVDB_DEBUGNET_EXITED,
};

enum uvdb_debugnet_thread_phase {
    UVDB_DEBUGNET_THREAD_NONE = 0,
    UVDB_DEBUGNET_THREAD_CREATED,
    UVDB_DEBUGNET_THREAD_STARTED,
    UVDB_DEBUGNET_THREAD_ENDED,
};

enum uvdb_debugnet_emergency_gate {
    UVDB_DEBUGNET_EMERGENCY_DISARMED = 0,
    UVDB_DEBUGNET_EMERGENCY_ARMED,
    UVDB_DEBUGNET_EMERGENCY_ACTIVE,
};

struct uvdb_debugnet_message {
    unsigned short size;
    char data[UVDB_DEBUGNET_MESSAGE_MAX];
};

static struct uvdb_debugnet_message* log_queue;
static unsigned int log_head;
static unsigned int log_tail;
static unsigned int log_count;
static int log_queue_lock;
static int log_lifecycle_lock;
static int log_state = UVDB_DEBUGNET_STOPPED;
static int log_stop_ready;
static int log_owner_exited;
static int log_owner_callback_active;
static unsigned int log_owner_generation_counter;
static unsigned int log_owner_generation_active;
static int log_process_exit_hook_registered;
static int log_process_exiting;
static int log_skip_socket_close;
static int log_emergency_gate = UVDB_DEBUGNET_EMERGENCY_DISARMED;
static unsigned int log_writer_gate = UVDB_DEBUGNET_GATE_CLOSED;
static SceUID log_thread = -1;
static int log_thread_phase = UVDB_DEBUGNET_THREAD_NONE;
static SceUID log_semaphore = -1;
static SceUID log_owner_thread = -1;
static SceUID log_owner_event = -1;
static int log_socket = UVDB_DEBUGNET_SOCKET_INVALID;
static struct sockaddr_in log_destination;
static enum uvdb_log_level log_level = UVDB_LOG_NONE;
static struct uvdb_debugnet_stats log_stats;

#ifdef UVDB_DEBUGNET_TESTING
extern int uvdb_debugnet_test_register_exit_handler(void (*handler)(void));
extern void uvdb_debugnet_test_exit_process(int status);
#endif

static void debugnet_process_exit_handler(void);

/*
 * Vita's CRT runs exit handlers before _exit() releases newlib's allocator and
 * heap.  Register this once per process so an ordinary return from main stops
 * and joins the sender while its queue is still valid.  Direct process exits,
 * crashes, and SceShell termination can bypass the C exit path; the exact-
 * thread event handler remains the nonblocking fallback for those cases.
 */
static int debugnet_ensure_process_exit_hook(void)
{
    if(__atomic_load_n(&log_process_exit_hook_registered, __ATOMIC_ACQUIRE))
        return 0;

#ifdef UVDB_DEBUGNET_TESTING
    int result =
        uvdb_debugnet_test_register_exit_handler(
            debugnet_process_exit_handler);
#else
    int result = atexit(debugnet_process_exit_handler);
#endif
    if(result != 0)
        return -1;

    __atomic_store_n(&log_process_exit_hook_registered, 1,
                     __ATOMIC_RELEASE);
    return 0;
}

static void debugnet_reset_stats(void)
{
    __atomic_store_n(&log_stats.queued, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&log_stats.sent, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&log_stats.dropped, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&log_stats.truncated, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&log_stats.send_errors, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&log_stats.last_send_error, 0, __ATOMIC_RELAXED);
}

static int debugnet_try_queue_lock(void)
{
    int expected = 0;
    return __atomic_compare_exchange_n(&log_queue_lock, &expected, 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void debugnet_unlock_queue(void)
{
    __atomic_store_n(&log_queue_lock, 0, __ATOMIC_RELEASE);
}

static int debugnet_try_lifecycle_lock(void)
{
    int expected = 0;
    return __atomic_compare_exchange_n(&log_lifecycle_lock, &expected, 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void debugnet_unlock_lifecycle(void)
{
    __atomic_store_n(&log_lifecycle_lock, 0, __ATOMIC_RELEASE);
}

/*
 * Claim the current session's one-shot emergency wake.  This is shared by the
 * exact-owner thread callback and the C process-exit hook.  A claim protects
 * the semaphore UID until the signal is complete; normal cleanup disarms (or
 * waits out) the gate before deleting that semaphore.
 */
static void debugnet_request_emergency_stop(int skip_socket_close)
{
    if(skip_socket_close)
        __atomic_store_n(&log_skip_socket_close, 1, __ATOMIC_RELEASE);

    __atomic_fetch_or(&log_writer_gate, UVDB_DEBUGNET_GATE_CLOSED,
                      __ATOMIC_ACQ_REL);
    __atomic_store_n(&log_owner_exited, 1, __ATOMIC_RELEASE);

    int state = __atomic_load_n(&log_state, __ATOMIC_ACQUIRE);
    while(state == UVDB_DEBUGNET_RUNNING ||
          state == UVDB_DEBUGNET_STARTING)
    {
        if(__atomic_compare_exchange_n(&log_state, &state,
                                       UVDB_DEBUGNET_STOPPING, 0,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE))
            break;
    }
    __atomic_store_n(&log_stop_ready, 1, __ATOMIC_RELEASE);

    int expected = UVDB_DEBUGNET_EMERGENCY_ARMED;
    if(!__atomic_compare_exchange_n(&log_emergency_gate, &expected,
                                    UVDB_DEBUGNET_EMERGENCY_ACTIVE, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;

    SceUID semaphore = __atomic_load_n(&log_semaphore, __ATOMIC_ACQUIRE);
    if(semaphore >= 0)
        sceKernelSignalSema(semaphore, 1);

    __atomic_store_n(&log_emergency_gate, UVDB_DEBUGNET_EMERGENCY_DISARMED,
                     __ATOMIC_RELEASE);
}

static int debugnet_disarm_emergency_gate(void)
{
    SceInt64 deadline = sceKernelGetSystemTimeWide() +
                        UVDB_DEBUGNET_WRITER_TIMEOUT_US;
    for(;;)
    {
        int gate = __atomic_load_n(&log_emergency_gate, __ATOMIC_ACQUIRE);
        if(gate == UVDB_DEBUGNET_EMERGENCY_DISARMED)
            return 0;
        if(gate == UVDB_DEBUGNET_EMERGENCY_ARMED)
        {
            int expected = UVDB_DEBUGNET_EMERGENCY_ARMED;
            if(__atomic_compare_exchange_n(
                   &log_emergency_gate, &expected,
                   UVDB_DEBUGNET_EMERGENCY_DISARMED, 0,
                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return 0;
            continue;
        }
        if(sceKernelGetSystemTimeWide() >= deadline)
            return -1;
        sceKernelDelayThread(100);
    }
}

static int debugnet_close_socket(void)
{
    for(;;)
    {
        int socket = __atomic_load_n(&log_socket, __ATOMIC_ACQUIRE);
        if(socket == UVDB_DEBUGNET_SOCKET_INVALID)
            return 0;
        if(socket == UVDB_DEBUGNET_SOCKET_CLOSING)
            return -1;

        int expected = socket;
        if(!__atomic_compare_exchange_n(&log_socket, &expected,
                                        UVDB_DEBUGNET_SOCKET_CLOSING, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;

        (void)sceNetSyscallShutdown(socket, SHUT_RDWR);
        int result = sceNetSyscallClose(socket);
        if(result < 0)
        {
            __atomic_store_n(&log_socket, socket, __ATOMIC_RELEASE);
            return result;
        }
        __atomic_store_n(&log_socket, UVDB_DEBUGNET_SOCKET_INVALID,
                         __ATOMIC_RELEASE);
        return 0;
    }
}

static const char* debugnet_level_name(enum uvdb_log_level level)
{
    switch(level)
    {
        case UVDB_LOG_ERROR: return "ERROR";
        case UVDB_LOG_INFO: return "INFO";
        case UVDB_LOG_DEBUG: return "DEBUG";
        case UVDB_LOG_TRACE: return "TRACE";
        default: return "LOG";
    }
}

static int debugnet_send(const char* data, size_t size)
{
    int socket = __atomic_load_n(&log_socket, __ATOMIC_ACQUIRE);
    if(socket < 0)
        return -1;
    uint32_t args[6] = {
        (uint32_t)socket,
        (uint32_t)(uintptr_t)data,
        (uint32_t)size,
        MSG_DONTWAIT,
        (uint32_t)(uintptr_t)&log_destination,
        sizeof(log_destination),
    };
    return sceNetSyscallSendto((SceNetSyscallParameter*)args);
}

/*
 * A thread-event callback must not wait for the sender, take either logger
 * lock, close the socket, or release storage.  It only publishes an emergency
 * stop request and wakes the worker.  The worker owns the socket shutdown;
 * explicit uvdb_debugnet_stop() (when the process remains alive) owns resource
 * reclamation.
 */
static int debugnet_owner_thread_event(SceInt32 type, SceUID thread_id,
                                       SceInt32 arg, void* common)
{
    (void)arg;

    __atomic_add_fetch(&log_owner_callback_active, 1, __ATOMIC_ACQ_REL);
    unsigned int generation = (unsigned int)(uintptr_t)common;
    if(generation != 0 &&
       generation == __atomic_load_n(&log_owner_generation_active,
                                     __ATOMIC_ACQUIRE) &&
       type == SCE_KERNEL_THREAD_EVENT_TYPE_EXIT &&
       thread_id == __atomic_load_n(&log_owner_thread, __ATOMIC_ACQUIRE))
    {
        debugnet_request_emergency_stop(0);
    }
    __atomic_sub_fetch(&log_owner_callback_active, 1, __ATOMIC_RELEASE);
    return 0;
}

static int debugnet_thread_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;

    for(;;)
    {
        SceUID semaphore =
            __atomic_load_n(&log_semaphore, __ATOMIC_ACQUIRE);
        if(semaphore < 0 || sceKernelWaitSema(semaphore, 1, NULL) < 0)
            break;
        if(__atomic_load_n(&log_owner_exited, __ATOMIC_ACQUIRE))
            goto finished;

        for(;;)
        {
            struct uvdb_debugnet_message message;
            int have_message = 0;
            while(!debugnet_try_queue_lock())
            {
                if(__atomic_load_n(&log_owner_exited, __ATOMIC_ACQUIRE))
                    goto finished;
                sceKernelDelayThread(50);
            }

            if(log_count)
            {
                message = log_queue[log_head];
                log_head = (log_head + 1) % UVDB_DEBUGNET_QUEUE_SLOTS;
                log_count--;
                __atomic_store_n(&log_stats.queued, log_count,
                                 __ATOMIC_RELAXED);
                have_message = 1;
            }
            int should_stop =
                __atomic_load_n(&log_state, __ATOMIC_ACQUIRE) ==
                    UVDB_DEBUGNET_STOPPING &&
                __atomic_load_n(&log_stop_ready, __ATOMIC_ACQUIRE) &&
                !log_count &&
                (__atomic_load_n(&log_writer_gate, __ATOMIC_ACQUIRE) &
                 UVDB_DEBUGNET_GATE_WRITERS) == 0;
            debugnet_unlock_queue();

            if(__atomic_load_n(&log_owner_exited, __ATOMIC_ACQUIRE))
                goto finished;

            if(have_message)
            {
                int result = debugnet_send(message.data, message.size);
                if(result == (int)message.size)
                {
                    __atomic_add_fetch(&log_stats.sent, 1,
                                       __ATOMIC_RELAXED);
                }
                else
                {
                    __atomic_store_n(&log_stats.last_send_error, result,
                                     __ATOMIC_RELAXED);
                    __atomic_add_fetch(&log_stats.send_errors, 1,
                                       __ATOMIC_RELAXED);
                }
            }

            if(should_stop && !have_message)
                goto finished;
            if(!have_message)
                break;
        }
    }

finished:
    __atomic_fetch_or(&log_writer_gate, UVDB_DEBUGNET_GATE_CLOSED,
                      __ATOMIC_ACQ_REL);
    if(!__atomic_load_n(&log_skip_socket_close, __ATOMIC_ACQUIRE))
        (void)debugnet_close_socket();
    __atomic_store_n(&log_state, UVDB_DEBUGNET_EXITED, __ATOMIC_RELEASE);
    return 0;
}

static void debugnet_invalidate_owner_handler(void)
{
    __atomic_store_n(&log_owner_generation_active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&log_owner_thread, -1, __ATOMIC_RELEASE);
}

/*
 * Release resources only after the worker is absent or known to have ended and
 * the current owner callback has been unregistered.  Failed kernel deletions
 * retain their handles so a later uvdb_debugnet_stop() can retry them.
 */
static int debugnet_cleanup_inactive_resources(void)
{
    __atomic_fetch_or(&log_writer_gate, UVDB_DEBUGNET_GATE_CLOSED,
                      __ATOMIC_ACQ_REL);
    debugnet_invalidate_owner_handler();

    if(debugnet_disarm_emergency_gate() < 0)
    {
        __atomic_store_n(&log_state, UVDB_DEBUGNET_EXITED,
                         __ATOMIC_RELEASE);
        return -1;
    }

    int phase = __atomic_load_n(&log_thread_phase, __ATOMIC_ACQUIRE);
    if(phase == UVDB_DEBUGNET_THREAD_STARTED ||
       __atomic_load_n(&log_owner_event, __ATOMIC_ACQUIRE) >= 0)
    {
        __atomic_store_n(&log_state, UVDB_DEBUGNET_EXITED,
                         __ATOMIC_RELEASE);
        return -1;
    }

    int cleanup_failed = 0;
    if(__atomic_load_n(&log_skip_socket_close, __ATOMIC_ACQUIRE))
    {
        if(__atomic_load_n(&log_socket, __ATOMIC_ACQUIRE) !=
           UVDB_DEBUGNET_SOCKET_INVALID)
            cleanup_failed = 1;
    }
    else if(debugnet_close_socket() < 0)
    {
        cleanup_failed = 1;
    }

    if(log_semaphore >= 0)
    {
        if(sceKernelDeleteSema(log_semaphore) < 0)
            cleanup_failed = 1;
        else
            __atomic_store_n(&log_semaphore, -1, __ATOMIC_RELEASE);
    }

    free(log_queue);
    log_queue = NULL;
    log_head = 0;
    log_tail = 0;
    log_count = 0;
    __atomic_store_n(&log_stats.queued, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&log_stop_ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&log_level, UVDB_LOG_NONE, __ATOMIC_RELEASE);

    int complete =
        log_thread < 0 &&
        phase == UVDB_DEBUGNET_THREAD_NONE &&
        __atomic_load_n(&log_owner_event, __ATOMIC_ACQUIRE) < 0 &&
        __atomic_load_n(&log_semaphore, __ATOMIC_ACQUIRE) < 0 &&
        __atomic_load_n(&log_socket, __ATOMIC_ACQUIRE) ==
            UVDB_DEBUGNET_SOCKET_INVALID;
    if(complete && !cleanup_failed)
    {
        __atomic_store_n(&log_owner_exited, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&log_state, UVDB_DEBUGNET_STOPPED,
                         __ATOMIC_RELEASE);
        return 0;
    }

    __atomic_store_n(&log_state, UVDB_DEBUGNET_EXITED, __ATOMIC_RELEASE);
    return -1;
}

static int debugnet_reap_failed_start(SceUID thread)
{
    int phase = __atomic_load_n(&log_thread_phase, __ATOMIC_ACQUIRE);
    if(phase == UVDB_DEBUGNET_THREAD_STARTED)
    {
        int status = 0;
        unsigned int timeout = UVDB_DEBUGNET_STOP_TIMEOUT_US;
        if(sceKernelWaitThreadEnd(thread, &status, &timeout) < 0)
            return -1;
        __atomic_store_n(&log_thread_phase, UVDB_DEBUGNET_THREAD_ENDED,
                         __ATOMIC_RELEASE);
        phase = UVDB_DEBUGNET_THREAD_ENDED;
    }
    if(phase != UVDB_DEBUGNET_THREAD_CREATED &&
       phase != UVDB_DEBUGNET_THREAD_ENDED)
        return -1;

    if(sceKernelDeleteThread(thread) < 0)
    {
        if(__atomic_load_n(&log_owner_event, __ATOMIC_ACQUIRE) < 0)
            (void)debugnet_cleanup_inactive_resources();
        return -1;
    }
    log_thread = -1;
    __atomic_store_n(&log_thread_phase, UVDB_DEBUGNET_THREAD_NONE,
                     __ATOMIC_RELEASE);
    return debugnet_cleanup_inactive_resources();
}

static int debugnet_wait_for_writers(void)
{
    SceInt64 deadline = sceKernelGetSystemTimeWide() +
                        UVDB_DEBUGNET_WRITER_TIMEOUT_US;
    while((__atomic_load_n(&log_writer_gate, __ATOMIC_ACQUIRE) &
           UVDB_DEBUGNET_GATE_WRITERS) != 0)
    {
        if(sceKernelGetSystemTimeWide() >= deadline)
            return -1;
        sceKernelDelayThread(100);
    }
    return 0;
}

static int debugnet_wait_for_owner_callback(void)
{
    SceInt64 deadline = sceKernelGetSystemTimeWide() +
                        UVDB_DEBUGNET_WRITER_TIMEOUT_US;
    while(__atomic_load_n(&log_owner_callback_active, __ATOMIC_ACQUIRE) != 0)
    {
        if(sceKernelGetSystemTimeWide() >= deadline)
            return -1;
        sceKernelDelayThread(100);
    }
    return 0;
}

static void debugnet_force_process_exit(void)
{
#ifdef UVDB_DEBUGNET_TESTING
    uvdb_debugnet_test_exit_process(0);
#else
    sceKernelExitProcess(0);
#endif
}

/*
 * This runs from newlib's exit-handler list after .fini_array destructors.
 * Networking may already be unavailable, so it never drains the queue and
 * instructs the worker to skip socket syscalls.  It serializes with an
 * in-progress lifecycle operation, waits out accepted producers, and joins
 * the sender using fixed timeouts.  If any of those safety proofs fail, the
 * only safe option is direct process termination before newlib releases the
 * heap beneath a still-running worker.
 */
static void debugnet_process_exit_handler(void)
{
    __atomic_store_n(&log_process_exiting, 1, __ATOMIC_RELEASE);
    debugnet_request_emergency_stop(1);

    SceInt64 deadline = sceKernelGetSystemTimeWide() +
                        UVDB_DEBUGNET_EXIT_LOCK_TIMEOUT_US;
    while(!debugnet_try_lifecycle_lock())
    {
        debugnet_request_emergency_stop(1);
        if(sceKernelGetSystemTimeWide() >= deadline)
        {
            debugnet_force_process_exit();
            return;
        }
        sceKernelDelayThread(100);
    }

    debugnet_request_emergency_stop(1);
    if(debugnet_wait_for_writers() < 0)
    {
        debugnet_unlock_lifecycle();
        debugnet_force_process_exit();
        return;
    }

    __atomic_store_n(&log_stop_ready, 1, __ATOMIC_RELEASE);
    SceUID semaphore = __atomic_load_n(&log_semaphore, __ATOMIC_ACQUIRE);
    if(semaphore >= 0)
        sceKernelSignalSema(semaphore, 1);

    SceUID thread = log_thread;
    int phase = __atomic_load_n(&log_thread_phase, __ATOMIC_ACQUIRE);
    if(thread >= 0 && phase == UVDB_DEBUGNET_THREAD_STARTED)
    {
        int status = 0;
        unsigned int timeout = UVDB_DEBUGNET_STOP_TIMEOUT_US;
        if(sceKernelWaitThreadEnd(thread, &status, &timeout) < 0)
        {
            debugnet_unlock_lifecycle();
            debugnet_force_process_exit();
            return;
        }
        __atomic_store_n(&log_thread_phase, UVDB_DEBUGNET_THREAD_ENDED,
                         __ATOMIC_RELEASE);
    }

    debugnet_unlock_lifecycle();
}

static int debugnet_unregister_owner_event(void)
{
    SceUID event = __atomic_load_n(&log_owner_event, __ATOMIC_ACQUIRE);
    debugnet_invalidate_owner_handler();
    if(event < 0)
        return debugnet_wait_for_owner_callback();

    int result = sceKernelUnregisterThreadEventHandler(event);
    if(result < 0 &&
       result != (int)SCE_KERNEL_ERROR_UNKNOWN_THREAD_EVENT_ID)
        return -1;

    /* Unknown means no registered object remains; stale callbacks are gated. */
    __atomic_store_n(&log_owner_event, -1, __ATOMIC_RELEASE);
    return debugnet_wait_for_owner_callback();
}

int uvdb_debugnet_start(const struct uvdb_debugnet_config* config)
{
    if(!config || !config->server_ip || !config->server_ip[0] || !config->port ||
       (unsigned int)config->level > (unsigned int)UVDB_LOG_TRACE)
        return -1;

    struct sockaddr_in destination;
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(config->port);
    if(inet_pton(AF_INET, config->server_ip, &destination.sin_addr) != 1)
        return -1;

    if(!debugnet_try_lifecycle_lock())
        return -1;
    if(__atomic_load_n(&log_process_exiting, __ATOMIC_ACQUIRE) ||
       __atomic_load_n(&log_state, __ATOMIC_ACQUIRE) != UVDB_DEBUGNET_STOPPED)
    {
        debugnet_unlock_lifecycle();
        return -1;
    }
    if(debugnet_ensure_process_exit_hook() < 0)
    {
        debugnet_unlock_lifecycle();
        return -1;
    }
    __atomic_store_n(&log_state, UVDB_DEBUGNET_STARTING, __ATOMIC_RELEASE);
    unsigned int owner_generation =
        __atomic_load_n(&log_owner_generation_counter, __ATOMIC_RELAXED) + 1u;
    if(owner_generation == 0)
        owner_generation = 1;
    __atomic_store_n(&log_owner_generation_counter, owner_generation,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&log_owner_generation_active, owner_generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&log_owner_thread, sceKernelGetThreadId(),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&log_owner_event, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&log_owner_exited, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&log_skip_socket_close, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&log_emergency_gate,
                     UVDB_DEBUGNET_EMERGENCY_DISARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&log_thread_phase, UVDB_DEBUGNET_THREAD_NONE,
                     __ATOMIC_RELEASE);
    unsigned int old_gate =
        __atomic_load_n(&log_writer_gate, __ATOMIC_ACQUIRE);
    unsigned int generation =
        ((old_gate & UVDB_DEBUGNET_GATE_GENERATION) + 0x00000100u) &
        UVDB_DEBUGNET_GATE_GENERATION;
    if(!generation)
        generation = 0x00000100u;
    __atomic_store_n(&log_writer_gate,
                     generation | UVDB_DEBUGNET_GATE_CLOSED,
                     __ATOMIC_RELEASE);

    struct uvdb_debugnet_message* queue =
        calloc(UVDB_DEBUGNET_QUEUE_SLOTS, sizeof(*queue));
    if(!queue)
    {
        (void)debugnet_cleanup_inactive_resources();
        debugnet_unlock_lifecycle();
        return -1;
    }
    log_queue = queue;

    SceUID semaphore = sceKernelCreateSema("uvdb debugnet", 0, 0, 1, NULL);
    if(semaphore < 0)
    {
        (void)debugnet_cleanup_inactive_resources();
        debugnet_unlock_lifecycle();
        return -1;
    }
    __atomic_store_n(&log_semaphore, semaphore, __ATOMIC_RELEASE);
    __atomic_store_n(&log_emergency_gate, UVDB_DEBUGNET_EMERGENCY_ARMED,
                     __ATOMIC_RELEASE);

    int socket = sceNetSyscallSocket("uvdb debugnet", AF_INET, SOCK_DGRAM, 0);
    if(socket < 0)
    {
        (void)debugnet_cleanup_inactive_resources();
        debugnet_unlock_lifecycle();
        return -1;
    }

    debugnet_reset_stats();
    log_head = 0;
    log_tail = 0;
    log_count = 0;
    __atomic_store_n(&log_stop_ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&log_queue_lock, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&log_level, config->level, __ATOMIC_RELEASE);
    __atomic_store_n(&log_socket, socket, __ATOMIC_RELEASE);
    log_destination = destination;

    SceUID thread = sceKernelCreateThread("uvdb debugnet", debugnet_thread_main,
                                          0x10000100,
                                          UVDB_DEBUGNET_THREAD_STACK,
                                          0, 0, NULL);
    if(thread < 0)
    {
        (void)debugnet_cleanup_inactive_resources();
        debugnet_unlock_lifecycle();
        return -1;
    }
    log_thread = thread;
    __atomic_store_n(&log_thread_phase, UVDB_DEBUGNET_THREAD_CREATED,
                     __ATOMIC_RELEASE);
    if(sceKernelStartThread(thread, 0, NULL) < 0)
    {
        if(sceKernelDeleteThread(thread) == 0)
        {
            log_thread = -1;
            __atomic_store_n(&log_thread_phase, UVDB_DEBUGNET_THREAD_NONE,
                             __ATOMIC_RELEASE);
        }
        (void)debugnet_cleanup_inactive_resources();
        debugnet_unlock_lifecycle();
        return -1;
    }
    __atomic_store_n(&log_thread_phase, UVDB_DEBUGNET_THREAD_STARTED,
                     __ATOMIC_RELEASE);

    int expected_state = UVDB_DEBUGNET_STARTING;
    if(!__atomic_compare_exchange_n(&log_state, &expected_state,
                                    UVDB_DEBUGNET_RUNNING, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        debugnet_invalidate_owner_handler();
        __atomic_store_n(&log_stop_ready, 1, __ATOMIC_RELEASE);
        sceKernelSignalSema(semaphore, 1);
        (void)debugnet_reap_failed_start(thread);
        debugnet_unlock_lifecycle();
        return -1;
    }
    SceUID owner_event = sceKernelRegisterThreadEventHandler(
        "uvdb debugnet owner",
        __atomic_load_n(&log_owner_thread, __ATOMIC_ACQUIRE),
        SCE_KERNEL_THREAD_EVENT_TYPE_EXIT,
        debugnet_owner_thread_event,
        (void*)(uintptr_t)owner_generation);
    if(owner_event < 0)
    {
        debugnet_invalidate_owner_handler();
        __atomic_store_n(&log_state, UVDB_DEBUGNET_STOPPING,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&log_stop_ready, 1, __ATOMIC_RELEASE);
        sceKernelSignalSema(semaphore, 1);
        (void)debugnet_reap_failed_start(thread);
        debugnet_unlock_lifecycle();
        return -1;
    }
    __atomic_store_n(&log_owner_event, owner_event, __ATOMIC_RELEASE);
    __atomic_store_n(&log_writer_gate, generation, __ATOMIC_RELEASE);
    if(__atomic_load_n(&log_state, __ATOMIC_ACQUIRE) !=
       UVDB_DEBUGNET_RUNNING)
    {
        __atomic_fetch_or(&log_writer_gate, UVDB_DEBUGNET_GATE_CLOSED,
                          __ATOMIC_ACQ_REL);
        if(debugnet_wait_for_writers() == 0)
        {
            if(debugnet_unregister_owner_event() == 0)
                (void)debugnet_reap_failed_start(thread);
        }
        debugnet_unlock_lifecycle();
        return -1;
    }
    debugnet_unlock_lifecycle();
    uvdb_debugnet_write(UVDB_LOG_INFO, "debugnet initialized\n");
    return 0;
}

static int debugnet_enqueue(const char* data, size_t size, int truncated)
{
    if(!debugnet_try_queue_lock())
    {
        __atomic_add_fetch(&log_stats.dropped, 1, __ATOMIC_RELAXED);
        return -1;
    }
    if(__atomic_load_n(&log_state, __ATOMIC_ACQUIRE) !=
           UVDB_DEBUGNET_RUNNING ||
       !log_queue)
    {
        debugnet_unlock_queue();
        return -1;
    }
    if(log_count == UVDB_DEBUGNET_QUEUE_SLOTS)
    {
        __atomic_add_fetch(&log_stats.dropped, 1, __ATOMIC_RELAXED);
        debugnet_unlock_queue();
        return -1;
    }

    struct uvdb_debugnet_message* message = &log_queue[log_tail];
    memcpy(message->data, data, size);
    message->data[size] = 0;
    message->size = (unsigned short)size;
    log_tail = (log_tail + 1) % UVDB_DEBUGNET_QUEUE_SLOTS;
    log_count++;
    __atomic_store_n(&log_stats.queued, log_count, __ATOMIC_RELAXED);
    if(truncated)
        __atomic_add_fetch(&log_stats.truncated, 1, __ATOMIC_RELAXED);
    SceUID semaphore =
        __atomic_load_n(&log_semaphore, __ATOMIC_ACQUIRE);
    debugnet_unlock_queue();
    if(semaphore >= 0)
        sceKernelSignalSema(semaphore, 1);
    return truncated ? 2 : 0;
}

static void debugnet_end_write(void)
{
    __atomic_sub_fetch(&log_writer_gate, 1, __ATOMIC_RELEASE);
}

static int debugnet_begin_write(enum uvdb_log_level level)
{
    unsigned int gate =
        __atomic_load_n(&log_writer_gate, __ATOMIC_ACQUIRE);
    unsigned int generation = gate & UVDB_DEBUGNET_GATE_GENERATION;
    if(gate & UVDB_DEBUGNET_GATE_CLOSED)
        return -1;
    if(level <= UVDB_LOG_NONE || level > UVDB_LOG_TRACE ||
       __atomic_load_n(&log_state, __ATOMIC_ACQUIRE) !=
           UVDB_DEBUGNET_RUNNING)
        return -1;
    if(level > __atomic_load_n(&log_level, __ATOMIC_ACQUIRE))
        return 1;

    for(;;)
    {
        if((gate & UVDB_DEBUGNET_GATE_CLOSED) ||
           (gate & UVDB_DEBUGNET_GATE_WRITERS) ==
               UVDB_DEBUGNET_GATE_WRITERS)
            return -1;
        unsigned int desired = gate + 1;
        if(__atomic_compare_exchange_n(&log_writer_gate, &gate, desired, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        {
            if(__atomic_load_n(&log_state, __ATOMIC_ACQUIRE) ==
               UVDB_DEBUGNET_RUNNING)
                return 0;
            debugnet_end_write();
            return -1;
        }
        if((gate & UVDB_DEBUGNET_GATE_GENERATION) != generation)
            return -1;
    }
}

int uvdb_debugnet_write(enum uvdb_log_level level, const char* text)
{
    if(!text)
        return -1;
    int check = debugnet_begin_write(level);
    if(check)
        return check;

    char packet[UVDB_DEBUGNET_MESSAGE_MAX];
    int prefix = snprintf(packet, sizeof(packet), "[VITA][%s]: ",
                          debugnet_level_name(level));
    if(prefix < 0)
    {
        debugnet_end_write();
        return -1;
    }
    size_t used = (size_t)prefix;
    if(used >= sizeof(packet))
    {
        debugnet_end_write();
        return -1;
    }
    size_t remaining = sizeof(packet) - 1 - used;
    size_t text_size = strnlen(text, remaining + 1);
    int truncated = text_size > remaining;
    if(truncated)
        text_size = remaining;
    memcpy(packet + used, text, text_size);
    used += text_size;
    packet[used] = 0;
    int result = debugnet_enqueue(packet, used, truncated);
    debugnet_end_write();
    return result;
}

int uvdb_debugnet_printf(enum uvdb_log_level level, const char* format, ...)
{
    if(!format)
        return -1;
    int check = debugnet_begin_write(level);
    if(check)
        return check;

    char packet[UVDB_DEBUGNET_MESSAGE_MAX];
    int prefix = snprintf(packet, sizeof(packet), "[VITA][%s]: ",
                          debugnet_level_name(level));
    if(prefix < 0)
    {
        debugnet_end_write();
        return -1;
    }
    size_t used = (size_t)prefix;
    if(used >= sizeof(packet))
    {
        debugnet_end_write();
        return -1;
    }
    size_t remaining = sizeof(packet) - used;

    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(packet + used, remaining, format, arguments);
    va_end(arguments);
    if(result < 0)
    {
        debugnet_end_write();
        return -1;
    }
    int truncated = (size_t)result >= remaining;
    used += truncated ? remaining - 1 : (size_t)result;
    result = debugnet_enqueue(packet, used, truncated);
    debugnet_end_write();
    return result;
}

int uvdb_debugnet_get_stats(struct uvdb_debugnet_stats* stats)
{
    if(!stats)
        return -1;
    stats->queued = __atomic_load_n(&log_stats.queued, __ATOMIC_RELAXED);
    stats->sent = __atomic_load_n(&log_stats.sent, __ATOMIC_RELAXED);
    stats->dropped = __atomic_load_n(&log_stats.dropped, __ATOMIC_RELAXED);
    stats->truncated = __atomic_load_n(&log_stats.truncated, __ATOMIC_RELAXED);
    stats->send_errors =
        __atomic_load_n(&log_stats.send_errors, __ATOMIC_RELAXED);
    stats->last_send_error =
        __atomic_load_n(&log_stats.last_send_error, __ATOMIC_RELAXED);
    return 0;
}

int uvdb_debugnet_stop(void)
{
    if(!debugnet_try_lifecycle_lock())
        return -1;
    int state = __atomic_load_n(&log_state, __ATOMIC_ACQUIRE);
    if(state == UVDB_DEBUGNET_STOPPED)
    {
        debugnet_unlock_lifecycle();
        return 0;
    }
    if(state == UVDB_DEBUGNET_STARTING)
    {
        debugnet_unlock_lifecycle();
        return -1;
    }

    SceUID thread = log_thread;
    if(thread == sceKernelGetThreadId())
    {
        debugnet_unlock_lifecycle();
        return -1;
    }
    __atomic_fetch_or(&log_writer_gate, UVDB_DEBUGNET_GATE_CLOSED,
                      __ATOMIC_ACQ_REL);
    if(state == UVDB_DEBUGNET_RUNNING)
    {
        __atomic_store_n(&log_state, UVDB_DEBUGNET_STOPPING,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&log_stop_ready, 0, __ATOMIC_RELEASE);
        SceUID semaphore =
            __atomic_load_n(&log_semaphore, __ATOMIC_ACQUIRE);
        if(semaphore >= 0)
            sceKernelSignalSema(semaphore, 1);
    }

    if(debugnet_wait_for_writers() < 0)
    {
        debugnet_unlock_lifecycle();
        return -1;
    }
    __atomic_store_n(&log_stop_ready, 1, __ATOMIC_RELEASE);
    SceUID stop_semaphore =
        __atomic_load_n(&log_semaphore, __ATOMIC_ACQUIRE);
    if(stop_semaphore >= 0)
        sceKernelSignalSema(stop_semaphore, 1);

    if(debugnet_disarm_emergency_gate() < 0)
    {
        debugnet_unlock_lifecycle();
        return -1;
    }

    if(thread >= 0)
    {
        int phase = __atomic_load_n(&log_thread_phase, __ATOMIC_ACQUIRE);
        if(phase == UVDB_DEBUGNET_THREAD_STARTED)
        {
            int status = 0;
            unsigned int timeout = UVDB_DEBUGNET_STOP_TIMEOUT_US;
            if(sceKernelWaitThreadEnd(thread, &status, &timeout) < 0)
            {
                debugnet_unlock_lifecycle();
                return -1;
            }
            __atomic_store_n(&log_thread_phase, UVDB_DEBUGNET_THREAD_ENDED,
                             __ATOMIC_RELEASE);
            phase = UVDB_DEBUGNET_THREAD_ENDED;
        }
        if(phase != UVDB_DEBUGNET_THREAD_CREATED &&
           phase != UVDB_DEBUGNET_THREAD_ENDED)
        {
            debugnet_unlock_lifecycle();
            return -1;
        }
        if(sceKernelDeleteThread(thread) < 0)
        {
            debugnet_unlock_lifecycle();
            return -1;
        }
        log_thread = -1;
        __atomic_store_n(&log_thread_phase, UVDB_DEBUGNET_THREAD_NONE,
                         __ATOMIC_RELEASE);
    }

    if(debugnet_unregister_owner_event() < 0)
    {
        debugnet_unlock_lifecycle();
        return -1;
    }

    int result = debugnet_cleanup_inactive_resources();
    debugnet_unlock_lifecycle();
    return result;
}

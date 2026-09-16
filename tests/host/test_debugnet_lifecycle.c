#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/net/net_syscalls.h>

#include "uvdb.h"

#define FAKE_SEMAPHORE_UID 0x201
#define FAKE_WORKER_UID 0x301
#define FAKE_WRITER_UID 0x302
#define FAKE_EVENT_UID 0x401
#define FAKE_SOCKET_UID 0x501

struct fake_kernel {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int semaphore_alive;
    int semaphore_count;
    int worker_created;
    int worker_started;
    int worker_joined;
    int worker_deleted;
    int worker_ended;
    int start_failures;
    int wait_call_count;
    pthread_t worker;
    SceKernelThreadEntry entry;
    SceKernelThreadEventHandler event_handler;
    void* event_common;
    SceUID event_target;
    int event_mask;
    int event_active;
    int fail_event_registration;
    int wait_failures;
    int delete_failures;
    int unregister_failures;
    int unregister_error;
    int trigger_owner_exit_during_unregister;
    int defer_owner_exit_after_unregister;
    SceKernelThreadEventHandler delayed_event_handler;
    void* delayed_event_common;
    SceUID delayed_event_target;
    int owner_exit_delivered;
    int event_register_count;
    int event_unregister_count;
    int callback_signal_count;
    int callback_forbidden_calls;
    int block_writer_signal;
    int writer_signal_blocked;
    int release_writer_signal;
    int block_worker_wait;
    int worker_wait_blocked;
    int release_worker_wait;
    int exit_hook_entered;
    int exit_hook_returned;
    unsigned int last_wait_timeout;
    SceInt64 time_step_us;
    int socket_open;
    int close_failures;
    int close_call_count;
    int socket_close_count;
    int send_count;
};

struct fake_exit_runtime {
    void (*handler)(void);
    int registration_attempts;
    int registration_failures;
    int registration_count;
    int invocation_count;
    int forced_exit_count;
    int forced_exit_status;
};

static struct fake_kernel fake;
static struct fake_exit_runtime fake_exit;
static _Thread_local SceUID fake_current_thread = 0x101;
static _Thread_local int fake_inside_callback;
static SceInt64 fake_time_us;

static void fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static void check(int condition, const char* message)
{
    if(!condition)
        fail(message);
}

static void fake_forbid_in_callback(void)
{
    if(fake_inside_callback)
        __atomic_add_fetch(&fake.callback_forbidden_calls, 1,
                           __ATOMIC_RELAXED);
}

static void fake_delay(unsigned int delay_us)
{
    usleep(delay_us);
}

static int wait_for_value(const int* value, int expected)
{
    for(unsigned int elapsed = 0; elapsed < 1000000u; elapsed += 1000u)
    {
        if(__atomic_load_n(value, __ATOMIC_ACQUIRE) == expected)
            return 0;
        fake_delay(1000);
    }
    return -1;
}

static void reset_fake(void)
{
    memset(&fake, 0, sizeof(fake));
    check(pthread_mutex_init(&fake.mutex, NULL) == 0,
          "initialize fake mutex");
    check(pthread_cond_init(&fake.condition, NULL) == 0,
          "initialize fake condition");
    fake_current_thread = 0x101;
    fake_time_us = 0;
    fake.time_step_us = 100;
}

static void destroy_fake(void)
{
    check(!fake.semaphore_alive, "logger deleted fake semaphore");
    check(!fake.socket_open, "logger closed fake socket");
    check(!fake.event_active, "logger released fake owner event");
    check(fake.delayed_event_handler == NULL,
          "no delayed owner callback remains queued");
    pthread_cond_destroy(&fake.condition);
    pthread_mutex_destroy(&fake.mutex);
}

static void* fake_worker_main(void* unused)
{
    (void)unused;
    fake_current_thread = FAKE_WORKER_UID;
    int result = fake.entry(0, NULL);
    __atomic_store_n(&fake.worker_ended, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&fake.mutex);
    pthread_cond_broadcast(&fake.condition);
    pthread_mutex_unlock(&fake.mutex);
    return (void*)(intptr_t)result;
}

static void trigger_owner_exit(void)
{
    check(fake.event_active, "owner exit handler is active");
    check((fake.event_mask & SCE_KERNEL_THREAD_EVENT_TYPE_EXIT) != 0,
          "owner exit handler uses the exit mask");
    fake.owner_exit_delivered = 1;
    fake_inside_callback = 1;
    int result = fake.event_handler(SCE_KERNEL_THREAD_EVENT_TYPE_EXIT,
                                    fake.event_target, 0,
                                    fake.event_common);
    fake_inside_callback = 0;
    check(result == 0, "owner exit callback succeeds");
}

static void trigger_delayed_owner_exit(SceUID delivered_thread)
{
    check(fake.delayed_event_handler != NULL,
          "a delayed owner callback was captured");
    fake_inside_callback = 1;
    int result = fake.delayed_event_handler(
        SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, delivered_thread, 0,
        fake.delayed_event_common);
    fake_inside_callback = 0;
    fake.delayed_event_handler = NULL;
    fake.delayed_event_common = NULL;
    fake.delayed_event_target = -1;
    check(result == 0, "delayed owner exit callback succeeds");
}

int uvdb_debugnet_test_register_exit_handler(void (*handler)(void))
{
    ++fake_exit.registration_attempts;
    if(fake_exit.registration_failures > 0)
    {
        --fake_exit.registration_failures;
        return -1;
    }
    check(handler != NULL, "register a valid process-exit handler");
    check(fake_exit.handler == NULL,
          "register only one process-exit handler");
    fake_exit.handler = handler;
    ++fake_exit.registration_count;
    return 0;
}

static void invoke_process_exit_handler(void)
{
    check(fake_exit.handler != NULL,
          "a process-exit handler was registered");
    ++fake_exit.invocation_count;
    fake_exit.handler();
}

static void* fake_process_exit_main(void* unused)
{
    (void)unused;
    fake_current_thread = 0x101;
    __atomic_store_n(&fake.exit_hook_entered, 1, __ATOMIC_RELEASE);
    invoke_process_exit_handler();
    __atomic_store_n(&fake.exit_hook_returned, 1, __ATOMIC_RELEASE);
    return NULL;
}

void uvdb_debugnet_test_exit_process(int status)
{
    ++fake_exit.forced_exit_count;
    fake_exit.forced_exit_status = status;
}

uint16_t htons(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

int inet_pton(int address_family, const char* source, void* destination)
{
    if(address_family != 2 || !source || strcmp(source, "127.0.0.1") != 0)
        return 0;
    ((uint32_t*)destination)[0] = 0x0100007fu;
    return 1;
}

SceUID sceKernelCreateSema(const char* name, SceUInt attr, int initial,
                           int maximum, void* option)
{
    (void)name;
    (void)attr;
    (void)maximum;
    (void)option;
    fake_forbid_in_callback();
    check(!fake.semaphore_alive, "only one fake semaphore is live");
    fake.semaphore_alive = 1;
    fake.semaphore_count = initial;
    return FAKE_SEMAPHORE_UID;
}

int sceKernelDeleteSema(SceUID semaphore)
{
    fake_forbid_in_callback();
    check(semaphore == FAKE_SEMAPHORE_UID && fake.semaphore_alive,
          "delete the live fake semaphore");
    fake.semaphore_alive = 0;
    return 0;
}

int sceKernelSignalSema(SceUID semaphore, int signal)
{
    check(semaphore == FAKE_SEMAPHORE_UID && signal == 1,
          "signal the logger semaphore once");
    if(fake_inside_callback)
        __atomic_add_fetch(&fake.callback_signal_count, 1,
                           __ATOMIC_RELAXED);
    pthread_mutex_lock(&fake.mutex);
    if(__atomic_load_n(&fake.block_writer_signal, __ATOMIC_ACQUIRE) &&
       fake_current_thread == FAKE_WRITER_UID)
    {
        __atomic_store_n(&fake.writer_signal_blocked, 1, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&fake.condition);
        while(!__atomic_load_n(&fake.release_writer_signal,
                               __ATOMIC_ACQUIRE))
            pthread_cond_wait(&fake.condition, &fake.mutex);
    }
    fake.semaphore_count = 1;
    pthread_cond_signal(&fake.condition);
    pthread_mutex_unlock(&fake.mutex);
    return 0;
}

int sceKernelWaitSema(SceUID semaphore, int signal, SceUInt* timeout)
{
    (void)timeout;
    fake_forbid_in_callback();
    check(semaphore == FAKE_SEMAPHORE_UID && signal == 1,
          "wait on the logger semaphore once");
    pthread_mutex_lock(&fake.mutex);
    if(__atomic_load_n(&fake.block_worker_wait, __ATOMIC_ACQUIRE) &&
       fake_current_thread == FAKE_WORKER_UID)
    {
        __atomic_store_n(&fake.worker_wait_blocked, 1, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&fake.condition);
        while(!__atomic_load_n(&fake.release_worker_wait,
                               __ATOMIC_ACQUIRE))
            pthread_cond_wait(&fake.condition, &fake.mutex);
    }
    while(fake.semaphore_alive && fake.semaphore_count == 0)
        pthread_cond_wait(&fake.condition, &fake.mutex);
    if(!fake.semaphore_alive)
    {
        pthread_mutex_unlock(&fake.mutex);
        return -1;
    }
    fake.semaphore_count = 0;
    pthread_mutex_unlock(&fake.mutex);
    return 0;
}

SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry entry,
                             int priority, SceSize stack_size,
                             unsigned int attributes, int cpu_affinity,
                             const void* option)
{
    (void)name;
    (void)priority;
    (void)stack_size;
    (void)attributes;
    (void)cpu_affinity;
    (void)option;
    fake_forbid_in_callback();
    check(!fake.worker_created, "only one fake worker is live");
    fake.entry = entry;
    fake.worker_created = 1;
    return FAKE_WORKER_UID;
}

int sceKernelStartThread(SceUID thread, SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    fake_forbid_in_callback();
    check(thread == FAKE_WORKER_UID && fake.worker_created,
          "start the created fake worker");
    if(fake.start_failures > 0)
    {
        --fake.start_failures;
        return -4;
    }
    fake.worker_started = 1;
    return pthread_create(&fake.worker, NULL, fake_worker_main, NULL) == 0
               ? 0
               : -1;
}

int sceKernelWaitThreadEnd(SceUID thread, int* status, SceUInt* timeout)
{
    fake_forbid_in_callback();
    ++fake.wait_call_count;
    check(thread == FAKE_WORKER_UID && fake.worker_started,
          "join the started fake worker");
    if(timeout)
        fake.last_wait_timeout = *timeout;
    if(fake.wait_failures > 0)
    {
        --fake.wait_failures;
        return -2;
    }

    unsigned int remaining = timeout ? *timeout : 0;
    while(!__atomic_load_n(&fake.worker_ended, __ATOMIC_ACQUIRE))
    {
        if(timeout && remaining == 0)
            return -2;
        unsigned int slice = !timeout || remaining > 1000u ? 1000u : remaining;
        fake_delay(slice);
        if(timeout)
        {
            remaining -= slice;
            *timeout = remaining;
        }
    }

    void* result = NULL;
    if(!fake.worker_joined)
    {
        if(pthread_join(fake.worker, &result) != 0)
            return -1;
        fake.worker_joined = 1;
    }
    if(status)
        *status = (int)(intptr_t)result;
    return 0;
}

int sceKernelDeleteThread(SceUID thread)
{
    fake_forbid_in_callback();
    check(thread == FAKE_WORKER_UID && fake.worker_created,
          "delete the fake worker object");
    if(fake.delete_failures > 0)
    {
        --fake.delete_failures;
        return -3;
    }
    check(!fake.worker_started || fake.worker_joined,
          "join a started worker before deletion");
    fake.worker_deleted = 1;
    fake.worker_created = 0;
    fake.worker_started = 0;
    fake.worker_joined = 0;
    return 0;
}

SceUID sceKernelGetThreadId(void)
{
    fake_forbid_in_callback();
    return fake_current_thread;
}

int sceKernelDelayThread(SceUInt delay_us)
{
    fake_forbid_in_callback();
    fake_delay(delay_us);
    return 0;
}

SceInt64 sceKernelGetSystemTimeWide(void)
{
    fake_forbid_in_callback();
    return __atomic_add_fetch(&fake_time_us, fake.time_step_us,
                              __ATOMIC_RELAXED);
}

SceUID sceKernelRegisterThreadEventHandler(
    const char* name, SceUID thread, SceInt32 event_mask,
    SceKernelThreadEventHandler handler, void* common)
{
    (void)name;
    fake_forbid_in_callback();
    ++fake.event_register_count;
    if(fake.fail_event_registration)
        return -1;
    check(!fake.event_active, "only one fake owner event is active");
    fake.event_target = thread;
    fake.event_mask = event_mask;
    fake.event_handler = handler;
    fake.event_common = common;
    fake.event_active = 1;
    return FAKE_EVENT_UID;
}

SceUID sceKernelUnregisterThreadEventHandler(SceUID event)
{
    fake_forbid_in_callback();
    ++fake.event_unregister_count;
    check(event == FAKE_EVENT_UID && fake.event_active,
          "unregister the active owner event");
    if(fake.trigger_owner_exit_during_unregister)
    {
        fake.trigger_owner_exit_during_unregister = 0;
        trigger_owner_exit();
    }
    if(fake.unregister_failures > 0)
    {
        --fake.unregister_failures;
        if(fake.unregister_error ==
           (int)SCE_KERNEL_ERROR_UNKNOWN_THREAD_EVENT_ID)
            fake.event_active = 0;
        return fake.unregister_error;
    }
    if(fake.defer_owner_exit_after_unregister)
    {
        fake.defer_owner_exit_after_unregister = 0;
        fake.delayed_event_handler = fake.event_handler;
        fake.delayed_event_common = fake.event_common;
        fake.delayed_event_target = fake.event_target;
    }
    fake.event_active = 0;
    return 0;
}

int sceNetSyscallSocket(const char* name, int domain, int type, int protocol)
{
    (void)name;
    (void)domain;
    (void)type;
    (void)protocol;
    fake_forbid_in_callback();
    check(!fake.socket_open, "only one fake socket is live");
    fake.socket_open = 1;
    return FAKE_SOCKET_UID;
}

int sceNetSyscallShutdown(int socket, int how)
{
    (void)how;
    fake_forbid_in_callback();
    check(socket == FAKE_SOCKET_UID && fake.socket_open,
          "shutdown the live fake socket");
    return 0;
}

int sceNetSyscallClose(int socket)
{
    fake_forbid_in_callback();
    check(socket == FAKE_SOCKET_UID && fake.socket_open,
          "close the live fake socket");
    ++fake.close_call_count;
    if(fake.close_failures > 0)
    {
        --fake.close_failures;
        return -5;
    }
    fake.socket_open = 0;
    ++fake.socket_close_count;
    return 0;
}

int sceNetSyscallSendto(SceNetSyscallParameter* parameters)
{
    fake_forbid_in_callback();
    const uint32_t* arguments = (const uint32_t*)parameters;
    ++fake.send_count;
    return (int)arguments[2];
}

static const struct uvdb_debugnet_config config = {
    .server_ip = "127.0.0.1",
    .port = 18194,
    .level = UVDB_LOG_TRACE,
};

static void test_exit_hook_registration_failure(void)
{
    reset_fake();
    fake_exit.registration_failures = 1;
    int attempts = fake_exit.registration_attempts;
    check(uvdb_debugnet_start(&config) == -1,
          "start fails closed when process-exit hook registration fails");
    check(fake_exit.registration_attempts == attempts + 1 &&
              fake_exit.registration_count == 0 &&
              fake_exit.handler == NULL,
          "failed registration installs no process-exit handler");
    check(!fake.semaphore_alive && !fake.socket_open &&
              !fake.worker_created && !fake.event_active,
          "hook failure allocates no logger resources");
    check(uvdb_debugnet_stop() == 0,
          "hook registration failure leaves logger stopped");
    destroy_fake();
}

static void test_exit_hook_registered_once(void)
{
    int attempts = fake_exit.registration_attempts;

    reset_fake();
    check(uvdb_debugnet_start(&config) == 0,
          "first successful start installs process-exit hook");
    check(fake_exit.registration_attempts == attempts + 1 &&
              fake_exit.registration_count == 1 &&
              fake_exit.handler != NULL,
          "process-exit hook is installed exactly once");
    check(uvdb_debugnet_stop() == 0,
          "stop first exit-hook session");
    destroy_fake();

    attempts = fake_exit.registration_attempts;
    reset_fake();
    check(uvdb_debugnet_start(&config) == 0,
          "restart after process-exit hook installation");
    check(fake_exit.registration_attempts == attempts &&
              fake_exit.registration_count == 1,
          "restart does not accumulate process-exit handlers");
    check(uvdb_debugnet_stop() == 0,
          "stop restarted exit-hook session");
    destroy_fake();
}

static void test_registration_is_required(void)
{
    reset_fake();
    fake.fail_event_registration = 1;
    check(uvdb_debugnet_start(&config) == -1,
          "start fails closed when owner event registration fails");
    check(fake.event_register_count == 1,
          "start attempted owner event registration");
    check(fake.worker_deleted, "failed start reaps the worker");
    destroy_fake();
}

static void test_failed_start_wait_retry(void)
{
    reset_fake();
    fake.fail_event_registration = 1;
    fake.wait_failures = 1;
    check(uvdb_debugnet_start(&config) == -1,
          "failed-start wait failure is reported");
    check(fake.worker_created && !fake.worker_deleted,
          "failed-start wait failure retains the worker for retry");
    check(uvdb_debugnet_stop() == 0,
          "stop retries failed-start worker wait and cleanup");
    check(fake.last_wait_timeout == 2000000u,
          "fake WaitThreadEnd receives the bounded Vita timeout");
    destroy_fake();
}

static void test_failed_start_delete_retry(void)
{
    reset_fake();
    fake.fail_event_registration = 1;
    fake.delete_failures = 1;
    check(uvdb_debugnet_start(&config) == -1,
          "failed-start delete failure is reported");
    check(fake.worker_created && fake.worker_joined && !fake.worker_deleted,
           "failed delete retains the ended worker object");
    check(fake.wait_call_count == 1,
          "failed delete follows one successful worker wait");
    check(uvdb_debugnet_stop() == 0,
           "stop retries failed-start worker deletion and cleanup");
    check(fake.wait_call_count == 1,
          "delete retry does not wait for the ended worker twice");
    destroy_fake();
}

static void test_unstarted_worker_delete_retry(void)
{
    reset_fake();
    fake.start_failures = 1;
    fake.delete_failures = 1;
    check(uvdb_debugnet_start(&config) == -1,
          "thread-start plus initial delete failure is reported");
    check(fake.worker_created && !fake.worker_started &&
              !fake.worker_deleted,
           "unstarted worker object is retained for cleanup retry");
    check(!fake.socket_open && !fake.semaphore_alive,
          "failed start releases independent network resources immediately");
    check(fake.wait_call_count == 0,
          "failed start never waits for an unstarted worker");
    check(uvdb_debugnet_stop() == 0,
          "stop retries direct deletion of an unstarted worker");
    check(fake.wait_call_count == 0 && fake.worker_deleted,
          "unstarted retry deletes directly without WaitThreadEnd");
    destroy_fake();
}

static void test_explicit_stop(void)
{
    reset_fake();
    fake_current_thread = 0x111;
    check(uvdb_debugnet_start(&config) == 0, "start explicit-stop session");
    check(fake.event_target == 0x111,
          "event handler targets the starting thread exactly");
    check(fake.event_mask == SCE_KERNEL_THREAD_EVENT_TYPE_EXIT,
          "event handler subscribes only to owner exit");
    check(uvdb_debugnet_printf(UVDB_LOG_INFO, "normal stop\n") == 0,
          "queue an ordinary log message");
    check(uvdb_debugnet_stop() == 0, "explicit stop succeeds");
    check(fake.event_unregister_count == 1,
          "explicit stop unregisters the owner event");
    check(fake.callback_forbidden_calls == 0,
          "explicit stop invokes no owner callback");
    destroy_fake();
}

static void test_unregister_failures_are_retryable(void)
{
    reset_fake();
    check(uvdb_debugnet_start(&config) == 0,
          "start unregister retry session");
    fake.unregister_failures = 1;
    fake.unregister_error = -99;
    check(uvdb_debugnet_stop() == -1,
          "generic unregister failure is reported");
    check(fake.event_active,
          "generic unregister failure retains the handler UID");
    check(uvdb_debugnet_stop() == 0,
          "generic unregister cleanup succeeds on retry");
    destroy_fake();

    reset_fake();
    check(uvdb_debugnet_start(&config) == 0,
          "start unknown-event retry session");
    fake.unregister_failures = 1;
    fake.unregister_error = (int)SCE_KERNEL_ERROR_UNKNOWN_THREAD_EVENT_ID;
    check(uvdb_debugnet_stop() == 0,
          "unknown event confirms no handler object remains");
    check(!fake.event_active,
          "unknown event permits safe cleanup after generation invalidation");
    destroy_fake();
}

static void test_socket_close_failure_is_retryable(void)
{
    reset_fake();
    check(uvdb_debugnet_start(&config) == 0,
          "start socket-close retry session");
    fake.close_failures = 2;
    check(uvdb_debugnet_stop() == -1,
          "worker and stop close failures are reported");
    check(fake.socket_open && fake.close_call_count == 2,
          "failed close retains the live socket descriptor");
    check(uvdb_debugnet_stop() == 0,
          "later stop retries and closes the retained socket");
    check(!fake.socket_open && fake.close_call_count == 3,
          "socket closes exactly once after two retryable failures");
    destroy_fake();
}

static void test_delayed_old_callback_is_generation_gated(void)
{
    reset_fake();
    fake_current_thread = 0x188;
    check(uvdb_debugnet_start(&config) == 0,
          "start delayed-callback source session");
    fake.defer_owner_exit_after_unregister = 1;
    check(uvdb_debugnet_stop() == 0,
          "stop source session while callback remains queued");
    check(fake.delayed_event_handler != NULL,
          "old callback survives unregister in the fake kernel");

    fake.worker_ended = 0;
    fake.worker_deleted = 0;
    fake_current_thread = 0x188;
    check(uvdb_debugnet_start(&config) == 0,
          "restart with a reused owner thread UID");
    trigger_delayed_owner_exit(fake_current_thread);
    check(fake.callback_signal_count == 0,
          "old callback does not signal the new session semaphore");
    check(uvdb_debugnet_write(UVDB_LOG_INFO,
                              "new generation remains live\n") == 0,
          "old callback cannot close the new writer gate");
    check(uvdb_debugnet_stop() == 0,
          "new generation stops normally after delayed callback");
    destroy_fake();
}

static void test_owner_exit_during_explicit_stop(void)
{
    reset_fake();
    check(uvdb_debugnet_start(&config) == 0,
          "start owner-exit versus stop session");
    fake.trigger_owner_exit_during_unregister = 1;
    check(uvdb_debugnet_stop() == 0,
           "owner exit racing event unregistration remains safe");
    check(fake.owner_exit_delivered && fake.callback_signal_count == 0,
          "unregister-time callback is generation gated before signaling");
    check(fake.callback_forbidden_calls == 0,
          "racing owner callback remained nonblocking");
    destroy_fake();
}

struct blocked_writer {
    int result;
};

static void* blocked_writer_main(void* opaque)
{
    struct blocked_writer* writer = opaque;
    fake_current_thread = FAKE_WRITER_UID;
    writer->result =
        uvdb_debugnet_write(UVDB_LOG_TRACE, "blocked writer\n");
    return NULL;
}

static void test_owner_exit_after_writer_wait_timeout(void)
{
    reset_fake();
    check(uvdb_debugnet_start(&config) == 0,
          "start writer-timeout owner-exit session");
    fake.time_step_us = 100000;
    __atomic_store_n(&fake.block_writer_signal, 1, __ATOMIC_RELEASE);

    struct blocked_writer writer = {-1};
    pthread_t writer_thread;
    check(pthread_create(&writer_thread, NULL, blocked_writer_main, &writer) ==
              0,
          "start blocked in-flight writer");
    check(wait_for_value(&fake.writer_signal_blocked, 1) == 0,
          "writer remains in flight through the stop attempt");

    check(uvdb_debugnet_stop() == -1,
          "writer timeout leaves cleanup retryable");
    trigger_owner_exit();
    check(fake.callback_signal_count == 1,
          "owner exit remains armed after the early stop failure");

    pthread_mutex_lock(&fake.mutex);
    __atomic_store_n(&fake.release_writer_signal, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&fake.condition);
    pthread_mutex_unlock(&fake.mutex);
    check(pthread_join(writer_thread, NULL) == 0,
          "join released in-flight writer");
    check(writer.result == 0, "in-flight writer completes its accepted write");
    check(wait_for_value(&fake.worker_ended, 1) == 0,
          "owner exit wakes the worker after the failed stop");
    check(uvdb_debugnet_stop() == 0,
          "cleanup succeeds after owner-exit recovery");
    destroy_fake();
}

struct writer_race {
    int stopped;
    int writes;
};

static void* writer_race_main(void* opaque)
{
    struct writer_race* race = opaque;
    while(!__atomic_load_n(&race->stopped, __ATOMIC_ACQUIRE))
    {
        int result = uvdb_debugnet_write(UVDB_LOG_TRACE, "writer race\n");
        __atomic_add_fetch(&race->writes, 1, __ATOMIC_RELAXED);
        if(result < 0)
            break;
    }
    return NULL;
}

static void test_owner_exit_with_inflight_writers(void)
{
    reset_fake();
    check(uvdb_debugnet_start(&config) == 0,
          "start in-flight writer session");
    struct writer_race race = {0};
    pthread_t writers[4];
    for(size_t index = 0; index < 4; ++index)
        check(pthread_create(&writers[index], NULL, writer_race_main, &race) ==
                  0,
              "start concurrent log writer");
    fake_delay(5000);
    trigger_owner_exit();
    __atomic_store_n(&race.stopped, 1, __ATOMIC_RELEASE);
    for(size_t index = 0; index < 4; ++index)
        check(pthread_join(writers[index], NULL) == 0,
              "join concurrent log writer");
    check(race.writes > 0, "concurrent writers exercised queue/gate races");
    check(wait_for_value(&fake.worker_ended, 1) == 0,
          "worker exits after concurrent owner-exit race");
    fake_current_thread = 0x177;
    check(uvdb_debugnet_stop() == 0,
          "reap owner-exit session after concurrent writers");
    check(fake.callback_forbidden_calls == 0,
          "writer race does not expand callback work");
    destroy_fake();
}

static void test_owner_exit_emergency_stop(void)
{
    reset_fake();
    fake_current_thread = 0x121;
    check(uvdb_debugnet_start(&config) == 0, "start owner-exit session");
    check(uvdb_debugnet_write(UVDB_LOG_DEBUG, "before owner exit\n") == 0,
          "queue before owner exit");

    trigger_owner_exit();
    check(fake.callback_signal_count == 1,
          "owner callback wakes the sender exactly once");
    check(fake.callback_forbidden_calls == 0,
          "owner callback performs no wait, close, free, or thread cleanup");
    check(uvdb_debugnet_write(UVDB_LOG_INFO, "after owner exit\n") == -1,
          "owner exit closes the writer gate immediately");
    check(wait_for_value(&fake.worker_ended, 1) == 0,
          "worker exits after its owner exits");
    check(fake.socket_close_count == 1,
          "worker closes the socket after owner exit");

    fake.unregister_failures = 1;
    fake.unregister_error = (int)SCE_KERNEL_ERROR_UNKNOWN_THREAD_EVENT_ID;
    fake_current_thread = 0x122;
    check(uvdb_debugnet_stop() == 0,
          "another thread reaps an exited-owner session");
    check(fake.worker_deleted, "exited-owner worker object is deleted");
    check(fake.event_unregister_count == 1,
          "cleanup attempts to unregister an auto-retired exit handler");
    destroy_fake();
}

static void test_restart_after_owner_exit(void)
{
    reset_fake();
    fake_current_thread = 0x131;
    check(uvdb_debugnet_start(&config) == 0, "start first restart session");
    trigger_owner_exit();
    check(wait_for_value(&fake.worker_ended, 1) == 0,
          "first restart worker exits");
    fake_current_thread = 0x132;
    check(uvdb_debugnet_stop() == 0, "reap first restart session");

    fake.worker_ended = 0;
    fake.worker_deleted = 0;
    fake.owner_exit_delivered = 0;
    check(uvdb_debugnet_start(&config) == 0,
          "logger restarts after exited-owner cleanup");
    check(fake.event_target == 0x132,
          "restart binds ownership to the new starting thread");
    check(uvdb_debugnet_stop() == 0, "stop restarted session");
    destroy_fake();
}

static void release_blocked_worker(void)
{
    pthread_mutex_lock(&fake.mutex);
    __atomic_store_n(&fake.release_worker_wait, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&fake.condition);
    pthread_mutex_unlock(&fake.mutex);
}

static void test_process_exit_with_full_queue(void)
{
    reset_fake();
    __atomic_store_n(&fake.block_worker_wait, 1, __ATOMIC_RELEASE);
    check(uvdb_debugnet_start(&config) == 0,
          "start terminal full-queue session");
    check(wait_for_value(&fake.worker_wait_blocked, 1) == 0,
          "hold worker before it can drain the queue");

    for(unsigned int index = 0; index < 128; ++index)
        (void)uvdb_debugnet_printf(UVDB_LOG_TRACE,
                                   "terminal queue item %u\n", index);
    struct uvdb_debugnet_stats stats;
    check(uvdb_debugnet_get_stats(&stats) == 0 && stats.queued == 64,
          "terminal scenario fills the bounded queue");

    pthread_t exit_thread;
    check(pthread_create(&exit_thread, NULL, fake_process_exit_main, NULL) ==
              0,
          "start simulated C process-exit handler");
    check(wait_for_value(&fake.exit_hook_entered, 1) == 0,
          "process-exit handler begins promptly");
    check(wait_for_value(&fake.wait_call_count, 1) == 0,
          "process-exit handler waits to join the sender");
    release_blocked_worker();
    check(wait_for_value(&fake.exit_hook_returned, 1) == 0,
          "process-exit handler returns without deadlock");
    check(pthread_join(exit_thread, NULL) == 0,
          "join simulated process-exit caller");

    check(fake.worker_joined && fake.worker_ended,
          "process-exit handler positively joins the sender");
    check(fake.owner_exit_delivered == 0 &&
              fake.callback_signal_count == 0,
          "C exit quiesces the sender before the owner callback");
    check(fake.close_call_count == 0 && fake.socket_open,
          "C exit makes no post-destructor socket call");
    check(fake_exit.forced_exit_count == 0,
          "successful quiescence does not force process termination");
    check(uvdb_debugnet_write(UVDB_LOG_INFO, "after terminal hook\n") == -1,
          "terminal hook permanently closes the writer gate");
}

static void test_explicit_stop_then_process_exit(void)
{
    reset_fake();
    check(uvdb_debugnet_start(&config) == 0,
          "start explicit-stop then process-exit session");
    check(uvdb_debugnet_stop() == 0,
          "explicit stop completes before process exit");
    int waits = fake.wait_call_count;
    int closes = fake.close_call_count;
    int unregisters = fake.event_unregister_count;
    invoke_process_exit_handler();
    check(fake.wait_call_count == waits && fake.close_call_count == closes &&
              fake.event_unregister_count == unregisters,
          "process-exit handler is idempotent after explicit stop");
    check(fake_exit.forced_exit_count == 0,
          "stopped logger needs no forced process exit");
    destroy_fake();
}

static void test_process_exit_wait_failure_falls_back(void)
{
    reset_fake();
    __atomic_store_n(&fake.block_worker_wait, 1, __ATOMIC_RELEASE);
    check(uvdb_debugnet_start(&config) == 0,
          "start terminal wait-failure session");
    check(wait_for_value(&fake.worker_wait_blocked, 1) == 0,
          "hold worker for injected join timeout");
    fake.wait_failures = 1;
    invoke_process_exit_handler();
    check(fake_exit.forced_exit_count == 1 &&
              fake_exit.forced_exit_status == 0,
          "unproven worker quiescence uses direct process-exit fallback");
    check(!fake.worker_deleted,
          "fallback never force-deletes a running sender");
    release_blocked_worker();
    check(wait_for_value(&fake.worker_ended, 1) == 0,
          "host fake can release the worker after fallback recording");
    check(pthread_join(fake.worker, NULL) == 0,
          "join fake worker after recorded process termination");
    fake.worker_joined = 1;
}

struct stop_call {
    int entered;
    int returned;
    int result;
};

static void* stop_call_main(void* opaque)
{
    struct stop_call* call = opaque;
    fake_current_thread = 0x191;
    __atomic_store_n(&call->entered, 1, __ATOMIC_RELEASE);
    call->result = uvdb_debugnet_stop();
    __atomic_store_n(&call->returned, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_process_exit_lifecycle_lock_contention(void)
{
    reset_fake();
    fake.time_step_us = 1;
    __atomic_store_n(&fake.block_worker_wait, 1, __ATOMIC_RELEASE);
    check(uvdb_debugnet_start(&config) == 0,
          "start lifecycle-lock contention session");
    check(wait_for_value(&fake.worker_wait_blocked, 1) == 0,
          "hold worker so normal stop retains the lifecycle lock");

    struct stop_call stop_call = {0};
    pthread_t stop_thread;
    check(pthread_create(&stop_thread, NULL, stop_call_main, &stop_call) == 0,
          "start concurrent normal stop");
    check(wait_for_value(&stop_call.entered, 1) == 0,
          "normal stop enters before process exit");
    check(wait_for_value(&fake.wait_call_count, 1) == 0,
          "normal stop waits for the blocked worker under lifecycle lock");

    pthread_t exit_thread;
    check(pthread_create(&exit_thread, NULL, fake_process_exit_main, NULL) ==
              0,
          "start process-exit handler during lifecycle contention");
    check(wait_for_value(&fake.exit_hook_entered, 1) == 0,
          "process-exit handler starts during normal stop");

    release_blocked_worker();
    check(pthread_join(stop_thread, NULL) == 0,
          "join concurrent normal stop");
    check(wait_for_value(&fake.exit_hook_returned, 1) == 0,
          "process-exit handler acquires lifecycle lock without deadlock");
    check(pthread_join(exit_thread, NULL) == 0,
          "join contended process-exit handler");
    check(fake_exit.forced_exit_count == 0,
          "bounded lifecycle contention resolves without forced exit");
    check(fake.close_call_count == 0,
          "contended exit path still avoids socket calls");
}

int main(int argc, char** argv)
{
    if(argc == 2 && strcmp(argv[1], "--process-exit-full") == 0)
    {
        test_process_exit_with_full_queue();
        puts("PASS: DebugNet full-queue process-exit quiescence");
        return 0;
    }
    if(argc == 2 && strcmp(argv[1], "--process-exit-after-stop") == 0)
    {
        test_explicit_stop_then_process_exit();
        puts("PASS: DebugNet explicit-stop process-exit idempotence");
        return 0;
    }
    if(argc == 2 && strcmp(argv[1], "--process-exit-fallback") == 0)
    {
        test_process_exit_wait_failure_falls_back();
        puts("PASS: DebugNet process-exit timeout fallback");
        return 0;
    }
    if(argc == 2 && strcmp(argv[1], "--process-exit-contention") == 0)
    {
        test_process_exit_lifecycle_lock_contention();
        puts("PASS: DebugNet process-exit lifecycle-lock contention");
        return 0;
    }
    check(argc == 1, "recognize lifecycle test scenario");
    test_exit_hook_registration_failure();
    test_exit_hook_registered_once();
    test_registration_is_required();
    test_failed_start_wait_retry();
    test_failed_start_delete_retry();
    test_unstarted_worker_delete_retry();
    test_explicit_stop();
    test_unregister_failures_are_retryable();
    test_socket_close_failure_is_retryable();
    test_delayed_old_callback_is_generation_gated();
    test_owner_exit_during_explicit_stop();
    test_owner_exit_after_writer_wait_timeout();
    test_owner_exit_emergency_stop();
    test_owner_exit_with_inflight_writers();
    test_restart_after_owner_exit();
    puts("PASS: DebugNet owner-exit lifecycle and fake-kernel cleanup");
    return 0;
}

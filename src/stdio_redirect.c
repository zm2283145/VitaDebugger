#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>

#include <psp2/kernel/threadmgr/lw_mutex.h>
#include <psp2/kernel/threadmgr/thread.h>

#include "stdio_redirect.h"
#include "uvdb.h"
#include "uvdb_console.h"

/*
 * Vita newlib does not export dup2. Duplicate source first, then atomically
 * exchange the staging and destination fd-map entries under newlib's own lock.
 * Closing staging releases the old destination mapping. Its close result does
 * not override a swap that already succeeded.
 */
static int redirect_dup2(int source, int destination,
                         int* deferred_close)
{
    extern void* __vita_fdmap[];
    extern SceKernelLwMutexWork _newlib_fd_mutex;

    if(!deferred_close || *deferred_close >= 0)
    {
        errno = EBUSY;
        return -1;
    }
    if(source == destination)
        return destination;
    int staging = dup(source);
    if(staging < 0)
        return -1;
    if(sceKernelLockLwMutex(&_newlib_fd_mutex, 1, NULL) < 0)
    {
        if(close(staging) < 0)
            *deferred_close = staging;
        errno = EIO;
        return -1;
    }
    void* old_destination = __vita_fdmap[destination];
    __vita_fdmap[destination] = __vita_fdmap[staging];
    __vita_fdmap[staging] = old_destination;
    sceKernelUnlockLwMutex(&_newlib_fd_mutex, 1);

    int saved_errno = errno;
    if(close(staging) < 0)
        *deferred_close = staging;
    errno = saved_errno;
    return destination;
}

/*
 * Restore without allocating a staging descriptor. The saved descriptor is
 * consumed: after the swap, closing it releases the replaced bridge mapping.
 * On lock failure the original remains saved and a later restore can retry.
 */
static int redirect_move_saved(int* saved, int destination,
                               int* deferred_close)
{
    extern void* __vita_fdmap[];
    extern SceKernelLwMutexWork _newlib_fd_mutex;

    if(!saved || *saved < 0)
    {
        errno = EBADF;
        return -1;
    }
    if(!deferred_close || *deferred_close >= 0)
    {
        errno = EBUSY;
        return -1;
    }
    int source = *saved;
    if(sceKernelLockLwMutex(&_newlib_fd_mutex, 1, NULL) < 0)
    {
        errno = EIO;
        return -1;
    }
    void* old_destination = __vita_fdmap[destination];
    __vita_fdmap[destination] = __vita_fdmap[source];
    __vita_fdmap[source] = old_destination;
    sceKernelUnlockLwMutex(&_newlib_fd_mutex, 1);

    int saved_errno = errno;
    *saved = -1;
    if(close(source) < 0)
        *deferred_close = source;
    errno = saved_errno;
    return destination;
}

enum uvdb_stdio_deferred_close {
    UVDB_STDIO_CLOSE_STDOUT_REDIRECT,
    UVDB_STDIO_CLOSE_STDERR_REDIRECT,
    UVDB_STDIO_CLOSE_STDOUT_RESTORE,
    UVDB_STDIO_CLOSE_STDERR_RESTORE,
    UVDB_STDIO_CLOSE_COUNT,
};

/* Keep the static -1 initializer below in lockstep with the slot inventory.
 * A zero-initialized added slot would otherwise be mistaken for stdin. */
typedef char uvdb_stdio_deferred_close_count_must_be_four[
    UVDB_STDIO_CLOSE_COUNT == 4 ? 1 : -1];

struct uvdb_stdio_state {
    int active;
    int stdout_redirected;
    int stderr_redirected;
    int read_fd;
    int write_fd;
    int saved_stdout;
    int saved_stderr;
    int deferred_close[UVDB_STDIO_CLOSE_COUNT];
    int thread_started;
    int thread_ready;
    int reader_wake_proven;
    pthread_t thread;
};

static pthread_mutex_t stdio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stdio_ready = PTHREAD_COND_INITIALIZER;
static struct uvdb_stdio_state stdio_state = {
    .read_fd = -1,
    .write_fd = -1,
    .saved_stdout = -1,
    .saved_stderr = -1,
    .deferred_close = {
        [UVDB_STDIO_CLOSE_STDOUT_REDIRECT] = -1,
        [UVDB_STDIO_CLOSE_STDERR_REDIRECT] = -1,
        [UVDB_STDIO_CLOSE_STDOUT_RESTORE] = -1,
        [UVDB_STDIO_CLOSE_STDERR_RESTORE] = -1,
    },
};
static int stdio_thread_id = -1;

int uvdb_stdio_is_internal_thread(int thread_id)
{
    return thread_id > 0 &&
           thread_id == __atomic_load_n(&stdio_thread_id, __ATOMIC_ACQUIRE);
}

static void* stdio_capture_main(void* argument)
{
    int read_fd = (int)(intptr_t)argument;
    __atomic_store_n(&stdio_thread_id, sceKernelGetThreadId(),
                     __ATOMIC_RELEASE);
    pthread_mutex_lock(&stdio_mutex);
    stdio_state.thread_ready = 1;
    pthread_cond_broadcast(&stdio_ready);
    pthread_mutex_unlock(&stdio_mutex);

    unsigned char buffer[1024];
    for(;;)
    {
        ssize_t result = read(read_fd, buffer, sizeof(buffer));
        if(result > 0)
        {
            /* Capture is deliberately attempted once. Retrying a rejected
             * tail would stall this reader and eventually block gameplay. */
            uvdb_console_capture(buffer, (size_t)result);
            continue;
        }
        if(result < 0 && errno == EINTR)
            continue;
        break;
    }
    /* The lifecycle owner keeps this number reserved and closes it after join,
     * preventing another thread from reusing it during shutdown. */
    __atomic_store_n(&stdio_thread_id, -1, __ATOMIC_RELEASE);
    return NULL;
}

/* These are exclusively owned Vita-newlib descriptors. On the supported
 * fd-map implementation a failed close leaves its slot reserved, so retaining
 * the number permits bounded retries later in this cleanup call and on a
 * subsequent restore call. */
static int close_if_open(int* descriptor)
{
    if(!descriptor)
        return -1;
    if(*descriptor >= 0)
    {
        if(close(*descriptor) < 0)
            return -1;
        *descriptor = -1;
    }
    return 0;
}

static void remember_first_error(int* first_error)
{
    if(first_error && !*first_error)
        *first_error = errno ? errno : EIO;
}

static int close_deferred_descriptors_locked(void)
{
    int first_error = 0;
    for(unsigned int index = 0; index < UVDB_STDIO_CLOSE_COUNT; ++index)
        if(close_if_open(&stdio_state.deferred_close[index]) < 0)
            remember_first_error(&first_error);
    if(first_error)
    {
        errno = first_error;
        return -1;
    }
    return 0;
}

static int stdio_has_owned_resources_locked(void)
{
    if(stdio_state.stdout_redirected || stdio_state.stderr_redirected ||
       stdio_state.read_fd >= 0 || stdio_state.write_fd >= 0 ||
       stdio_state.saved_stdout >= 0 || stdio_state.saved_stderr >= 0 ||
       stdio_state.thread_started)
        return 1;
    for(unsigned int index = 0; index < UVDB_STDIO_CLOSE_COUNT; ++index)
        if(stdio_state.deferred_close[index] >= 0)
            return 1;
    return 0;
}

static int stop_capture_thread_locked(void)
{
    int first_error = 0;
    int wake_error = 0;

    if(stdio_state.write_fd >= 0)
    {
        if(shutdown(stdio_state.write_fd, SHUT_RDWR) == 0)
            stdio_state.reader_wake_proven = 1;
        else
            wake_error = errno ? errno : EIO;
    }
    if(stdio_state.read_fd >= 0)
    {
        if(shutdown(stdio_state.read_fd, SHUT_RDWR) == 0)
            stdio_state.reader_wake_proven = 1;
        else if(!wake_error)
            wake_error = errno ? errno : EIO;
    }

    /* Without a successful shutdown, closing the writer proves wakeup only
     * when no redirected stream or deferred restore descriptor still aliases
     * the bridge endpoint. Keep the primary writer open otherwise so a later
     * call can retry shutdown or perform the final close. */
    int deferred_close_slots_empty = 1;
    for(unsigned int index = 0; index < UVDB_STDIO_CLOSE_COUNT; ++index)
        if(stdio_state.deferred_close[index] >= 0)
        {
            deferred_close_slots_empty = 0;
            break;
        }
    int writer_close_is_final =
        !stdio_state.stdout_redirected &&
        !stdio_state.stderr_redirected &&
        deferred_close_slots_empty;
    if(stdio_state.write_fd >= 0 &&
       (!stdio_state.thread_started || stdio_state.reader_wake_proven ||
        writer_close_is_final))
    {
        if(close_if_open(&stdio_state.write_fd) < 0)
            remember_first_error(&first_error);
        else if(stdio_state.thread_started && writer_close_is_final)
            stdio_state.reader_wake_proven = 1;
    }

    if(stdio_state.thread_started)
    {
        if(!stdio_state.reader_wake_proven)
        {
            errno = wake_error ? wake_error :
                    (first_error ? first_error : EAGAIN);
            return -1;
        }
        int join_result = pthread_join(stdio_state.thread, NULL);
        if(join_result)
        {
            if(!first_error)
                first_error = join_result;
            errno = first_error;
            return -1;
        }
        stdio_state.thread_started = 0;
        stdio_state.reader_wake_proven = 0;
    }
    if(close_if_open(&stdio_state.read_fd) < 0)
        remember_first_error(&first_error);
    stdio_state.thread_ready = 0;
    if(!stdio_state.thread_started)
        stdio_state.reader_wake_proven = 0;
    if(first_error)
    {
        errno = first_error;
        return -1;
    }
    return 0;
}

static int close_unused_saved_descriptors_locked(void)
{
    int first_error = 0;
    if(!stdio_state.stdout_redirected)
    {
        if(close_if_open(&stdio_state.saved_stdout) < 0)
            remember_first_error(&first_error);
    }
    if(!stdio_state.stderr_redirected)
    {
        if(close_if_open(&stdio_state.saved_stderr) < 0)
            remember_first_error(&first_error);
    }
    if(first_error)
    {
        errno = first_error;
        return -1;
    }
    return 0;
}

int uvdb_redirect_stdio(void)
{
    int first_error = 0;

    pthread_mutex_lock(&stdio_mutex);
    if(stdio_state.active)
    {
        int fully_redirected = stdio_state.stdout_redirected &&
                               stdio_state.stderr_redirected;
        pthread_mutex_unlock(&stdio_mutex);
        return fully_redirected ? 0 : -1;
    }

    /* stdout/stderr must be valid newlib descriptors before redirection. */
    stdio_state.saved_stdout = dup(STDOUT_FILENO);
    if(stdio_state.saved_stdout < 0)
        goto fail;
    stdio_state.saved_stderr = dup(STDERR_FILENO);
    if(stdio_state.saved_stderr < 0)
        goto fail;

    int endpoints[2] = {-1, -1};
    if(socketpair(AF_INET, SOCK_STREAM, 0, endpoints) < 0)
        goto fail;
    stdio_state.read_fd = endpoints[0];
    stdio_state.write_fd = endpoints[1];
    stdio_state.reader_wake_proven = 0;

    int flags = fcntl(stdio_state.write_fd, F_GETFL, 0);
    if(flags < 0 ||
       fcntl(stdio_state.write_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail;

    int thread_result = pthread_create(
        &stdio_state.thread,
        NULL,
        stdio_capture_main,
        (void*)(intptr_t)stdio_state.read_fd);
    if(thread_result)
    {
        errno = thread_result;
        goto fail;
    }
    stdio_state.thread_started = 1;
    while(!stdio_state.thread_ready)
        pthread_cond_wait(&stdio_ready, &stdio_mutex);

    if(redirect_dup2(
           stdio_state.write_fd,
           STDOUT_FILENO,
           &stdio_state.deferred_close[UVDB_STDIO_CLOSE_STDOUT_REDIRECT]) !=
       STDOUT_FILENO)
        goto fail;
    stdio_state.stdout_redirected = 1;
    if(redirect_dup2(
           stdio_state.write_fd,
           STDERR_FILENO,
           &stdio_state.deferred_close[UVDB_STDIO_CLOSE_STDERR_REDIRECT]) !=
       STDERR_FILENO)
        goto fail;
    stdio_state.stderr_redirected = 1;

    stdio_state.active = 1;
    pthread_mutex_unlock(&stdio_mutex);
    return 0;

fail:
    first_error = errno ? errno : EIO;
    if(stdio_state.stdout_redirected &&
       redirect_move_saved(
           &stdio_state.saved_stdout,
           STDOUT_FILENO,
           &stdio_state.deferred_close[UVDB_STDIO_CLOSE_STDOUT_RESTORE]) ==
           STDOUT_FILENO)
        stdio_state.stdout_redirected = 0;
    if(stdio_state.stderr_redirected &&
       redirect_move_saved(
           &stdio_state.saved_stderr,
           STDERR_FILENO,
           &stdio_state.deferred_close[UVDB_STDIO_CLOSE_STDERR_RESTORE]) ==
           STDERR_FILENO)
        stdio_state.stderr_redirected = 0;
    close_unused_saved_descriptors_locked();

    if(!stdio_state.stdout_redirected && !stdio_state.stderr_redirected)
        stop_capture_thread_locked();
    close_deferred_descriptors_locked();
    stdio_state.active = stdio_has_owned_resources_locked();
    pthread_mutex_unlock(&stdio_mutex);
    errno = first_error;
    return -1;
}

int uvdb_restore_stdio(void)
{
    pthread_mutex_lock(&stdio_mutex);
    if(!stdio_state.active)
    {
        pthread_mutex_unlock(&stdio_mutex);
        return 0;
    }

    int result = 0;
    int first_error = 0;
    if(stdio_state.stdout_redirected)
    {
        if(redirect_move_saved(
               &stdio_state.saved_stdout,
               STDOUT_FILENO,
               &stdio_state.deferred_close[UVDB_STDIO_CLOSE_STDOUT_RESTORE]) ==
           STDOUT_FILENO)
            stdio_state.stdout_redirected = 0;
        else
        {
            result = -1;
            remember_first_error(&first_error);
        }
    }
    if(stdio_state.stderr_redirected)
    {
        if(redirect_move_saved(
               &stdio_state.saved_stderr,
               STDERR_FILENO,
               &stdio_state.deferred_close[UVDB_STDIO_CLOSE_STDERR_RESTORE]) ==
           STDERR_FILENO)
            stdio_state.stderr_redirected = 0;
        else
        {
            result = -1;
            remember_first_error(&first_error);
        }
    }

    /* Never destroy the bridge or its remaining backup while a failed stream
     * restore still needs it. A later call can retry without data loss. */
    if(stdio_state.stdout_redirected || stdio_state.stderr_redirected)
    {
        pthread_mutex_unlock(&stdio_mutex);
        errno = first_error ? first_error : EBUSY;
        return -1;
    }

    if(close_unused_saved_descriptors_locked() < 0)
    {
        result = -1;
        remember_first_error(&first_error);
    }
    if(stop_capture_thread_locked() < 0)
    {
        result = -1;
        remember_first_error(&first_error);
    }
    if(close_deferred_descriptors_locked() < 0)
    {
        result = -1;
        remember_first_error(&first_error);
    }
    stdio_state.active = stdio_has_owned_resources_locked();
    if(stdio_state.active)
    {
        result = -1;
        if(!first_error)
            first_error = EBUSY;
    }
    pthread_mutex_unlock(&stdio_mutex);
    if(result < 0)
        errno = first_error ? first_error : EIO;
    return result;
}

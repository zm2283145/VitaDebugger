#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/signal.h> //for signal constants; these seem to match gdb's
#include <stdarg.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <psp2/net/net.h>
#include <psp2/net/net_syscalls.h>
#include <psp2common/net.h>
#include <psp2/kernel/threadmgr/msgpipe.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/modulemgr.h>
#include <kubridge.h>
#include "uvdb.h"
#include "uvdb_console_transport.h"
#include "uvdb_registers.h"
#include "uvdb_rsp.h"
#include "uvdb_vfp_policy.h"
#include "stdio_redirect.h"
#include "uvdb_thread_control.h"
#ifdef UVDB_KERNEL_THREAD_CONTROL
#include "vitadebug_kernel.h"
typedef char uvdb_thread_capacity_must_cover_kernel_inventory[
    UVDB_THREAD_INVENTORY_CAPACITY >= VD_KERNEL_MAX_THREADS ? 1 : -1];
typedef char uvdb_kernel_register_bank_count_must_match[
    (sizeof(((struct vd_thread_registers*)0)->entry) /
         sizeof(((struct vd_thread_registers*)0)->entry[0])) ==
        UVDB_ARM_REGISTER_BANK_COUNT ? 1 : -1];
#endif

#if defined(UVDB_KERNEL_VFP_READS) && !defined(UVDB_KERNEL_THREAD_CONTROL)
#error "UVDB_KERNEL_VFP_READS requires UVDB_KERNEL_THREAD_CONTROL"
#endif

#ifdef UVDB_KERNEL_VFP_READS
static const char uvdb_arm_vfp_target_xml[] =
#include "protocol/arm_vfp_target_xml.inc"
;
static int uvdb_rsp_vfp_enabled;

static void uvdb_refresh_rsp_vfp_capability(void)
{
    struct vd_kernel_status status;
    uvdb_rsp_vfp_enabled =
        vdKernelGetStatus(&status) >= 0 &&
        status.abi_version == VD_KERNEL_ABI_VERSION &&
        (status.capabilities & VD_KERNEL_CAP_THREAD_VFP_REGISTERS) != 0;
}
#endif

#define UVDB_DEFAULT_PORT 1234
#define UVDB_DEFAULT_MAX_BUFFER (256 * 1024)
#define UVDB_MIN_BUFFER 4096
#define UVDB_MAX_BUFFER (16 * 1024 * 1024)

//we prefer to use raw syscalls to avoid issues with signal safety
void _sceKernelExitProcessForUser(int);
int _sceKernelSendMsgPipeVector(SceUID, const SceKernelAddrPair*, unsigned int, uint32_t* rest);
int _sceKernelReceiveMsgPipeVector(SceUID, const SceKernelAddrPair*, unsigned int, uint32_t* rest);
extern char __executable_start[];

#define UVDB_MAX_THREADS 32
#define UVDB_THREAD_NAME_MAX 32

struct uvdb_thread_entry
{
    SceUID id;
    char name[UVDB_THREAD_NAME_MAX];
    uint8_t active;
};

static struct uvdb_thread_entry uvdb_threads[UVDB_MAX_THREADS];
static struct uvdb_thread_inventory uvdb_inventory;
static struct uvdb_thread_selection uvdb_selection = {
    .stopped = UVDB_RSP_THREAD_ALL,
    .general = UVDB_RSP_THREAD_ANY,
    .resume = UVDB_RSP_THREAD_ALL,
};

static int uvdb_lock_state;
static int uvdb_lifecycle_lock_state;
static int uvdb_socket_lifecycle_lock_state;

static void uvdb_lock(void)
{
    for(;;)
    {
        int old_value = 0;
        if(__atomic_compare_exchange_n(&uvdb_lock_state, &old_value, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return;
    }
}

static int uvdb_try_lock(void)
{
    int old_value = 0;
    return __atomic_compare_exchange_n(&uvdb_lock_state, &old_value, 1, 0,
                                       __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

static void uvdb_unlock(void)
{
    __atomic_store_n(&uvdb_lock_state, 0, __ATOMIC_SEQ_CST);
}

static void uvdb_lifecycle_lock(void)
{
    for(;;)
    {
        int old_value = 0;
        if(__atomic_compare_exchange_n(&uvdb_lifecycle_lock_state, &old_value,
                                       1, 0, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST))
            return;
    }
}

static void uvdb_lifecycle_unlock(void)
{
    __atomic_store_n(&uvdb_lifecycle_lock_state, 0, __ATOMIC_SEQ_CST);
}

static void uvdb_socket_lifecycle_lock(void)
{
    for(;;)
    {
        int old_value = 0;
        if(__atomic_compare_exchange_n(&uvdb_socket_lifecycle_lock_state,
                                       &old_value, 1, 0,
                                       __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST))
            return;
    }
}

static void uvdb_socket_lifecycle_unlock(void)
{
    __atomic_store_n(&uvdb_socket_lifecycle_lock_state, 0,
                     __ATOMIC_SEQ_CST);
}

static struct uvdb_thread_entry* uvdb_find_thread(SceUID id)
{
    for(size_t i = 0; i < UVDB_MAX_THREADS; ++i)
        if(uvdb_threads[i].active && uvdb_threads[i].id == id)
            return &uvdb_threads[i];
    return NULL;
}

int uvdb_register_thread(const char* name)
{
    SceUID id = sceKernelGetThreadId();
    uvdb_lock();
    struct uvdb_thread_entry* entry = uvdb_find_thread(id);
    if(!entry)
    {
        for(size_t i = 0; i < UVDB_MAX_THREADS; ++i)
            if(!uvdb_threads[i].active)
            {
                entry = &uvdb_threads[i];
                memset(entry, 0, sizeof(*entry));
                entry->id = id;
                entry->active = 1;
                break;
            }
    }
    if(entry && name)
    {
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = 0;
    }
    uvdb_unlock();
    return entry ? 0 : -1;
}

int uvdb_unregister_thread(void)
{
    SceUID id = sceKernelGetThreadId();
    uvdb_lock();
    struct uvdb_thread_entry* entry = uvdb_find_thread(id);
    if(entry)
        memset(entry, 0, sizeof(*entry));
    uvdb_unlock();
    return entry ? 0 : -1;
}

static int uvdb_socket = -1;
static int uvdb_listen_socket = -1;
static uint32_t uvdb_socket_generation;
static struct uvdb_console_transport uvdb_console_transport;
static SceUID uvdb_pipe = -1;
static size_t uvdb_max_buffer = UVDB_DEFAULT_MAX_BUFFER;
static unsigned short uvdb_port = UVDB_DEFAULT_PORT;
static volatile enum uvdb_state uvdb_state = UVDB_STATE_IDLE;
static int uvdb_io_failed;
static unsigned int uvdb_handler_mask;
static volatile int uvdb_target_stopped;
static volatile int uvdb_async_stop_pending;
static volatile int uvdb_async_stop_cancelled;
static volatile int uvdb_server_stop;
static volatile int uvdb_shutdown_pending;
static SceUID uvdb_server_thread = -1;
static int uvdb_server_thread_ended;
/*
 * The exception context belongs to the thread that entered the handler.  That
 * is not necessarily the protocol-facing stopped thread: initial attach and
 * asynchronous Ctrl-C are delivered by the private server thread after the
 * kernel has stopped the application threads.
 */
static SceUID uvdb_exception_thread = -1;
#ifdef UVDB_KERNEL_THREAD_CONTROL
#define UVDB_STOP_OWNER_NONE 0
#define UVDB_STOP_OWNER_CONTROLLER 1
#define UVDB_STOP_OWNER_LEASE 2
static volatile int uvdb_lease_stop;
static volatile int uvdb_stop_failed;
static volatile int uvdb_stop_owner;
static volatile unsigned int uvdb_stop_token;
static SceUID uvdb_lease_thread = -1;
static int uvdb_lease_thread_ended;
#endif
static struct uvdb_fault_info uvdb_last_fault = {
    .exception_type = UVDB_EXCEPTION_NONE,
};

static void breakpoint_remove_all(void);
static int uvdb_kernel_end_stop(void);
#ifdef UVDB_KERNEL_THREAD_CONTROL
static int breakpoint_any_active(void);
static void uvdb_claim_stop_controller(void);
static void uvdb_release_stop_controller(void);
static int uvdb_kernel_recover_stop(void);
static void uvdb_kernel_abandon_stop(void);
#endif
static int uvdb_server_main(SceSize args, void* argp);
static enum uvdb_console_write_result uvdb_console_raw_socket_write(
    void* context,
    const void* data,
    size_t size,
    size_t* bytes_sent,
    int* native_error);
static enum uvdb_console_write_result uvdb_console_server_socket_write(
    void* context,
    const void* data,
    size_t size,
    size_t* bytes_sent,
    int* native_error);
#ifdef UVDB_KERNEL_THREAD_CONTROL
static int uvdb_lease_main(SceSize args, void* argp);
#endif

static int uvdb_is_controller_thread(SceUID id)
{
    if(id <= 0)
        return 0;
    if(id == uvdb_server_thread)
        return 1;
#ifdef UVDB_KERNEL_THREAD_CONTROL
    if(id == uvdb_lease_thread)
        return 1;
#endif
    return 0;
}

static int uvdb_is_internal_thread(SceUID id)
{
    return uvdb_is_controller_thread(id) ||
           uvdb_stdio_is_internal_thread(id);
}

static int uvdb_should_inventory_thread(SceUID id)
{
    /* Hide helpers during ordinary stops, but keep a non-controller helper
     * visible if it is itself the faulting thread so GDB can report the fault. */
    return !uvdb_is_internal_thread(id) ||
           (id == uvdb_exception_thread && !uvdb_is_controller_thread(id));
}

#ifdef UVDB_KERNEL_THREAD_CONTROL
static int uvdb_kernel_status_is_compatible(void)
{
    struct vd_kernel_status status = {0};
    return vdKernelGetStatus(&status) >= 0 &&
           status.abi_version == VD_KERNEL_ABI_VERSION &&
           (status.capabilities &
                VD_KERNEL_REQUIRED_THREAD_CONTROL_CAPABILITIES) ==
               VD_KERNEL_REQUIRED_THREAD_CONTROL_CAPABILITIES &&
           status.max_threads == VD_KERNEL_MAX_THREADS;
}

static const struct vd_arm_registers* uvdb_kernel_user_register_bank(
    const struct vd_thread_registers* registers)
{
    if(!registers)
        return NULL;
    struct uvdb_arm_register_bank_state
        states[UVDB_ARM_REGISTER_BANK_COUNT];
    for(unsigned int index = 0; index < UVDB_ARM_REGISTER_BANK_COUNT;
        ++index)
    {
        states[index].sp = registers->entry[index].sp;
        states[index].pc = registers->entry[index].pc;
        states[index].cpsr = registers->entry[index].cpsr;
    }
    int selected = uvdb_select_user_arm_register_bank(states);
    return selected >= 0 ? &registers->entry[selected] : NULL;
}
#endif

/*
 * Keep one protocol-facing inventory. The stopped thread is first because GDB
 * subsequently selects the first qfThreadInfo result. In kernel-assisted
 * builds, the caller-process kernel list is authoritative and cooperative
 * registrations only annotate names. Library-only builds retain the
 * cooperative registry as their inventory source.
 */
static int uvdb_refresh_thread_inventory(void)
{
#ifdef UVDB_KERNEL_THREAD_CONTROL
    if(__atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST))
        return -1;
    unsigned int token = __atomic_load_n(&uvdb_stop_token,
                                          __ATOMIC_SEQ_CST);
    if(!token || vdKernelRenewStop(token, 2000) < 0)
    {
        __atomic_store_n(&uvdb_stop_failed, 1, __ATOMIC_SEQ_CST);
        return -1;
    }
    SceUID kernel_threads[VD_KERNEL_MAX_THREADS];
    int copied = 0;
    int total = 0;
    if(vdKernelGetThreadList(kernel_threads, VD_KERNEL_MAX_THREADS,
                             &copied, &total) < 0 ||
       copied < 0 || copied > VD_KERNEL_MAX_THREADS || total != copied)
        return -1;

    uvdb_thread_inventory_reset(&uvdb_inventory);
    if(uvdb_selection.stopped > 0 &&
       uvdb_should_inventory_thread(uvdb_selection.stopped))
    {
        int stopped_present = 0;
        for(int i = 0; i < copied; ++i)
            if(kernel_threads[i] == uvdb_selection.stopped)
            {
                stopped_present = 1;
                break;
            }
        if(stopped_present &&
           uvdb_thread_inventory_add(&uvdb_inventory,
                                     uvdb_selection.stopped) < 0)
            return -1;
    }
    for(int i = 0; i < copied; ++i)
        if(uvdb_should_inventory_thread(kernel_threads[i]))
        {
            /* Every foreign thread exposed to GDB must be owned by this stop
             * session. An independently debug-suspended thread is visible in
             * the process list but has no readable session snapshot. */
            if(kernel_threads[i] != uvdb_exception_thread)
            {
                struct vd_thread_registers snapshot;
                if(vdKernelGetThreadRegisters(token, kernel_threads[i],
                                               &snapshot) < 0 ||
                   !uvdb_kernel_user_register_bank(&snapshot))
                    return -1;
            }
            if(uvdb_thread_inventory_add(&uvdb_inventory,
                                         kernel_threads[i]) < 0)
                return -1;
        }
#else
    uvdb_thread_inventory_reset(&uvdb_inventory);
    if(uvdb_selection.stopped > 0 &&
       uvdb_should_inventory_thread(uvdb_selection.stopped) &&
       uvdb_thread_inventory_add(&uvdb_inventory,
                                 uvdb_selection.stopped) < 0)
        return -1;
    for(size_t i = 0; i < UVDB_MAX_THREADS; ++i)
        if(uvdb_threads[i].active &&
           uvdb_should_inventory_thread(uvdb_threads[i].id) &&
           uvdb_thread_inventory_add(&uvdb_inventory,
                                     uvdb_threads[i].id) < 0)
            return -1;
#endif
    uvdb_thread_selection_reconcile(&uvdb_selection, &uvdb_inventory);
    return 0;
}

struct buffer
{
    SceUID memblock_uid;
    char* buf;
    size_t size;
    size_t cap;
    size_t packet_start;
};

static void buffer_popleft(struct buffer* buf, size_t cnt)
{
    if(cnt > buf->size)
        cnt = buf->size;
    memmove(buf->buf, buf->buf+cnt, buf->size-cnt);
    buf->size -= cnt;
}

static size_t buffer_getspace(struct buffer* buf, char** pos)
{
    if(buf->size == buf->cap)
    {
        size_t cap2 = buf->cap * 2;
        if(!cap2)
            cap2 = UVDB_MIN_BUFFER;
        if(cap2 > uvdb_max_buffer)
            cap2 = uvdb_max_buffer;
        if(cap2 <= buf->cap)
        {
            *pos = NULL;
            return 0;
        }
        SceUID memblock2 = sceKernelAllocMemBlock("gdb socket buffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, cap2, NULL);
        if(memblock2 < 0)
        {
            *pos = NULL;
            return 0;
        }
        void* base = NULL;
        if(sceKernelGetMemBlockBase(memblock2, &base) < 0 || !base)
        {
            sceKernelFreeMemBlock(memblock2);
            *pos = NULL;
            return 0;
        }
        if(buf->size)
            memcpy(base, buf->buf, buf->size);
        if(buf->memblock_uid >= 0)
            sceKernelFreeMemBlock(buf->memblock_uid);
        buf->memblock_uid = memblock2;
        buf->buf = base;
        buf->cap = cap2;
    }
    *pos = buf->buf + buf->size;
    return buf->cap - buf->size;
}

static size_t buffer_poll(struct buffer* buf, char** pos)
{
    size_t chk_size = buffer_getspace(buf, pos);
    if(!chk_size)
    {
        uvdb_io_failed = 1;
        return 0;
    }
    uint32_t args[6] = {uvdb_socket, (uint32_t)*pos, chk_size, 0, 0, 0};
    ssize_t ans = sceNetSyscallRecvfrom((void*)args);
    if(ans <= 0)
    {
        uvdb_io_failed = 1;
        return 0;
    }
    buf->size += ans;
    return ans;
}

static void buffer_write(struct buffer* buf, const void* source, size_t sz)
{
    const char* data = source;
    while(sz)
    {
        char* pos;
        size_t chk = buffer_getspace(buf, &pos);
        if(!chk)
        {
            uvdb_io_failed = 1;
            return;
        }
        if(chk > sz)
            chk = sz;
        memcpy(pos, data, chk);
        data += chk;
        buf->size += chk;
        sz -= chk;
    }
}

static void buffer_start_packet(struct buffer* buf)
{
    buffer_write(buf, "$", 1);
    buf->packet_start = buf->size;
}

static char int2hex(int value)
{
    if(value < 10)
        return value + '0';
    return value - 10 + 'a';
}

static void buffer_end_packet(struct buffer* buf)
{
    uint8_t cksum = 0;
    for(size_t i = buf->packet_start; i < buf->size; i++)
        cksum += (uint8_t)buf->buf[i];
    uint8_t footer[3] = {'#', int2hex(cksum>>4), int2hex(cksum&15)};
    buffer_write(buf, footer, 3);
}

static void buffer_flush(struct buffer* buf)
{
    size_t pos = 0;
    while(pos < buf->size)
    {
        uint32_t args[6] = {uvdb_socket, (uint32_t)(buf->buf+pos), buf->size-pos, 0, 0, 0};
        ssize_t chk = sceNetSyscallSendto((void*)args);
        if(chk <= 0)
        {
            uvdb_io_failed = 1;
            break;
        }
        pos += chk;
    }
    buf->size = 0;
}

static struct buffer in_buf = {.memblock_uid = -1};
static struct buffer out_buf = {.memblock_uid = -1};

static void buffer_release(struct buffer* buf)
{
    if(buf->memblock_uid >= 0)
        sceKernelFreeMemBlock(buf->memblock_uid);
    memset(buf, 0, sizeof(*buf));
    buf->memblock_uid = -1;
}

static void uvdb_close_socket(int* socket)
{
    if(!socket)
        return;

    uvdb_socket_lifecycle_lock();
    int descriptor = *socket;
    if(descriptor >= 0)
    {
        *socket = -1;
        if(socket == &uvdb_socket)
            uvdb_console_transport_end_connection(&uvdb_console_transport);
        sceNetSyscallShutdown(descriptor, SHUT_RDWR);
        sceNetSyscallClose(descriptor);
    }
    uvdb_socket_lifecycle_unlock();
}

static int uvdb_publish_socket(int* socket, int descriptor)
{
    if(!socket || descriptor < 0)
        return -1;

    int result = -1;
    uvdb_socket_lifecycle_lock();
    if(*socket < 0)
    {
        *socket = descriptor;
        if(socket == &uvdb_socket)
        {
            uvdb_socket_generation++;
            if(!uvdb_socket_generation)
                uvdb_socket_generation++;
        }
        result = 0;
    }
    uvdb_socket_lifecycle_unlock();
    return result;
}

static void uvdb_active_socket_snapshot(int* descriptor,
                                        uint32_t* generation)
{
    uvdb_socket_lifecycle_lock();
    *descriptor = uvdb_socket;
    *generation = uvdb_socket_generation;
    uvdb_socket_lifecycle_unlock();
}

/* Shutdown wakes a descriptor owner without transferring close ownership.
 * Holding the short socket lock through the syscall prevents a simultaneous
 * close followed by descriptor-number reuse from targeting an unrelated fd. */
static int uvdb_shutdown_socket(int* socket)
{
    if(!socket)
        return -1;

    int result = 0;
    uvdb_socket_lifecycle_lock();
    if(*socket >= 0)
        result = sceNetSyscallShutdown(*socket, SHUT_RDWR);
    uvdb_socket_lifecycle_unlock();
    return result;
}

static int uvdb_shutdown_socket_if_current(int* socket, int descriptor)
{
    if(!socket || descriptor < 0)
        return -1;

    int result = 0;
    uvdb_socket_lifecycle_lock();
    if(*socket == descriptor)
        result = sceNetSyscallShutdown(descriptor, SHUT_RDWR);
    uvdb_socket_lifecycle_unlock();
    return result;
}

/* Abort is the Vita network API's cancellation primitive for a thread blocked
 * in accept. It retains descriptor ownership, unlike a cross-thread close. */
static int uvdb_abort_socket(int* socket)
{
    if(!socket)
        return -1;

    int result = 0;
    uvdb_socket_lifecycle_lock();
    if(*socket >= 0)
        result = sceNetSyscallSocketAbort(*socket, 0);
    uvdb_socket_lifecycle_unlock();
    return result;
}

static void uvdb_release_handlers(void)
{
    if(uvdb_handler_mask & (1u << KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT))
        kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT);
    if(uvdb_handler_mask & (1u << KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT))
        kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT);
    if(uvdb_handler_mask & (1u << KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION))
        kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION);
    uvdb_handler_mask = 0;
}

/* Preserve a thread handle until both termination and deletion are proven.
 * A failed wait may still refer to a live thread; a failed delete refers to an
 * ended thread whose resource can be deleted by a later shutdown retry. */
static int uvdb_wait_delete_thread(SceUID* thread, int* ended)
{
    if(!thread || !ended)
        return -1;
    if(*thread < 0)
    {
        *ended = 0;
        return 0;
    }
    if(!*ended)
    {
        int status = 0;
        if(sceKernelWaitThreadEnd(*thread, &status, NULL) < 0)
            return -1;
        *ended = 1;
    }
    if(sceKernelDeleteThread(*thread) < 0)
        return -1;
    *thread = -1;
    *ended = 0;
    return 0;
}

int uvdb_configure(const struct uvdb_config* config)
{
    uvdb_lifecycle_lock();
    if(__atomic_load_n(&uvdb_shutdown_pending, __ATOMIC_SEQ_CST) ||
       uvdb_state != UVDB_STATE_IDLE || uvdb_socket >= 0 ||
       uvdb_listen_socket >= 0)
    {
        uvdb_lifecycle_unlock();
        return -1;
    }

    unsigned short port = UVDB_DEFAULT_PORT;
    size_t max_buffer = UVDB_DEFAULT_MAX_BUFFER;
    if(config)
    {
        port = config->port;
        max_buffer = config->max_packet_buffer;
        if(!port || max_buffer < UVDB_MIN_BUFFER || max_buffer > UVDB_MAX_BUFFER)
        {
            uvdb_lifecycle_unlock();
            return -1;
        }
    }
    uvdb_port = port;
    uvdb_max_buffer = max_buffer;
    uvdb_lifecycle_unlock();
    return 0;
}

enum uvdb_state uvdb_get_state(void)
{
    return uvdb_state;
}

int uvdb_get_last_fault(struct uvdb_fault_info* info)
{
    if(!info)
        return -1;
    *info = uvdb_last_fault;
    return uvdb_last_fault.exception_type == UVDB_EXCEPTION_NONE ? 0 : 1;
}

static int uvdb_stop_server_locked(void)
{
    SceUID thread = uvdb_server_thread;
    #ifdef UVDB_KERNEL_THREAD_CONTROL
    SceUID lease_thread = uvdb_lease_thread;
    #endif
    SceUID caller = sceKernelGetThreadId();
    if(thread == caller
       #ifdef UVDB_KERNEL_THREAD_CONTROL
       || lease_thread == caller
       #endif
       )
        return -1;

    __atomic_store_n(&uvdb_server_stop, 1, __ATOMIC_SEQ_CST);
    /* Stop accepting producer bytes immediately. The serialized socket owner
     * completes generation cleanup after its service thread has exited. */
    uvdb_console_session_close_active_gate();
    /* Shutdown wakes recv without changing the descriptor to -1. A running
     * server can then enter its exception/all-stop cleanup path instead of
     * mistaking shutdown for a request to open a fresh listening socket. */
    uvdb_shutdown_socket(&uvdb_socket);
    // Do not take uvdb_lock here: the blocked service or exception path may be
    // holding it while waiting for network input.
    // Abort wakes a blocking accept without closing its descriptor. The owner
    // observes server_stop and performs the serialized close itself.
    uvdb_abort_socket(&uvdb_listen_socket);

    if(thread >= 0 &&
       uvdb_wait_delete_thread(&uvdb_server_thread,
                               &uvdb_server_thread_ended) < 0)
        return -1;
    if(thread < 0)
    {
        /* There is no joinable service-thread handle for a direct
         * uvdb_enter() owner. This barrier closes the race where it acquired
         * uvdb_lock just before the stop flag became visible but had not yet
         * published its listener when SocketAbort ran. real_uvdb_enter checks
         * the flag again before blocking in accept. Keep the lease helper
         * alive until that owner has finished any stopped-side cleanup. */
        uvdb_lock();
        uvdb_close_socket(&uvdb_socket);
        uvdb_close_socket(&uvdb_listen_socket);
        if(uvdb_state != UVDB_STATE_ERROR)
            uvdb_state = UVDB_STATE_IDLE;
        uvdb_unlock();
    }
    #ifdef UVDB_KERNEL_THREAD_CONTROL
    /* Keep the lease helper alive until the server has completed any
     * shutdown-triggered all-stop and breakpoint cleanup. */
    __atomic_store_n(&uvdb_lease_stop, 1, __ATOMIC_SEQ_CST);
    if(lease_thread >= 0 &&
       uvdb_wait_delete_thread(&uvdb_lease_thread,
                               &uvdb_lease_thread_ended) < 0)
        return -1;
    #endif
    if(thread >= 0)
    {
        uvdb_close_socket(&uvdb_socket);
        uvdb_close_socket(&uvdb_listen_socket);
    }
    return 0;
}

int uvdb_stop_server(void)
{
    uvdb_lifecycle_lock();
    int result = uvdb_stop_server_locked();
    uvdb_lifecycle_unlock();
    return result;
}

void uvdb_shutdown(void)
{
    uvdb_lifecycle_lock();
    __atomic_store_n(&uvdb_shutdown_pending, 1, __ATOMIC_SEQ_CST);
    uvdb_debugnet_stop();
    if(uvdb_stop_server_locked() < 0)
    {
        /* A helper may still be executing. Preserve every object it can
         * access so a later uvdb_shutdown call can retry safely. */
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_lifecycle_unlock();
        return;
    }
    /* The server can no longer initiate an all-stop that suspends the capture
     * helper while restore waits to join it. */
    if(uvdb_restore_stdio() < 0)
    {
        /* Preserve the console queue and descriptor backups for retry. */
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_lifecycle_unlock();
        return;
    }
    uvdb_lock();
#ifdef UVDB_KERNEL_THREAD_CONTROL
    int breakpoints_active = breakpoint_any_active();
    int coherent_stop = 0;
    int owns_stop = 0;
    if(breakpoints_active ||
       __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST))
    {
        uvdb_claim_stop_controller();
        owns_stop = 1;
        coherent_stop = uvdb_kernel_recover_stop() >= 0;
    }

    if((breakpoints_active ||
        __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST)) &&
       !coherent_stop)
    {
        /* A void shutdown API cannot report partial teardown. Preserve the
         * exception handlers, breakpoint table, and stop token so a later
         * retry can recover safely instead of resuming into an orphaned UDF. */
        uvdb_state = UVDB_STATE_ERROR;
        if(owns_stop)
            uvdb_release_stop_controller();
        uvdb_unlock();
        uvdb_lifecycle_unlock();
        return;
    }

    if(!breakpoints_active || coherent_stop)
        breakpoint_remove_all();
    if(__atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST))
    {
        int end_result = uvdb_kernel_end_stop();
        if(end_result < 0 &&
           __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST) &&
           !__atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST))
            end_result = uvdb_kernel_end_stop();
        if(end_result < 0)
        {
            uvdb_state = UVDB_STATE_ERROR;
            if(owns_stop)
                uvdb_release_stop_controller();
            uvdb_unlock();
            uvdb_lifecycle_unlock();
            return;
        }
    }
    if(owns_stop)
        uvdb_release_stop_controller();
#else
    breakpoint_remove_all();
#endif
    uvdb_close_socket(&uvdb_socket);
    uvdb_close_socket(&uvdb_listen_socket);
    uvdb_release_handlers();
    if(uvdb_pipe >= 0)
    {
        sceKernelDeleteMsgPipe(uvdb_pipe);
        uvdb_pipe = -1;
    }
    buffer_release(&in_buf);
    buffer_release(&out_buf);
    memset(uvdb_threads, 0, sizeof(uvdb_threads));
    uvdb_thread_inventory_reset(&uvdb_inventory);
    uvdb_thread_selection_reset(&uvdb_selection);
    uvdb_exception_thread = -1;
    uvdb_io_failed = 0;
    uvdb_target_stopped = 0;
    uvdb_async_stop_pending = 0;
    uvdb_async_stop_cancelled = 0;
    uvdb_server_stop = 0;
    #ifdef UVDB_KERNEL_THREAD_CONTROL
    uvdb_lease_stop = 0;
    uvdb_stop_failed = 0;
    uvdb_stop_owner = UVDB_STOP_OWNER_NONE;
    uvdb_stop_token = 0;
    #endif
    uvdb_console_reset();
    uvdb_console_transport_init(&uvdb_console_transport);
    __atomic_store_n(&uvdb_shutdown_pending, 0, __ATOMIC_SEQ_CST);
    uvdb_state = UVDB_STATE_IDLE;
    uvdb_unlock();
    uvdb_lifecycle_unlock();
}

#define POLL() while(cur == end) { size_t sz = buffer_poll(&in_buf, &cur); if(!sz && uvdb_io_failed) return 0; end = cur + sz; }

static size_t recv_packet(char** data)
{
    char* cur = in_buf.buf;
    char* end = cur + in_buf.size;
retry:;
    char c = 0;
    while(c != '$')
    {
        POLL();
        c = *cur++;
    }
    size_t start_packet = cur - in_buf.buf;
    while(c != '#')
    {
        POLL();
        c = *cur++;
    }
    size_t end_packet = cur - in_buf.buf - 1;
    uint8_t cksum = 0;
    for(size_t i = start_packet; i < end_packet; i++)
        cksum += (uint8_t)in_buf.buf[i];
    POLL();
    char c1 = *cur++;
    POLL();
    char c2 = *cur++;
    if(c1 != int2hex(cksum>>4) || c2 != int2hex(cksum&15))
        goto retry;
    if(!uvdb_console_transport_no_ack(&uvdb_console_transport))
    {
        buffer_write(&out_buf, "+", 1);
        buffer_flush(&out_buf);
    }
    *data = in_buf.buf + start_packet;
    in_buf.buf[end_packet] = 0;
    return end_packet - start_packet;
}

static void discard_packet(char* data, size_t sz)
{
    buffer_popleft(&in_buf, data - in_buf.buf + sz + 3);
}

static int send_packet(void)
{
    buffer_end_packet(&out_buf);
    buffer_flush(&out_buf);
    if(uvdb_io_failed)
        return -1;
    if(uvdb_console_transport_no_ack(&uvdb_console_transport))
        return 0;
    char* cur = in_buf.buf;
    char* end = cur + in_buf.size;
    char c = 0;
    while(c != '+')
    {
        POLL();
        c = *cur++;
    }
    buffer_popleft(&in_buf, cur - in_buf.buf);
    return 0;
}

#undef POLL
#define IS(s) (sz == sizeof(s) - 1 && !memcmp(pkt, s, sizeof(s) - 1))
#define STARTSWITH(s) (sz >= sizeof(s) - 1 && !memcmp(pkt, s, sizeof(s) - 1))
#define STRING(s) s, sizeof(s) - 1

struct stream
{
    uint64_t cur;
    uint64_t start;
    uint64_t end;
    size_t marker_index;
    int wrote;
};

#define PARSE_HEX(type, name, cond) static type name(char** s)\
{\
    type ans = 0;\
    for(cond)\
    {\
        char c = *(*s)++;\
        if(c >= '0' && c <= '9')\
            ans = 16 * ans + (c - '0');\
        else\
        {\
            c &= -33;\
            if(c >= 'A' && c <= 'F')\
                ans = 16 * ans + 10 + (c - 'A');\
            else\
                return ans;\
        }\
    }\
    return ans;\
}

PARSE_HEX(uint64_t, parse_hex, ;;)
PARSE_HEX(uint8_t, parse_hex_byte, int c = 0; c < 2; c++)

#undef PARSE_HEX

static struct stream parse_stream(char* s)
{
    struct stream ans = {};
    ans.marker_index = SIZE_MAX;
    ans.start = parse_hex(&s);
    uint64_t length = parse_hex(&s);
    ans.end = ans.start + length;
    if(ans.end < ans.start)
        ans.end = UINT64_MAX;
    return ans;
}

static void stream_write(struct stream* st, const char* buf, size_t sz)
{
    uint64_t chunk_start = st->cur;
    uint64_t chunk_end = chunk_start + sz;
    if(chunk_end < chunk_start)
        chunk_end = UINT64_MAX;
    st->cur = chunk_end;
    uint64_t copy_start = chunk_start < st->start ? st->start : chunk_start;
    uint64_t copy_end = chunk_end > st->end ? st->end : chunk_end;
    if(copy_start >= copy_end)
        return;
    if(!st->wrote)
    {
        st->marker_index = out_buf.size;
        buffer_write(&out_buf, "m", 1);
        st->wrote = 1;
    }
    size_t offset = (size_t)(copy_start - chunk_start);
    size_t count = (size_t)(copy_end - copy_start);
    buffer_write(&out_buf, buf + offset, count);
}

static void stream_close(struct stream* st)
{
    if(!st->wrote)
        buffer_write(&out_buf, "l", 1);
    else if(st->cur <= st->end)
        out_buf.buf[st->marker_index] = 'l';
}

static void stream_write_hex32(struct stream* st, uint32_t value)
{
    char text[] = "0x00000000";
    for(int i = 0; i < 8; ++i)
        text[9 - i] = int2hex((value >> (i * 4)) & 0xf);
    stream_write(st, text, sizeof(text) - 1);
}

static void stream_write_module_name(struct stream* st, const char* name,
                                     size_t capacity)
{
    for(size_t i = 0; i < capacity && name[i]; ++i)
    {
        unsigned char value = (unsigned char)name[i];
        char c = (char)value;
        /* Keep both XML and the unescaped RSP payload well formed. Vita
         * module names are normally printable ASCII, but a malformed name
         * must not be able to terminate or escape the packet. */
        if(value < 0x20 || value > 0x7e ||
           c == '&' || c == '<' || c == '>' || c == '\'' || c == '"' ||
           c == '$' || c == '#' || c == '}' || c == '*')
            c = '_';
        stream_write(st, &c, 1);
    }
}

static size_t module_segment_addresses(const SceKernelModuleInfo* info,
                                       uint32_t addresses[4])
{
    size_t count = 0;
    for(size_t segment = 0; segment < 4; ++segment)
        if(info->segments[segment].vaddr && info->segments[segment].memsz)
            addresses[count++] =
                (uint32_t)(uintptr_t)info->segments[segment].vaddr;
    return count;
}

static void stream_write_libraries(struct stream* st)
{
    stream_write(st, STRING("<library-list version=\"1.0\">"));
    SceUID modules[128];
    SceSize count = sizeof(modules) / sizeof(modules[0]);
    if(sceKernelGetModuleList(0xff, modules, &count) >= 0)
        for(SceSize i = 0; i < count; ++i)
        {
            SceKernelModuleInfo info = {.size = sizeof(info)};
            if(sceKernelGetModuleInfo(modules[i], &info) < 0)
                continue;
            if(!info.module_name[0])
                continue;
            uint32_t addresses[4];
            size_t segment_count = module_segment_addresses(&info, addresses);
            /* The GDB library-list DTD requires at least one segment. */
            if(!segment_count)
                continue;
            stream_write(st, STRING("<library name=\""));
            stream_write_module_name(st, info.module_name,
                                     sizeof(info.module_name));
            stream_write(st, STRING("\">"));
            for(size_t segment = 0; segment < segment_count; ++segment)
            {
                stream_write(st, STRING("<segment address=\""));
                stream_write_hex32(st, addresses[segment]);
                stream_write(st, STRING("\"/>"));
            }
            stream_write(st, STRING("</library>"));
        }
    stream_write(st, STRING("</library-list>"));
    stream_close(st);
}

static void write_hex(char* start, size_t sz)
{
    while(sz--)
    {
        uint8_t c = *start++;
        uint8_t q[2] = {int2hex(c>>4), int2hex(c&15)};
        buffer_write(&out_buf, q, 2);
    }
}

static void read_hex(char** p, char* start, size_t sz)
{
    while(sz--)
    {
        if(**p == 'x' && (*p)[1] == 'x')
        {
            *p += 2;
            start++;
        }
        else if(!**p || !(*p)[1])
            *start++ = 0;
        else
            *start++ = parse_hex_byte(p);
    }
}

static void skip_hex(char** p, size_t cnt)
{
    *p += strnlen(*p, 2*cnt);
}

#ifndef UVDB_KERNEL_THREAD_CONTROL
static void write_x(size_t sz)
{
    while(sz--)
        buffer_write(&out_buf, "xx", 2);
}
#endif

static int write_rsp_register_packet(
    const struct uvdb_rsp_core_registers* core,
    const struct uvdb_rsp_vfp_registers* vfp)
{
    char packet[UVDB_RSP_VFP_PACKET_HEX_SIZE];
    size_t packet_size = 0;
#ifdef UVDB_KERNEL_VFP_READS
    const int include_vfp = uvdb_rsp_vfp_enabled;
#else
    const int include_vfp = 0;
    (void)vfp;
#endif
    if(uvdb_rsp_encode_register_packet(packet, sizeof(packet), core, vfp,
                                        include_vfp, &packet_size) < 0)
        return -1;
    buffer_write(&out_buf, packet, packet_size);
    return 0;
}

static int write_rsp_single_register(
    uint32_t register_number,
    const struct uvdb_rsp_core_registers* core,
    const struct uvdb_rsp_vfp_registers* vfp)
{
    /* The widest individual register in the legacy layout is a 96-bit FPA
     * slot. The explicit VFP layout tops out at a 64-bit D register. */
    char packet[24];
    size_t packet_size = 0;
#ifdef UVDB_KERNEL_VFP_READS
    const int include_vfp = uvdb_rsp_vfp_enabled;
#else
    const int include_vfp = 0;
    (void)vfp;
#endif
    if(uvdb_rsp_encode_single_register(
           packet, sizeof(packet), core, vfp, include_vfp,
           register_number, &packet_size) < 0)
        return -1;
    buffer_write(&out_buf, packet, packet_size);
    return 0;
}

static void copy_exception_thread_registers(
    const KuKernelExceptionContext* ctx,
    struct uvdb_rsp_core_registers* core)
{
    memcpy(core->r, &ctx->r0, sizeof(core->r));
    core->cpsr = ctx->SPSR;
}

static void apply_exception_thread_registers(
    KuKernelExceptionContext* ctx,
    const struct uvdb_rsp_core_registers* core)
{
    memcpy(&ctx->r0, core->r, sizeof(core->r));
    ctx->SPSR = core->cpsr;
}

static int write_exception_thread_registers(KuKernelExceptionContext* ctx)
{
    struct uvdb_rsp_core_registers core;
    copy_exception_thread_registers(ctx, &core);
    // Kubridge's exception context does not currently expose the interrupted
    // thread's saved VFP bank. Preserve the negotiated shape but mark it
    // unavailable in experimental VFP builds.
    return write_rsp_register_packet(&core, NULL);
}

static int write_exception_thread_register(
    KuKernelExceptionContext* ctx,
    uint32_t register_number)
{
    struct uvdb_rsp_core_registers core;
    copy_exception_thread_registers(ctx, &core);
    /* Kubridge does not expose this exception context's saved VFP bank. A
     * negotiated VFP p request therefore receives an unavailable marker. */
    return write_rsp_single_register(register_number, &core, NULL);
}

#ifdef UVDB_KERNEL_THREAD_CONTROL
static int read_kernel_thread_registers(
    SceUID thread_id,
    struct uvdb_rsp_core_registers* core)
{
    unsigned int token = __atomic_load_n(&uvdb_stop_token,
                                          __ATOMIC_SEQ_CST);
    struct vd_thread_registers registers;
    if(!core || !token ||
       vdKernelGetThreadRegisters(token, thread_id, &registers) < 0)
        return -1;

    /* The raw entries are current/exception contexts, not fixed user/kernel
     * banks. Select the first valid user-mode context and reject snapshots
     * that expose only zero or privileged state. */
    const struct vd_arm_registers* user =
        uvdb_kernel_user_register_bank(&registers);
    if(!user)
        return -1;
    memcpy(core->r, user->r, sizeof(user->r));
    core->r[13] = user->sp;
    core->r[14] = user->lr;
    core->r[15] = user->pc;
    core->cpsr = user->cpsr;
    return 0;
}

#ifdef UVDB_KERNEL_VFP_READS
static int read_kernel_thread_vfp_registers(
    SceUID thread_id,
    struct uvdb_rsp_vfp_registers* vfp,
    int* available)
{
    unsigned int token = __atomic_load_n(&uvdb_stop_token,
                                          __ATOMIC_SEQ_CST);
    struct vd_thread_vfp_registers snapshot;
    if(!vfp || !available || !token)
        return -1;

    int snapshot_result =
        vdKernelGetThreadVfpRegisters(token, thread_id, &snapshot);
    if(snapshot_result < 0)
    {
        /* A stopped thread may not yet own a saved VFP context. Treat only
         * that normalized result as an unavailable register bank; a lost
         * session or any integrity error remains fatal. */
        if(uvdb_vfp_classify_snapshot_result(snapshot_result) !=
               UVDB_VFP_SNAPSHOT_UNAVAILABLE ||
           __atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST) ||
           __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST) != token)
            return -1;
        *available = 0;
        return 0;
    }
    if(snapshot.layout_version != VD_KERNEL_VFP_LAYOUT_D32_V1 ||
       snapshot.d_register_count != VD_KERNEL_VFP_D_REGISTER_COUNT)
        return -1;

    memcpy(vfp->d, snapshot.d, sizeof(vfp->d));
    /* The dedicated VFP probe established that D32 v1 stores FPSCR in raw
     * entry 0, independently of ARM's state-dependent core-bank selection. */
    vfp->fpscr = snapshot.fpscr_entry[VD_KERNEL_VFP_FPSCR_ENTRY_D32_V1];
    *available = 1;
    return 0;
}
#endif

static int write_kernel_thread_registers(SceUID thread_id)
{
    struct uvdb_rsp_core_registers core;
    if(read_kernel_thread_registers(thread_id, &core) < 0)
        return -1;

#ifdef UVDB_KERNEL_VFP_READS
    if(uvdb_rsp_vfp_enabled)
    {
        struct uvdb_rsp_vfp_registers vfp;
        int available = 0;
        if(read_kernel_thread_vfp_registers(
               thread_id, &vfp, &available) < 0)
            return -1;
        return write_rsp_register_packet(&core, available ? &vfp : NULL);
    }
#endif
    return write_rsp_register_packet(&core, NULL);
}

static int write_kernel_thread_register(
    SceUID thread_id,
    uint32_t register_number)
{
    struct uvdb_rsp_core_registers core;
    if(read_kernel_thread_registers(thread_id, &core) < 0)
        return -1;

#ifdef UVDB_KERNEL_VFP_READS
    if(uvdb_rsp_vfp_enabled &&
       register_number >= UVDB_RSP_REGISTER_VFP_D_FIRST)
    {
        struct uvdb_rsp_vfp_registers vfp;
        int available = 0;
        if(read_kernel_thread_vfp_registers(
               thread_id, &vfp, &available) < 0)
            return -1;
        return write_rsp_single_register(register_number, &core,
                                         available ? &vfp : NULL);
    }
#endif
    return write_rsp_single_register(register_number, &core, NULL);
}
#endif

static void write_hex_uint32(uint32_t value)
{
    char digits[8];
    size_t count = 0;
    do
    {
        digits[count++] = int2hex(value & 15);
        value >>= 4;
    }
    while(value && count < sizeof(digits));
    while(count)
        buffer_write(&out_buf, &digits[--count], 1);
}

static int uvdb_thread_is_visible(SceUID id)
{
    return uvdb_thread_inventory_contains(&uvdb_inventory, id);
}

static size_t safe_memcpy(char* dst, const char* src, size_t sz);

#define UVDB_MAX_BREAKPOINTS 32

struct uvdb_breakpoint
{
    uintptr_t address;
    uint8_t original[4];
    uint8_t size;
    uint8_t active;
    uint8_t temporary;
};

static struct uvdb_breakpoint uvdb_breakpoints[UVDB_MAX_BREAKPOINTS];

static struct uvdb_breakpoint* breakpoint_find(uintptr_t address)
{
    address &= ~(uintptr_t)1;
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].active && uvdb_breakpoints[i].address == address)
            return &uvdb_breakpoints[i];
    return NULL;
}

static int breakpoint_insert_internal(uintptr_t address, size_t size, int temporary)
{
    address &= ~(uintptr_t)1;
    if(size != 2 && size != 4)
        return -1;
    /* A tagged Thumb address is accepted for compatibility, but an A32 trap
     * must never straddle two instructions at a halfword-only address. */
    if(size == 4 && (address & 3u))
        return -1;
    if(breakpoint_find(address))
        return 0;

    struct uvdb_breakpoint* bp = NULL;
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(!uvdb_breakpoints[i].active)
        {
            bp = &uvdb_breakpoints[i];
            break;
        }
    if(!bp || safe_memcpy((char*)bp->original, (const char*)address, size) != size)
        return -1;

    static const uint8_t thumb_udf[2] = {0x00, 0xde};
    static const uint8_t arm_udf[4] = {0xf0, 0x00, 0xf0, 0xe7};
    const void* trap = size == 2 ? (const void*)thumb_udf : (const void*)arm_udf;
    kuKernelCpuUnrestrictedMemcpy((void*)address, trap, size);
    kuKernelFlushCaches((void*)address, size);
    bp->address = address;
    bp->size = (uint8_t)size;
    bp->temporary = temporary != 0;
    bp->active = 1;
    return 0;
}

static int breakpoint_insert(uintptr_t address, size_t size)
{
    return breakpoint_insert_internal(address, size, 0);
}

/* BXWritePC/LoadWritePC clear the state-selection bit and, for A32, the low
 * alignment bit as well. Canonicalize computed destinations before applying
 * breakpoint_insert_internal's stricter public-address validation. */
static int breakpoint_insert_step_target_sized(
    uintptr_t address,
    size_t size,
    uintptr_t current_pc)
{
    if(size != 2u && size != 4u)
        return -1;
    address &= size == 2u ? ~(uintptr_t)1 : ~(uintptr_t)3;
    if(address == (current_pc & ~(uintptr_t)1))
        return -1;
    return breakpoint_insert_internal(address, size, 1);
}

static int breakpoint_insert_step_target(
    uintptr_t address,
    uintptr_t current_pc)
{
    size_t size = (address & 1u) ? 2u : 4u;
    return breakpoint_insert_step_target_sized(address, size, current_pc);
}

static int breakpoint_remove(uintptr_t address)
{
    struct uvdb_breakpoint* bp = breakpoint_find(address);
    if(!bp)
        return 0;
    kuKernelCpuUnrestrictedMemcpy((void*)bp->address, bp->original, bp->size);
    kuKernelFlushCaches((void*)bp->address, bp->size);
    memset(bp, 0, sizeof(*bp));
    return 0;
}

static void breakpoint_remove_all(void)
{
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].active)
            breakpoint_remove(uvdb_breakpoints[i].address);
}

#ifdef UVDB_KERNEL_THREAD_CONTROL
static int breakpoint_any_active(void)
{
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].active)
            return 1;
    return 0;
}
#endif

static void breakpoint_remove_temporary(void)
{
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].active && uvdb_breakpoints[i].temporary)
            breakpoint_remove(uvdb_breakpoints[i].address);
}

static int breakpoint_insert_after_thumb_it(
    const struct uvdb_rsp_core_registers* core,
    uintptr_t pc,
    size_t instruction_size,
    unsigned int current_itstate)
{
    uintptr_t next = pc + instruction_size;
    unsigned int itstate = uvdb_thumb_itstate_advance(current_itstate);
    while(itstate)
    {
        if(uvdb_arm_condition_passed(itstate >> 4, core->cpsr))
            return breakpoint_insert_internal(next, 2, 1);

        uint16_t skipped;
        if(safe_memcpy((char*)&skipped, (const char*)next,
                       sizeof(skipped)) != sizeof(skipped))
            return -1;
        unsigned int prefix = skipped >> 11;
        next += (prefix == 0x1d || prefix == 0x1e || prefix == 0x1f)
            ? 4 : 2;
        itstate = uvdb_thumb_itstate_advance(itstate);
    }
    return breakpoint_insert_internal(next, 2, 1);
}

static int breakpoint_insert_step(const struct uvdb_rsp_core_registers* core)
{
    if(!core)
        return -1;
    uintptr_t pc = core->r[15];
    if(core->cpsr & 32)
    {
        uint16_t instruction;
        if(safe_memcpy((char*)&instruction, (const char*)pc, sizeof(instruction)) != sizeof(instruction))
            return -1;
        unsigned int prefix = instruction >> 11;
        size_t instruction_size = (prefix == 0x1d || prefix == 0x1e || prefix == 0x1f) ? 4 : 2;
        unsigned int current_itstate =
            uvdb_thumb_itstate_from_cpsr(core->cpsr);

        /* A condition-failed instruction inside an existing IT block has no
         * control-flow or register effects. Advance past subsequent skipped
         * slots so the temporary UDF itself cannot be conditionally skipped. */
        if(current_itstate &&
           !uvdb_arm_condition_passed(current_itstate >> 4, core->cpsr))
            return breakpoint_insert_after_thumb_it(
                core, pc, instruction_size, current_itstate);

        struct uvdb_step_target direct_target;
        int direct_result = uvdb_thumb16_plan_direct_step(
            instruction, (uint32_t)pc, core->cpsr, core->r,
            &direct_target);
        if(direct_result < 0)
            return -1;
        if(direct_result > 0)
            return breakpoint_insert_internal(direct_target.address,
                                               direct_target.breakpoint_size,
                                               1);

        // IT blocks conditionally execute up to four following instructions.
        // Decode the saved flags and stop at the first instruction that will
        // execute, or immediately after the block if every slot is skipped.
        if((instruction & 0xff00) == 0xbf00 &&
           (instruction & 0x000f) != 0 &&
           (instruction & 0x00f0) != 0x00f0)
        {
            unsigned int itstate = instruction & 0xff;
            uintptr_t next = pc + 2;
            for(int slot = 0; slot < 4; ++slot)
            {
                if(uvdb_arm_condition_passed(itstate >> 4, core->cpsr))
                    return breakpoint_insert_internal(next, 2, 1);

                uint16_t skipped;
                if(safe_memcpy((char*)&skipped, (const char*)next,
                               sizeof(skipped)) != sizeof(skipped))
                    return -1;
                unsigned int skipped_prefix = skipped >> 11;
                next += (skipped_prefix == 0x1d ||
                         skipped_prefix == 0x1e ||
                         skipped_prefix == 0x1f) ? 4 : 2;
                itstate = uvdb_thumb_itstate_advance(itstate);
                if(!itstate)
                    break;
            }
            return breakpoint_insert_internal(next, 2, 1);
        }

        // BX/BLX register.
        if((instruction & 0xff00) == 0x4700)
        {
            unsigned int rm = (instruction >> 3) & 0xf;
            const uint32_t* registers = core->r;
            uintptr_t target = rm == 15 ? pc + 4 : registers[rm];
            return breakpoint_insert_step_target(target, pc);
        }

        // MOV PC, Rm (high-register form). This remains in Thumb state.
        if((instruction & 0xff00) == 0x4600)
        {
            unsigned int rd = (instruction & 7) |
                              ((instruction >> 4) & 8);
            if(rd == 15)
            {
                unsigned int rm = (instruction >> 3) & 0xf;
                const uint32_t* registers = core->r;
                uintptr_t target = rm == 15 ? pc + 4 : registers[rm];
                return breakpoint_insert_step_target_sized(target, 2, pc);
            }
        }

        // POP {..., PC}. The saved PC follows each selected low register on
        // the current stack; read it without directly dereferencing user RAM.
        if((instruction & 0xff00) == 0xbd00)
        {
            unsigned int register_count =
                (unsigned int)__builtin_popcount(instruction & 0xff);
            uintptr_t target;
            const char* saved_pc = (const char*)(uintptr_t)
                (core->r[13] + register_count * sizeof(uint32_t));
            if(safe_memcpy((char*)&target, saved_pc, sizeof(target)) !=
               sizeof(target))
                return -1;
            return breakpoint_insert_step_target(target, pc);
        }

        if(instruction_size == 4)
        {
            uint16_t second;
            if(safe_memcpy((char*)&second, (const char*)(pc + 2), sizeof(second)) != sizeof(second))
                return -1;

            direct_result = uvdb_thumb32_plan_branch_step(
                instruction, second, (uint32_t)pc, core->cpsr,
                &direct_target);
            if(direct_result < 0)
                return -1;
            if(direct_result > 0)
                return breakpoint_insert_internal(
                    direct_target.address, direct_target.breakpoint_size, 1);

            // Thumb-2 table branch byte/halfword. Read the selected table
            // entry through safe_memcpy and branch relative to Align(PC, 4).
            if((instruction & 0xfff0) == 0xe8d0 &&
               (second & 0xffe0) == 0xf000)
            {
                unsigned int rn = instruction & 0xf;
                unsigned int rm = second & 0xf;
                unsigned int halfword = (second >> 4) & 1;
                if(rm == 15)
                    return -1;
                const uint32_t* registers = core->r;
                uintptr_t base = rn == 15
                    ? (pc + 4) & ~(uintptr_t)3
                    : registers[rn];
                uintptr_t table_address = base +
                    ((uintptr_t)registers[rm] << halfword);
                uint16_t table_offset = 0;
                size_t entry_size = halfword ? 2 : 1;
                if(safe_memcpy((char*)&table_offset,
                               (const char*)table_address, entry_size) !=
                   entry_size)
                    return -1;
                uintptr_t target = ((pc + 4) & ~(uintptr_t)3) +
                                   (uintptr_t)table_offset * 2;
                return breakpoint_insert_step_target_sized(target, 2, pc);
            }

            // Thumb-2 LDMIA/POP.W restoring PC. PC is stored after every
            // lower-numbered register selected by the register list.
            if((instruction & 0xffd0) == 0xe890 && (second & 0x8000))
            {
                unsigned int rn = instruction & 0xf;
                if(rn == 15)
                    return -1;
                const uint32_t* registers = core->r;
                unsigned int lower_count =
                    (unsigned int)__builtin_popcount(second & 0x7fff);
                uintptr_t saved_pc_address = registers[rn] +
                    lower_count * sizeof(uint32_t);
                uintptr_t target;
                if(safe_memcpy((char*)&target,
                               (const char*)saved_pc_address,
                               sizeof(target)) != sizeof(target))
                    return -1;
                return breakpoint_insert_step_target(target, pc);
            }

            // Thumb-2 LDMDB restoring PC. Since PC is the highest register,
            // its saved word is immediately below the original base address.
            if((instruction & 0xffd0) == 0xe910 && (second & 0x8000))
            {
                unsigned int rn = instruction & 0xf;
                if(rn == 15)
                    return -1;
                const uint32_t* registers = core->r;
                uintptr_t target;
                uintptr_t saved_pc_address = registers[rn] - sizeof(uint32_t);
                if(safe_memcpy((char*)&target,
                               (const char*)saved_pc_address,
                               sizeof(target)) != sizeof(target))
                    return -1;
                return breakpoint_insert_step_target(target, pc);
            }

            // Thumb-2 LDR.W PC, [Rn, #imm12]. Loads to PC can interwork, so
            // choose the destination breakpoint size from the loaded bit 0.
            if((instruction & 0xfff0) == 0xf8d0 &&
               (second & 0xf000) == 0xf000)
            {
                unsigned int rn = instruction & 0xf;
                const uint32_t* registers = core->r;
                uintptr_t base = rn == 15
                    ? (pc + 4) & ~(uintptr_t)3
                    : registers[rn];
                uintptr_t target;
                uintptr_t load_address = base + (second & 0x0fff);
                if(safe_memcpy((char*)&target, (const char*)load_address,
                               sizeof(target)) != sizeof(target))
                    return -1;
                return breakpoint_insert_step_target(target, pc);
            }

            // SUBS PC, LR, #imm8 exception return form.
            if(instruction == 0xf3de && (second & 0xff00) == 0x8f00)
                return breakpoint_insert_step_target(
                    core->r[14] - (second & 0xff), pc);

            if(uvdb_thumb32_instruction_may_write_pc(instruction, second))
                return -1;
        }

        if(instruction_size == 2 &&
           uvdb_thumb16_instruction_may_write_pc(instruction))
            return -1;
        if(current_itstate)
            return breakpoint_insert_after_thumb_it(
                core, pc, instruction_size, current_itstate);
        return breakpoint_insert_internal(pc + instruction_size, 2, 1);
    }

    uint32_t instruction;
    if(safe_memcpy((char*)&instruction, (const char*)pc, sizeof(instruction)) != sizeof(instruction))
        return -1;

    struct uvdb_step_target direct_target;
    int direct_result = uvdb_arm_plan_direct_step(
        instruction, (uint32_t)pc, core->cpsr, core->r, &direct_target);
    if(direct_result < 0)
        return -1;
    if(direct_result > 0)
        return breakpoint_insert_internal(direct_target.address,
                                           direct_target.breakpoint_size, 1);

    // ARM LDM variants that restore PC. Account for increment/decrement and
    // before/after addressing to locate PC's word in the transfer area.
    if((instruction & 0x0e108000) == 0x08108000)
    {
        unsigned int condition = instruction >> 28;
        if(!uvdb_arm_condition_passed(condition, core->cpsr))
            return breakpoint_insert_internal(pc + 4, 4, 1);
        unsigned int rn = (instruction >> 16) & 0xf;
        if(rn == 15)
            return -1;
        const uint32_t* registers = core->r;
        unsigned int lower_count =
            (unsigned int)__builtin_popcount(instruction & 0x7fff);
        unsigned int increment = (instruction >> 23) & 1;
        unsigned int before = (instruction >> 24) & 1;
        uintptr_t saved_pc_address;
        if(increment)
            saved_pc_address = registers[rn] +
                (lower_count + before) * sizeof(uint32_t);
        else
            saved_pc_address = registers[rn] -
                (before ? sizeof(uint32_t) : 0);
        uintptr_t target;
        if(safe_memcpy((char*)&target, (const char*)saved_pc_address,
                       sizeof(target)) != sizeof(target))
            return -1;
        return breakpoint_insert_step_target(target, pc);
    }

    // ARM LDR PC, [Rn, +/-imm12] including pre- and post-index forms.
    if((instruction & 0x0e50f000) == 0x0410f000)
    {
        unsigned int condition = instruction >> 28;
        if(!uvdb_arm_condition_passed(condition, core->cpsr))
            return breakpoint_insert_internal(pc + 4, 4, 1);
        unsigned int rn = (instruction >> 16) & 0xf;
        const uint32_t* registers = core->r;
        uintptr_t load_address = rn == 15 ? pc + 8 : registers[rn];
        if(instruction & (1u << 24))
        {
            uintptr_t offset = instruction & 0x0fff;
            load_address = instruction & (1u << 23)
                ? load_address + offset : load_address - offset;
        }
        uintptr_t target;
        if(safe_memcpy((char*)&target, (const char*)load_address,
                       sizeof(target)) != sizeof(target))
            return -1;
        return breakpoint_insert_step_target(target, pc);
    }

    /* Do not let an unsupported PC-writing form escape the sequential trap.
     * A failed condition is known to fall through; a taken shifted ALU write,
     * register-offset LDR PC, BXJ, or other undecoded transfer stays stopped
     * and is reported to GDB as an unsupported software-step request. */
    if(uvdb_arm_instruction_may_write_pc(instruction))
    {
        unsigned int condition = instruction >> 28;
        if(condition != 0xfu &&
           !uvdb_arm_condition_passed(condition, core->cpsr))
            return breakpoint_insert_internal(pc + 4, 4, 1);
        return -1;
    }

    return breakpoint_insert_internal(pc + 4, 4, 1);
}

static int breakpoint_insert_step_thread(
    SceUID thread_id,
    KuKernelExceptionContext* exception_context,
    int has_pc_override,
    uint32_t pc_override)
{
    struct uvdb_rsp_core_registers core;
    if(thread_id == uvdb_exception_thread)
        copy_exception_thread_registers(exception_context, &core);
#ifdef UVDB_KERNEL_THREAD_CONTROL
    else if(read_kernel_thread_registers(thread_id, &core) < 0)
        return -1;
#else
    else
        return -1;
#endif
    if(has_pc_override)
        core.r[15] = pc_override;
    return breakpoint_insert_step(&core);
}

static size_t safe_memcpy(char* dst, const char* src, size_t sz)
{
    size_t ans = 0;
    while(sz)
    {
        size_t chk;
        uint32_t rest[3] = {1, (uint32_t)&chk, 0};
        SceKernelAddrPair q = {(uint32_t)src, sz};
        if(_sceKernelSendMsgPipeVector(uvdb_pipe, &q, 1, rest))
            break;
        if(!chk)
            break;
        ans += chk;
        src += chk;
        sz -= chk;
        while(chk)
        {
            size_t chk2;
            uint32_t rest[3] = {1, (uint32_t)&chk2, 0};
            SceKernelAddrPair q = {(uint32_t)dst, chk};
            if(_sceKernelReceiveMsgPipeVector(uvdb_pipe, &q, 1, rest))
                return ans;
            if(!chk2)
                return ans;
            dst += chk2;
            chk -= chk2;
        }
    }
    return ans;
}

static int send_stop_reply(int signal)
{
    buffer_start_packet(&out_buf);
    uint8_t prefix[3] = {'T', int2hex(signal >> 4), int2hex(signal & 15)};
    buffer_write(&out_buf, prefix, sizeof(prefix));
    buffer_write(&out_buf, STRING("thread:"));
    write_hex_uint32((uint32_t)uvdb_selection.stopped);
    buffer_write(&out_buf, STRING(";"));
    return send_packet();
}

static int uvdb_pump_console_before_stop(void)
{
    if(uvdb_socket < 0 ||
       !uvdb_console_transport_no_ack(&uvdb_console_transport))
        return 0;
    int active_socket = uvdb_socket;
    int result = uvdb_console_transport_pump(
        &uvdb_console_transport,
        uvdb_console_raw_socket_write,
        &active_socket);
    if(result != UVDB_CONSOLE_PUMP_FATAL)
        return 0;

    uvdb_console_transport_end_connection(&uvdb_console_transport);
    uvdb_shutdown_socket_if_current(&uvdb_socket, active_socket);
    return -1;
}

#ifdef UVDB_KERNEL_THREAD_CONTROL
static void uvdb_claim_stop_controller(void)
{
    for(;;)
    {
        int expected = UVDB_STOP_OWNER_NONE;
        if(__atomic_compare_exchange_n(&uvdb_stop_owner, &expected,
                                        UVDB_STOP_OWNER_CONTROLLER, 0,
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
            return;
    }
}

static void uvdb_release_stop_controller(void)
{
    __atomic_store_n(&uvdb_stop_owner, UVDB_STOP_OWNER_NONE,
                     __ATOMIC_SEQ_CST);
}

static int uvdb_kernel_begin_stop(void)
{
    uvdb_claim_stop_controller();
    if(__atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST))
    {
        int active_result =
            __atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST) ? -1 : 0;
        uvdb_release_stop_controller();
        return active_result;
    }
    struct vd_kernel_stop_result result = {0};
    int status = vdKernelBeginStop(2000, uvdb_lease_thread, &result);
    if(status < 0 || !result.token)
    {
        uvdb_release_stop_controller();
        return -1;
    }
    /* Clear a prior lease failure before publishing the new usable token. */
    __atomic_store_n(&uvdb_stop_failed, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&uvdb_stop_token, result.token, __ATOMIC_SEQ_CST);
    int begin_result = result.already_suspended_count == 0 ? 0 : -1;
    uvdb_release_stop_controller();
    return begin_result;
}

static int uvdb_kernel_recover_stop(void)
{
    unsigned int token = __atomic_load_n(&uvdb_stop_token,
                                          __ATOMIC_SEQ_CST);
    if(token && vdKernelRenewStop(token, 2000) >= 0)
    {
        __atomic_store_n(&uvdb_stop_failed, 0, __ATOMIC_SEQ_CST);
        /* Controller ownership remains asserted for the caller's cleanup. */
        return 0;
    }

    /* The previous session may already have expired. Try to reacquire an
     * all-stop from this exception controller before declaring executable-
     * memory cleanup unsafe. */
    struct vd_kernel_stop_result recovery = {0};
    int recovery_result = vdKernelBeginStop(2000, uvdb_lease_thread,
                                             &recovery);
    if(recovery_result >= 0 && recovery.token)
    {
        __atomic_store_n(&uvdb_stop_failed, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&uvdb_stop_token, recovery.token,
                         __ATOMIC_SEQ_CST);
        /* Controller ownership remains asserted for the caller's cleanup. */
        return 0;
    }

    /* Keep the old token published on failure. It may still name a live
     * session that cleanup can explicitly end; dropping it here would force an
     * otherwise avoidable watchdog-only recovery. */
    __atomic_store_n(&uvdb_stop_failed, 1, __ATOMIC_SEQ_CST);
    return -1;
}

static int uvdb_kernel_end_stop(void)
{
    unsigned int token = __atomic_load_n(&uvdb_stop_token,
                                          __ATOMIC_SEQ_CST);
    if(!token)
        return __atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST) ? -1 : 0;

    /* Keep the token published until resume succeeds. If the kernel reports a
     * partial resume, recover a coherent all-stop before returning failure so
     * the common cleanup path can safely restore patched instructions. */
    int resumed = 0;
    int result = vdKernelEndStop(token, &resumed);
    if(result >= 0)
    {
        __atomic_store_n(&uvdb_stop_token, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&uvdb_stop_failed, 0, __ATOMIC_SEQ_CST);
        return 0;
    }
    return uvdb_kernel_recover_stop() >= 0 ? -1 : -2;
}

static void uvdb_kernel_abandon_stop(void)
{
    __atomic_store_n(&uvdb_stop_token, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&uvdb_stop_failed, 1, __ATOMIC_SEQ_CST);
}
#else
static int uvdb_kernel_begin_stop(void) { return 0; }
static int uvdb_kernel_end_stop(void) { return 0; }
#endif

static void uvdb_note_target_running(void)
{
    uvdb_thread_selection_note_resume(&uvdb_selection);
    uvdb_thread_inventory_reset(&uvdb_inventory);
    uvdb_exception_thread = -1;
    __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_SEQ_CST);
}

/* The caller owns UVDB_STOP_OWNER_CONTROLLER for this entire transaction.
 * Keeping ownership through every recovery/end retry prevents the lease
 * helper from publishing a stale renewal result between cleanup phases. */
static void uvdb_fail_stopped_client_owned(void)
{
    /* Restore patched instructions while the all-stop lease is still held. */
#ifdef UVDB_KERNEL_THREAD_CONTROL
    int breakpoints_active = breakpoint_any_active();
    int coherent_stop = 1;
    if(!__atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST) ||
       __atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST))
        coherent_stop = uvdb_kernel_recover_stop() >= 0;

    if(!uvdb_stop_cleanup_can_release(coherent_stop, breakpoints_active))
    {
        /* Never deliberately resume an uncertain session while UDF patches
         * remain. Preserve the token, handlers, breakpoint table, and stopped
         * state so a later fault or shutdown retry can reacquire all-stop and
         * restore code safely. The kernel lease watchdog remains the final
         * target-resume backstop. */
        uvdb_release_stop_controller();
        uvdb_close_socket(&uvdb_socket);
        uvdb_state = UVDB_STATE_ERROR;
        return;
    }

    if(coherent_stop)
        breakpoint_remove_all();

    int end_result = uvdb_kernel_end_stop();
    if(end_result < 0 &&
       __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST) &&
       !__atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST))
    {
        /* EndStop re-established a coherent all-stop before reporting its
         * partial-resume failure. Reassert code cleanup before one retry. */
        breakpoint_remove_all();
        end_result = uvdb_kernel_end_stop();
    }
    if(end_result < 0 &&
       __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST))
        uvdb_kernel_abandon_stop();
    uvdb_release_stop_controller();
#else
    breakpoint_remove_all();
    int end_result = uvdb_kernel_end_stop();
    (void)end_result;
#endif
    uvdb_close_socket(&uvdb_socket);
    uvdb_state = UVDB_STATE_ERROR;
    uvdb_note_target_running();
}

static void uvdb_fail_stopped_client(void)
{
#ifdef UVDB_KERNEL_THREAD_CONTROL
    uvdb_claim_stop_controller();
#endif
    uvdb_fail_stopped_client_owned();
}

static int uvdb_refresh_stopped_inventory(void)
{
#ifdef UVDB_KERNEL_THREAD_CONTROL
    uvdb_claim_stop_controller();
#endif
    if(uvdb_refresh_thread_inventory() >= 0)
    {
#ifdef UVDB_KERNEL_THREAD_CONTROL
        uvdb_release_stop_controller();
#endif
        return 0;
    }
    uvdb_fail_stopped_client_owned();
    return -1;
}

/* Hold background lease failure publication out of a mutation/resume window.
 * The synchronous refresh proves the session is current after ownership is
 * claimed. */
static int uvdb_begin_stopped_operation(void)
{
#ifdef UVDB_KERNEL_THREAD_CONTROL
    uvdb_claim_stop_controller();
#endif
    if(uvdb_refresh_thread_inventory() >= 0)
        return 0;
    uvdb_fail_stopped_client_owned();
    return -1;
}

static void uvdb_end_stopped_operation(void)
{
#ifdef UVDB_KERNEL_THREAD_CONTROL
    uvdb_release_stop_controller();
#endif
}

static int uvdb_apply_resume_plan(
    const struct uvdb_resume_plan* plan,
    KuKernelExceptionContext* ctx,
    int has_pc_override,
    uint32_t pc_override)
{
    if(!plan)
        return -1;
    if(plan->kind == UVDB_RESUME_STEP &&
       breakpoint_insert_step_thread(plan->step_thread, ctx,
                                     has_pc_override, pc_override) < 0)
        return -1;
    if(plan->kind != UVDB_RESUME_CONTINUE &&
       plan->kind != UVDB_RESUME_STEP)
        return -1;
    if(uvdb_kernel_end_stop() < 0)
        return -2;
    if(has_pc_override)
        ctx->pc = pc_override;
    return 0;
}

static void uvdb_finish_resume_packet(char* packet)
{
    /* The next exception parses this synthetic status query. Keeping it in the
     * receive buffer preserves the existing all-stop no-immediate-reply flow. */
    memcpy(packet, "?#3f", 4);
    out_buf.size--;
    buffer_flush(&out_buf);
    uvdb_note_target_running();
}

static void uvdb_main_loop(KuKernelExceptionContext* ctx, int stop_signal)
{
#ifdef UVDB_KERNEL_VFP_READS
    // Negotiate the extended register shape only with the exact matching ABI
    // and an explicitly enabled experimental kernel. A mismatched/default
    // plugin transparently retains the legacy core-only packet contract.
    uvdb_refresh_rsp_vfp_capability();
#endif
    for(;;)
    {
        char* pkt;
        size_t sz = recv_packet(&pkt);
#ifdef UVDB_KERNEL_THREAD_CONTROL
        if(__atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST))
        {
            uvdb_fail_stopped_client();
            return;
        }
#endif
        if(uvdb_io_failed)
        {
            uvdb_fail_stopped_client();
            return;
        }
        buffer_start_packet(&out_buf);
        int enable_no_ack = 0;
        if(IS("qSupported") || STARTSWITH("qSupported:"))
        {
            buffer_write(&out_buf, STRING("qXfer:features:read+;qXfer:libraries:read+;vContSupported+;QStartNoAckMode+;PacketSize="));
            write_hex_uint32((uint32_t)(uvdb_max_buffer - 4u));
        }
        else if(IS("QStartNoAckMode"))
        {
            buffer_write(&out_buf, STRING("OK"));
            enable_no_ack =
                !uvdb_console_transport_no_ack(&uvdb_console_transport);
        }
        else if(STARTSWITH("qXfer:features:read:target.xml:"))
        {
            struct stream st = parse_stream(pkt + sizeof("qXfer:features:read:target.xml:") - 1);
#ifdef UVDB_KERNEL_VFP_READS
            if(uvdb_rsp_vfp_enabled)
                stream_write(&st, uvdb_arm_vfp_target_xml,
                             sizeof(uvdb_arm_vfp_target_xml) - 1);
            else
#endif
                stream_write(&st, STRING("<?xml version=\"1.0\"?>\n<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n<target>\n<architecture>armv7</architecture>\n<osabi>GNU/Linux</osabi>\n</target>\n"));
            stream_close(&st);
        }
        else if(STARTSWITH("qXfer:libraries:read::"))
        {
            struct stream st = parse_stream(
                pkt + sizeof("qXfer:libraries:read::") - 1);
            stream_write_libraries(&st);
        }
        else if(IS("?"))
        {
            if(uvdb_refresh_stopped_inventory() < 0)
                return;
            out_buf.size--;
            if(uvdb_pump_console_before_stop() < 0)
            {
                discard_packet(pkt, sz);
                uvdb_fail_stopped_client();
                return;
            }
            if(send_stop_reply(stop_signal) < 0)
            {
                discard_packet(pkt, sz);
                uvdb_fail_stopped_client();
                return;
            }
            discard_packet(pkt, sz);
            continue;
        }
        else if(IS("qfThreadInfo"))
        {
            if(uvdb_refresh_stopped_inventory() < 0)
                return;
            if(!uvdb_inventory.count)
                buffer_write(&out_buf, STRING("l"));
            else
            {
                buffer_write(&out_buf, STRING("m"));
                for(size_t i = 0; i < uvdb_inventory.count; ++i)
                {
                    if(i)
                        buffer_write(&out_buf, STRING(","));
                    write_hex_uint32((uint32_t)uvdb_inventory.ids[i]);
                }
            }
        }
        else if(IS("qsThreadInfo"))
            buffer_write(&out_buf, STRING("l"));
        else if(IS("qC"))
        {
            if(uvdb_refresh_stopped_inventory() < 0)
                return;
            buffer_write(&out_buf, STRING("QC"));
            write_hex_uint32((uint32_t)uvdb_selection.stopped);
        }
        else if(IS("qAttached"))
            buffer_write(&out_buf, STRING("1"));
        else if(STARTSWITH("qThreadExtraInfo,"))
        {
            const size_t prefix_size = sizeof("qThreadExtraInfo,") - 1;
            int32_t id = -1;
            if(uvdb_refresh_stopped_inventory() < 0)
                return;
            if(uvdb_rsp_parse_thread_id(pkt + prefix_size, sz - prefix_size,
                                        &id) < 0 ||
               !uvdb_thread_is_visible(id))
                buffer_write(&out_buf, STRING("E16"));
            else
            {
                struct uvdb_thread_entry* entry = uvdb_find_thread(id);
                if(entry && entry->name[0])
                    write_hex(entry->name, strlen(entry->name));
                else if(id == uvdb_selection.stopped)
                    write_hex("stopped thread", sizeof("stopped thread") - 1);
                else
                    write_hex("process thread", sizeof("process thread") - 1);
            }
        }
        else if(sz >= 3 && pkt[0] == 'H' &&
                (pkt[1] == 'g' || pkt[1] == 'c'))
        {
            if(uvdb_refresh_stopped_inventory() < 0)
                return;
            if(uvdb_thread_selection_apply(&uvdb_selection, pkt[1], pkt + 2,
                                            sz - 2, &uvdb_inventory) < 0)
                buffer_write(&out_buf, STRING("E16"));
            else
                buffer_write(&out_buf, STRING("OK"));
        }
        else if(sz > 1 && pkt[0] == 'T')
        {
            int32_t id = -1;
            if(uvdb_refresh_stopped_inventory() < 0)
                return;
            int visible = uvdb_rsp_parse_thread_id(pkt + 1, sz - 1, &id) == 0 &&
                          uvdb_thread_is_visible(id);
            buffer_write(&out_buf, visible ? "OK" : "E16", visible ? 2 : 3);
        }
        else if(IS("vCont?"))
            buffer_write(&out_buf, STRING("vCont;c;s"));
        else if(STARTSWITH("vCont;"))
        {
            struct uvdb_resume_plan plan;
            if(uvdb_begin_stopped_operation() < 0)
                return;
            int parse_result = uvdb_rsp_parse_vcont(
                pkt, sz, &uvdb_inventory, &plan);
            int resume_result = parse_result < 0 ? -1 :
                uvdb_apply_resume_plan(&plan, ctx, 0, 0);
            if(resume_result == -1)
            {
                uvdb_end_stopped_operation();
                buffer_write(&out_buf, STRING("E16"));
            }
            else if(resume_result < 0)
            {
                uvdb_fail_stopped_client_owned();
                return;
            }
            else
            {
                uvdb_end_stopped_operation();
                uvdb_finish_resume_packet(pkt);
                return;
            }
        }
        else if(IS("g"))
        {
            if(uvdb_refresh_stopped_inventory() < 0)
                return;
            SceUID general_thread = uvdb_thread_selection_general(
                &uvdb_selection, &uvdb_inventory);
            if(general_thread <= 0)
                buffer_write(&out_buf, STRING("E16"));
            else if(general_thread != uvdb_exception_thread)
#ifdef UVDB_KERNEL_THREAD_CONTROL
            {
                if(write_kernel_thread_registers(general_thread) < 0)
                    buffer_write(&out_buf, STRING("E16"));
            }
#else
                write_x(42 * 4);
#endif
            else
            {
                if(write_exception_thread_registers(ctx) < 0)
                    buffer_write(&out_buf, STRING("E16"));
            }
        }
        else if(sz && pkt[0] == 'p')
        {
#ifdef UVDB_KERNEL_VFP_READS
            const int include_vfp = uvdb_rsp_vfp_enabled;
#else
            const int include_vfp = 0;
#endif
            uint32_t register_number = 0;
            if(uvdb_rsp_parse_register_read_packet(
                   pkt, sz, include_vfp, &register_number) < 0)
                buffer_write(&out_buf, STRING("E01"));
            else
            {
                if(uvdb_refresh_stopped_inventory() < 0)
                    return;
                SceUID general_thread = uvdb_thread_selection_general(
                    &uvdb_selection, &uvdb_inventory);
                if(general_thread <= 0)
                    buffer_write(&out_buf, STRING("E16"));
                else if(general_thread != uvdb_exception_thread)
#ifdef UVDB_KERNEL_THREAD_CONTROL
                {
                    if(write_kernel_thread_register(
                           general_thread, register_number) < 0)
                        buffer_write(&out_buf, STRING("E16"));
                }
#else
                    buffer_write(&out_buf, STRING("E16"));
#endif
                else if(write_exception_thread_register(
                            ctx, register_number) < 0)
                    buffer_write(&out_buf, STRING("E16"));
            }
        }
        else if(STARTSWITH("m"))
        {
            char* p = pkt + 1;
            uintptr_t addr = parse_hex(&p);
            size_t size = parse_hex(&p);
            while(size)
            {
                size_t chk = size;
                if(chk > 64)
                    chk = 64;
                char buf[64];
                size_t copy_sz = safe_memcpy(buf, (void*)addr, chk);
                write_hex(buf, copy_sz);
                if(copy_sz < chk)
                    break;
                addr += chk;
                size -= chk;
            }
        }
        else if(STARTSWITH("G"))
        {
#ifdef UVDB_KERNEL_VFP_READS
            if(uvdb_rsp_vfp_enabled)
            {
                // Do not acknowledge a full register write while VFP writes
                // are unsupported; that would silently discard its VFP tail.
                buffer_write(&out_buf, STRING("E16"));
            }
            else
#endif
            {
                if(uvdb_begin_stopped_operation() < 0)
                    return;
                SceUID general_thread = uvdb_thread_selection_general(
                    &uvdb_selection, &uvdb_inventory);
                if(general_thread <= 0 ||
                   general_thread != uvdb_exception_thread)
                    buffer_write(&out_buf, STRING("E16"));
                else
                {
                    char* p = pkt + 1;
                    read_hex(&p, (void*)ctx, 16*4);
                    skip_hex(&p, 25*4);
                    read_hex(&p, (void*)&ctx->SPSR, 4);
                    buffer_write(&out_buf, "OK", 2);
                }
                uvdb_end_stopped_operation();
            }
        }
        else if(sz && pkt[0] == 'P')
        {
#ifdef UVDB_KERNEL_VFP_READS
            const int include_vfp = uvdb_rsp_vfp_enabled;
#else
            const int include_vfp = 0;
#endif
            struct uvdb_rsp_core_register_write write;
            int parse_result = uvdb_rsp_parse_core_register_write_packet(
                pkt, sz, include_vfp, &write);
            if(parse_result == UVDB_RSP_REGISTER_UNSUPPORTED)
                buffer_write(&out_buf, STRING("E16"));
            else if(parse_result < 0)
                buffer_write(&out_buf, STRING("E01"));
            else
            {
                /* Keep individual writes inside the same renewed all-stop
                 * ownership boundary as G and memory mutation. The kernel ABI
                 * does not yet provide coherent foreign-thread restoration,
                 * so only the exception context can be changed safely. */
                if(uvdb_begin_stopped_operation() < 0)
                    return;
                SceUID general_thread = uvdb_thread_selection_general(
                    &uvdb_selection, &uvdb_inventory);
                if(general_thread <= 0 ||
                   general_thread != uvdb_exception_thread)
                    buffer_write(&out_buf, STRING("E16"));
                else
                {
                    struct uvdb_rsp_core_registers core;
                    copy_exception_thread_registers(ctx, &core);
                    if(uvdb_rsp_apply_core_register_write(
                           &core, &write, NULL) < 0)
                        buffer_write(&out_buf, STRING("E16"));
                    else
                    {
                        apply_exception_thread_registers(ctx, &core);
                        buffer_write(&out_buf, STRING("OK"));
                    }
                }
                uvdb_end_stopped_operation();
            }
        }
        else if(STARTSWITH("M"))
        {
            if(uvdb_begin_stopped_operation() < 0)
                return;
            char* p = pkt + 1;
            uintptr_t addr = parse_hex(&p);
            size_t size = parse_hex(&p);
            while(size)
            {
                size_t chk = size;
                if(chk > 64)
                    chk = 64;
                char buf[64];
                read_hex(&p, buf, chk);
                char test[64];
                //this is racey, but should work in practice
                size_t safe_size = safe_memcpy(test, (void*)addr, chk);
                kuKernelCpuUnrestrictedMemcpy((void*)addr, buf, safe_size);
                kuKernelFlushCaches((void*)addr, safe_size);
                if(safe_size < chk)
                    break;
                addr += chk;
                size -= chk;
            }
            if(size)
                buffer_write(&out_buf, "E0e", 3);
            else
                buffer_write(&out_buf, "OK", 2);
            uvdb_end_stopped_operation();
        }
        else if(STARTSWITH("Z0,") || STARTSWITH("z0,"))
        {
            if(uvdb_begin_stopped_operation() < 0)
                return;
            int insert = pkt[0] == 'Z';
            char* p = pkt + 3;
            uintptr_t address = parse_hex(&p);
            size_t kind = parse_hex(&p);
            int result = insert ? breakpoint_insert(address, kind) : breakpoint_remove(address);
            buffer_write(&out_buf, result < 0 ? "E16" : "OK", result < 0 ? 3 : 2);
            uvdb_end_stopped_operation();
        }
        else if(IS("k"))
            _sceKernelExitProcessForUser(1);
        else if(IS("D"))
        {
            if(uvdb_begin_stopped_operation() < 0)
                return;
            /* Code must be restored before releasing the all-stop boundary. */
            breakpoint_remove_all();
            if(uvdb_kernel_end_stop() < 0)
            {
                uvdb_fail_stopped_client_owned();
                return;
            }
            uvdb_end_stopped_operation();
            buffer_write(&out_buf, STRING("OK"));
            discard_packet(pkt, sz);
            int detach_result = send_packet();
            uvdb_close_socket(&uvdb_socket);
            uvdb_state = detach_result < 0 ? UVDB_STATE_ERROR
                                           : UVDB_STATE_IDLE;
            uvdb_note_target_running();
            uvdb_thread_selection_reset(&uvdb_selection);
            return;
        }
        else if(sz && (pkt[0] == 'C' || pkt[0] == 'S'))
            buffer_write(&out_buf, STRING("E16"));
        else if(sz && (pkt[0] == 'c' || pkt[0] == 's'))
        {
            int stepping = pkt[0] == 's';
            int has_address = sz > 1;
            uint32_t address = 0;
            if(uvdb_begin_stopped_operation() < 0)
                return;
            struct uvdb_resume_plan plan;
            int invalid = uvdb_thread_selection_plan_legacy(
                &uvdb_selection, &uvdb_inventory, stepping, &plan) < 0;
            SceUID legacy_thread = uvdb_thread_selection_step(
                &uvdb_selection, &uvdb_inventory);
            if(has_address &&
               uvdb_rsp_parse_u32_hex(pkt + 1, sz - 1, &address) < 0)
                invalid = 1;
            if(has_address && !invalid &&
               legacy_thread != uvdb_exception_thread)
                invalid = 1;

            int resume_result = invalid ? -1 :
                uvdb_apply_resume_plan(&plan, ctx, has_address, address);
            if(resume_result == -1)
            {
                uvdb_end_stopped_operation();
                buffer_write(&out_buf, "E16", 3);
                discard_packet(pkt, sz);
                send_packet();
                continue;
            }
            if(resume_result < 0)
            {
                uvdb_fail_stopped_client_owned();
                return;
            }
            uvdb_end_stopped_operation();
            uvdb_finish_resume_packet(pkt);
            return; // no breakpoint cleanup; persistent points remain armed
        }
        else if(STARTSWITH("F"))
        {
            if(uvdb_begin_stopped_operation() < 0)
                return;
            char* p = pkt + 1;
            ctx->r0 = parse_hex(&p);
            //see above for explanation what this does
            memcpy(pkt, "?#3f", 4);
            for(size_t i = 1; i < sz; i++)
                pkt[i+3] = 0;
            out_buf.size--;
            buffer_flush(&out_buf);
            if(uvdb_kernel_end_stop() < 0)
            {
                uvdb_fail_stopped_client_owned();
                return;
            }
            uvdb_end_stopped_operation();
            uvdb_note_target_running();
            return;
        }
        else if(IS("qOffsets"))
        {
            SceUID module = sceKernelGetModuleIdByAddr(__executable_start);
            SceKernelModuleInfo info = {.size = sizeof(info)};
            uint32_t addresses[4];
            size_t segment_count = 0;
            if(module >= 0 && sceKernelGetModuleInfo(module, &info) >= 0)
                segment_count = module_segment_addresses(&info, addresses);
            if(!segment_count)
                buffer_write(&out_buf, STRING("E01"));
            else
            {
                /* TextSeg/DataSeg are absolute PT_LOAD start addresses, not
                 * relocation deltas. Deriving them from module metadata keeps
                 * qOffsets consistent with qXfer:libraries:read even when the
                 * linker's first writable section is not .init_array. */
                buffer_write(&out_buf, STRING("TextSeg="));
                write_hex_uint32(addresses[0]);
                if(segment_count > 1)
                {
                    buffer_write(&out_buf, STRING(";DataSeg="));
                    write_hex_uint32(addresses[1]);
                }
            }
        }
        discard_packet(pkt, sz);
        if(send_packet() < 0)
        {
            uvdb_fail_stopped_client();
            return;
        }
        if(enable_no_ack)
        {
            if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
            {
                uvdb_fail_stopped_client();
                return;
            }
            int enable_result = uvdb_console_transport_enable_no_ack(
                &uvdb_console_transport);
            if(enable_result != UVDB_CONSOLE_READY &&
               enable_result != UVDB_CONSOLE_BUSY)
            {
                uvdb_fail_stopped_client();
                return;
            }
            if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
            {
                uvdb_fail_stopped_client();
                return;
            }
        }
    }
}

#undef WRITE
#undef STARTSWITH
#undef IS

static __attribute__((naked)) void uvdb_trap_pc(void)
{
    asm volatile("udf #0");
}

static void exception_handler(KuKernelExceptionContext* ctx)
{
    SceUID exception_thread = sceKernelGetThreadId();
    int internal_controller = uvdb_is_controller_thread(exception_thread);
    __atomic_store_n(&uvdb_target_stopped, 1, __ATOMIC_SEQ_CST);
    int signal = SIGSEGV;
    if(ctx->exceptionType == KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION)
        signal = SIGILL;
    uint32_t pc = ctx->pc;
    if((ctx->SPSR & 32))
    {
        ctx->SPSR &= -33;
        pc |= 1;
    }
    if(pc == (uint32_t)uvdb_trap_pc)
    {
        pc = ctx->r0;
        signal = SIGTRAP;
    }
    else if(ctx->exceptionType == KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION && breakpoint_find(pc))
    {
        signal = SIGTRAP;
    }
    if((pc & 1))
    {
        ctx->SPSR |= 32;
        pc &= -2;
    }
    ctx->pc = pc;
    uvdb_lock();
    int async_stop = 0;
    if(internal_controller)
    {
        if(__atomic_exchange_n(&uvdb_async_stop_cancelled, 0,
                                __ATOMIC_SEQ_CST))
        {
            /* A real application fault won the race with this queued server
             * Ctrl-C trap and already satisfied GDB's stop request. */
            uvdb_note_target_running();
            uvdb_unlock();
            return;
        }
        async_stop = __atomic_exchange_n(&uvdb_async_stop_pending, 0,
                                          __ATOMIC_SEQ_CST);
    }
    else
    {
        int expected = 1;
        if(__atomic_compare_exchange_n(&uvdb_async_stop_pending, &expected,
                                        0, 0, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
            __atomic_store_n(&uvdb_async_stop_cancelled, 1,
                              __ATOMIC_SEQ_CST);
    }
    uvdb_last_fault.exception_type = (enum uvdb_exception_type)ctx->exceptionType;
    uvdb_last_fault.signal = signal;
    uvdb_last_fault.fault_status = ctx->FSR;
    uvdb_last_fault.fault_address = ctx->FAR;
    uvdb_last_fault.pc = pc;
    uvdb_last_fault.lr = ctx->lr;
    uvdb_last_fault.sp = ctx->sp;
    uvdb_exception_thread = exception_thread;
    if(internal_controller)
        uvdb_thread_selection_note_resume(&uvdb_selection);
    else
        uvdb_thread_selection_note_stop(&uvdb_selection,
                                        exception_thread, NULL);
    if(uvdb_kernel_begin_stop() < 0)
    {
        uvdb_fail_stopped_client();
        uvdb_unlock();
        return;
    }
    if(uvdb_begin_stopped_operation() < 0)
    {
        uvdb_unlock();
        return;
    }
    if(internal_controller)
    {
        if(!uvdb_inventory.count)
        {
            uvdb_fail_stopped_client_owned();
            uvdb_unlock();
            return;
        }
        uvdb_thread_selection_note_stop(&uvdb_selection,
                                        uvdb_inventory.ids[0],
                                        &uvdb_inventory);
    }
    else if(!uvdb_thread_inventory_contains(&uvdb_inventory,
                                             exception_thread))
    {
        uvdb_fail_stopped_client_owned();
        uvdb_unlock();
        return;
    }
    /* All application threads are now stopped; restoring a temporary trap is
     * no longer racing a peer executing the same code page. */
    breakpoint_remove_temporary();
    uvdb_end_stopped_operation();
    int reported_signal = async_stop ? SIGINT : signal;
    /* Every successful resume leaves one synthetic '?' packet in the receive
     * buffer. Let the common query path emit the sole stop reply, including
     * for an asynchronous Ctrl-C. Sending here as well would produce two T02
     * packets and leave a stale stop notification in a no-ack GDB session. */
    uvdb_main_loop(ctx, reported_signal);
    uvdb_unlock();
}

int uvdb_remote_syscall(const char* name, int nargs, ...)
{
    KuKernelExceptionContext ctx = {};
    uvdb_lock();
    if(uvdb_socket < 0)
    {
        //someone attempted to do a remote syscall before the first uvdb_enter
        //do a uvdb_enter now to avoid confusing the code
        uvdb_unlock();
        uvdb_enter();
        uvdb_lock();
    }
    char* pkt;
    size_t sz = recv_packet(&pkt);
    discard_packet(pkt, sz);
    buffer_start_packet(&out_buf);
    buffer_write(&out_buf, "F", 1);
    buffer_write(&out_buf, name, strlen(name));
    va_list va;
    va_start(va, nargs);
    for(int i = 0; i < nargs; i++)
    {
        uintptr_t value = va_arg(va, uintptr_t);
        char packet[9] = ",";
        for(int i = 0; i < 8; i++)
            packet[8-i] = int2hex((value >> (4*i)) & 15);
        buffer_write(&out_buf, packet, 9);
    }
    va_end(va);
    send_packet();
    uvdb_main_loop(&ctx, 0);
    uvdb_unlock();
    return ctx.r0;
}

static __attribute__((used)) uint64_t real_uvdb_enter(uintptr_t lr)
{
    uint64_t no_trap = (uint64_t)lr << 32 | lr;
    uint64_t trap = (uint64_t)(uint32_t)uvdb_trap_pc << 32 | lr;
#ifdef UVDB_KERNEL_THREAD_CONTROL
    /* A stale companion may retain import-compatible NIDs while exposing an
     * older stop/register ABI. Refuse direct entry before installing handlers
     * or opening a debugger socket. */
    if(!uvdb_kernel_status_is_compatible())
    {
        uvdb_state = UVDB_STATE_ERROR;
        return no_trap;
    }
#endif
    uvdb_register_thread(NULL);
    uvdb_lock();
    if(uvdb_socket >= 0)
    {
        uvdb_unlock();
        return trap;
    }
    if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
    {
        uvdb_unlock();
        return no_trap;
    }
    uvdb_io_failed = 0;
    in_buf.size = 0;
    out_buf.size = 0;
    uvdb_thread_selection_reset(&uvdb_selection);
    uvdb_thread_inventory_reset(&uvdb_inventory);
    uvdb_exception_thread = -1;
    __atomic_store_n(&uvdb_async_stop_pending, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&uvdb_async_stop_cancelled, 0, __ATOMIC_SEQ_CST);
    if(uvdb_pipe < 0)
    {
        uvdb_pipe = sceKernelCreateMsgPipe("pipe to catch efault", 0x40, 0xc, 4*4096, NULL);
        if(uvdb_pipe < 0)
        {
            uvdb_unlock();
            return no_trap;
        }
    }
    if(!uvdb_handler_mask)
    {
        struct KuKernelExceptionHandlerOpt opt = {
            .size = sizeof(opt),
        };
        KuKernelExceptionHandler old;
        const unsigned int all_handlers =
            (1u << KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT) |
            (1u << KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT) |
            (1u << KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION);
        if(kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, exception_handler, &old, &opt) >= 0)
            uvdb_handler_mask |= 1u << KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT;
        if(kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT, exception_handler, &old, &opt) >= 0)
            uvdb_handler_mask |= 1u << KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT;
        if(kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION, exception_handler, &old, &opt) >= 0)
            uvdb_handler_mask |= 1u << KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION;
        if(uvdb_handler_mask != all_handlers)
        {
            uvdb_release_handlers();
            uvdb_state = UVDB_STATE_ERROR;
            uvdb_unlock();
            return no_trap;
        }
    }
    int listen_socket = sceNetSyscallSocket("gdb socket", AF_INET,
                                            SOCK_STREAM, 0);
    if(listen_socket < 0)
    {
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    if(uvdb_publish_socket(&uvdb_listen_socket, listen_socket) < 0)
    {
        uvdb_close_socket(&listen_socket);
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    int value = 1;
    uint32_t args[5] = {listen_socket, SOL_SOCKET, SO_REUSEADDR,
                        (uint32_t)&value, sizeof(value)};
    if(sceNetSyscallSetsockopt((void*)&args))
    {
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    args[1] = IPPROTO_TCP;
    args[2] = TCP_NODELAY;
    if(sceNetSyscallSetsockopt((void*)&args))
    {
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr = {},
        .sin_port = htons(uvdb_port),
    };
    if(sceNetSyscallBind(listen_socket, &sin, sizeof(sin)))
    {
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    uvdb_state = UVDB_STATE_LISTENING;
    if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
    {
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = UVDB_STATE_IDLE;
        uvdb_unlock();
        return no_trap;
    }
    if(sceNetSyscallListen(listen_socket, 1))
    {
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }

    int accepted_socket = sceNetSyscallAccept(listen_socket, NULL, NULL);

    if(accepted_socket < 0)
    {
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = __atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST)
                         ? UVDB_STATE_IDLE
                         : UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    if(uvdb_publish_socket(&uvdb_socket, accepted_socket) < 0)
    {
        uvdb_close_socket(&accepted_socket);
        uvdb_close_socket(&uvdb_listen_socket);
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
    }
    uvdb_close_socket(&uvdb_listen_socket);
    /* stop_server may have observed no connected socket immediately before
     * accept published this one. Never enter the protocol loop in that race. */
    if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
    {
        uvdb_close_socket(&uvdb_socket);
        uvdb_state = UVDB_STATE_IDLE;
        uvdb_unlock();
        return no_trap;
    }
    uvdb_console_transport_begin_connection(&uvdb_console_transport);
    uvdb_state = UVDB_STATE_CONNECTED;
    uvdb_unlock();
    return trap;
}

__attribute__((naked)) void uvdb_enter(void)
{
    asm volatile(
        "mov r0, lr\n"
        "bl real_uvdb_enter\n"
        "bx r1\n"
    );
}

/* This callback can run from the stopped exception path, so keep it on raw
 * network syscalls and avoid public-wrapper TLS/errno state. */
static enum uvdb_console_write_result uvdb_console_raw_socket_write(
    void* context,
    const void* data,
    size_t size,
    size_t* bytes_sent,
    int* native_error)
{
    int socket = *(const int*)context;
    uint32_t send_args[6] = {
        (uint32_t)socket,
        (uint32_t)data,
        (uint32_t)size,
        MSG_DONTWAIT,
        0,
        0,
    };
    int result = sceNetSyscallSendto((void*)send_args);
    *bytes_sent = 0;
    *native_error = result < 0 ? result : 0;
    if((uint32_t)result == (uint32_t)SCE_NET_ERROR_EAGAIN)
        return UVDB_CONSOLE_WRITE_WOULD_BLOCK;
    if(result < 0)
        return UVDB_CONSOLE_WRITE_ERROR;
    *bytes_sent = (size_t)result;
    return UVDB_CONSOLE_WRITE_COMPLETE;
}

/* The running service thread may use the public wrapper, which gives a
 * documented errno location when its nonblocking send returns plain -1. */
static enum uvdb_console_write_result uvdb_console_server_socket_write(
    void* context,
    const void* data,
    size_t size,
    size_t* bytes_sent,
    int* native_error)
{
    int socket = *(const int*)context;
    int result = sceNetSend(socket, data, (unsigned int)size,
                            SCE_NET_MSG_DONTWAIT);
    int error = 0;
    if(result == -1)
    {
        int* error_location = sceNetErrnoLoc();
        error = error_location ? *error_location : result;
    }
    else if(result < 0)
        error = result;
    *bytes_sent = 0;
    *native_error = result < 0 && !error ? result : error;
    if((uint32_t)result == (uint32_t)SCE_NET_ERROR_EAGAIN ||
       (result == -1 && (error == SCE_NET_EAGAIN ||
                         error == SCE_NET_EWOULDBLOCK)))
        return UVDB_CONSOLE_WRITE_WOULD_BLOCK;
    if(result < 0)
        return UVDB_CONSOLE_WRITE_ERROR;
    *bytes_sent = (size_t)result;
    return UVDB_CONSOLE_WRITE_COMPLETE;
}

/* The exception handler deliberately uses raw networking syscalls, but this
 * service thread runs in ordinary user context. Public epoll gives it an
 * unambiguous zero-timeout readiness result; the raw DONTWAIT receive syscall
 * does not have a documented error-normalization contract on retail Vita.
 * The epoll object is owned entirely by this thread and never survives a
 * connection generation. */
static void uvdb_server_epoll_reset(int* epoll_id, int* watched_socket,
                                    uint32_t* watched_generation)
{
    if(*epoll_id >= 0)
        sceNetEpollDestroy(*epoll_id);
    *epoll_id = -1;
    *watched_socket = -1;
    *watched_generation = 0;
}

static int uvdb_server_epoll_prepare(int* epoll_id, int* watched_socket,
                                     uint32_t* watched_generation,
                                     int socket, uint32_t generation)
{
    if(*epoll_id >= 0 && *watched_socket == socket &&
       *watched_generation == generation)
        return 0;

    uvdb_server_epoll_reset(epoll_id, watched_socket, watched_generation);
    int created = sceNetEpollCreate("uvdb socket events", 0);
    if(created < 0)
        return -1;

    SceNetEpollEvent event;
    memset(&event, 0, sizeof(event));
    event.events = SCE_NET_EPOLLIN | SCE_NET_EPOLLERR |
                   SCE_NET_EPOLLHUP;
    event.data.fd = socket;
    if(sceNetEpollControl(created, SCE_NET_EPOLL_CTL_ADD, socket,
                          &event) < 0)
    {
        sceNetEpollDestroy(created);
        return -1;
    }

    *epoll_id = created;
    *watched_socket = socket;
    *watched_generation = generation;
    return 0;
}

static int uvdb_server_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    uvdb_register_thread("uvdb server");
    int epoll_id = -1;
    int watched_socket = -1;
    uint32_t watched_generation = 0;

    while(!__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
    {
        if(uvdb_socket < 0)
        {
            uvdb_server_epoll_reset(&epoll_id, &watched_socket,
                                    &watched_generation);
            if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
                break;
            uvdb_enter();
            continue;
        }

        if(!uvdb_try_lock())
        {
            sceKernelDelayThread(1000);
            continue;
        }

        if(uvdb_socket < 0 ||
           __atomic_load_n(&uvdb_target_stopped, __ATOMIC_SEQ_CST))
        {
            uvdb_unlock();
            sceKernelDelayThread(1000);
            continue;
        }

        int active_socket = -1;
        uint32_t active_generation = 0;
        uvdb_active_socket_snapshot(&active_socket, &active_generation);
        int request_stop = 0;
        int connection_failed = 0;
        unsigned char command = 0;
        SceNetEpollEvent ready_event;
        memset(&ready_event, 0, sizeof(ready_event));
        int ready = -1;
        if(active_socket >= 0 &&
           uvdb_server_epoll_prepare(&epoll_id, &watched_socket,
                                     &watched_generation, active_socket,
                                     active_generation) == 0)
            ready = sceNetEpollWait(epoll_id, &ready_event, 1, 0);

        if(ready == 0)
        {
            int pump_result = uvdb_console_transport_pump(
                &uvdb_console_transport,
                uvdb_console_server_socket_write,
                &active_socket);
            if(pump_result == UVDB_CONSOLE_PUMP_FATAL)
            {
                /* A partial RSP frame cannot be retried. Wake the stopped-side
                 * receiver so common all-stop cleanup can retire this client. */
                uvdb_console_transport_end_connection(
                    &uvdb_console_transport);
                uvdb_shutdown_socket_if_current(&uvdb_socket, active_socket);
                connection_failed = 1;
            }
        }
        else if(ready < 0)
            connection_failed = 1;
        else if(ready_event.events & SCE_NET_EPOLLIN)
        {
            uint32_t peek_args[6] = {
                (uint32_t)active_socket,
                (uint32_t)&command,
                1,
                MSG_PEEK,
                0,
                0,
            };
            int received = sceNetSyscallRecvfrom((void*)peek_args);
            if(received <= 0)
                connection_failed = 1;
            else if(command == 3)
            {
                uint32_t recv_args[6] = {
                    (uint32_t)active_socket,
                    (uint32_t)&command,
                    1,
                    0,
                    0,
                    0,
                };
                int consumed = sceNetSyscallRecvfrom((void*)recv_args);
                if(consumed == 1)
                {
                    __atomic_store_n(&uvdb_async_stop_cancelled, 0,
                                      __ATOMIC_SEQ_CST);
                    __atomic_store_n(&uvdb_async_stop_pending, 1,
                                      __ATOMIC_SEQ_CST);
                    request_stop = 1;
                }
                else
                    connection_failed = 1;
            }
            else if(uvdb_console_transport_no_ack(
                        &uvdb_console_transport) &&
                    (command == '+' || command == '-'))
            {
                /* Ignore a delayed acknowledgement from the mode transition
                 * (or a legacy client) so one stray byte cannot starve
                 * console output. Readiness makes this blocking single-byte
                 * consume safe. */
                uint32_t recv_args[6] = {
                    (uint32_t)active_socket,
                    (uint32_t)&command,
                    1,
                    0,
                    0,
                    0,
                };
                if(sceNetSyscallRecvfrom((void*)recv_args) != 1)
                    connection_failed = 1;
            }
        }
        else if(ready_event.events & (SCE_NET_EPOLLERR |
                                      SCE_NET_EPOLLHUP))
            connection_failed = 1;
        /* A normal RSP packet wins over console output and remains queued for
         * the stopped handler that is taking ownership of the byte stream. */
        uvdb_unlock();

        if(request_stop)
            uvdb_enter();
        else if(connection_failed)
        {
            uvdb_server_epoll_reset(&epoll_id, &watched_socket,
                                    &watched_generation);
            if(uvdb_socket < 0 &&
               __atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
                break;
            /* Enter the normal exception/all-stop path before restoring code.
             * The failed peer makes recv_packet fail there, and the common
             * cleanup removes breakpoints before releasing the stop. */
            uvdb_enter();
            if(uvdb_socket >= 0)
            {
                /* If trapping was unavailable, fail closed: sever the stale
                 * session but do not patch executable memory while peers run. */
                uvdb_lock();
                uvdb_close_socket(&uvdb_socket);
                uvdb_state = UVDB_STATE_ERROR;
                uvdb_thread_selection_reset(&uvdb_selection);
                uvdb_thread_inventory_reset(&uvdb_inventory);
                uvdb_exception_thread = -1;
                uvdb_unlock();
            }
        }
        sceKernelDelayThread(1000);
    }

    uvdb_server_epoll_reset(&epoll_id, &watched_socket,
                            &watched_generation);
    uvdb_unregister_thread();
    return 0;
}

#ifdef UVDB_KERNEL_THREAD_CONTROL
static int uvdb_lease_main(SceSize args, void* argp)
{
    (void)args;
    (void)argp;
    uvdb_register_thread("uvdb lease keeper");
    while(!__atomic_load_n(&uvdb_lease_stop, __ATOMIC_SEQ_CST))
    {
        unsigned int token = __atomic_load_n(&uvdb_stop_token,
                                              __ATOMIC_SEQ_CST);
        int expected_owner = UVDB_STOP_OWNER_NONE;
        if(token &&
           !__atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST) &&
           __atomic_compare_exchange_n(&uvdb_stop_owner, &expected_owner,
                                        UVDB_STOP_OWNER_LEASE, 0,
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
        {
            int renew_result = vdKernelRenewStop(token, 2000);
            if(renew_result < 0 &&
               __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST) == token)
            {
                /* Retain the token for the controller's bounded re-reconcile
                 * and cleanup path. Ownership keeps EndStop from completing
                 * between renewal and failure publication. */
                __atomic_store_n(&uvdb_stop_failed, 1, __ATOMIC_SEQ_CST);
                int socket = uvdb_socket;
                uvdb_shutdown_socket_if_current(&uvdb_socket, socket);
            }
            __atomic_store_n(&uvdb_stop_owner, UVDB_STOP_OWNER_NONE,
                             __ATOMIC_SEQ_CST);
        }
        sceKernelDelayThread(500000);
    }
    uvdb_unregister_thread();
    return 0;
}
#endif

static int uvdb_start_server_locked(void)
{
    if(__atomic_load_n(&uvdb_shutdown_pending, __ATOMIC_SEQ_CST))
        return -1;
    if(uvdb_server_thread >= 0)
        return uvdb_server_thread_ended ||
               __atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST)
                   ? -1 : 0;
#ifdef UVDB_KERNEL_THREAD_CONTROL
    if(uvdb_lease_thread >= 0)
        return -1;
    /* Validate the actual loaded companion rather than relying only on import
     * compatibility. No helper thread is created for a mismatched ABI. */
    if(!uvdb_kernel_status_is_compatible())
    {
        uvdb_state = UVDB_STATE_ERROR;
        return -1;
    }
#endif

    __atomic_store_n(&uvdb_server_stop, 0, __ATOMIC_SEQ_CST);
    #ifdef UVDB_KERNEL_THREAD_CONTROL
    __atomic_store_n(&uvdb_lease_stop, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&uvdb_stop_failed, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&uvdb_stop_owner, UVDB_STOP_OWNER_NONE,
                     __ATOMIC_SEQ_CST);
    SceUID lease_thread = sceKernelCreateThread(
        "uvdb lease keeper",
        uvdb_lease_main,
        0x10000100,
        16 * 1024,
        0,
        0,
        NULL);
    if(lease_thread < 0)
        return -1;
    uvdb_lease_thread = lease_thread;
    uvdb_lease_thread_ended = 0;
    if(sceKernelStartThread(lease_thread, 0, NULL) < 0)
    {
        if(sceKernelDeleteThread(lease_thread) >= 0)
            uvdb_lease_thread = -1;
        else
            uvdb_lease_thread_ended = 1;
        return -1;
    }
    #endif
    SceUID thread = sceKernelCreateThread(
        "uvdb server",
        uvdb_server_main,
        0x10000100,
        64 * 1024,
        0,
        0,
        NULL);
    if(thread < 0)
    {
        #ifdef UVDB_KERNEL_THREAD_CONTROL
        __atomic_store_n(&uvdb_lease_stop, 1, __ATOMIC_SEQ_CST);
        uvdb_wait_delete_thread(&uvdb_lease_thread,
                                &uvdb_lease_thread_ended);
        #endif
        return -1;
    }

    uvdb_server_thread = thread;
    uvdb_server_thread_ended = 0;
    if(sceKernelStartThread(thread, 0, NULL) < 0)
    {
        if(sceKernelDeleteThread(thread) >= 0)
            uvdb_server_thread = -1;
        else
            uvdb_server_thread_ended = 1;
        #ifdef UVDB_KERNEL_THREAD_CONTROL
        __atomic_store_n(&uvdb_lease_stop, 1, __ATOMIC_SEQ_CST);
        uvdb_wait_delete_thread(&uvdb_lease_thread,
                                &uvdb_lease_thread_ended);
        #endif
        return -1;
    }
    return 0;
}

int uvdb_start_server(void)
{
    uvdb_lifecycle_lock();
    int result = uvdb_start_server_locked();
    uvdb_lifecycle_unlock();
    return result;
}

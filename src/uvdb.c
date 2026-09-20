#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#ifdef UVDB_HOST_INTEGRATION_TEST
#include <signal.h>
#include "uvdb_host_platform.h"
#else
#include <sys/socket.h>
#include <sys/signal.h> //for signal constants; these seem to match gdb's
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <psp2/net/net.h>
#include <psp2/net/net_syscalls.h>
#include <psp2common/net.h>
#ifdef UVDB_MONITOR_DISPLAY
#include <psp2/display.h>
#endif
#include <psp2/kernel/threadmgr/msgpipe.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/modulemgr.h>
#include <kubridge.h>
#endif
#include "uvdb.h"
#include "uvdb_breakpoint_patch.h"
#include "uvdb_console_transport.h"
#include "uvdb_exception_guard.h"
#include "uvdb_exception_handlers.h"
#include "uvdb_exclusive_step.h"
#include "uvdb_fileio_flow.h"
#include "uvdb_monitor.h"
#include "uvdb_protocol_gate.h"
#include "uvdb_registers.h"
#include "uvdb_rsp.h"
#include "uvdb_rsp_frame.h"
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
#define UVDB_THREAD_JOIN_TIMEOUT_US 5000000
#define UVDB_GDB_ADMISSION_POLLS 2000
#define UVDB_GDB_ADMISSION_PEEK 1024

#ifdef UVDB_HOST_INTEGRATION_TEST
typedef uintptr_t uvdb_net_syscall_arg;
#else
typedef uint32_t uvdb_net_syscall_arg;
#endif

typedef char uvdb_fileio_sigint_must_match_rsp[
    SIGINT == UVDB_FILEIO_SIGINT ? 1 : -1];
typedef char uvdb_exception_handler_token_must_match_kubridge[
    sizeof(uvdb_exception_handler_token) == sizeof(KuKernelExceptionHandler)
        ? 1 : -1];

//we prefer to use raw syscalls to avoid issues with signal safety
void _sceKernelExitProcessForUser(int);
int _sceKernelSendMsgPipeVector(
    SceUID, const SceKernelAddrPair*, unsigned int,
    uvdb_net_syscall_arg* rest);
int _sceKernelReceiveMsgPipeVector(
    SceUID, const SceKernelAddrPair*, unsigned int,
    uvdb_net_syscall_arg* rest);
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

static volatile uint32_t uvdb_lock_owner;
static int uvdb_lifecycle_lock_state;
static int uvdb_socket_lifecycle_lock_state;

enum uvdb_try_lock_result {
    UVDB_TRY_LOCK_SELF = -1,
    UVDB_TRY_LOCK_BUSY = 0,
    UVDB_TRY_LOCK_ACQUIRED = 1,
};

static uint32_t uvdb_lock_owner_token(SceUID thread)
{
    uint32_t owner = (uint32_t)thread;
    return owner ? owner : UINT32_MAX;
}

static int uvdb_try_lock_for_thread(SceUID thread)
{
    const uint32_t owner = uvdb_lock_owner_token(thread);
    uint32_t observed = __atomic_load_n(
        &uvdb_lock_owner, __ATOMIC_ACQUIRE);
    if(observed == owner)
        return UVDB_TRY_LOCK_SELF;
    if(observed)
        return UVDB_TRY_LOCK_BUSY;
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(
               &uvdb_lock_owner, &expected, owner, 0,
               __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
               ? UVDB_TRY_LOCK_ACQUIRED :
                 (expected == owner ? UVDB_TRY_LOCK_SELF :
                                      UVDB_TRY_LOCK_BUSY);
}

static void uvdb_lock(void)
{
    const SceUID thread = sceKernelGetThreadId();
    for(;;)
        if(uvdb_try_lock_for_thread(thread) == UVDB_TRY_LOCK_ACQUIRED)
            return;
}

static int uvdb_try_lock(void)
{
    return uvdb_try_lock_for_thread(sceKernelGetThreadId()) ==
        UVDB_TRY_LOCK_ACQUIRED;
}

static void uvdb_unlock(void)
{
    uint32_t expected = uvdb_lock_owner_token(sceKernelGetThreadId());
    /* A mismatched release must never clear another thread's ownership. The
     * caller cannot safely repair that invariant here, so retain the lock and
     * let the bounded lifecycle/error path expose the defect. */
    (void)__atomic_compare_exchange_n(
        &uvdb_lock_owner, &expected, 0u, 0,
        __ATOMIC_RELEASE, __ATOMIC_RELAXED);
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
static int uvdb_candidate_socket = -1;
static int uvdb_listen_socket = -1;
static uint32_t uvdb_socket_generation;
static struct uvdb_console_transport uvdb_console_transport;
static SceUID uvdb_pipe = -1;
static size_t uvdb_max_buffer = UVDB_DEFAULT_MAX_BUFFER;
static unsigned short uvdb_port = UVDB_DEFAULT_PORT;
static volatile enum uvdb_state uvdb_state = UVDB_STATE_IDLE;
static volatile int uvdb_io_failed;
#ifdef UVDB_RAW_RECV_DIAGNOSTIC
enum uvdb_raw_recv_phase
{
    UVDB_RAW_RECV_PHASE_NONE = 0,
    UVDB_RAW_RECV_PHASE_REQUEST = 1,
    UVDB_RAW_RECV_PHASE_RESPONSE_ACK = 2,
};

struct uvdb_raw_recv_diagnostic
{
    int captured;
    int32_t result;
    int32_t descriptor;
    uint32_t generation;
    uint32_t network_closing;
    uint32_t phase;
    uint32_t packet_io_active;
    uint32_t target_stopped;
    uint32_t protocol_owned;
};

static struct uvdb_raw_recv_diagnostic uvdb_raw_recv_diagnostic;
static volatile uint32_t uvdb_raw_recv_phase;
#endif
static struct uvdb_exception_handlers uvdb_handlers;
static struct uvdb_exception_guard uvdb_exception_guard;
static struct uvdb_protocol_gate uvdb_protocol_gate;
static struct uvdb_rsp_request_lifetime uvdb_request_lifetime;
static volatile int uvdb_target_stopped;
static volatile int uvdb_async_stop_pending;
static volatile int uvdb_async_stop_cancelled;
static volatile int uvdb_server_stop;
static volatile int uvdb_shutdown_pending;
static volatile int uvdb_accept_active;
static volatile int uvdb_packet_io_active;
static volatile int uvdb_network_closing;
#ifdef UVDB_ADMISSION_DIAGNOSTIC
struct uvdb_admission_diagnostic_state_internal
{
    volatile uint32_t active_writers;
    volatile uint32_t revision;
    volatile uint32_t next_sequence;
    volatile uint32_t event_sequence[
        UVDB_ADMISSION_DIAGNOSTIC_EVENT_COUNT];
    volatile uint32_t event_occurrences[
        UVDB_ADMISSION_DIAGNOSTIC_EVENT_COUNT];
    volatile int32_t event_descriptor[
        UVDB_ADMISSION_DIAGNOSTIC_EVENT_COUNT];
    volatile uint32_t event_generation[
        UVDB_ADMISSION_DIAGNOSTIC_EVENT_COUNT];
    volatile uint32_t event_owner[
        UVDB_ADMISSION_DIAGNOSTIC_EVENT_COUNT];
    volatile uint32_t event_state[
        UVDB_ADMISSION_DIAGNOSTIC_EVENT_COUNT];
    volatile int32_t current_socket;
    volatile int32_t current_candidate;
    volatile int32_t current_listener;
    volatile uint32_t current_generation;
    volatile uint32_t current_owner;
    volatile uint32_t test_title_ready;
    volatile uint32_t first_packet_size;
    volatile uint32_t first_packet_recorded;
    volatile uint32_t connection_epoch;
};

static struct uvdb_admission_diagnostic_state_internal
    uvdb_admission_diagnostic_state;
#endif
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

#define UVDB_MONITOR_OUTPUT_CAPACITY (16u * 1024u)
static char uvdb_monitor_output[UVDB_MONITOR_OUTPUT_CAPACITY];
static struct uvdb_monitor_thread
    uvdb_monitor_threads[UVDB_MONITOR_MAX_THREADS];
static struct uvdb_monitor_module
    uvdb_monitor_modules[UVDB_MONITOR_MAX_MODULES];
#ifdef UVDB_MONITOR_DISPLAY
static struct uvdb_monitor_display uvdb_monitor_display_cache;
static uint32_t uvdb_monitor_display_cache_generation;
static struct uvdb_monitor_display_stop uvdb_monitor_display_stop;
#endif

static int breakpoint_remove_all(void);
static size_t breakpoint_active_count(void);
static int uvdb_kernel_end_stop(void);
static int uvdb_refresh_stopped_inventory(void);
#ifdef UVDB_KERNEL_THREAD_CONTROL
static int breakpoint_any_active(void);
static void uvdb_claim_stop_controller(void);
static void uvdb_release_stop_controller(void);
static int uvdb_kernel_recover_stop(void);
static void uvdb_kernel_abandon_stop(void);
#endif
static int uvdb_server_main(SceSize args, void* argp);
#ifdef UVDB_MONITOR_DISPLAY
#ifdef UVDB_HOST_INTEGRATION_TEST
static void uvdb_monitor_trigger_initial_stop(void);
#else
static __attribute__((naked)) void uvdb_monitor_trigger_initial_stop(void);
#endif
#endif
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

static uint32_t uvdb_protocol_owner_for_thread(SceUID id)
{
    /* Vita thread IDs are nonzero handles. Retain a deterministic nonzero
     * fallback so an unexpected syscall failure cannot bypass serialization. */
    uint32_t owner = (uint32_t)id & UINT32_C(0x7fffffff);
    return owner ? owner : UINT32_C(0x7fffffff);
}

static void uvdb_note_io_failure(void)
{
    __atomic_store_n(&uvdb_io_failed, 1, __ATOMIC_RELEASE);
}

static int uvdb_has_io_failure(void)
{
    return __atomic_load_n(&uvdb_io_failed, __ATOMIC_ACQUIRE) != 0;
}

static void uvdb_clear_io_failure(void)
{
    __atomic_store_n(&uvdb_io_failed, 0, __ATOMIC_RELEASE);
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
    /* In a library-only persistent attach, the private server thread owns the
     * only register context that can be read safely. Expose it as the stopped
     * thread for this synthetic trap even though helpers remain hidden during
     * ordinary application stops. Once an application breakpoint fires, its
     * exception thread becomes the normal first inventory entry. */
    if(uvdb_exception_thread > 0 &&
       uvdb_is_controller_thread(uvdb_exception_thread) &&
       uvdb_thread_inventory_add(&uvdb_inventory,
                                 uvdb_exception_thread) < 0)
        return -1;
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

static void uvdb_active_socket_snapshot(int* descriptor,
                                        uint32_t* generation);
static int uvdb_begin_packet_io(int* descriptor, uint32_t* generation);
static int uvdb_packet_io_cancelled(int descriptor, uint32_t generation);
static int uvdb_raw_io_would_block(ssize_t result);
#ifdef UVDB_ADMISSION_DIAGNOSTIC
static void uvdb_admission_diagnostic_reset(void);
static void uvdb_admission_diagnostic_record(
    enum uvdb_admission_diagnostic_event event,
    int descriptor,
    uint32_t generation,
    uint32_t owner);
static void uvdb_admission_diagnostic_begin_connection(void);
static void uvdb_admission_diagnostic_socket_state(
    int* socket, int descriptor, uint32_t generation);
static void uvdb_admission_diagnostic_promote_socket(
    int descriptor, uint32_t generation);
#endif
#ifdef UVDB_RAW_RECV_DIAGNOSTIC
static void uvdb_capture_raw_recv_diagnostic(
    ssize_t result, int descriptor, uint32_t generation);
#endif

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
        uvdb_note_io_failure();
        return 0;
    }
    int active_socket = -1;
    uint32_t active_generation = 0;
    if(uvdb_begin_packet_io(
           &active_socket, &active_generation) < 0)
    {
        uvdb_note_io_failure();
        return 0;
    }
    uvdb_net_syscall_arg args[6] = {
        (uvdb_net_syscall_arg)active_socket,
        (uvdb_net_syscall_arg)*pos,
        (uvdb_net_syscall_arg)chk_size,
        MSG_DONTWAIT, 0, 0,
    };
    /* A retail RST need not wake a raw blocking recvfrom. Polling the same
     * generation bounds peer-reset and shutdown observation while preserving
     * an indefinitely idle debugger session. The surrounding whole-protocol
     * gate excludes every other packet path from mutating `buf`. */
    uvdb_unlock();
    ssize_t ans;
    for(;;)
    {
        ans = sceNetSyscallRecvfrom((void*)args);
#ifdef UVDB_RAW_RECV_DIAGNOSTIC
        if(ans <= 0)
            uvdb_capture_raw_recv_diagnostic(
                ans, active_socket, active_generation);
#endif
        if(!uvdb_raw_io_would_block(ans))
            break;
        if(uvdb_packet_io_cancelled(active_socket, active_generation))
        {
            ans = -1;
            break;
        }
        sceKernelDelayThread(1000);
    }
    uvdb_lock();
    __atomic_store_n(&uvdb_packet_io_active, 0, __ATOMIC_RELEASE);
    if(uvdb_packet_io_cancelled(active_socket, active_generation))
        ans = -1;
    if(ans <= 0)
    {
        uvdb_note_io_failure();
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
            uvdb_note_io_failure();
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
        int active_socket = -1;
        uint32_t active_generation = 0;
        if(uvdb_begin_packet_io(
               &active_socket, &active_generation) < 0)
        {
            uvdb_note_io_failure();
            break;
        }
        uvdb_net_syscall_arg args[6] = {
            (uvdb_net_syscall_arg)active_socket,
            (uvdb_net_syscall_arg)(buf->buf + pos),
            (uvdb_net_syscall_arg)(buf->size - pos),
            MSG_DONTWAIT,
            0,
            0,
        };
        uvdb_unlock();
        ssize_t chk;
        for(;;)
        {
            chk = sceNetSyscallSendto((void*)args);
            if(!uvdb_raw_io_would_block(chk))
                break;
            if(uvdb_packet_io_cancelled(
                   active_socket, active_generation))
            {
                chk = -1;
                break;
            }
            sceKernelDelayThread(1000);
        }
        uvdb_lock();
        __atomic_store_n(&uvdb_packet_io_active, 0, __ATOMIC_RELEASE);
        if(uvdb_packet_io_cancelled(active_socket, active_generation))
            chk = -1;
        if(chk <= 0)
        {
            uvdb_note_io_failure();
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
#ifdef UVDB_ADMISSION_DIAGNOSTIC
        uvdb_admission_diagnostic_socket_state(socket, -1, 0);
#endif
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
#ifdef UVDB_ADMISSION_DIAGNOSTIC
        uvdb_admission_diagnostic_socket_state(
            socket, descriptor, uvdb_socket_generation);
#endif
        result = 0;
    }
    uvdb_socket_lifecycle_unlock();
    return result;
}

static int uvdb_promote_candidate_socket(int descriptor)
{
    int result = -1;
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    uint32_t generation = 0;
    uvdb_admission_diagnostic_record(
        UVDB_ADMISSION_EVENT_PROMOTION_BEGIN, descriptor, 0, 0);
#endif
    uvdb_socket_lifecycle_lock();
    if(uvdb_candidate_socket == descriptor && uvdb_socket < 0)
    {
        uvdb_candidate_socket = -1;
        uvdb_socket = descriptor;
        uvdb_socket_generation++;
        if(!uvdb_socket_generation)
            uvdb_socket_generation++;
#ifdef UVDB_ADMISSION_DIAGNOSTIC
        generation = uvdb_socket_generation;
        uvdb_admission_diagnostic_promote_socket(
            descriptor, generation);
#endif
        result = 0;
    }
    uvdb_socket_lifecycle_unlock();
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    if(result == 0)
        uvdb_admission_diagnostic_record(
            UVDB_ADMISSION_EVENT_SOCKET_PUBLISHED, descriptor,
            generation, 0);
#endif
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

/* Publish the exact network-pointer lifetime inside the surrounding
 * whole-protocol ownership. network_closing prevents a new waiter from
 * appearing after shutdown sampled zero. Caller holds uvdb_lock; this function
 * deliberately uses only the short descriptor lock. */
static int uvdb_begin_packet_io(int* descriptor, uint32_t* generation)
{
    if(!descriptor || !generation)
        return -1;
    int result = -1;
    uvdb_socket_lifecycle_lock();
    if(!__atomic_load_n(&uvdb_network_closing, __ATOMIC_ACQUIRE) &&
       uvdb_socket >= 0 &&
       !__atomic_exchange_n(&uvdb_packet_io_active, 1,
                            __ATOMIC_ACQ_REL))
    {
        *descriptor = uvdb_socket;
        *generation = uvdb_socket_generation;
        result = 0;
    }
    uvdb_socket_lifecycle_unlock();
    return result;
}

static int uvdb_packet_io_cancelled(int descriptor, uint32_t generation)
{
    int active_socket = -1;
    uint32_t active_generation = 0;
    uvdb_active_socket_snapshot(&active_socket, &active_generation);
    return __atomic_load_n(&uvdb_network_closing, __ATOMIC_ACQUIRE) ||
           active_socket != descriptor ||
           active_generation != generation;
}

static int uvdb_raw_io_would_block(ssize_t result)
{
    return (uint32_t)result == (uint32_t)SCE_NET_ERROR_EAGAIN ||
           result == -(ssize_t)SCE_NET_EAGAIN ||
           result == -(ssize_t)SCE_NET_EWOULDBLOCK;
}

#ifdef UVDB_ADMISSION_DIAGNOSTIC
static uint32_t uvdb_admission_diagnostic_state_bits(void)
{
    uint32_t state = 0;
    if(__atomic_load_n(&uvdb_target_stopped, __ATOMIC_ACQUIRE))
        state |= UVDB_ADMISSION_STATE_TARGET_STOPPED;
    if(__atomic_load_n(&uvdb_network_closing, __ATOMIC_ACQUIRE))
        state |= UVDB_ADMISSION_STATE_NETWORK_CLOSING;
    if(__atomic_load_n(
           &uvdb_admission_diagnostic_state.test_title_ready,
           __ATOMIC_ACQUIRE))
        state |= UVDB_ADMISSION_STATE_TEST_TITLE_READY;
    if(!uvdb_protocol_gate_is_idle(&uvdb_protocol_gate))
        state |= UVDB_ADMISSION_STATE_PROTOCOL_OWNED;
    return state;
}

static uint32_t uvdb_admission_diagnostic_owner(void)
{
    return __atomic_load_n(
               &uvdb_protocol_gate.state, __ATOMIC_ACQUIRE) &
           UINT32_C(0x7fffffff);
}

static void uvdb_admission_diagnostic_write_begin(void)
{
    __atomic_add_fetch(
        &uvdb_admission_diagnostic_state.active_writers, 1u,
        __ATOMIC_ACQ_REL);
}

static void uvdb_admission_diagnostic_write_end(void)
{
    __atomic_add_fetch(
        &uvdb_admission_diagnostic_state.revision, 1u,
        __ATOMIC_RELEASE);
    __atomic_sub_fetch(
        &uvdb_admission_diagnostic_state.active_writers, 1u,
        __ATOMIC_RELEASE);
}

static void uvdb_admission_diagnostic_reset(void)
{
    uint32_t test_title_ready = __atomic_load_n(
        &uvdb_admission_diagnostic_state.test_title_ready,
        __ATOMIC_ACQUIRE);
    uvdb_admission_diagnostic_write_begin();
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.next_sequence, 0u,
        __ATOMIC_RELAXED);
    for(unsigned int event = 0;
        event < UVDB_ADMISSION_DIAGNOSTIC_EVENT_COUNT; ++event)
    {
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_sequence[event],
            0u, __ATOMIC_RELAXED);
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_occurrences[event],
            0u, __ATOMIC_RELAXED);
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_descriptor[event],
            0, __ATOMIC_RELAXED);
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_generation[event],
            0u, __ATOMIC_RELAXED);
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_owner[event],
            0u, __ATOMIC_RELAXED);
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_state[event],
            0u, __ATOMIC_RELAXED);
    }
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.current_socket, -1,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.current_candidate, -1,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.current_listener, -1,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.current_generation, 0u,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.first_packet_size, 0u,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.first_packet_recorded, 0u,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.connection_epoch, 0u,
        __ATOMIC_RELAXED);
    uvdb_admission_diagnostic_write_end();
    if(test_title_ready)
        uvdb_admission_diagnostic_record(
            UVDB_ADMISSION_EVENT_TEST_TITLE_READY, -1, 0, 0);
}

static void uvdb_admission_diagnostic_begin_connection(void)
{
    uvdb_admission_diagnostic_write_begin();
    for(unsigned int event = UVDB_ADMISSION_EVENT_CANDIDATE_ACCEPTED;
        event <= UVDB_ADMISSION_EVENT_TARGET_RUNNING; ++event)
    {
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_sequence[event],
            0u, __ATOMIC_RELAXED);
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_occurrences[event],
            0u, __ATOMIC_RELAXED);
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_descriptor[event],
            0, __ATOMIC_RELAXED);
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_generation[event],
            0u, __ATOMIC_RELAXED);
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_owner[event],
            0u, __ATOMIC_RELAXED);
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.event_state[event],
            0u, __ATOMIC_RELAXED);
    }
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.first_packet_size, 0u,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.first_packet_recorded, 0u,
        __ATOMIC_RELAXED);
    __atomic_add_fetch(
        &uvdb_admission_diagnostic_state.connection_epoch, 1u,
        __ATOMIC_RELAXED);
    uvdb_admission_diagnostic_write_end();
}

static void uvdb_admission_diagnostic_record(
    enum uvdb_admission_diagnostic_event event,
    int descriptor,
    uint32_t generation,
    uint32_t owner)
{
    if((unsigned int)event >= UVDB_ADMISSION_DIAGNOSTIC_EVENT_COUNT)
        return;
    uvdb_admission_diagnostic_write_begin();
    uint32_t occurrence = __atomic_fetch_add(
        &uvdb_admission_diagnostic_state.event_occurrences[event], 1u,
        __ATOMIC_RELAXED);
    if(occurrence != 0)
    {
        uvdb_admission_diagnostic_write_end();
        return;
    }

    uint32_t sequence = __atomic_add_fetch(
        &uvdb_admission_diagnostic_state.next_sequence, 1u,
        __ATOMIC_ACQ_REL);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.event_descriptor[event],
        descriptor, __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.event_generation[event],
        generation, __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.event_owner[event],
        owner ? owner : uvdb_admission_diagnostic_owner(),
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.event_state[event],
        uvdb_admission_diagnostic_state_bits(), __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.event_sequence[event],
        sequence, __ATOMIC_RELEASE);
    uvdb_admission_diagnostic_write_end();
}

static void uvdb_admission_diagnostic_socket_state(
    int* socket, int descriptor, uint32_t generation)
{
    uvdb_admission_diagnostic_write_begin();
    volatile int32_t* destination = NULL;
    if(socket == &uvdb_socket)
        destination = &uvdb_admission_diagnostic_state.current_socket;
    else if(socket == &uvdb_candidate_socket)
        destination = &uvdb_admission_diagnostic_state.current_candidate;
    else if(socket == &uvdb_listen_socket)
        destination = &uvdb_admission_diagnostic_state.current_listener;
    if(destination)
        __atomic_store_n(destination, descriptor, __ATOMIC_RELEASE);
    if(socket == &uvdb_socket && descriptor >= 0)
        __atomic_store_n(
            &uvdb_admission_diagnostic_state.current_generation,
            generation, __ATOMIC_RELEASE);
    uvdb_admission_diagnostic_write_end();
}

static void uvdb_admission_diagnostic_promote_socket(
    int descriptor, uint32_t generation)
{
    uvdb_admission_diagnostic_write_begin();
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.current_candidate, -1,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.current_socket, descriptor,
        __ATOMIC_RELAXED);
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.current_generation, generation,
        __ATOMIC_RELAXED);
    uvdb_admission_diagnostic_write_end();
}

void uvdb_admission_diagnostic_mark_test_ready(void)
{
    uvdb_admission_diagnostic_write_begin();
    __atomic_store_n(
        &uvdb_admission_diagnostic_state.test_title_ready, 1u,
        __ATOMIC_RELEASE);
    uvdb_admission_diagnostic_write_end();
    uvdb_admission_diagnostic_record(
        UVDB_ADMISSION_EVENT_TEST_TITLE_READY, -1, 0, 0);
}

int uvdb_admission_diagnostic_get(
    struct uvdb_admission_diagnostic* diagnostic)
{
    if(!diagnostic)
        return -1;
    for(unsigned int attempt = 0; attempt < 3u; ++attempt)
    {
        if(__atomic_load_n(
               &uvdb_admission_diagnostic_state.active_writers,
               __ATOMIC_ACQUIRE))
            continue;
        uint32_t before = __atomic_load_n(
            &uvdb_admission_diagnostic_state.revision,
            __ATOMIC_ACQUIRE);
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->version = UVDB_ADMISSION_DIAGNOSTIC_VERSION;
        diagnostic->size = sizeof(*diagnostic);
        diagnostic->event_count =
            UVDB_ADMISSION_DIAGNOSTIC_EVENT_COUNT;
        for(unsigned int event = 0;
            event < UVDB_ADMISSION_DIAGNOSTIC_EVENT_COUNT; ++event)
        {
            diagnostic->event_sequence[event] = __atomic_load_n(
                &uvdb_admission_diagnostic_state
                     .event_sequence[event],
                __ATOMIC_ACQUIRE);
            diagnostic->event_occurrences[event] = __atomic_load_n(
                &uvdb_admission_diagnostic_state
                     .event_occurrences[event],
                __ATOMIC_ACQUIRE);
            diagnostic->event_descriptor[event] = __atomic_load_n(
                &uvdb_admission_diagnostic_state
                     .event_descriptor[event],
                __ATOMIC_RELAXED);
            diagnostic->event_generation[event] = __atomic_load_n(
                &uvdb_admission_diagnostic_state
                     .event_generation[event],
                __ATOMIC_RELAXED);
            diagnostic->event_owner[event] = __atomic_load_n(
                &uvdb_admission_diagnostic_state.event_owner[event],
                __ATOMIC_RELAXED);
            diagnostic->event_state[event] = __atomic_load_n(
                &uvdb_admission_diagnostic_state.event_state[event],
                __ATOMIC_RELAXED);
        }
        diagnostic->current_socket = __atomic_load_n(
            &uvdb_admission_diagnostic_state.current_socket,
            __ATOMIC_ACQUIRE);
        diagnostic->current_candidate = __atomic_load_n(
            &uvdb_admission_diagnostic_state.current_candidate,
            __ATOMIC_ACQUIRE);
        diagnostic->current_listener = __atomic_load_n(
            &uvdb_admission_diagnostic_state.current_listener,
            __ATOMIC_ACQUIRE);
        diagnostic->current_generation = __atomic_load_n(
            &uvdb_admission_diagnostic_state.current_generation,
            __ATOMIC_ACQUIRE);
        diagnostic->current_owner =
            uvdb_admission_diagnostic_owner();
        diagnostic->current_state =
            uvdb_admission_diagnostic_state_bits();
        diagnostic->first_packet_size = __atomic_load_n(
            &uvdb_admission_diagnostic_state.first_packet_size,
            __ATOMIC_ACQUIRE);
        diagnostic->connection_epoch = __atomic_load_n(
            &uvdb_admission_diagnostic_state.connection_epoch,
            __ATOMIC_ACQUIRE);
        uint32_t after = __atomic_load_n(
            &uvdb_admission_diagnostic_state.revision,
            __ATOMIC_ACQUIRE);
        if(before == after &&
           !__atomic_load_n(
               &uvdb_admission_diagnostic_state.active_writers,
               __ATOMIC_ACQUIRE))
        {
            diagnostic->revision = after;
            return 0;
        }
    }
    return -1;
}
#endif

#ifdef UVDB_RAW_RECV_DIAGNOSTIC
static void uvdb_capture_raw_recv_diagnostic(
    ssize_t result, int descriptor, uint32_t generation)
{
    int expected = 0;
    if(!__atomic_compare_exchange_n(
           &uvdb_raw_recv_diagnostic.captured, &expected, -1, 0,
           __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    uvdb_raw_recv_diagnostic.result = (int32_t)result;
    uvdb_raw_recv_diagnostic.descriptor = descriptor;
    uvdb_raw_recv_diagnostic.generation = generation;
    uvdb_raw_recv_diagnostic.network_closing =
        (uint32_t)__atomic_load_n(
            &uvdb_network_closing, __ATOMIC_ACQUIRE);
    uvdb_raw_recv_diagnostic.phase =
        __atomic_load_n(&uvdb_raw_recv_phase, __ATOMIC_ACQUIRE);
    uvdb_raw_recv_diagnostic.packet_io_active =
        (uint32_t)__atomic_load_n(
            &uvdb_packet_io_active, __ATOMIC_ACQUIRE);
    uvdb_raw_recv_diagnostic.target_stopped =
        (uint32_t)__atomic_load_n(
            &uvdb_target_stopped, __ATOMIC_ACQUIRE);
    uvdb_raw_recv_diagnostic.protocol_owned =
        (uint32_t)!uvdb_protocol_gate_is_idle(&uvdb_protocol_gate);
    /* Raw exception-side syscalls deliberately avoid the public wrapper's
     * thread-local errno path. Calling sceNetErrnoLoc() here would make the
     * diagnostic change the safety boundary it is measuring. */
    __atomic_store_n(
        &uvdb_raw_recv_diagnostic.captured, 1, __ATOMIC_RELEASE);
}

static void uvdb_write_raw_recv_hex32(uint32_t value)
{
    char digits[8];
    for(size_t index = 0; index < sizeof(digits); ++index)
        digits[index] = int2hex(
            (value >> (28u - (uint32_t)index * 4u)) & 15u);
    buffer_write(&out_buf, digits, sizeof(digits));
}

static void uvdb_write_raw_recv_diagnostic(void)
{
    if(__atomic_load_n(
           &uvdb_raw_recv_diagnostic.captured, __ATOMIC_ACQUIRE) != 1)
    {
        buffer_write(&out_buf, "captured=0", sizeof("captured=0") - 1u);
        return;
    }
#define FIELD(name, value) \
    do \
    { \
        buffer_write(&out_buf, name "=", sizeof(name "=") - 1u); \
        uvdb_write_raw_recv_hex32((uint32_t)(value)); \
    } while(0)
    FIELD("captured", 1);
    buffer_write(&out_buf, ";", 1u);
    FIELD("result", uvdb_raw_recv_diagnostic.result);
    buffer_write(&out_buf, ";", 1u);
    FIELD("descriptor", uvdb_raw_recv_diagnostic.descriptor);
    buffer_write(&out_buf, ";", 1u);
    FIELD("generation", uvdb_raw_recv_diagnostic.generation);
    buffer_write(&out_buf, ";", 1u);
    FIELD("closing", uvdb_raw_recv_diagnostic.network_closing);
    buffer_write(&out_buf, ";", 1u);
    FIELD("phase", uvdb_raw_recv_diagnostic.phase);
    buffer_write(&out_buf, ";", 1u);
    FIELD("packet_io", uvdb_raw_recv_diagnostic.packet_io_active);
    buffer_write(&out_buf, ";", 1u);
    FIELD("target_stopped", uvdb_raw_recv_diagnostic.target_stopped);
    buffer_write(&out_buf, ";", 1u);
    FIELD("protocol_owned", uvdb_raw_recv_diagnostic.protocol_owned);
    buffer_write(
        &out_buf, ";errno_available=00000000",
        sizeof(";errno_available=00000000") - 1u);
#undef FIELD
}
#endif

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

/*
 * A TCP connect is not a debugger session. Port scanners and health checks
 * commonly connect and either send no bytes or send another protocol. Keep
 * the target running and the listening socket reusable until the peer has
 * supplied one complete checksum-valid RSP frame. MSG_PEEK leaves that frame
 * queued for the normal stopped-side receiver.
 */
static int uvdb_wait_for_gdb_admission(int socket)
{
    int epoll = sceNetEpollCreate("uvdb gdb admission", 0);
    if(epoll < 0)
        return -1;

    SceNetEpollEvent watch;
    memset(&watch, 0, sizeof(watch));
    watch.events = SCE_NET_EPOLLIN | SCE_NET_EPOLLERR |
                   SCE_NET_EPOLLHUP;
    watch.data.fd = socket;
    if(sceNetEpollControl(epoll, SCE_NET_EPOLL_CTL_ADD, socket,
                          &watch) < 0)
    {
        sceNetEpollDestroy(epoll);
        return -1;
    }

    int result = 0;
    for(unsigned int poll = 0; poll < UVDB_GDB_ADMISSION_POLLS; ++poll)
    {
        if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_ACQUIRE))
        {
            result = -1;
            break;
        }

        SceNetEpollEvent ready;
        memset(&ready, 0, sizeof(ready));
        int count = sceNetEpollWait(epoll, &ready, 1, 0);
        if(count < 0)
        {
            result = -1;
            break;
        }
        if(!count)
        {
            sceKernelDelayThread(1000);
            continue;
        }
        if(ready.events & (SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP))
            break;
        if(!(ready.events & SCE_NET_EPOLLIN))
        {
            sceKernelDelayThread(1000);
            continue;
        }

        unsigned char input[UVDB_GDB_ADMISSION_PEEK];
        uvdb_net_syscall_arg arguments[6] = {
            (uvdb_net_syscall_arg)socket,
            (uvdb_net_syscall_arg)input,
            sizeof(input),
            MSG_PEEK,
            0,
            0,
        };
        int received = sceNetSyscallRecvfrom((void*)arguments);
        if(received <= 0)
            break;

        size_t offset = 0;
        while(offset < (size_t)received &&
              (input[offset] == '+' || input[offset] == '-'))
            ++offset;
        if(offset == (size_t)received)
        {
            sceKernelDelayThread(1000);
            continue;
        }

        struct uvdb_rsp_frame frame;
        int frame_result = uvdb_rsp_scan_frame(
            input + offset, (size_t)received - offset,
            sizeof(input) - 3u, &frame);
        if(frame_result == UVDB_RSP_FRAME_COMPLETE &&
           frame.payload_size != 0)
        {
#ifdef UVDB_ADMISSION_DIAGNOSTIC
            uvdb_admission_diagnostic_record(
                UVDB_ADMISSION_EVENT_VALID_FRAME, socket, 0, 0);
#endif
            result = 1;
            break;
        }
        if(frame_result == UVDB_RSP_FRAME_DISCARD ||
           received == (int)sizeof(input))
            break;
        sceKernelDelayThread(1000);
    }

    sceNetEpollDestroy(epoll);
    return result;
}

/* A socket abort/shutdown is expected to wake Vita network syscalls promptly,
 * but teardown must never wait forever if a firmware or wrapper violates that
 * contract. Keep every referenced buffer/socket alive when this deadline is
 * exceeded so a later shutdown retry can finish safely. */
static int uvdb_wait_for_network_quiescence(void)
{
    enum { UVDB_NETWORK_QUIESCE_POLLS = 5000 };
    for(unsigned int poll = 0; poll < UVDB_NETWORK_QUIESCE_POLLS; ++poll)
    {
        if(!__atomic_load_n(&uvdb_accept_active, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&uvdb_packet_io_active, __ATOMIC_ACQUIRE) &&
           uvdb_protocol_gate_is_idle(&uvdb_protocol_gate))
            return 0;
        sceKernelDelayThread(1000);
    }
    return -1;
}

static int uvdb_wait_for_exception_quiescence(void)
{
    enum { UVDB_EXCEPTION_QUIESCE_POLLS = 5000 };
    for(unsigned int poll = 0; poll < UVDB_EXCEPTION_QUIESCE_POLLS; ++poll)
    {
        if(uvdb_exception_guard_is_idle(&uvdb_exception_guard))
            return 0;
        sceKernelDelayThread(1000);
    }
    return -1;
}

static int uvdb_prepare_protocol_restart(void)
{
    /* The protocol owner is released immediately before the exception guard,
     * so protocol idleness alone is not a complete callback-generation fence. */
    if(uvdb_wait_for_exception_quiescence() < 0)
        return -1;
    return uvdb_protocol_gate_reopen(&uvdb_protocol_gate);
}

static void exception_handler(KuKernelExceptionContext* ctx);

static int uvdb_handler_backend_replace(
    void* context,
    uint32_t exception_type,
    uvdb_exception_handler_token replacement,
    uvdb_exception_handler_token* previous)
{
    (void)context;
    struct KuKernelExceptionHandlerOpt opt = {
        .size = sizeof(opt),
    };
    /* KuBridge copies the displaced handler to user memory while holding its
     * process exception-handler spin lock, then publishes the replacement
     * before releasing that lock. Pass the registry's permanent slot directly
     * so a dispatcher cannot observe our callback before its predecessor. */
    return kuKernelRegisterExceptionHandler(
        exception_type, (KuKernelExceptionHandler)replacement,
        previous ? (KuKernelExceptionHandler*)previous : NULL, &opt);
}

static int uvdb_handler_backend_release(
    void* context,
    uint32_t exception_type)
{
    (void)context;
    kuKernelReleaseExceptionHandler(exception_type);
    return 0;
}

static const struct uvdb_exception_handler_backend uvdb_handler_backend = {
    .replace = uvdb_handler_backend_replace,
    .release = uvdb_handler_backend_release,
};

static int uvdb_release_handlers(void)
{
    /* KuBridge lacks compare-and-restore. This operation relies on the
     * documented exclusive-slot ownership contract, but retains every failed
     * restoration bit so shutdown cannot silently complete on uncertainty. */
    return uvdb_exception_handlers_restore(
        &uvdb_handlers, &uvdb_handler_backend, NULL);
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
        unsigned int timeout = UVDB_THREAD_JOIN_TIMEOUT_US;
        if(sceKernelWaitThreadEnd(*thread, &status, &timeout) < 0)
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
       uvdb_candidate_socket >= 0 || uvdb_listen_socket >= 0)
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
    SceUID caller = sceKernelGetThreadId();
    if(thread == caller
       #ifdef UVDB_KERNEL_THREAD_CONTROL
       || uvdb_lease_thread == caller
       #endif
       )
        return -1;

    __atomic_store_n(&uvdb_server_stop, 1, __ATOMIC_SEQ_CST);
    uvdb_protocol_gate_close(&uvdb_protocol_gate);
    __atomic_store_n(&uvdb_network_closing, 1, __ATOMIC_RELEASE);
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    uvdb_admission_diagnostic_record(
        UVDB_ADMISSION_EVENT_NETWORK_CLOSING, -1, 0, 0);
#endif
    /* Stop accepting producer bytes immediately. The serialized socket owner
     * completes generation cleanup after its service thread has exited. */
    uvdb_console_session_close_active_gate();
    /* Shutdown wakes recv without changing the descriptor to -1. A running
     * server can then enter its exception/all-stop cleanup path instead of
     * mistaking shutdown for a request to open a fresh listening socket. */
    uvdb_shutdown_socket(&uvdb_socket);
    uvdb_shutdown_socket(&uvdb_candidate_socket);
    // Do not take uvdb_lock here: the blocked service or exception path may be
    // holding it while waiting for network input.
    // Abort wakes a blocking accept without closing its descriptor. The owner
    // observes server_stop and performs the serialized close itself.
    uvdb_abort_socket(&uvdb_listen_socket);
    /* accept/recv/send publish a lifetime flag and run without uvdb_lock.
     * Wait outside that lock before any buffer or handler teardown. Socket
     * shutdown/abort above is the cancellation mechanism for each wait. */
    if(uvdb_wait_for_network_quiescence() < 0)
        return -1;

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
        uvdb_close_socket(&uvdb_candidate_socket);
        uvdb_close_socket(&uvdb_listen_socket);
        if(uvdb_state != UVDB_STATE_ERROR)
            uvdb_state = UVDB_STATE_IDLE;
        uvdb_unlock();
        if(uvdb_wait_for_network_quiescence() < 0)
            return -1;
    }
    if(thread >= 0)
    {
        uvdb_close_socket(&uvdb_socket);
        uvdb_close_socket(&uvdb_candidate_socket);
        uvdb_close_socket(&uvdb_listen_socket);
    }
    /* Protocol-gate idleness precedes the tail of exception_handler(): that
     * callback releases the protocol owner before dropping its guard lifetime.
     * Do not report a stopped generation as reusable until the full callback
     * has left, or an immediate start can misclassify its first trap as nested. */
    if(uvdb_wait_for_exception_quiescence() < 0)
        return -1;
    return 0;
}

#ifdef UVDB_KERNEL_THREAD_CONTROL
/* The lifecycle lock serializes handle ownership. Call this only after every
 * breakpoint slot is proven clear; the keeper is the component preventing a
 * live stop lease from expiring over an uncertain executable UDF. */
static int uvdb_stop_lease_keeper_locked(void)
{
    SceUID lease_thread = uvdb_lease_thread;
    if(lease_thread == sceKernelGetThreadId())
        return -1;
    __atomic_store_n(&uvdb_lease_stop, 1, __ATOMIC_SEQ_CST);
    if(lease_thread >= 0 &&
       uvdb_wait_delete_thread(&uvdb_lease_thread,
                               &uvdb_lease_thread_ended) < 0)
        return -1;
    return 0;
}

/* Caller holds both lifecycle serialization and uvdb_lock. Drop uvdb_lock
 * before joining the lease keeper because its normal exit unregisters itself
 * through that lock. Once no breakpoint obligation remains, ending renewal is
 * safe even if EndStop failed: the kernel watchdog can resume original code. */
static void uvdb_fail_shutdown_locked(int owns_stop)
{
    int restore_required = breakpoint_any_active();
    uvdb_state = UVDB_STATE_ERROR;
    if(owns_stop)
        uvdb_release_stop_controller();
    uvdb_unlock();
    if(!restore_required)
        uvdb_stop_lease_keeper_locked();
}
#endif

int uvdb_stop_server(void)
{
    uvdb_lifecycle_lock();
    int result = uvdb_stop_server_locked();
    if(result >= 0)
    {
        /* The joined server has published the result of its last restore
         * attempt in the transaction slots. This check applies to cooperative
         * and kernel builds; neither may report a clean stop over an uncertain
         * executable patch. The kernel build additionally retains its keeper
         * so a subsequent retry still owns a coherent all-stop. */
        uvdb_lock();
        int restore_required = breakpoint_active_count() != 0;
        if(restore_required)
            uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        if(restore_required)
            result = -1;
#ifdef UVDB_KERNEL_THREAD_CONTROL
        else
            result = uvdb_stop_lease_keeper_locked();
#endif
    }
    if(result >= 0)
    {
        /* Intentional shutdown can make a blocked receive report the same
         * transport error used for an unexpected peer loss. Once the server
         * is joined, callbacks are drained, sockets are retired, and every
         * restoration obligation has passed, the public stopped state is
         * unambiguously IDLE. */
        uvdb_lock();
        uvdb_state = UVDB_STATE_IDLE;
        uvdb_unlock();
    }
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
    /* The joined server cannot initiate another all-stop. Settle any existing
     * breakpoint obligation before joining the capture helper: that helper may
     * itself still be suspended by the lease we must retain through restore. */
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
        uvdb_fail_shutdown_locked(owns_stop);
        uvdb_lifecycle_unlock();
        return;
    }

    if((!breakpoints_active || coherent_stop) &&
       breakpoint_remove_all() < 0)
    {
        /* Never tear down handlers or release a stop while executable bytes
         * differ from their recorded originals. A later shutdown retries the
         * retained restoration obligation. */
        uvdb_fail_shutdown_locked(owns_stop);
        uvdb_lifecycle_unlock();
        return;
    }
    if(__atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST))
    {
        int end_result = uvdb_kernel_end_stop();
        if(end_result < 0 &&
           __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST) &&
           !__atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST))
            end_result = uvdb_kernel_end_stop();
        if(end_result < 0)
        {
            uvdb_fail_shutdown_locked(owns_stop);
            uvdb_lifecycle_unlock();
            return;
        }
    }
    if(owns_stop)
        uvdb_release_stop_controller();
#else
    if(breakpoint_remove_all() < 0)
    {
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        uvdb_lifecycle_unlock();
        return;
    }
#endif
    uvdb_close_socket(&uvdb_socket);
    uvdb_close_socket(&uvdb_candidate_socket);
    uvdb_close_socket(&uvdb_listen_socket);
    if(uvdb_release_handlers() < 0)
    {
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        uvdb_lifecycle_unlock();
        return;
    }
    /* Restore kernel dispatch first, then close this user-side gate. KuBridge
     * can have copied our callback pointer before replacement without having
     * entered it yet, so captured predecessors and the closed guard remain
     * process-lifetime state. Every callback that does enter is counted,
     * including one still executing a predecessor after its primary peer
     * returns. Do not hold uvdb_lock while waiting for visible callbacks. */
    uvdb_exception_guard_close(&uvdb_exception_guard);
    uvdb_unlock();
    if(uvdb_wait_for_exception_quiescence() < 0)
    {
        uvdb_lock();
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        uvdb_lifecycle_unlock();
        return;
    }
#ifdef UVDB_KERNEL_THREAD_CONTROL
    /* Executable patches and handlers are gone before uvdb_lock is dropped.
     * The keeper may now take that lock once to unregister and terminate. */
    if(uvdb_stop_lease_keeper_locked() < 0)
    {
        uvdb_lock();
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        uvdb_lifecycle_unlock();
        return;
    }
#endif
    /* With the server gone and no renewal guarding executable patches, a
     * capture helper suspended by the former all-stop can finish and join. */
    if(uvdb_restore_stdio() < 0)
    {
        uvdb_lock();
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        uvdb_lifecycle_unlock();
        return;
    }
    uvdb_lock();
    if(uvdb_pipe >= 0)
    {
        sceKernelDeleteMsgPipe(uvdb_pipe);
        uvdb_pipe = -1;
    }
    buffer_release(&in_buf);
    buffer_release(&out_buf);
    uvdb_rsp_request_lifetime_init(&uvdb_request_lifetime);
    uvdb_thread_inventory_reset(&uvdb_inventory);
    uvdb_thread_selection_reset(&uvdb_selection);
    uvdb_exception_thread = -1;
    uvdb_clear_io_failure();
    uvdb_target_stopped = 0;
    uvdb_async_stop_pending = 0;
    uvdb_async_stop_cancelled = 0;
    uvdb_console_reset();
    uvdb_console_transport_init(&uvdb_console_transport);
#ifdef UVDB_KERNEL_THREAD_CONTROL
    uvdb_lease_stop = 0;
    uvdb_stop_failed = 0;
    uvdb_stop_owner = UVDB_STOP_OWNER_NONE;
    uvdb_stop_token = 0;
#endif
    memset(uvdb_threads, 0, sizeof(uvdb_threads));
    /* Full shutdown is terminal. KuBridge does not expose a dispatcher
     * quiescence primitive, so an already-dispatched callback could enter even
     * after the visible active count reached zero. Keeping shutdown_pending,
     * both gates, and captured predecessor tokens intact makes that late path
     * safe while this linked image remains loaded. stop_server/start_server is
     * the supported nonterminal reconnect lifecycle. */
    uvdb_state = UVDB_STATE_IDLE;
    uvdb_unlock();
    uvdb_lifecycle_unlock();
}

static size_t recv_packet(char** data)
{
    if(!data)
        return 0;
    for(;;)
    {
        struct uvdb_rsp_frame frame;
        int result = uvdb_rsp_scan_frame(
            in_buf.buf, in_buf.size, uvdb_max_buffer - 4u, &frame);
        if(result == UVDB_RSP_FRAME_COMPLETE)
        {
#ifdef UVDB_ADMISSION_DIAGNOSTIC
            if(!__atomic_exchange_n(
                   &uvdb_admission_diagnostic_state.first_packet_recorded,
                   1u, __ATOMIC_ACQ_REL))
            {
                uvdb_admission_diagnostic_write_begin();
                __atomic_store_n(
                    &uvdb_admission_diagnostic_state.first_packet_size,
                    (uint32_t)frame.payload_size, __ATOMIC_RELAXED);
                uvdb_admission_diagnostic_record(
                    UVDB_ADMISSION_EVENT_FIRST_PACKET,
                    __atomic_load_n(
                        &uvdb_admission_diagnostic_state.current_socket,
                        __ATOMIC_ACQUIRE),
                    __atomic_load_n(
                        &uvdb_admission_diagnostic_state.current_generation,
                        __ATOMIC_ACQUIRE),
                    0);
                uvdb_admission_diagnostic_write_end();
            }
#endif
            if(!uvdb_console_transport_no_ack(&uvdb_console_transport))
            {
                buffer_write(&out_buf, "+", 1);
                buffer_flush(&out_buf);
                if(uvdb_has_io_failure())
                    return 0;
            }
            if(uvdb_rsp_request_lifetime_begin(
                   &uvdb_request_lifetime) < 0)
            {
                uvdb_note_io_failure();
                return 0;
            }
            *data = in_buf.buf + frame.payload_offset;
            in_buf.buf[frame.payload_offset + frame.payload_size] = 0;
            return frame.payload_size;
        }
        if(result == UVDB_RSP_FRAME_DISCARD)
        {
            if(!frame.consumed_size ||
               frame.consumed_size > in_buf.size)
            {
                uvdb_note_io_failure();
                return 0;
            }
            const int send_nack = uvdb_rsp_frame_should_nack(
                &frame,
                uvdb_console_transport_no_ack(&uvdb_console_transport));
            buffer_popleft(&in_buf, frame.consumed_size);
            if(send_nack)
            {
                /* ACK-mode corruption recovery is one raw NACK per discarded
                 * malformed frame. Prefix noise never requests a NACK, and
                 * no-ack sessions suppress it entirely. */
                buffer_write(&out_buf, "-", 1u);
                buffer_flush(&out_buf);
                if(uvdb_has_io_failure())
                    return 0;
            }
            continue;
        }

        char* unused = NULL;
#ifdef UVDB_RAW_RECV_DIAGNOSTIC
        __atomic_store_n(
            &uvdb_raw_recv_phase, UVDB_RAW_RECV_PHASE_REQUEST,
            __ATOMIC_RELEASE);
#endif
        if(!buffer_poll(&in_buf, &unused) && uvdb_has_io_failure())
        {
#ifdef UVDB_RAW_RECV_DIAGNOSTIC
            __atomic_store_n(
                &uvdb_raw_recv_phase, UVDB_RAW_RECV_PHASE_NONE,
                __ATOMIC_RELEASE);
#endif
            return 0;
        }
#ifdef UVDB_RAW_RECV_DIAGNOSTIC
        __atomic_store_n(
            &uvdb_raw_recv_phase, UVDB_RAW_RECV_PHASE_NONE,
            __ATOMIC_RELEASE);
#endif
    }
}

static void discard_packet(char* data, size_t sz)
{
    if(!data || !in_buf.buf)
    {
        uvdb_note_io_failure();
        return;
    }
    const uintptr_t base = (uintptr_t)in_buf.buf;
    const uintptr_t position = (uintptr_t)data;
    if(position < base || position - base > in_buf.size)
    {
        uvdb_note_io_failure();
        return;
    }
    const size_t offset = (size_t)(position - base);
    if(sz > in_buf.size - offset || in_buf.size - offset - sz < 3u)
    {
        uvdb_note_io_failure();
        return;
    }
    buffer_popleft(&in_buf, offset + sz + 3u);
    if(uvdb_rsp_request_lifetime_release(
           &uvdb_request_lifetime) < 0)
        uvdb_note_io_failure();
}

static int send_packet(void)
{
    /* Waiting for the peer's ACK may compact/refill in_buf. Never retain a
     * payload pointer into that buffer across the wait. */
    if(uvdb_rsp_request_lifetime_is_active(&uvdb_request_lifetime))
    {
        uvdb_note_io_failure();
        return -1;
    }
    buffer_end_packet(&out_buf);
    buffer_flush(&out_buf);
    if(uvdb_has_io_failure())
        return -1;
    if(uvdb_console_transport_no_ack(&uvdb_console_transport))
        return 0;
    for(;;)
    {
        for(size_t i = 0; i < in_buf.size; ++i)
        {
            if(in_buf.buf[i] == '+')
            {
                buffer_popleft(&in_buf, i + 1u);
                return 0;
            }
            /* Retransmission is intentionally unsupported because the frame
             * has already left the bounded output buffer. Fail closed rather
             * than letting a NACK fill the receive buffer indefinitely. */
            if(in_buf.buf[i] == '-')
            {
                uvdb_note_io_failure();
                return -1;
            }
        }
        if(in_buf.size)
            buffer_popleft(&in_buf, in_buf.size);
        char* unused = NULL;
#ifdef UVDB_RAW_RECV_DIAGNOSTIC
        __atomic_store_n(
            &uvdb_raw_recv_phase, UVDB_RAW_RECV_PHASE_RESPONSE_ACK,
            __ATOMIC_RELEASE);
#endif
        if(!buffer_poll(&in_buf, &unused) && uvdb_has_io_failure())
        {
#ifdef UVDB_RAW_RECV_DIAGNOSTIC
            __atomic_store_n(
                &uvdb_raw_recv_phase, UVDB_RAW_RECV_PHASE_NONE,
                __ATOMIC_RELEASE);
#endif
            return -1;
        }
#ifdef UVDB_RAW_RECV_DIAGNOSTIC
        __atomic_store_n(
            &uvdb_raw_recv_phase, UVDB_RAW_RECV_PHASE_NONE,
            __ATOMIC_RELEASE);
#endif
    }
}

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

static int parse_stream(
    const char* text,
    size_t text_size,
    struct stream* stream)
{
    if(!stream)
        return -1;
    struct uvdb_rsp_xfer_range range;
    if(uvdb_rsp_parse_xfer_range(text, text_size, &range) < 0)
        return -1;
    struct stream ans = {0};
    ans.marker_index = SIZE_MAX;
    ans.start = range.offset;
    ans.end = ans.start + range.length;
    if(ans.end < ans.start)
        ans.end = UINT64_MAX;
    *stream = ans;
    return 0;
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

static void write_hex(const char* start, size_t sz)
{
    while(sz--)
    {
        uint8_t c = *start++;
        uint8_t q[2] = {int2hex(c>>4), int2hex(c&15)};
        buffer_write(&out_buf, q, 2);
    }
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
static int breakpoint_insert_step_target_sized(
    uintptr_t address,
    size_t size,
    uintptr_t current_pc);

static int exclusive_step_read(
    void* context,
    uint32_t address,
    unsigned char* destination,
    size_t size)
{
    (void)context;
    size_t copied = safe_memcpy(
        (char*)destination, (const char*)(uintptr_t)address, size);
    return copied == size ? (int)copied : -1;
}

static int breakpoint_insert_exclusive_sequence(
    const struct uvdb_rsp_core_registers* core,
    uintptr_t pc,
    int hold_peers)
{
    /* Running several instructions to preserve the architectural exclusive
     * monitor is allowed only for the exact exception thread while the kernel
     * stop token holds every peer. The bounded scanner rejects control flow,
     * waits, syscalls, nested loads, mismatched stores, and uncertain reads. */
    if(!core || !hold_peers)
        return -1;
    const struct uvdb_exclusive_step_limits limits = {
        .instruction_limit = 16,
        .byte_limit = 64,
    };
    struct uvdb_exclusive_step_target target;
    if(uvdb_exclusive_step_scan(
           exclusive_step_read, NULL, (uint32_t)pc, core->cpsr,
           &limits, &target) != 1)
        return -1;
    return breakpoint_insert_step_target_sized(
        target.address, target.breakpoint_size, pc);
}

#define UVDB_MAX_BREAKPOINTS 32

struct uvdb_breakpoint {
    struct uvdb_breakpoint_patch_slot patch;
    uint8_t temporary;
};

static struct uvdb_breakpoint uvdb_breakpoints[UVDB_MAX_BREAKPOINTS];

static struct uvdb_breakpoint* breakpoint_find(uintptr_t address)
{
    address &= ~(uintptr_t)1;
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].patch.state !=
               UVDB_BREAKPOINT_PATCH_EMPTY &&
           uvdb_breakpoints[i].patch.address == address)
            return &uvdb_breakpoints[i];
    return NULL;
}

static size_t breakpoint_patch_read(
    void* user,
    uintptr_t address,
    void* output,
    size_t size)
{
    (void)user;
    return safe_memcpy((char*)output, (const char*)address, size);
}

static size_t breakpoint_patch_write(
    void* user,
    uintptr_t address,
    const void* input,
    size_t size)
{
    (void)user;
    kuKernelCpuUnrestrictedMemcpy((void*)address, input, size);
    /* The unrestricted copy has no result code. Exact safe readback in the
     * transaction helper is therefore the authoritative success check. */
    return size;
}

static int breakpoint_patch_sync(
    void* user,
    uintptr_t address,
    size_t size)
{
    (void)user;
    kuKernelFlushCaches((void*)address, size);
    return 0;
}

static const struct uvdb_breakpoint_patch_io breakpoint_patch_io = {
    .read = breakpoint_patch_read,
    .write = breakpoint_patch_write,
    .sync = breakpoint_patch_sync,
};

static int breakpoint_insert_internal(uintptr_t address, size_t size, int temporary)
{
    address &= ~(uintptr_t)1;
    if(size != 2 && size != 4)
        return -1;
    /* A tagged Thumb address is accepted for compatibility, but an A32 trap
     * must never straddle two instructions at a halfword-only address. */
    if(size == 4 && (address & 3u))
        return -1;
    struct uvdb_breakpoint* existing = breakpoint_find(address);
    if(existing)
        return existing->patch.state == UVDB_BREAKPOINT_PATCH_INSTALLED
            ? 0 : UVDB_BREAKPOINT_PATCH_ERROR_RESTORE_PENDING;

    struct uvdb_breakpoint* bp = NULL;
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].patch.state ==
           UVDB_BREAKPOINT_PATCH_EMPTY)
        {
            bp = &uvdb_breakpoints[i];
            break;
        }
    if(!bp)
        return -1;

    static const uint8_t thumb_udf[2] = {0x00, 0xde};
    static const uint8_t arm_udf[4] = {0xf0, 0x00, 0xf0, 0xe7};
    const void* trap = size == 2 ? (const void*)thumb_udf : (const void*)arm_udf;
    bp->temporary = temporary != 0;
    int result = uvdb_breakpoint_patch_install(
        &bp->patch, &breakpoint_patch_io, address, trap, size);
    if(bp->patch.state == UVDB_BREAKPOINT_PATCH_EMPTY)
        bp->temporary = 0;
    return result;
}

static int breakpoint_insert(uintptr_t address, size_t size)
{
    return breakpoint_insert_internal(address, size, 0);
}

/* BXWritePC/LoadWritePC clear the Thumb state-selection bit. ARM targets must
 * already be word aligned; a ...10 destination is architecturally invalid and
 * must not be rounded to a different instruction. */
static int breakpoint_insert_step_target_sized(
    uintptr_t address,
    size_t size,
    uintptr_t current_pc)
{
    if(size != 2u && size != 4u)
        return -1;
    if(size == 4u && (address & 3u))
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
    int result = uvdb_breakpoint_patch_restore(
        &bp->patch, &breakpoint_patch_io);
    if(result == UVDB_BREAKPOINT_PATCH_OK)
        bp->temporary = 0;
    return result;
}

static int breakpoint_remove_all(void)
{
    int result = 0;
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].patch.state !=
               UVDB_BREAKPOINT_PATCH_EMPTY &&
           breakpoint_remove(uvdb_breakpoints[i].patch.address) < 0)
            result = -1;
    return result;
}

static size_t breakpoint_active_count(void)
{
    size_t count = 0;
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoint_patch_requires_restore(
               &uvdb_breakpoints[i].patch))
            count++;
    return count;
}

static int breakpoint_restore_pending(void)
{
    int result = 0;
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].patch.state ==
               UVDB_BREAKPOINT_PATCH_RESTORE_PENDING &&
           breakpoint_remove(uvdb_breakpoints[i].patch.address) < 0)
            result = -1;
    return result;
}

#ifdef UVDB_KERNEL_THREAD_CONTROL
static int breakpoint_any_active(void)
{
    return breakpoint_active_count() != 0;
}
#endif

static int breakpoint_remove_temporary(void)
{
    int result = 0;
    for(size_t i = 0; i < UVDB_MAX_BREAKPOINTS; ++i)
        if(uvdb_breakpoints[i].patch.state !=
               UVDB_BREAKPOINT_PATCH_EMPTY &&
           uvdb_breakpoints[i].temporary &&
           breakpoint_remove(uvdb_breakpoints[i].patch.address) < 0)
            result = -1;
    return result;
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

static int breakpoint_insert_step(
    const struct uvdb_rsp_core_registers* core,
    int hold_peers)
{
    if(!core)
        return -1;
    if(!uvdb_step_cpsr_state_supported(core->cpsr))
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

        uint16_t second = 0;
        if(instruction_size == 4 &&
           safe_memcpy((char*)&second, (const char*)(pc + 2),
                       sizeof(second)) != sizeof(second))
            return -1;
        if(hold_peers && uvdb_step_instruction_may_block(
               (uint32_t)instruction | ((uint32_t)second << 16), 1))
            return -1;
        if(instruction_size == 4 &&
           uvdb_thumb32_instruction_starts_exclusive(instruction, second))
            return current_itstate ? -1 :
                breakpoint_insert_exclusive_sequence(core, pc, hold_peers);

        /* Enforce placement before the condition-failed shortcut so malformed
         * control flow cannot bypass architectural IT restrictions. */
        if(!uvdb_thumb_it_step_placement_valid(
               instruction, second, instruction_size, current_itstate))
            return -1;

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
        {
            if(current_itstate &&
               uvdb_thumb_itstate_advance(current_itstate) != 0)
                return -1;
            return breakpoint_insert_internal(direct_target.address,
                                               direct_target.breakpoint_size,
                                               1);
        }

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

        // POP {..., PC}. The saved PC follows each selected low register on
        // the current stack; read it without directly dereferencing user RAM.
        if((instruction & 0xff00) == 0xbd00)
        {
            if(current_itstate &&
               uvdb_thumb_itstate_advance(current_itstate) != 0)
                return -1;
            unsigned int register_count =
                (unsigned int)__builtin_popcount(instruction & 0xff);
            uintptr_t target;
            uintptr_t saved_pc_address =
                core->r[13] + register_count * sizeof(uint32_t);
            if(!uvdb_step_word_address_valid((uint32_t)saved_pc_address))
                return -1;
            const char* saved_pc = (const char*)saved_pc_address;
            if(safe_memcpy((char*)&target, saved_pc, sizeof(target)) !=
               sizeof(target))
                return -1;
            return breakpoint_insert_step_target(target, pc);
        }

        if(instruction_size == 4)
        {
            direct_result = uvdb_thumb32_plan_branch_step(
                instruction, second, (uint32_t)pc, core->cpsr,
                &direct_target);
            if(direct_result < 0)
                return -1;
            if(direct_result > 0)
            {
                if(current_itstate &&
                   uvdb_thumb_itstate_advance(current_itstate) != 0)
                    return -1;
                return breakpoint_insert_internal(
                    direct_target.address, direct_target.breakpoint_size, 1);
            }

            // Thumb-2 table branch byte/halfword. Read the selected table
            // entry through safe_memcpy and branch relative to PC+4.
            if((instruction & 0xfff0) == 0xe8d0 &&
               (second & 0xffe0) == 0xf000)
            {
                unsigned int rn = instruction & 0xf;
                unsigned int rm = second & 0xf;
                unsigned int halfword = (second >> 4) & 1;
                if((current_itstate &&
                    uvdb_thumb_itstate_advance(current_itstate) != 0) ||
                   rn == 13 || rm == 13 || rm == 15)
                    return -1;
                const uint32_t* registers = core->r;
                uintptr_t base = rn == 15
                    ? pc + 4
                    : registers[rn];
                uintptr_t table_address = base +
                    ((uintptr_t)registers[rm] << halfword);
                uint16_t table_offset = 0;
                size_t entry_size = halfword ? 2 : 1;
                if(safe_memcpy((char*)&table_offset,
                               (const char*)table_address, entry_size) !=
                   entry_size)
                    return -1;
                uintptr_t target = (pc + 4) +
                                   (uintptr_t)table_offset * 2;
                return breakpoint_insert_step_target_sized(target, 2, pc);
            }

            // Thumb-2 LDMIA/POP.W restoring PC. PC is stored after every
            // lower-numbered register selected by the register list.
            if((instruction & 0xffd0) == 0xe890 && (second & 0x8000))
            {
                if(current_itstate &&
                   uvdb_thumb_itstate_advance(current_itstate) != 0)
                    return -1;
                unsigned int rn = instruction & 0xf;
                unsigned int register_count =
                    (unsigned int)__builtin_popcount(second);
                int writeback = (instruction & 0x20) != 0;
                if(rn == 15 || register_count < 2 ||
                   (second & 0xc000) == 0xc000 ||
                   (writeback && (second & (1u << rn))))
                    return -1;
                const uint32_t* registers = core->r;
                unsigned int lower_count =
                    (unsigned int)__builtin_popcount(second & 0x7fff);
                uintptr_t saved_pc_address = registers[rn] +
                    lower_count * sizeof(uint32_t);
                if(!uvdb_step_word_address_valid(
                       (uint32_t)saved_pc_address))
                    return -1;
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
                if(current_itstate &&
                   uvdb_thumb_itstate_advance(current_itstate) != 0)
                    return -1;
                unsigned int rn = instruction & 0xf;
                unsigned int register_count =
                    (unsigned int)__builtin_popcount(second);
                int writeback = (instruction & 0x20) != 0;
                if(rn == 15 || register_count < 2 ||
                   (second & 0xc000) == 0xc000 ||
                   (writeback && (second & (1u << rn))))
                    return -1;
                const uint32_t* registers = core->r;
                uintptr_t target;
                uintptr_t saved_pc_address = registers[rn] - sizeof(uint32_t);
                if(!uvdb_step_word_address_valid(
                       (uint32_t)saved_pc_address))
                    return -1;
                if(safe_memcpy((char*)&target,
                               (const char*)saved_pc_address,
                               sizeof(target)) != sizeof(target))
                    return -1;
                return breakpoint_insert_step_target(target, pc);
            }

            uint32_t load_address = 0;
            int load_result = uvdb_thumb32_plan_load_pc_address(
                instruction, second, (uint32_t)pc, core->r,
                &load_address);
            if(load_result < 0)
                return -1;
            if(load_result > 0)
            {
                if(current_itstate &&
                   uvdb_thumb_itstate_advance(current_itstate) != 0)
                    return -1;
                uintptr_t target;
                if(!uvdb_step_word_address_valid(load_address))
                    return -1;
                if(safe_memcpy((char*)&target,
                               (const char*)(uintptr_t)load_address,
                               sizeof(target)) != sizeof(target))
                    return -1;
                return breakpoint_insert_step_target(target, pc);
            }

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
    if(hold_peers && uvdb_step_instruction_may_block(instruction, 0))
        return -1;
    if(uvdb_arm_instruction_starts_exclusive(instruction))
    {
        unsigned int condition = instruction >> 28;
        if(condition != 0xfu &&
           !uvdb_arm_condition_passed(condition, core->cpsr))
            return breakpoint_insert_internal(pc + 4, 4, 1);
        return breakpoint_insert_exclusive_sequence(core, pc, hold_peers);
    }

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
        /* LDM with S=1 and PC restores CPSR from SPSR (exception return),
         * which has no valid user-mode stepping interpretation. */
        if(instruction & (1u << 22))
            return -1;
        unsigned int rn = (instruction >> 16) & 0xf;
        unsigned int register_count =
            (unsigned int)__builtin_popcount(instruction & 0xffff);
        int writeback = (instruction & (1u << 21)) != 0;
        if(rn == 15 || register_count < 2 ||
           (writeback && (instruction & (1u << rn))))
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
        if(!uvdb_step_word_address_valid((uint32_t)saved_pc_address))
            return -1;
        uintptr_t target;
        if(safe_memcpy((char*)&target, (const char*)saved_pc_address,
                       sizeof(target)) != sizeof(target))
            return -1;
        return breakpoint_insert_step_target(target, pc);
    }

    uint32_t load_address = 0;
    int load_result = uvdb_arm_plan_load_pc_address(
        instruction, (uint32_t)pc, core->cpsr, core->r, &load_address);
    if(load_result < 0)
        return -1;
    if(load_result > 0)
    {
        uintptr_t target;
        if(!uvdb_step_word_address_valid(load_address))
            return -1;
        if(safe_memcpy((char*)&target,
                       (const char*)(uintptr_t)load_address,
                       sizeof(target)) != sizeof(target))
            return -1;
        return breakpoint_insert_step_target(target, pc);
    }

    /* Do not let an unsupported PC-writing form escape the sequential trap.
     * A failed condition is known to fall through; a taken BXJ, privileged
     * exception return, register-controlled-shift PC write, or other undecoded
     * transfer stays stopped and is reported as an unsupported step request. */
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
    uint32_t pc_override,
    int hold_peers)
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
    return breakpoint_insert_step(&core, hold_peers);
}

static size_t safe_memcpy(char* dst, const char* src, size_t sz)
{
    size_t ans = 0;
    while(sz)
    {
        size_t chk;
        uvdb_net_syscall_arg rest[3] = {
            1, (uvdb_net_syscall_arg)&chk, 0,
        };
        SceKernelAddrPair q = {(uvdb_net_syscall_arg)src, sz};
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
            uvdb_net_syscall_arg rest[3] = {
                1, (uvdb_net_syscall_arg)&chk2, 0,
            };
            SceKernelAddrPair q = {(uvdb_net_syscall_arg)dst, chk};
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

/* Execute the status-query branch's complete buffer lifetime in one place so
 * a reply can never be reordered ahead of consuming its request. Return -2
 * when stopped inventory could not be refreshed (the caller retains the
 * existing stop failure policy), and -1 for transport/lifetime failure. */
static int uvdb_handle_status_query_request(
    char* packet, size_t packet_size, int stop_signal)
{
    if(uvdb_refresh_stopped_inventory() < 0)
        return -2;
    if(!out_buf.size)
    {
        uvdb_note_io_failure();
        return -1;
    }
    out_buf.size--;
    if(uvdb_pump_console_before_stop() < 0)
    {
        discard_packet(packet, packet_size);
        return -1;
    }
    /* send_packet waits for '+' in ACK mode and may compact in_buf. Release
     * the current payload borrow before constructing and sending Txx. */
    discard_packet(packet, packet_size);
    return send_stop_reply(stop_signal);
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
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    uvdb_admission_diagnostic_record(
        UVDB_ADMISSION_EVENT_TARGET_RUNNING, -1, 0, 0);
#endif
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

    if(coherent_stop && breakpoint_remove_all() < 0)
    {
        /* Restoration is still uncertain. Keep the coherent stop session and
         * recovery metadata alive; releasing here could execute a partial UDF
         * patch. */
        uvdb_release_stop_controller();
        uvdb_close_socket(&uvdb_socket);
        uvdb_state = UVDB_STATE_ERROR;
        return;
    }

    int end_result = uvdb_kernel_end_stop();
    if(end_result < 0 &&
       __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST) &&
       !__atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST))
    {
        /* EndStop re-established a coherent all-stop before reporting its
         * partial-resume failure. Reassert code cleanup before one retry. */
        if(breakpoint_remove_all() < 0)
        {
            uvdb_release_stop_controller();
            uvdb_close_socket(&uvdb_socket);
            uvdb_state = UVDB_STATE_ERROR;
            return;
        }
        end_result = uvdb_kernel_end_stop();
    }
    if(end_result < 0 &&
       __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST))
        uvdb_kernel_abandon_stop();
    uvdb_release_stop_controller();
#else
    if(breakpoint_remove_all() < 0)
    {
        uvdb_close_socket(&uvdb_socket);
        uvdb_state = UVDB_STATE_ERROR;
        return;
    }
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

static void uvdb_monitor_copy_name(
    char* destination,
    size_t capacity,
    const char* source,
    size_t source_capacity)
{
    if(!destination || !capacity)
        return;
    size_t count = 0;
    if(source)
        while(count + 1u < capacity && count < source_capacity &&
              source[count])
        {
            destination[count] = source[count];
            count++;
        }
    destination[count] = 0;
}

static void uvdb_monitor_fill_status(
    struct uvdb_monitor_snapshot* snapshot,
    int stop_signal)
{
    struct uvdb_monitor_status* status = &snapshot->status;
    status->state = (enum uvdb_monitor_state)uvdb_state;
    status->port = uvdb_port;
    status->packet_size = (uint32_t)(uvdb_max_buffer - 4u);
    status->thread_count = (uint32_t)uvdb_inventory.count;
    status->software_breakpoint_count =
        (uint32_t)breakpoint_active_count();
    status->stopped_thread = uvdb_selection.stopped;
    status->general_thread = uvdb_selection.general;
    status->resume_thread = uvdb_selection.resume;
    status->exception_thread = uvdb_exception_thread;
    status->stop_signal = stop_signal;
    status->target_stopped =
        __atomic_load_n(&uvdb_target_stopped, __ATOMIC_SEQ_CST) != 0;
    status->no_ack_mode =
        uvdb_console_transport_no_ack(&uvdb_console_transport);

#ifdef UVDB_KERNEL_THREAD_CONTROL
    status->kernel_compiled = 1;
    struct vd_kernel_status kernel_status = {0};
    status->kernel_status_available =
        vdKernelGetStatus(&kernel_status) >= 0;
    if(status->kernel_status_available)
    {
        status->kernel_abi = kernel_status.abi_version;
        status->kernel_capabilities = kernel_status.capabilities;
        status->kernel_max_threads = kernel_status.max_threads;
        status->kernel_compatible =
            kernel_status.abi_version == VD_KERNEL_ABI_VERSION &&
            (kernel_status.capabilities &
                 VD_KERNEL_REQUIRED_THREAD_CONTROL_CAPABILITIES) ==
                VD_KERNEL_REQUIRED_THREAD_CONTROL_CAPABILITIES &&
            kernel_status.max_threads == VD_KERNEL_MAX_THREADS;
    }
    status->stop_session_active =
        __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST) != 0;
    status->stop_session_failed =
        __atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST) != 0;
#endif
#ifdef UVDB_KERNEL_VFP_READS
    status->vfp_reads_enabled = uvdb_rsp_vfp_enabled;
#endif

    status->last_fault_available =
        uvdb_last_fault.exception_type != UVDB_EXCEPTION_NONE;
    if(status->last_fault_available)
    {
        status->last_fault_type = uvdb_last_fault.exception_type;
        status->last_fault_status = uvdb_last_fault.fault_status;
        status->last_fault_address = uvdb_last_fault.fault_address;
        status->last_fault_pc = uvdb_last_fault.pc;
    }
}

static void uvdb_monitor_fill_threads(
    struct uvdb_monitor_snapshot* snapshot)
{
    snapshot->threads = uvdb_monitor_threads;
    snapshot->thread_count = uvdb_inventory.count;
    int32_t general = uvdb_thread_selection_general(
        &uvdb_selection, &uvdb_inventory);
    int32_t resume = uvdb_thread_selection_step(
        &uvdb_selection, &uvdb_inventory);
    for(size_t i = 0; i < snapshot->thread_count; ++i)
    {
        struct uvdb_monitor_thread* output = &uvdb_monitor_threads[i];
        memset(output, 0, sizeof(*output));
        output->id = uvdb_inventory.ids[i];
        if(output->id == uvdb_selection.stopped)
            output->flags |= UVDB_MONITOR_THREAD_STOPPED;
        if(output->id == general)
            output->flags |= UVDB_MONITOR_THREAD_GENERAL;
        if(output->id == resume)
            output->flags |= UVDB_MONITOR_THREAD_RESUME;
        if(output->id == uvdb_exception_thread)
            output->flags |= UVDB_MONITOR_THREAD_EXCEPTION;

        struct uvdb_thread_entry* registered =
            uvdb_find_thread(output->id);
        if(registered && registered->name[0])
            uvdb_monitor_copy_name(output->name, sizeof(output->name),
                                   registered->name,
                                   sizeof(registered->name));
        else if(output->flags & UVDB_MONITOR_THREAD_STOPPED)
            uvdb_monitor_copy_name(output->name, sizeof(output->name),
                                   "stopped thread",
                                   sizeof("stopped thread") - 1u);
        else
            uvdb_monitor_copy_name(output->name, sizeof(output->name),
                                   "process thread",
                                   sizeof("process thread") - 1u);
    }
}

static void uvdb_monitor_fill_modules(
    struct uvdb_monitor_snapshot* snapshot)
{
    SceUID module_ids[UVDB_MONITOR_MAX_MODULES];
    SceSize count = UVDB_MONITOR_MAX_MODULES;
    int result = sceKernelGetModuleList(0xff, module_ids, &count);
    snapshot->module_query_result = result;
    if(result < 0)
        return;

    snapshot->module_reported_count = count;
    size_t scanned = count;
    if(scanned > UVDB_MONITOR_MAX_MODULES)
        scanned = UVDB_MONITOR_MAX_MODULES;
    snapshot->module_scanned_count = scanned;
    snapshot->modules = uvdb_monitor_modules;

    for(size_t i = 0; i < scanned; ++i)
    {
        SceKernelModuleInfo info = {.size = sizeof(info)};
        if(sceKernelGetModuleInfo(module_ids[i], &info) < 0)
        {
            snapshot->module_skipped_count++;
            continue;
        }

        struct uvdb_monitor_module* output =
            &uvdb_monitor_modules[snapshot->module_count];
        memset(output, 0, sizeof(*output));
        output->id = (uint32_t)module_ids[i];
        uvdb_monitor_copy_name(output->name, sizeof(output->name),
                               info.module_name,
                               sizeof(info.module_name));
        for(size_t segment = 0;
            segment < UVDB_MONITOR_MAX_SEGMENTS; ++segment)
        {
            if(!info.segments[segment].vaddr ||
               !info.segments[segment].memsz)
                continue;
            struct uvdb_monitor_segment* segment_output =
                &output->segments[output->segment_count++];
            segment_output->address =
                (uint32_t)(uintptr_t)info.segments[segment].vaddr;
            segment_output->memory_size = info.segments[segment].memsz;
            segment_output->permissions = info.segments[segment].perms;
            segment_output->index = (uint32_t)segment;
        }
        snapshot->module_count++;
    }
}

static void uvdb_monitor_fill_console(
    struct uvdb_monitor_snapshot* snapshot)
{
    struct uvdb_console_stats queue;
    struct uvdb_console_transport_stats transport;
    if(uvdb_console_get_stats(&queue) < 0 ||
       uvdb_console_transport_get_stats(
           &uvdb_console_transport, &transport) < 0)
        return;

    struct uvdb_monitor_console* output = &snapshot->console;
    output->available = 1;
#define COPY_QUEUE_STAT(name) output->name = queue.name
    COPY_QUEUE_STAT(session_open);
    COPY_QUEUE_STAT(session_generation);
    COPY_QUEUE_STAT(queued_records);
    COPY_QUEUE_STAT(queued_bytes);
    COPY_QUEUE_STAT(sessions_opened);
    COPY_QUEUE_STAT(reconnects);
    COPY_QUEUE_STAT(accepted_records);
    COPY_QUEUE_STAT(accepted_bytes);
    COPY_QUEUE_STAT(sent_records);
    COPY_QUEUE_STAT(sent_bytes);
    COPY_QUEUE_STAT(dropped_disconnected_records);
    COPY_QUEUE_STAT(dropped_disconnected_bytes);
    COPY_QUEUE_STAT(dropped_contention_records);
    COPY_QUEUE_STAT(dropped_contention_bytes);
    COPY_QUEUE_STAT(dropped_full_records);
    COPY_QUEUE_STAT(dropped_full_bytes);
    COPY_QUEUE_STAT(dropped_stale_records);
    COPY_QUEUE_STAT(dropped_stale_bytes);
#undef COPY_QUEUE_STAT
    output->no_ack_mode = uvdb_console_transport.no_ack_mode;
    output->transport_failed = uvdb_console_transport.failed;
#define COPY_TRANSPORT_STAT(name) output->name = transport.name
    COPY_TRANSPORT_STAT(frames_sent);
    COPY_TRANSPORT_STAT(frame_bytes_sent);
    COPY_TRANSPORT_STAT(would_block);
    COPY_TRANSPORT_STAT(commit_busy);
    COPY_TRANSPORT_STAT(partial_writes);
    COPY_TRANSPORT_STAT(hard_errors);
    COPY_TRANSPORT_STAT(session_errors);
    COPY_TRANSPORT_STAT(last_native_error);
#undef COPY_TRANSPORT_STAT
}

#ifdef UVDB_MONITOR_DISPLAY
static void uvdb_monitor_sample_framebuffer(
    struct uvdb_monitor_framebuffer* output,
    SceDisplaySetBufSync sync)
{
    SceDisplayFrameBuf framebuffer = {.size = sizeof(framebuffer)};
    output->query_result = sceDisplayGetFrameBuf(&framebuffer, sync);
    if(output->query_result < 0)
        return;
    output->address = (uint32_t)(uintptr_t)framebuffer.base;
    output->pitch = framebuffer.pitch;
    output->pixel_format = framebuffer.pixelformat;
    output->width = framebuffer.width;
    output->height = framebuffer.height;
}

/* Display APIs are intentionally called only from the ordinary server thread.
 * The exception/RSP path may have interrupted a display call while one of its
 * private locks was held, so re-entering SceDisplay from that path could
 * deadlock. Collect into a local value before taking the debugger lock. */
static void uvdb_monitor_collect_display(
    struct uvdb_monitor_display* output)
{
    memset(output, 0, sizeof(*output));
    output->available = 1;
    output->primary_head = sceDisplayGetPrimaryHead();
    output->vcount = sceDisplayGetVcount();

    float refresh_rate = 0.0f;
    output->refresh_query_result = sceDisplayGetRefreshRate(&refresh_rate);
    if(output->refresh_query_result >= 0)
    {
        uint32_t refresh_bits = 0;
        memcpy(&refresh_bits, &refresh_rate, sizeof(refresh_bits));
        if(uvdb_monitor_ieee754_to_millihz(
               refresh_bits, &output->refresh_millihz) < 0)
            output->refresh_query_result = -1;
    }

    int maximum_width = 0;
    int maximum_height = 0;
    output->maximum_query_result =
        sceDisplayGetMaximumFrameBufResolution(
            &maximum_width, &maximum_height);
    if(output->maximum_query_result >= 0 &&
       maximum_width >= 0 && maximum_height >= 0)
    {
        output->maximum_width = (uint32_t)maximum_width;
        output->maximum_height = (uint32_t)maximum_height;
    }
    else if(output->maximum_query_result >= 0)
        output->maximum_query_result = -1;

    uvdb_monitor_sample_framebuffer(
        &output->immediate, SCE_DISPLAY_SETBUF_IMMEDIATE);
    uvdb_monitor_sample_framebuffer(
        &output->next_frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static void uvdb_monitor_refresh_display_cache(void)
{
    int before_socket = -1;
    uint32_t before_generation = 0;
    uvdb_active_socket_snapshot(&before_socket, &before_generation);

    struct uvdb_monitor_display sample;
    uvdb_monitor_collect_display(&sample);

    if(uvdb_try_lock())
    {
        int after_socket = -1;
        uint32_t after_generation = 0;
        uvdb_active_socket_snapshot(&after_socket, &after_generation);
        if(before_socket == after_socket &&
           before_generation == after_generation)
        {
            uvdb_monitor_display_cache = sample;
            uvdb_monitor_display_cache_generation =
                before_socket >= 0 ? before_generation : 0;
        }
        uvdb_unlock();
    }
}

/* A server-owned accept defers its synthetic stop until this ordinary-thread
 * sample is complete. The state is revalidated under uvdb_lock before the
 * sample is published, so a competing application fault or reconnect wins
 * without ever exposing data from the wrong connection generation. */
static void uvdb_monitor_finish_initial_display_stop(void)
{
    uvdb_lock();
    uint32_t generation =
        uvdb_monitor_display_stop_pending_generation(
            &uvdb_monitor_display_stop);
    uvdb_unlock();
    if(!generation)
        return;

    struct uvdb_monitor_display sample;
    uvdb_monitor_collect_display(&sample);

    int trigger_stop = 0;
    uvdb_lock();
    int active_socket = -1;
    uint32_t active_generation = 0;
    uvdb_active_socket_snapshot(&active_socket, &active_generation);
    if(uvdb_monitor_display_generation_is_current(
           generation, active_socket, active_generation) &&
       !__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST) &&
       !__atomic_load_n(&uvdb_target_stopped, __ATOMIC_SEQ_CST) &&
       uvdb_monitor_display_stop_arm(
           &uvdb_monitor_display_stop, generation) == 0)
    {
        uvdb_monitor_display_cache = sample;
        uvdb_monitor_display_cache_generation = generation;
        trigger_stop = 1;
    }
    else if(uvdb_monitor_display_stop_pending_generation(
                &uvdb_monitor_display_stop) == generation)
        uvdb_monitor_display_stop_reset(&uvdb_monitor_display_stop);
    uvdb_unlock();

    if(trigger_stop)
        uvdb_monitor_trigger_initial_stop();
}
#else
static void uvdb_monitor_refresh_display_cache(void) {}
static void uvdb_monitor_finish_initial_display_stop(void) {}
#endif

static void uvdb_monitor_fill_display(
    struct uvdb_monitor_snapshot* snapshot)
{
#ifdef UVDB_MONITOR_DISPLAY
    int active_socket = -1;
    uint32_t active_generation = 0;
    uvdb_active_socket_snapshot(&active_socket, &active_generation);
    if(uvdb_monitor_display_generation_is_current(
           uvdb_monitor_display_cache_generation,
           active_socket, active_generation))
        snapshot->display = uvdb_monitor_display_cache;
#else
    (void)snapshot;
#endif
}

static int uvdb_write_monitor_result(
    enum uvdb_monitor_command command,
    int stop_signal)
{
    struct uvdb_monitor_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    if(command == UVDB_MONITOR_COMMAND_STATUS ||
       command == UVDB_MONITOR_COMMAND_THREADS)
    {
        if(uvdb_refresh_stopped_inventory() < 0)
            return -1;
    }
    if(command == UVDB_MONITOR_COMMAND_STATUS)
        uvdb_monitor_fill_status(&snapshot, stop_signal);
    else if(command == UVDB_MONITOR_COMMAND_THREADS)
        uvdb_monitor_fill_threads(&snapshot);
    else if(command == UVDB_MONITOR_COMMAND_MODULES)
        uvdb_monitor_fill_modules(&snapshot);
    else if(command == UVDB_MONITOR_COMMAND_CONSOLE)
        uvdb_monitor_fill_console(&snapshot);
    else if(command == UVDB_MONITOR_COMMAND_DISPLAY)
        uvdb_monitor_fill_display(&snapshot);

    size_t raw_capacity = (uvdb_max_buffer - 4u) / 2u;
    if(raw_capacity > sizeof(uvdb_monitor_output))
        raw_capacity = sizeof(uvdb_monitor_output);
    size_t output_size = 0;
    int render_result = uvdb_monitor_render(
        command, &snapshot, uvdb_monitor_output, raw_capacity, &output_size);
    if(render_result < 0)
    {
        static const char failure[] =
            "error: monitor response could not be rendered\n";
        write_hex(failure, sizeof(failure) - 1u);
        return 0;
    }
    write_hex(uvdb_monitor_output, output_size);
    return 0;
}

static int uvdb_apply_resume_plan(
    const struct uvdb_resume_plan* plan,
    KuKernelExceptionContext* ctx,
    int has_pc_override,
    uint32_t pc_override)
{
    /* A prior partially failed patch/restore transaction is a hard execution
     * barrier. Retry its recorded originals first and never resume while any
     * slot remains uncertain. */
    if(breakpoint_restore_pending() < 0)
        return -2;
#ifdef UVDB_KERNEL_THREAD_CONTROL
    int stop_session_active =
        __atomic_load_n(&uvdb_stop_token, __ATOMIC_SEQ_CST) != 0;
    int stop_session_failed =
        __atomic_load_n(&uvdb_stop_failed, __ATOMIC_SEQ_CST) != 0;
#else
    int stop_session_active = 0;
    int stop_session_failed = 0;
#endif
    if(uvdb_resume_plan_validate(
           plan, uvdb_exception_thread,
           __atomic_load_n(&uvdb_target_stopped, __ATOMIC_SEQ_CST),
           stop_session_active, stop_session_failed,
           has_pc_override) < 0)
        return -1;
    int hold_peers = plan->scope == UVDB_RESUME_SCOPE_STOPPED_THREAD;
    if(plan->kind == UVDB_RESUME_STEP)
    {
        int patch_result = breakpoint_insert_step_thread(
            plan->step_thread, ctx, has_pc_override, pc_override,
            hold_peers);
        if(patch_result == UVDB_BREAKPOINT_PATCH_ERROR_RESTORE_PENDING)
            return -2;
        if(patch_result < 0)
            return -1;
    }

    /* The active kernel stop session excludes its original exception
     * controller. Returning from that handler executes exactly the selected
     * stopped thread while the lease keeper renews suspension of every peer.
     * The temporary UDF exception reuses the same token and restores code. */
    if(hold_peers)
        return 0;
    if(uvdb_kernel_end_stop() < 0)
        return -2;
    if(has_pc_override)
        ctx->pc = pc_override;
    return 0;
}

static int uvdb_finish_resume_packet(char* packet)
{
    /* The next exception parses this synthetic status query. Keeping it in the
     * receive buffer preserves the existing all-stop no-immediate-reply flow. */
    memcpy(packet, "?#3f", 4);
    if(uvdb_rsp_request_lifetime_release(
           &uvdb_request_lifetime) < 0)
    {
        uvdb_note_io_failure();
        return -1;
    }
    out_buf.size--;
    buffer_flush(&out_buf);
    if(uvdb_has_io_failure())
        return -1;
    uvdb_note_target_running();
    return 0;
}

static void uvdb_main_loop(
    KuKernelExceptionContext* ctx,
    int stop_signal,
    enum uvdb_fileio_context fileio_context)
{
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    uvdb_admission_diagnostic_record(
        UVDB_ADMISSION_EVENT_MAIN_LOOP_ENTERED,
        __atomic_load_n(
            &uvdb_admission_diagnostic_state.current_socket,
            __ATOMIC_ACQUIRE),
        __atomic_load_n(
            &uvdb_admission_diagnostic_state.current_generation,
            __ATOMIC_ACQUIRE),
        0);
#endif
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
        if(uvdb_has_io_failure())
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
#ifdef UVDB_RAW_RECV_DIAGNOSTIC
        else if(IS("qUvdbRawRecvDiagnostic"))
            uvdb_write_raw_recv_diagnostic();
#endif
        else if(STARTSWITH("qRcmd,"))
        {
            enum uvdb_monitor_command command = UVDB_MONITOR_COMMAND_NONE;
            int parse_result = uvdb_monitor_parse_qrcmd(
                pkt, sz, &command);
            if(parse_result == UVDB_MONITOR_PARSE_MALFORMED)
            {
                static const char malformed[] =
                    "error: malformed monitor command; use 'monitor help'\n";
                write_hex(malformed, sizeof(malformed) - 1u);
            }
            else if(parse_result == UVDB_MONITOR_PARSE_UNKNOWN)
            {
                static const char unknown[] =
                    "error: unknown monitor command; use 'monitor help'\n";
                write_hex(unknown, sizeof(unknown) - 1u);
            }
            else if(uvdb_write_monitor_result(command, stop_signal) < 0)
                return;
        }
        else if(STARTSWITH("qXfer:features:read:target.xml:"))
        {
            const size_t prefix_size =
                sizeof("qXfer:features:read:target.xml:") - 1u;
            struct stream st;
            if(parse_stream(pkt + prefix_size, sz - prefix_size, &st) < 0)
            {
                buffer_write(&out_buf, STRING("E01"));
                goto packet_complete;
            }
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
            const size_t prefix_size =
                sizeof("qXfer:libraries:read::") - 1u;
            struct stream st;
            if(parse_stream(pkt + prefix_size, sz - prefix_size, &st) < 0)
            {
                buffer_write(&out_buf, STRING("E01"));
                goto packet_complete;
            }
            stream_write_libraries(&st);
        }
        else if(IS("?"))
        {
            int status_result = uvdb_handle_status_query_request(
                pkt, sz, stop_signal);
            if(status_result == -2)
                return;
            if(status_result < 0)
            {
                uvdb_fail_stopped_client();
                return;
            }
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
                if(uvdb_finish_resume_packet(pkt) < 0)
                    uvdb_fail_stopped_client();
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
        else if(sz && pkt[0] == 'm')
        {
            struct uvdb_rsp_memory_request request;
            const size_t maximum_read = (uvdb_max_buffer - 4u) / 2u;
            if(uvdb_rsp_parse_memory_read_packet(
                   pkt, sz, maximum_read, &request) < 0)
                buffer_write(&out_buf, STRING("E01"));
            else
            {
                uintptr_t addr = (uintptr_t)request.address;
                size_t remaining = request.size;
                char probe[64];
                while(remaining)
                {
                    size_t chunk = remaining > sizeof(probe)
                        ? sizeof(probe) : remaining;
                    if(safe_memcpy(probe, (const void*)addr, chunk) != chunk)
                        break;
                    addr += chunk;
                    remaining -= chunk;
                }
                if(remaining)
                {
                    buffer_write(&out_buf, STRING("E0e"));
                    goto packet_complete;
                }

                addr = (uintptr_t)request.address;
                remaining = request.size;
                const size_t reply_start = out_buf.size;
                while(remaining)
                {
                    size_t chk = remaining;
                    if(chk > 64u)
                        chk = 64u;
                    char buf[64];
                    size_t copy_sz = safe_memcpy(
                        buf, (const void*)addr, chk);
                    if(copy_sz < chk)
                    {
                        /* Discard any earlier hexadecimal chunks. A late
                         * source fault must produce one error reply, never a
                         * syntactically valid-looking short memory reply. */
                        out_buf.size = reply_start;
                        buffer_write(&out_buf, STRING("E0e"));
                        break;
                    }
                    write_hex(buf, copy_sz);
                    addr += chk;
                    remaining -= chk;
                }
            }
        }
        else if(sz && pkt[0] == 'G')
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
                    struct uvdb_rsp_core_registers core;
                    if(uvdb_rsp_parse_core_register_packet(
                           pkt, sz, 0, &core) < 0)
                        buffer_write(&out_buf, STRING("E01"));
                    else
                    {
                        apply_exception_thread_registers(ctx, &core);
                        buffer_write(&out_buf, STRING("OK"));
                    }
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
        else if(sz && pkt[0] == 'M')
        {
            struct uvdb_rsp_memory_request request;
            const size_t maximum_write = (uvdb_max_buffer - 4u) / 2u;
            if(uvdb_rsp_parse_memory_write_packet(
                   pkt, sz, maximum_write, &request) < 0)
                buffer_write(&out_buf, STRING("E01"));
            else
            {
                if(uvdb_begin_stopped_operation() < 0)
                    return;
                uintptr_t address = (uintptr_t)request.address;
                size_t remaining = request.size;
                char test[64];
                /* Validate the complete destination span before changing a
                 * byte. With all application peers stopped, its mappings
                 * cannot disappear between this pass and the copy pass. */
                while(remaining)
                {
                    size_t chunk = remaining > sizeof(test)
                        ? sizeof(test) : remaining;
                    if(safe_memcpy(test, (const void*)address, chunk) !=
                       chunk)
                        break;
                    address += chunk;
                    remaining -= chunk;
                }

                if(remaining)
                    buffer_write(&out_buf, STRING("E0e"));
                else
                {
                    address = (uintptr_t)request.address;
                    remaining = request.size;
                    size_t data_offset = 0;
                    while(remaining)
                    {
                        size_t chunk = remaining > 64u ? 64u : remaining;
                        char decoded[64];
                        if(uvdb_rsp_decode_hex_bytes(
                               decoded, chunk,
                               request.data + data_offset * 2u,
                               chunk * 2u) < 0)
                        {
                            remaining = SIZE_MAX;
                            break;
                        }
                        if(kuKernelCpuUnrestrictedMemcpy(
                               (void*)address, decoded, chunk) < 0)
                        {
                            remaining = SIZE_MAX;
                            break;
                        }
                        kuKernelFlushCaches((void*)address, chunk);
                        char verified[64];
                        if(safe_memcpy(verified, (const void*)address,
                                       chunk) != chunk ||
                           memcmp(verified, decoded, chunk) != 0)
                        {
                            remaining = SIZE_MAX;
                            break;
                        }
                        address += chunk;
                        data_offset += chunk;
                        remaining -= chunk;
                    }
                    buffer_write(&out_buf,
                                 remaining ? "E0e" : "OK",
                                 remaining ? 3u : 2u);
                }
                uvdb_end_stopped_operation();
            }
        }
        else if(STARTSWITH("Z0,") || STARTSWITH("z0,"))
        {
            int insert = 0;
            uint32_t parsed_address = 0;
            size_t kind = 0;
            if(uvdb_rsp_parse_software_breakpoint_packet(
                   pkt, sz, &insert, &parsed_address, &kind) < 0)
                buffer_write(&out_buf, STRING("E01"));
            else
            {
                if(uvdb_begin_stopped_operation() < 0)
                    return;
                uintptr_t address = (uintptr_t)parsed_address;
                int result = insert
                    ? breakpoint_insert(address, kind)
                    : breakpoint_remove(address);
                if(result == UVDB_BREAKPOINT_PATCH_ERROR_RESTORE_PENDING)
                {
                    uvdb_fail_stopped_client_owned();
                    return;
                }
                buffer_write(&out_buf, result < 0 ? "E16" : "OK",
                             result < 0 ? 3u : 2u);
                uvdb_end_stopped_operation();
            }
        }
        else if(IS("k"))
            _sceKernelExitProcessForUser(1);
        else if(IS("D"))
        {
            if(uvdb_begin_stopped_operation() < 0)
                return;
            /* Code must be restored before releasing the all-stop boundary. */
            if(breakpoint_remove_all() < 0)
            {
                uvdb_fail_stopped_client_owned();
                return;
            }
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
#ifndef UVDB_KERNEL_THREAD_CONTROL
            /* GDB names the stopped thread with positive Hc while stepping over
             * a software breakpoint. In a cooperative library-only process
             * with exactly that one visible application thread, process scope
             * is equivalent and is the only truthful execution model: there is
             * no kernel lease with which to promise selected-thread isolation.
             * Keep multi-thread and address-override requests fail-closed. */
            if(!invalid && !has_address && stepping &&
               plan.scope == UVDB_RESUME_SCOPE_STOPPED_THREAD &&
               uvdb_inventory.count == 1 &&
               uvdb_inventory.ids[0] == uvdb_exception_thread &&
               plan.step_thread == uvdb_exception_thread)
                plan.scope = UVDB_RESUME_SCOPE_PROCESS;
#endif

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
            if(uvdb_finish_resume_packet(pkt) < 0)
                uvdb_fail_stopped_client();
            return; // no breakpoint cleanup; persistent points remain armed
        }
        else if(STARTSWITH("F"))
        {
            struct uvdb_rsp_fileio_result fileio_result;
            if(uvdb_rsp_parse_fileio_packet(
                   pkt, sz, &fileio_result) < 0)
            {
                buffer_write(&out_buf, STRING("E01"));
                goto packet_complete;
            }
            struct uvdb_fileio_transition transition;
            if(uvdb_fileio_transition_decide(
                   &fileio_result, fileio_context, &transition) < 0)
            {
                buffer_write(&out_buf, STRING("E01"));
                goto packet_complete;
            }
            if(transition.action == UVDB_FILEIO_ACTION_FAIL_CLOSED)
            {
                /* uvdb_remote_syscall has only a zero-initialized synthetic
                 * register context and has not published an all-stop. A fake
                 * T02 would let GDB inspect or mutate running peers through
                 * invalid registers. Consume the reply, sever this protocol
                 * generation, and return failure without claiming a stop. */
                if(out_buf.size)
                    out_buf.size--;
                discard_packet(pkt, sz);
                uvdb_note_io_failure();
                uvdb_close_socket(&uvdb_socket);
                uvdb_state = UVDB_STATE_ERROR;
                return;
            }
            if(uvdb_begin_stopped_operation() < 0)
                return;
            ctx->r0 = transition.result;
            if(transition.action ==
               UVDB_FILEIO_ACTION_REPORT_INTERRUPT)
            {
                /* A literal File-I/O Ctrl-C leaves the target stopped. Remove
                 * the empty response frame opened for `F`, consume the reply,
                 * and emit exactly one T02. Continuing this loop, rather than
                 * synthesizing `?` and returning, is the no-resume transition. */
                uvdb_end_stopped_operation();
                if(!out_buf.size)
                {
                    uvdb_fail_stopped_client();
                    return;
                }
                out_buf.size--;
                discard_packet(pkt, sz);
                stop_signal = transition.stop_signal;
                if(send_stop_reply(transition.stop_signal) < 0)
                {
                    uvdb_fail_stopped_client();
                    return;
                }
                continue;
            }
            //see above for explanation what this does
            memcpy(pkt, "?#3f", 4);
            for(size_t i = 1; i < sz; i++)
                pkt[i+3] = 0;
            if(uvdb_rsp_request_lifetime_release(
                   &uvdb_request_lifetime) < 0)
            {
                uvdb_note_io_failure();
                uvdb_fail_stopped_client_owned();
                return;
            }
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
packet_complete:
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

#ifndef UVDB_HOST_INTEGRATION_TEST
static __attribute__((naked)) void uvdb_trap_pc(void)
{
    asm volatile("udf #0");
}
#endif

static uint32_t uvdb_trap_address(void)
{
#ifdef UVDB_HOST_INTEGRATION_TEST
    return UINT32_C(0x7f00d00d);
#else
    return (uint32_t)(uintptr_t)uvdb_trap_pc;
#endif
}

#ifdef UVDB_MONITOR_DISPLAY
/* Unlike uvdb_enter(), this cannot fall back into accept if the peer vanished
 * while the post-accept display sample was being collected. */
#ifdef UVDB_HOST_INTEGRATION_TEST
static void uvdb_monitor_trigger_initial_stop(void)
{
}
#else
static __attribute__((naked)) void uvdb_monitor_trigger_initial_stop(void)
{
    asm volatile(
        "mov r0, lr\n"
        "b uvdb_trap_pc\n"
    );
}
#endif
#endif

#ifdef UVDB_EXPERIMENTAL_NESTED_FAULT_EXIT
/* KuBridge does not expose its default bootstrap handler when the previous
 * user callback is NULL. This opt-in trampoline is therefore the only bounded
 * fallback for a fault raised inside the debugger itself: abandon the nested
 * exception stack and terminate the target from ordinary user context. It is
 * intentionally disabled until a dedicated hardware gate proves exception
 * return and process-exit behavior on each supported firmware. */
static void uvdb_nested_fault_exit(void)
{
    _sceKernelExitProcessForUser(1);
    for(;;)
        sceKernelDelayThread(1000000);
}
#endif

static int uvdb_chain_previous_exception_handler(
    KuKernelExceptionContext* ctx)
{
    if(!ctx || ctx->exceptionType >= UVDB_EXCEPTION_GUARD_TYPE_COUNT)
        return 0;
    KuKernelExceptionHandler previous = (KuKernelExceptionHandler)
        uvdb_exception_handlers_previous(&uvdb_handlers,
                                          ctx->exceptionType);
    if(!previous || previous == exception_handler ||
       uvdb_exception_guard_begin_chain(&uvdb_exception_guard) != 1)
        return 0;
    previous(ctx);
    uvdb_exception_guard_end_chain(&uvdb_exception_guard);
    return 1;
}

static int uvdb_exception_session_claimable(void)
{
    /* Exception context must not spin on the descriptor lifecycle lock: the
     * interrupted code may itself own it. This is only a conservative gate;
     * the normal packet path validates the descriptor generation later. */
    return __atomic_load_n(&uvdb_socket, __ATOMIC_ACQUIRE) >= 0 &&
        !__atomic_load_n(&uvdb_network_closing, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST) &&
        !__atomic_load_n(&uvdb_shutdown_pending, __ATOMIC_SEQ_CST);
}

static void uvdb_handle_unclaimed_exception(
    KuKernelExceptionContext* ctx)
{
    if(!uvdb_chain_previous_exception_handler(ctx))
    {
        uvdb_exception_guard_note_unhandled(&uvdb_exception_guard);
#ifdef UVDB_EXPERIMENTAL_NESTED_FAULT_EXIT
        ctx->pc = (uint32_t)(uintptr_t)uvdb_nested_fault_exit;
        ctx->SPSR &= ~UINT32_C(32);
#endif
    }
}

/* Fail an exception before global debugger-state ownership is available.
 * This path is deliberately lock-free: it may be running on the thread that
 * was interrupted while owning uvdb_lock. Retire protocol ownership first,
 * then offer the original context to the captured predecessor under the
 * existing single-chain guard. */
static void uvdb_fail_exception_without_state_lock(
    KuKernelExceptionContext* ctx,
    uint32_t exception_type,
    int guard_result,
    uint32_t protocol_owner,
    int owns_protocol)
{
    uvdb_note_io_failure();
    __atomic_store_n(&uvdb_state, UVDB_STATE_ERROR, __ATOMIC_RELEASE);
    if(owns_protocol)
        (void)uvdb_protocol_gate_release(
            &uvdb_protocol_gate, protocol_owner);
    uvdb_handle_unclaimed_exception(ctx);
    (void)uvdb_exception_guard_leave(
        &uvdb_exception_guard, exception_type, guard_result);
}

static void exception_handler(KuKernelExceptionContext* ctx)
{
    if(!ctx)
        return;
    const uint32_t exception_type = ctx->exceptionType;
    int guard_result = uvdb_exception_guard_enter(
        &uvdb_exception_guard, exception_type);
    if(guard_result == UVDB_EXCEPTION_GUARD_CLOSED)
    {
        /* Handler slots have already been restored, but a callback dispatched
         * through our former slot can arrive late. Captured predecessor tokens
         * remain immutable through quiescence, and this full callback is still
         * counted. With a NULL predecessor, returning re-dispatches the fault
         * through the now-restored kernel default rather than our old slot. */
        uvdb_handle_unclaimed_exception(ctx);
        (void)uvdb_exception_guard_leave(
            &uvdb_exception_guard, exception_type, guard_result);
        return;
    }
    if(guard_result == UVDB_EXCEPTION_GUARD_INVALID)
    {
        uvdb_note_io_failure();
        __atomic_store_n(&uvdb_state, UVDB_STATE_ERROR, __ATOMIC_RELEASE);
        return;
    }
    if(guard_result == UVDB_EXCEPTION_GUARD_NESTED)
    {
        /* Never spin on uvdb_lock from a fault raised by the debugger or a
         * simultaneous peer. A distinct prior user handler gets one bounded
         * chance to claim it. Recursive faults in that handler cannot chain
         * again because begin_chain is serialized. */
        uvdb_note_io_failure();
        __atomic_store_n(&uvdb_state, UVDB_STATE_ERROR, __ATOMIC_RELEASE);
        uvdb_handle_unclaimed_exception(ctx);
        (void)uvdb_exception_guard_leave(
            &uvdb_exception_guard, exception_type, guard_result);
        return;
    }

    SceUID exception_thread = sceKernelGetThreadId();
    /* A detached, closing, or not-yet-connected debugger has no protocol
     * peer that could claim this stop. Give the captured application handler
     * its one guarded opportunity before touching the exception context. */
    if(!uvdb_exception_session_claimable())
    {
        __atomic_store_n(&uvdb_state, UVDB_STATE_ERROR, __ATOMIC_RELEASE);
        uvdb_handle_unclaimed_exception(ctx);
        (void)uvdb_exception_guard_leave(
            &uvdb_exception_guard, exception_type, guard_result);
        return;
    }

    const uint32_t protocol_owner =
        uvdb_protocol_owner_for_thread(exception_thread);
    if(uvdb_protocol_gate_try_acquire(
           &uvdb_protocol_gate, protocol_owner) !=
       UVDB_PROTOCOL_GATE_ACQUIRED)
    {
        /* A remote File-I/O operation or simultaneous stopped handler already
         * owns the shared RSP buffers. Never wait from exception context. */
        uvdb_fail_exception_without_state_lock(
            ctx, exception_type, guard_result, protocol_owner, 0);
        return;
    }
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    uvdb_admission_diagnostic_record(
        UVDB_ADMISSION_EVENT_PROTOCOL_ACQUIRED,
        __atomic_load_n(
            &uvdb_admission_diagnostic_state.current_socket,
            __ATOMIC_ACQUIRE),
        __atomic_load_n(
            &uvdb_admission_diagnostic_state.current_generation,
            __ATOMIC_ACQUIRE),
        protocol_owner);
#endif

    /* Exception context must never spin on the global state lock. The fault
     * may have interrupted this exact thread inside a uvdb_lock critical
     * section, in which case waiting would self-deadlock permanently. It is
     * also unsafe to inspect breakpoint or controller state before ownership
     * is established. Fail closed and give the captured predecessor its one
     * guarded opportunity to handle the original context. */
    if(uvdb_try_lock_for_thread(exception_thread) !=
       UVDB_TRY_LOCK_ACQUIRED)
    {
        uvdb_fail_exception_without_state_lock(
            ctx, exception_type, guard_result, protocol_owner, 1);
        return;
    }

    int internal_controller = uvdb_is_controller_thread(exception_thread);
    __atomic_store_n(&uvdb_target_stopped, 1, __ATOMIC_SEQ_CST);
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    uvdb_admission_diagnostic_record(
        UVDB_ADMISSION_EVENT_TARGET_STOPPED,
        __atomic_load_n(
            &uvdb_admission_diagnostic_state.current_socket,
            __ATOMIC_ACQUIRE),
        __atomic_load_n(
            &uvdb_admission_diagnostic_state.current_generation,
            __ATOMIC_ACQUIRE),
        protocol_owner);
#endif
    int signal = SIGSEGV;
    if(ctx->exceptionType == KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION)
        signal = SIGILL;
    uint32_t pc = ctx->pc;
    if((ctx->SPSR & 32))
    {
        ctx->SPSR &= -33;
        pc |= 1;
    }
    int synthetic_trap = pc == uvdb_trap_address();
    if(synthetic_trap)
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
#ifdef UVDB_MONITOR_DISPLAY
    enum uvdb_monitor_display_stop_action display_stop_action =
        uvdb_monitor_display_stop_on_exception(
            &uvdb_monitor_display_stop,
            synthetic_trap && exception_thread == uvdb_server_thread);
    if(display_stop_action == UVDB_MONITOR_DISPLAY_STOP_IGNORE)
    {
        /* A real application fault already supplied this connection's first
         * stop while the server's deferred display trap was queued. */
        uvdb_note_target_running();
        uvdb_unlock();
        goto exception_done;
    }
#endif
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
            goto exception_done;
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
        goto exception_done;
    }
    if(uvdb_begin_stopped_operation() < 0)
    {
        uvdb_unlock();
        goto exception_done;
    }
    if(internal_controller)
    {
        if(!uvdb_inventory.count)
        {
            uvdb_fail_stopped_client_owned();
            uvdb_unlock();
            goto exception_done;
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
        goto exception_done;
    }
    /* All application threads are now stopped; restoring a temporary trap is
     * no longer racing a peer executing the same code page. */
    if(breakpoint_remove_temporary() < 0)
    {
        uvdb_fail_stopped_client_owned();
        uvdb_unlock();
        goto exception_done;
    }
    uvdb_end_stopped_operation();
    int reported_signal = async_stop ? SIGINT : signal;
    /* Every successful resume leaves one synthetic '?' packet in the receive
     * buffer. Let the common query path emit the sole stop reply, including
     * for an asynchronous Ctrl-C. Sending here as well would produce two T02
     * packets and leave a stale stop notification in a no-ack GDB session. */
    uvdb_main_loop(ctx, reported_signal,
                   UVDB_FILEIO_CONTEXT_REAL_STOP);
    uvdb_unlock();
exception_done:
    (void)uvdb_protocol_gate_release(
        &uvdb_protocol_gate, protocol_owner);
    (void)uvdb_exception_guard_leave(
        &uvdb_exception_guard, exception_type, guard_result);
}

int uvdb_remote_syscall(const char* name, int nargs, ...)
{
    KuKernelExceptionContext ctx = {};
    if(!name || nargs < 0)
        return -1;
    uvdb_lock();
    if(uvdb_socket < 0)
    {
        //someone attempted to do a remote syscall before the first uvdb_enter
        //do a uvdb_enter now to avoid confusing the code
        uvdb_unlock();
        uvdb_enter();
        uvdb_lock();
    }
    const uint32_t protocol_owner = uvdb_protocol_owner_for_thread(
        sceKernelGetThreadId());
    if(uvdb_protocol_gate_try_acquire(
           &uvdb_protocol_gate, protocol_owner) !=
       UVDB_PROTOCOL_GATE_ACQUIRED)
    {
        uvdb_unlock();
        return -1;
    }
    int syscall_result = -1;
    char* pkt;
    size_t sz = recv_packet(&pkt);
    if(uvdb_has_io_failure())
        goto syscall_done;
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
    if(send_packet() < 0)
        goto syscall_done;
    uvdb_main_loop(&ctx, 0, UVDB_FILEIO_CONTEXT_UNSTOPPED);
    if(!uvdb_has_io_failure())
        syscall_result = (int)ctx.r0;
syscall_done:
    (void)uvdb_protocol_gate_release(
        &uvdb_protocol_gate, protocol_owner);
    uvdb_unlock();
    return syscall_result;
}

static __attribute__((used)) uint64_t real_uvdb_enter(uintptr_t lr)
{
    uint64_t no_trap = (uint64_t)lr << 32 | lr;
    uint64_t trap = (uint64_t)uvdb_trap_address() << 32 | lr;
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
    if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
    {
        /* Shutdown owns the connected descriptor. Never synthesize a trap
         * merely because its fd has not yet been retired by the server. */
        uvdb_unlock();
        return no_trap;
    }
    if(uvdb_socket >= 0)
    {
        uvdb_unlock();
        return trap;
    }
    if(uvdb_listen_socket >= 0 ||
       __atomic_load_n(&uvdb_accept_active, __ATOMIC_ACQUIRE))
    {
        /* Another owner is already establishing this process-wide session.
         * It will deliver the synthetic stop after publishing the peer. */
        uvdb_unlock();
        return no_trap;
    }
    uvdb_clear_io_failure();
    in_buf.size = 0;
    out_buf.size = 0;
    uvdb_rsp_request_lifetime_init(&uvdb_request_lifetime);
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
    if(!uvdb_handlers.installed_mask)
    {
        /* A partial install that was rolled back can still have a callback
         * copied by KuBridge. Never erase its predecessor tokens or create a
         * second generation without a kernel dispatcher fence. */
        if(uvdb_handlers.ever_published_mask)
        {
            uvdb_state = UVDB_STATE_ERROR;
            uvdb_unlock();
            return no_trap;
        }
        if(!uvdb_handlers.self &&
           uvdb_exception_handlers_init(
               &uvdb_handlers,
               (uvdb_exception_handler_token)exception_handler) < 0)
        {
            uvdb_state = UVDB_STATE_ERROR;
            uvdb_unlock();
            return no_trap;
        }
        if(uvdb_exception_handlers_install(
               &uvdb_handlers, &uvdb_handler_backend, NULL) < 0)
        {
            uvdb_state = UVDB_STATE_ERROR;
            uvdb_unlock();
            return no_trap;
        }
    }
    if(uvdb_handlers.installed_mask !=
       ((UINT32_C(1) << UVDB_EXCEPTION_HANDLER_TYPE_COUNT) - 1u))
    {
        /* A failed predecessor restore is a retained ownership obligation,
         * not a partially usable handler set. Only teardown retries it. */
        uvdb_state = UVDB_STATE_ERROR;
        uvdb_unlock();
        return no_trap;
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
    uvdb_net_syscall_arg args[5] = {
        (uvdb_net_syscall_arg)listen_socket,
        SOL_SOCKET,
        SO_REUSEADDR,
        (uvdb_net_syscall_arg)&value,
        sizeof(value),
    };
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
#ifdef UVDB_ADMISSION_DIAGNOSTIC
    uvdb_admission_diagnostic_record(
        UVDB_ADMISSION_EVENT_LISTENER_READY, listen_socket, 0, 0);
#endif

    int accepted_socket = -1;
    for(;;)
    {
        __atomic_store_n(&uvdb_accept_active, 1, __ATOMIC_RELEASE);
        uvdb_unlock();
        accepted_socket =
            sceNetSyscallAccept(listen_socket, NULL, NULL);
        uvdb_lock();
        __atomic_store_n(&uvdb_accept_active, 0, __ATOMIC_RELEASE);

        if(accepted_socket < 0)
        {
            uvdb_close_socket(&uvdb_listen_socket);
            uvdb_state =
                __atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST)
                    ? UVDB_STATE_IDLE : UVDB_STATE_ERROR;
            uvdb_unlock();
            return no_trap;
        }
        if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
        {
            uvdb_close_socket(&accepted_socket);
            uvdb_close_socket(&uvdb_listen_socket);
            uvdb_state = UVDB_STATE_IDLE;
            uvdb_unlock();
            return no_trap;
        }
#ifdef UVDB_ADMISSION_DIAGNOSTIC
        uvdb_admission_diagnostic_begin_connection();
        uvdb_admission_diagnostic_record(
            UVDB_ADMISSION_EVENT_CANDIDATE_ACCEPTED,
            accepted_socket, 0, 0);
#endif
        if(uvdb_publish_socket(
               &uvdb_candidate_socket, accepted_socket) < 0)
        {
            uvdb_close_socket(&accepted_socket);
            uvdb_close_socket(&uvdb_listen_socket);
            uvdb_state = UVDB_STATE_ERROR;
            uvdb_unlock();
            return no_trap;
        }

        __atomic_store_n(&uvdb_accept_active, 1, __ATOMIC_RELEASE);
        uvdb_unlock();
        int admitted = uvdb_wait_for_gdb_admission(accepted_socket);
        uvdb_lock();
        __atomic_store_n(&uvdb_accept_active, 0, __ATOMIC_RELEASE);

        if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
        {
            uvdb_close_socket(&uvdb_candidate_socket);
            uvdb_close_socket(&uvdb_listen_socket);
            uvdb_state = UVDB_STATE_IDLE;
            uvdb_unlock();
            return no_trap;
        }
        if(admitted < 0)
        {
            uvdb_close_socket(&uvdb_candidate_socket);
            uvdb_close_socket(&uvdb_listen_socket);
            uvdb_state = UVDB_STATE_ERROR;
            uvdb_unlock();
            return no_trap;
        }
        if(!admitted)
        {
            uvdb_close_socket(&uvdb_candidate_socket);
            accepted_socket = -1;
            continue;
        }
        if(uvdb_promote_candidate_socket(accepted_socket) < 0)
        {
            uvdb_close_socket(&uvdb_candidate_socket);
            uvdb_close_socket(&uvdb_listen_socket);
            uvdb_state = UVDB_STATE_ERROR;
            uvdb_unlock();
            return no_trap;
        }
        break;
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
#ifdef UVDB_MONITOR_DISPLAY
    int defer_initial_display_stop = 0;
    /* Invalidate before the first monitor query can run. Only the private
     * server thread can safely sample SceDisplay between accept and stop. */
    uvdb_monitor_display_cache_generation = 0;
    if(sceKernelGetThreadId() == uvdb_server_thread)
    {
        int active_socket = -1;
        uint32_t active_generation = 0;
        uvdb_active_socket_snapshot(&active_socket, &active_generation);
        if(active_socket == accepted_socket && active_generation &&
           uvdb_monitor_display_stop_begin(
               &uvdb_monitor_display_stop, active_generation) == 0)
            defer_initial_display_stop = 1;
    }
#endif
    uvdb_unlock();
#ifdef UVDB_MONITOR_DISPLAY
    if(defer_initial_display_stop)
        return no_trap;
#endif
    return trap;
}

#ifdef UVDB_HOST_INTEGRATION_TEST
void uvdb_enter(void)
{
    (void)real_uvdb_enter(0);
}
#else
__attribute__((naked)) void uvdb_enter(void)
{
    asm volatile(
        "mov r0, lr\n"
        "bl real_uvdb_enter\n"
        "bx r1\n"
    );
}
#endif

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
    uvdb_net_syscall_arg send_args[6] = {
        (uvdb_net_syscall_arg)socket,
        (uvdb_net_syscall_arg)data,
        (uvdb_net_syscall_arg)size,
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
#ifdef UVDB_MONITOR_DISPLAY
    uvdb_lock();
    uvdb_monitor_display_stop_reset(&uvdb_monitor_display_stop);
    uvdb_monitor_display_cache_generation = 0;
    uvdb_unlock();
#endif
    int epoll_id = -1;
    int watched_socket = -1;
    uint32_t watched_generation = 0;
    unsigned int display_refresh_delay = 0;

    while(!__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
    {
        if(!display_refresh_delay)
        {
            uvdb_monitor_refresh_display_cache();
            display_refresh_delay = 250;
        }
        else
            --display_refresh_delay;

        if(uvdb_socket < 0)
        {
            uvdb_server_epoll_reset(&epoll_id, &watched_socket,
                                    &watched_generation);
            if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
                break;
            /* This disconnected sample remains generation 0 and therefore
             * unavailable to a later connection. The post-accept handoff
             * below publishes the first connection-tagged sample. */
            uvdb_monitor_refresh_display_cache();
            display_refresh_delay = 250;
            uvdb_enter();
            uvdb_monitor_finish_initial_display_stop();
            continue;
        }

        if(!uvdb_try_lock())
        {
            sceKernelDelayThread(1000);
            continue;
        }

        if(uvdb_socket < 0 ||
           __atomic_load_n(&uvdb_packet_io_active, __ATOMIC_ACQUIRE) ||
           !uvdb_protocol_gate_is_idle(&uvdb_protocol_gate) ||
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
            uvdb_net_syscall_arg peek_args[6] = {
                (uvdb_net_syscall_arg)active_socket,
                (uvdb_net_syscall_arg)&command,
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
                uvdb_net_syscall_arg recv_args[6] = {
                    (uvdb_net_syscall_arg)active_socket,
                    (uvdb_net_syscall_arg)&command,
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
                uvdb_net_syscall_arg recv_args[6] = {
                    (uvdb_net_syscall_arg)active_socket,
                    (uvdb_net_syscall_arg)&command,
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
            if(__atomic_load_n(&uvdb_server_stop, __ATOMIC_SEQ_CST))
            {
                /* stop_server already published cancellation. Do not turn
                 * its expected HUP into a synthetic exception at unchanged
                 * PC; retire the descriptor and let the join complete. */
                uvdb_lock();
                uvdb_close_socket(&uvdb_socket);
                if(uvdb_state != UVDB_STATE_ERROR)
                    uvdb_state = UVDB_STATE_IDLE;
                uvdb_unlock();
                break;
            }
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
    /* stop_server normally drains the prior callback before returning. Keep a
     * second gate here so callers recovering from an earlier bounded timeout
     * still cannot reopen protocol ownership during a late callback tail. */
    if(uvdb_prepare_protocol_restart() < 0)
    {
        uvdb_state = UVDB_STATE_ERROR;
        return -1;
    }

#ifdef UVDB_ADMISSION_DIAGNOSTIC
    uvdb_admission_diagnostic_reset();
#endif
    __atomic_store_n(&uvdb_server_stop, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&uvdb_network_closing, 0, __ATOMIC_RELEASE);
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

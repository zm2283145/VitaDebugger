#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UVDB_HOST_INTEGRATION_TEST 1
#include "../../src/uvdb.c"

enum {
    FAKE_SOCKET = 7,
    FAKE_THREAD = 0x44,
    FAKE_OTHER_THREAD = 0x45,
};

static int failures;
static unsigned char fake_receive[1024];
static size_t fake_receive_size;
static size_t fake_receive_offset;
static unsigned char fake_transmit[2048];
static size_t fake_transmit_size;
static int fake_framed_send_while_borrowed;
static SceUID fake_thread = FAKE_THREAD;
static unsigned int fake_delay_calls;
static int fake_release_guard_on_delay;
static uint32_t fake_guard_type;
static int fake_guard_entry;
static int fake_predecessor_calls;
static KuKernelExceptionContext fake_predecessor_context;
static void* fake_memblocks[8];
static unsigned int fake_lifecycle_sequence;
static unsigned int fake_shutdown_sequence;
static unsigned int fake_abort_sequence;
static unsigned int fake_join_sequence;

char __executable_start[1];

static void check(int condition, const char* name)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", name);
        ++failures;
    }
}

static void fake_predecessor(KuKernelExceptionContext* context)
{
    ++fake_predecessor_calls;
    if(context)
        fake_predecessor_context = *context;
}

static void reset_core(void)
{
    static char input_storage[1024];
    static char output_storage[2048];

    memset(input_storage, 0, sizeof(input_storage));
    memset(output_storage, 0, sizeof(output_storage));
    memset(fake_receive, 0, sizeof(fake_receive));
    memset(fake_transmit, 0, sizeof(fake_transmit));
    fake_receive_size = 0;
    fake_receive_offset = 0;
    fake_transmit_size = 0;
    fake_framed_send_while_borrowed = 0;
    fake_delay_calls = 0;
    fake_release_guard_on_delay = 0;
    fake_guard_type = 0;
    fake_guard_entry = UVDB_EXCEPTION_GUARD_INVALID;
    fake_predecessor_calls = 0;
    fake_lifecycle_sequence = 0;
    fake_shutdown_sequence = 0;
    fake_abort_sequence = 0;
    fake_join_sequence = 0;
    memset(&fake_predecessor_context, 0, sizeof(fake_predecessor_context));

    in_buf = (struct buffer){
        .memblock_uid = -1,
        .buf = input_storage,
        .cap = sizeof(input_storage),
    };
    out_buf = (struct buffer){
        .memblock_uid = -1,
        .buf = output_storage,
        .cap = sizeof(output_storage),
    };
    uvdb_socket = FAKE_SOCKET;
    uvdb_listen_socket = -1;
    uvdb_socket_generation = 1u;
    uvdb_lock_owner = 0;
    uvdb_lifecycle_lock_state = 0;
    uvdb_socket_lifecycle_lock_state = 0;
    uvdb_clear_io_failure();
    __atomic_store_n(&uvdb_packet_io_active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&uvdb_accept_active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&uvdb_network_closing, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&uvdb_server_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&uvdb_shutdown_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&uvdb_target_stopped, 1, __ATOMIC_RELEASE);
    uvdb_state = UVDB_STATE_CONNECTED;
    uvdb_rsp_request_lifetime_init(&uvdb_request_lifetime);
    uvdb_console_transport_init(&uvdb_console_transport);
    uvdb_protocol_gate_init(&uvdb_protocol_gate);
    uvdb_exception_guard_init(&uvdb_exception_guard);
    memset(&uvdb_handlers, 0, sizeof(uvdb_handlers));
    memset(uvdb_breakpoints, 0, sizeof(uvdb_breakpoints));
    memset(uvdb_threads, 0, sizeof(uvdb_threads));
    uvdb_threads[0].id = FAKE_THREAD;
    uvdb_threads[0].active = 1;
    memcpy(uvdb_threads[0].name, "fixture", sizeof("fixture"));
    uvdb_thread_inventory_reset(&uvdb_inventory);
    uvdb_thread_selection_reset(&uvdb_selection);
    uvdb_selection.stopped = FAKE_THREAD;
    uvdb_exception_thread = FAKE_THREAD;
    fake_thread = FAKE_THREAD;
}

static void queue_receive(const char* bytes)
{
    fake_receive_size = strlen(bytes);
    memcpy(fake_receive, bytes, fake_receive_size);
}

static void test_real_status_query_ordering(void)
{
    reset_core();
    queue_receive("$?#3f+");
    uvdb_lock();
    KuKernelExceptionContext context = {0};
    uvdb_main_loop(&context, SIGTRAP, UVDB_FILEIO_CONTEXT_REAL_STOP);
    check(!uvdb_rsp_request_lifetime_is_active(&uvdb_request_lifetime) &&
              !fake_framed_send_while_borrowed,
          "real main loop releases request before response ACK wait");
    check(fake_transmit_size > 1u && fake_transmit[0] == '+',
          "request ACK precedes framed stop reply");
    struct uvdb_rsp_frame frame = {0};
    check(uvdb_rsp_scan_frame(
              fake_transmit + 1u, fake_transmit_size - 1u,
              sizeof(fake_transmit), &frame) == UVDB_RSP_FRAME_COMPLETE &&
              frame.payload_size == strlen("T05thread:44;") &&
              !memcmp(fake_transmit + 1u + frame.payload_offset,
                      "T05thread:44;", frame.payload_size),
          "production main-loop status payload is intact");
    check(in_buf.size == 0u && uvdb_has_io_failure(),
          "main loop consumes request/ACK then fails closed on scripted EOF");
    uvdb_unlock();
}

static void test_real_corrupt_frame_recovery(void)
{
    reset_core();
    queue_receive("$m0,1#00$m0,1#fa");
    uvdb_lock();
    char* packet = NULL;
    size_t packet_size = recv_packet(&packet);
    check(packet_size == 4u && packet && !memcmp(packet, "m0,1", 4u),
          "production receiver resynchronizes to valid frame");
    check(fake_transmit_size == 2u && fake_transmit[0] == '-' &&
              fake_transmit[1] == '+',
          "ACK mode emits one NACK then ACK after recovery");
    discard_packet(packet, packet_size);
    check(!uvdb_rsp_request_lifetime_is_active(&uvdb_request_lifetime) &&
              !uvdb_has_io_failure(),
          "recovered request can be discarded normally");
    uvdb_unlock();

    reset_core();
    uvdb_console_transport.no_ack_mode = 1u;
    queue_receive("$m0,1#00$m0,1#fa");
    uvdb_lock();
    packet = NULL;
    packet_size = recv_packet(&packet);
    check(packet_size == 4u && fake_transmit_size == 0u,
          "no-ack production receiver suppresses corrupt-frame NACK");
    discard_packet(packet, packet_size);
    uvdb_unlock();
}

static void test_exception_lock_contention_handoff(void)
{
    reset_core();
    __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_RELEASE);
    KuKernelExceptionContext context = {
        .pc = UINT32_C(0x81001234),
        .SPSR = UINT32_C(0x60000010),
        .exceptionType = KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT,
        .FAR = UINT32_C(0xdeadbeef),
    };
    const KuKernelExceptionContext original_context = context;
    uvdb_handlers.self = (uvdb_exception_handler_token)(uintptr_t)
        exception_handler;
    uvdb_handlers.previous[KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT] =
        (uvdb_exception_handler_token)(uintptr_t)fake_predecessor;
    uvdb_lock();
    check(uvdb_try_lock_for_thread(FAKE_THREAD) == UVDB_TRY_LOCK_SELF,
          "owner-aware try-lock detects interrupted owner");
    exception_handler(&context);
    check(fake_predecessor_calls == 1 &&
              !memcmp(&context, &original_context, sizeof(context)) &&
              !memcmp(&fake_predecessor_context, &original_context,
                      sizeof(original_context)),
          "lock contention hands original context to predecessor once");
    check(uvdb_protocol_gate_is_idle(&uvdb_protocol_gate) &&
              uvdb_exception_guard_is_idle(&uvdb_exception_guard) &&
              uvdb_lock_owner == uvdb_lock_owner_token(FAKE_THREAD),
          "handoff retires gate and guard without unlocking interrupted owner");
    check(uvdb_state == UVDB_STATE_ERROR && uvdb_has_io_failure() &&
              __atomic_load_n(&uvdb_target_stopped, __ATOMIC_ACQUIRE) == 0,
          "lock-contention failure is explicit and does not invent a new stop");
    uvdb_unlock();

    uvdb_lock_owner = uvdb_lock_owner_token(FAKE_OTHER_THREAD);
    check(uvdb_try_lock_for_thread(FAKE_THREAD) == UVDB_TRY_LOCK_BUSY,
          "owner-aware try-lock distinguishes foreign contention");
    uvdb_unlock();
    check(uvdb_lock_owner == uvdb_lock_owner_token(FAKE_OTHER_THREAD),
          "mismatched unlock cannot clear a foreign owner's lock");
    uvdb_lock_owner = 0;

    reset_core();
    __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_RELEASE);
    uvdb_handlers.self = (uvdb_exception_handler_token)(uintptr_t)
        exception_handler;
    uvdb_handlers.previous[KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT] =
        (uvdb_exception_handler_token)(uintptr_t)fake_predecessor;
    const uint32_t other_owner = uvdb_protocol_owner_for_thread(
        FAKE_OTHER_THREAD);
    check(uvdb_protocol_gate_try_acquire(
              &uvdb_protocol_gate, other_owner) ==
              UVDB_PROTOCOL_GATE_ACQUIRED,
          "foreign protocol owner fixture is established");
    exception_handler(&context);
    check(fake_predecessor_calls == 1 &&
              uvdb_exception_guard_is_idle(&uvdb_exception_guard) &&
              __atomic_load_n(&uvdb_target_stopped,
                              __ATOMIC_ACQUIRE) == 0,
          "real handler fails protocol contention to predecessor once");
    check(uvdb_protocol_gate_release(
              &uvdb_protocol_gate, other_owner) == 0,
          "contended protocol owner remains intact for its real owner");
}

static void test_stop_start_exception_quiescence(void)
{
    reset_core();
    uvdb_server_thread = -1;
    fake_guard_type = KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT;
    fake_guard_entry = uvdb_exception_guard_enter(
        &uvdb_exception_guard, fake_guard_type);
    fake_release_guard_on_delay = 1;
    check(fake_guard_entry == UVDB_EXCEPTION_GUARD_PRIMARY,
          "lifecycle fixture enters exception tail");
    check(uvdb_stop_server() == 0 && fake_delay_calls > 0u &&
              uvdb_exception_guard_is_idle(&uvdb_exception_guard) &&
              uvdb_protocol_gate_is_closing(&uvdb_protocol_gate) &&
              uvdb_state == UVDB_STATE_IDLE,
          "real stop lifecycle drains exception guard before returning");

    fake_delay_calls = 0;
    fake_release_guard_on_delay = 0;
    uvdb_server_thread = -1;
    check(uvdb_start_server() == 0 &&
              !uvdb_protocol_gate_is_closing(&uvdb_protocol_gate),
          "real start lifecycle reopens protocol only after guard idle");
    int next_entry = uvdb_exception_guard_enter(
        &uvdb_exception_guard, KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT);
    check(next_entry == UVDB_EXCEPTION_GUARD_PRIMARY,
          "first callback after immediate restart is primary");
    check(uvdb_exception_guard_leave(
              &uvdb_exception_guard,
              KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, next_entry) == 0,
          "post-restart callback lifetime drains normally");
    fake_lifecycle_sequence = 0;
    fake_shutdown_sequence = 0;
    fake_abort_sequence = 0;
    fake_join_sequence = 0;
    uvdb_socket = FAKE_SOCKET;
    uvdb_listen_socket = 8;
    uvdb_state = UVDB_STATE_CONNECTED;
    check(uvdb_stop_server() == 0,
          "connected fake-server stop completes");
    check(fake_shutdown_sequence > 0u,
          "connected stop requests socket shutdown");
    check(fake_abort_sequence > fake_shutdown_sequence,
          "connected stop aborts accept after socket shutdown");
    check(fake_join_sequence > fake_abort_sequence,
          "connected stop joins fake server after cancellation");
    check(uvdb_socket < 0 && uvdb_listen_socket < 0 &&
              uvdb_state == UVDB_STATE_IDLE,
          "connected stop retires descriptors and normalizes IDLE");

    reset_core();
    uvdb_protocol_gate_close(&uvdb_protocol_gate);
    fake_guard_type = KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT;
    fake_guard_entry = uvdb_exception_guard_enter(
        &uvdb_exception_guard, fake_guard_type);
    check(uvdb_prepare_protocol_restart() < 0 &&
              fake_delay_calls == 5000u &&
              uvdb_protocol_gate_is_closing(&uvdb_protocol_gate),
          "restart timeout leaves the protocol gate closed");
    check(uvdb_exception_guard_leave(
              &uvdb_exception_guard, fake_guard_type,
              fake_guard_entry) == 0,
          "timed-out lifecycle fixture can be drained explicitly");

    reset_core();
    uvdb_server_thread = -1;
    uvdb_breakpoints[0].patch.state =
        UVDB_BREAKPOINT_PATCH_RESTORE_PENDING;
    check(uvdb_stop_server() < 0 && uvdb_state == UVDB_STATE_ERROR,
          "stop cannot normalize IDLE over a retained patch obligation");
}

static void test_shutdown_does_not_synthesize_trap(void)
{
    reset_core();
    __atomic_store_n(&uvdb_server_stop, 1, __ATOMIC_RELEASE);
    const uintptr_t resume = UINT32_C(0x81005678);
    const uint64_t expected = (uint64_t)resume << 32 | resume;
    check(real_uvdb_enter(resume) == expected &&
              uvdb_socket == FAKE_SOCKET &&
              uvdb_exception_guard_is_idle(&uvdb_exception_guard),
          "connected shutdown returns without synthesizing an exception");
}

int main(void)
{
    test_real_status_query_ordering();
    test_real_corrupt_frame_recovery();
    test_exception_lock_contention_handoff();
    test_stop_start_exception_quiescence();
    test_shutdown_does_not_synthesize_trap();
    if(failures)
        return 1;
    puts("PASS: integrated uvdb.c protocol, exception, and lifecycle ordering");
    return 0;
}

SceUID sceKernelAllocMemBlock(
    const char* name, int type, size_t size, void* options)
{
    (void)name;
    (void)type;
    (void)options;
    for(SceUID uid = 1; uid < (SceUID)(sizeof(fake_memblocks) /
                                      sizeof(fake_memblocks[0])); ++uid)
        if(!fake_memblocks[uid])
        {
            fake_memblocks[uid] = malloc(size);
            return fake_memblocks[uid] ? uid : -1;
        }
    return -1;
}

int sceKernelGetMemBlockBase(SceUID uid, void** base)
{
    if(!base || uid <= 0 || uid >= (SceUID)(sizeof(fake_memblocks) /
                                             sizeof(fake_memblocks[0])) ||
       !fake_memblocks[uid])
        return -1;
    *base = fake_memblocks[uid];
    return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    if(uid <= 0 || uid >= (SceUID)(sizeof(fake_memblocks) /
                                    sizeof(fake_memblocks[0])))
        return -1;
    free(fake_memblocks[uid]);
    fake_memblocks[uid] = NULL;
    return 0;
}

int sceKernelDelayThread(unsigned int microseconds)
{
    (void)microseconds;
    ++fake_delay_calls;
    if(fake_release_guard_on_delay &&
       !uvdb_exception_guard_is_idle(&uvdb_exception_guard))
    {
        fake_release_guard_on_delay = 0;
        (void)uvdb_exception_guard_leave(
            &uvdb_exception_guard, fake_guard_type, fake_guard_entry);
    }
    return 0;
}

SceUID sceKernelGetThreadId(void)
{
    return fake_thread;
}

SceUID sceKernelCreateMsgPipe(
    const char* name, int attributes, int unknown, size_t size,
    void* options)
{
    (void)name; (void)attributes; (void)unknown; (void)size; (void)options;
    return 80;
}

int sceKernelDeleteMsgPipe(SceUID uid) { (void)uid; return 0; }
SceUID sceKernelCreateThread(
    const char* name, int (*entry)(SceSize, void*), int priority,
    size_t stack_size, unsigned int attributes, int affinity,
    void* options)
{
    (void)name; (void)entry; (void)priority; (void)stack_size;
    (void)attributes; (void)affinity; (void)options;
    return 90;
}
int sceKernelStartThread(SceUID uid, SceSize args, void* argp)
{ (void)uid; (void)args; (void)argp; return 0; }
int sceKernelWaitThreadEnd(SceUID uid, int* status, void* timeout)
{
    (void)uid; (void)status; (void)timeout;
    fake_join_sequence = ++fake_lifecycle_sequence;
    return 0;
}
int sceKernelDeleteThread(SceUID uid) { (void)uid; return 0; }
SceUID sceKernelGetModuleIdByAddr(const void* address)
{ (void)address; return -1; }
int sceKernelGetModuleInfo(SceUID uid, SceKernelModuleInfo* info)
{ (void)uid; (void)info; return -1; }
int sceKernelGetModuleList(int flags, SceUID* modules, SceSize* count)
{ (void)flags; (void)modules; if(count) *count = 0; return 0; }

ssize_t sceNetSyscallRecvfrom(void* arguments)
{
    uvdb_net_syscall_arg* args = arguments;
    if(!args || (int)args[0] != FAKE_SOCKET ||
       fake_receive_offset >= fake_receive_size)
        return -1;
    size_t available = fake_receive_size - fake_receive_offset;
    size_t requested = (size_t)args[2];
    size_t amount = available < requested ? available : requested;
    memcpy((void*)(uintptr_t)args[1],
           fake_receive + fake_receive_offset, amount);
    fake_receive_offset += amount;
    return (ssize_t)amount;
}

ssize_t sceNetSyscallSendto(void* arguments)
{
    uvdb_net_syscall_arg* args = arguments;
    if(!args || (int)args[0] != FAKE_SOCKET)
        return -1;
    const unsigned char* source = (const unsigned char*)(uintptr_t)args[1];
    size_t size = (size_t)args[2];
    if(size && source[0] == '$' &&
       uvdb_rsp_request_lifetime_is_active(&uvdb_request_lifetime))
        ++fake_framed_send_while_borrowed;
    if(size > sizeof(fake_transmit) - fake_transmit_size)
        return -1;
    memcpy(fake_transmit + fake_transmit_size, source, size);
    fake_transmit_size += size;
    return (ssize_t)size;
}

int sceNetSyscallSocket(const char* name, int domain, int type, int protocol)
{ (void)name; (void)domain; (void)type; (void)protocol; return FAKE_SOCKET; }
int sceNetSyscallSetsockopt(void* arguments) { (void)arguments; return 0; }
int sceNetSyscallBind(int socket, const void* address, unsigned int size)
{ (void)socket; (void)address; (void)size; return 0; }
int sceNetSyscallListen(int socket, int backlog)
{ (void)socket; (void)backlog; return 0; }
int sceNetSyscallAccept(int socket, void* address, void* address_size)
{ (void)socket; (void)address; (void)address_size; return FAKE_SOCKET; }
int sceNetSyscallShutdown(int socket, int how)
{
    (void)socket; (void)how;
    ++fake_lifecycle_sequence;
    if(!fake_shutdown_sequence)
        fake_shutdown_sequence = fake_lifecycle_sequence;
    return 0;
}
int sceNetSyscallSocketAbort(int socket, int flags)
{
    (void)socket; (void)flags;
    fake_abort_sequence = ++fake_lifecycle_sequence;
    return 0;
}
int sceNetSyscallClose(int socket) { (void)socket; return 0; }
int* sceNetErrnoLoc(void) { static int error; return &error; }
int sceNetSend(int socket, const void* data, size_t size, int flags)
{ (void)socket; (void)data; (void)flags; return (int)size; }
int sceNetEpollCreate(const char* name, int flags)
{ (void)name; (void)flags; return 1; }
int sceNetEpollControl(
    int epoll, int operation, int socket, SceNetEpollEvent* event)
{ (void)epoll; (void)operation; (void)socket; (void)event; return 0; }
int sceNetEpollWait(
    int epoll, SceNetEpollEvent* events, int maximum, int timeout)
{ (void)epoll; (void)events; (void)maximum; (void)timeout; return 0; }
int sceNetEpollDestroy(int epoll) { (void)epoll; return 0; }

int kuKernelCpuUnrestrictedMemcpy(
    void* destination, const void* source, size_t size)
{ memcpy(destination, source, size); return 0; }
void kuKernelFlushCaches(const void* address, size_t size)
{ (void)address; (void)size; }
int kuKernelRegisterExceptionHandler(
    uint32_t exception_type, KuKernelExceptionHandler replacement,
    KuKernelExceptionHandler* previous,
    struct KuKernelExceptionHandlerOpt* options)
{
    (void)exception_type; (void)replacement; (void)options;
    if(previous) *previous = NULL;
    return 0;
}
void kuKernelReleaseExceptionHandler(uint32_t exception_type)
{ (void)exception_type; }

void _sceKernelExitProcessForUser(int status) { (void)status; }
int _sceKernelSendMsgPipeVector(
    SceUID uid, const SceKernelAddrPair* pairs, unsigned int count,
    uvdb_net_syscall_arg* rest)
{ (void)uid; (void)pairs; (void)count; (void)rest; return -1; }
int _sceKernelReceiveMsgPipeVector(
    SceUID uid, const SceKernelAddrPair* pairs, unsigned int count,
    uvdb_net_syscall_arg* rest)
{ (void)uid; (void)pairs; (void)count; (void)rest; return -1; }

int uvdb_stdio_is_internal_thread(int thread_id)
{ (void)thread_id; return 0; }
int uvdb_restore_stdio(void) { return 0; }
int uvdb_debugnet_stop(void) { return 0; }

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
static _Thread_local SceUID fake_thread = FAKE_THREAD;
static unsigned char fake_target_memory[256];
static unsigned char fake_pipe_data[64];
static size_t fake_pipe_size;
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
static unsigned int fake_join_timeout;
static int fake_wait_failures;
static int fake_delete_calls;
static unsigned char fake_probe_receive[64];
static size_t fake_probe_receive_size;
static size_t fake_probe_receive_offset;
static int fake_accept_calls;
static int fake_epoll_socket;
static int fake_probe_socket_closed;
static int fake_probe_socket_shutdown;
static int fake_connected_socket_closed;
static int fake_connected_socket_shutdown;
static int fake_block_receive;
static int fake_receive_blocked;
static int fake_release_receive;
static int fake_require_nonblocking_receive;
static int fake_nonblocking_receive_violation;
static int fake_receive_would_block_count;
static ssize_t fake_receive_would_block_result;
static int fake_peer_reset;
static int fake_block_send;
static int fake_send_blocked;
static int fake_release_send;
static int fake_require_nonblocking_send;
static int fake_nonblocking_send_violation;
static int fake_send_would_block_count;
static int fake_epoll_block;
static int fake_epoll_wait_blocked;
static int fake_epoll_release;
static unsigned int fake_epoll_events;
static int fake_server_main_exited;
static int fake_stress_running;
static int fake_stress_gate_violation;
static int fake_stress_protocol_entries;
static int fake_stress_fault_entries;
static int fake_stress_console_attempts;

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
    for(size_t i = 0; i < sizeof(fake_target_memory); ++i)
        fake_target_memory[i] = (unsigned char)i;
    memset(fake_pipe_data, 0, sizeof(fake_pipe_data));
    fake_pipe_size = 0;
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
    fake_join_timeout = 0;
    fake_wait_failures = 0;
    fake_delete_calls = 0;
    memset(fake_probe_receive, 0, sizeof(fake_probe_receive));
    fake_probe_receive_size = 0;
    fake_probe_receive_offset = 0;
    fake_accept_calls = 0;
    fake_epoll_socket = -1;
    fake_probe_socket_closed = 0;
    fake_probe_socket_shutdown = 0;
    fake_connected_socket_closed = 0;
    fake_connected_socket_shutdown = 0;
    fake_block_receive = 0;
    fake_receive_blocked = 0;
    fake_release_receive = 0;
    fake_require_nonblocking_receive = 0;
    fake_nonblocking_receive_violation = 0;
    fake_receive_would_block_count = 0;
    fake_receive_would_block_result = SCE_NET_ERROR_EAGAIN;
    fake_peer_reset = 0;
    fake_block_send = 0;
    fake_send_blocked = 0;
    fake_release_send = 0;
    fake_require_nonblocking_send = 0;
    fake_nonblocking_send_violation = 0;
    fake_send_would_block_count = 0;
    fake_epoll_block = 0;
    fake_epoll_wait_blocked = 0;
    fake_epoll_release = 0;
    fake_epoll_events = 0;
    fake_server_main_exited = 0;
    fake_stress_running = 0;
    fake_stress_gate_violation = 0;
    fake_stress_protocol_entries = 0;
    fake_stress_fault_entries = 0;
    fake_stress_console_attempts = 0;
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
    uvdb_candidate_socket = -1;
    uvdb_listen_socket = -1;
    uvdb_socket_generation = 1u;
    uvdb_pipe = 80;
    uvdb_server_thread = -1;
    uvdb_server_thread_ended = 0;
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
#ifdef UVDB_KERNEL_THREAD_CONTROL
    __atomic_store_n(&uvdb_lease_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&uvdb_stop_failed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(
        &uvdb_stop_owner, UVDB_STOP_OWNER_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&uvdb_stop_token, 1u, __ATOMIC_RELEASE);
    uvdb_lease_thread = -1;
    uvdb_lease_thread_ended = 0;
#endif
}

static void queue_receive(const char* bytes)
{
    fake_receive_size = strlen(bytes);
    memcpy(fake_receive, bytes, fake_receive_size);
}

static void queue_rsp_payload(
    const char* payload,
    size_t payload_size,
    int append_ack)
{
    fake_receive_size = 0;
    fake_receive_offset = 0;
    check(payload != NULL, "RSP fixture payload is present");
    if(!payload)
        return;
    unsigned int checksum = 0;
    check(payload_size + 4u + (size_t)append_ack <=
              sizeof(fake_receive),
          "RSP fixture fits fake receive storage");
    fake_receive[0] = '$';
    memcpy(fake_receive + 1u, payload, payload_size);
    for(size_t i = 0; i < payload_size; ++i)
        checksum += (unsigned char)payload[i];
    static const char digits[] = "0123456789abcdef";
    fake_receive[1u + payload_size] = '#';
    fake_receive[2u + payload_size] = digits[(checksum >> 4) & 0xfu];
    fake_receive[3u + payload_size] = digits[checksum & 0xfu];
    fake_receive_size = payload_size + 4u;
    if(append_ack)
        fake_receive[fake_receive_size++] = '+';
}

static void append_rsp_payload(
    const char* payload,
    size_t payload_size,
    int append_ack)
{
    check(payload != NULL && payload_size + 4u + (size_t)append_ack <=
              sizeof(fake_receive) - fake_receive_size,
          "RSP fixture fits fake receive storage");
    if(!payload ||
       payload_size + 4u + (size_t)append_ack >
           sizeof(fake_receive) - fake_receive_size)
        return;
    unsigned int checksum = 0;
    size_t start = fake_receive_size;
    fake_receive[start] = '$';
    memcpy(fake_receive + start + 1u, payload, payload_size);
    for(size_t i = 0; i < payload_size; ++i)
        checksum += (unsigned char)payload[i];
    static const char digits[] = "0123456789abcdef";
    fake_receive[start + 1u + payload_size] = '#';
    fake_receive[start + 2u + payload_size] =
        digits[(checksum >> 4) & 0xfu];
    fake_receive[start + 3u + payload_size] =
        digits[checksum & 0xfu];
    fake_receive_size += payload_size + 4u;
    if(append_ack)
        fake_receive[fake_receive_size++] = '+';
}

static int wait_for_atomic_value(const int* value, int expected)
{
    for(unsigned int attempt = 0; attempt < 2000u; ++attempt)
    {
        if(__atomic_load_n(value, __ATOMIC_ACQUIRE) == expected)
            return 0;
        usleep(1000);
    }
    return -1;
}

static int wait_for_atomic_nonzero(const int* value)
{
    for(unsigned int attempt = 0; attempt < 2000u; ++attempt)
    {
        if(__atomic_load_n(value, __ATOMIC_ACQUIRE) != 0)
            return 0;
        usleep(1000);
    }
    return -1;
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

static void test_server_join_timeout_is_bounded_and_retryable(void)
{
    reset_core();
    uvdb_server_thread = 90;
    uvdb_server_thread_ended = 0;
    fake_wait_failures = 1;

    check(uvdb_stop_server() < 0 &&
              fake_join_timeout == UVDB_THREAD_JOIN_TIMEOUT_US,
          "server stop bounds its worker join");
    check(uvdb_server_thread == 90 && !uvdb_server_thread_ended &&
              fake_delete_calls == 0,
          "join timeout retains the live worker handle");

    check(uvdb_stop_server() == 0 &&
              uvdb_server_thread < 0 && fake_delete_calls == 1,
          "later stop retries and completes retained worker cleanup");
}

static void test_non_rsp_probe_does_not_consume_session(void)
{
    reset_core();
    uvdb_socket = -1;
    uvdb_state = UVDB_STATE_IDLE;
    memcpy(fake_probe_receive, "GET / HTTP/1.0\r\n\r\n", 18);
    fake_probe_receive_size = 18;
    queue_receive("$?#3f");

    const uintptr_t resume = UINT32_C(0x8100789a);
    const uint64_t no_trap = (uint64_t)resume << 32 | resume;
    uint64_t result = real_uvdb_enter(resume);
    check(result != no_trap && fake_accept_calls == 2,
          "listener rejects a non-RSP probe then admits GDB");
    check(fake_probe_socket_closed && uvdb_candidate_socket < 0 &&
              uvdb_socket == FAKE_SOCKET &&
              uvdb_state == UVDB_STATE_CONNECTED,
          "probe closes without consuming the promoted debugger session");
    check(fake_receive_offset == 0,
          "admission leaves the first GDB packet queued for normal RSP");
}

static void test_silent_probe_timeout_is_bounded(void)
{
    reset_core();
    uvdb_socket = -1;
    check(uvdb_wait_for_gdb_admission(FAKE_SOCKET) == 0 &&
              fake_delay_calls == UVDB_GDB_ADMISSION_POLLS,
          "silent TCP probe expires at the fixed admission bound");

    reset_core();
    queue_receive("+$?#3f");
    check(uvdb_wait_for_gdb_admission(FAKE_SOCKET) == 1 &&
              fake_receive_offset == 0,
          "initial ACK prefix plus valid RSP is admitted without consumption");
}

static void test_candidate_socket_cleanup(void)
{
    reset_core();
    uvdb_socket = -1;
    uvdb_candidate_socket = 9;
    uvdb_listen_socket = 8;
    uvdb_server_thread = 90;
    uvdb_server_thread_ended = 0;
    uvdb_state = UVDB_STATE_LISTENING;

    check(uvdb_stop_server() == 0 &&
              fake_probe_socket_shutdown &&
              fake_probe_socket_closed &&
              uvdb_candidate_socket < 0 &&
              uvdb_listen_socket < 0,
          "server stop cancels and retires a pending admission socket");
}

static void test_production_frame_boundaries_and_escapes(void)
{
    reset_core();
    char maximum_payload[1020];
    memset(maximum_payload, 'q', sizeof(maximum_payload));
    queue_rsp_payload(maximum_payload, sizeof(maximum_payload), 0);
    uvdb_lock();
    char* packet = NULL;
    size_t packet_size = recv_packet(&packet);
    check(packet_size == sizeof(maximum_payload) &&
              packet && packet[0] == 'q' &&
              packet[packet_size - 1u] == 'q' &&
              fake_transmit_size == 1u && fake_transmit[0] == '+',
          "production receiver accepts the exact maximum RSP frame");
    discard_packet(packet, packet_size);
    check(!uvdb_rsp_request_lifetime_is_active(&uvdb_request_lifetime) &&
              in_buf.size == 0u && !uvdb_has_io_failure(),
          "maximum RSP frame releases its exact borrowed wire span");
    uvdb_unlock();

    reset_core();
    static const unsigned char escaped_frame[] = {
        '$', 'q', '}', 0x04, '}', 0x03, '#', '7', '2',
    };
    memcpy(fake_receive, escaped_frame, sizeof(escaped_frame));
    fake_receive_size = sizeof(escaped_frame);
    uvdb_lock();
    packet = NULL;
    packet_size = recv_packet(&packet);
    check(packet_size == 5u && packet &&
              !memcmp(packet, escaped_frame + 1u, packet_size),
          "production receiver retains escaped delimiters in one frame");
    discard_packet(packet, packet_size);
    check(in_buf.size == 0u && !uvdb_has_io_failure(),
          "escaped frame discard consumes its complete wire packet");
    uvdb_unlock();

    reset_core();
    uvdb_max_buffer = sizeof(fake_receive);
    memset(fake_receive, 'x', sizeof(fake_receive));
    fake_receive[0] = '$';
    fake_receive_size = sizeof(fake_receive);
    uvdb_lock();
    packet = NULL;
    check(recv_packet(&packet) == 0u && uvdb_has_io_failure() &&
              fake_transmit_size == 1u && fake_transmit[0] == '-',
          "production receiver rejects oversized unterminated input once");
    uvdb_unlock();
}

static void run_disconnect_during_command(
    const char* command,
    size_t command_size,
    const char* expected_reply_prefix,
    size_t expected_reply_prefix_size,
    size_t expected_reply_size,
    int check_r0,
    uint32_t expected_r0,
    int check_memory,
    unsigned char expected_memory,
    const char* name)
{
    reset_core();
    queue_rsp_payload(command, command_size, 0);
    KuKernelExceptionContext context = {
        .r0 = UINT32_C(0x11223344),
        .r1 = UINT32_C(0x55667788),
        .pc = UINT32_C(0x81001234),
        .SPSR = UINT32_C(0x60000010),
    };
    uvdb_lock();
    uvdb_main_loop(&context, SIGTRAP, UVDB_FILEIO_CONTEXT_REAL_STOP);
    struct uvdb_rsp_frame response = {0};
    int response_result = fake_transmit_size > 1u
        ? uvdb_rsp_scan_frame(
              fake_transmit + 1u, fake_transmit_size - 1u,
              sizeof(fake_transmit), &response)
        : UVDB_RSP_FRAME_INCOMPLETE;
    check(response_result == UVDB_RSP_FRAME_COMPLETE &&
              response.payload_size == expected_reply_size &&
              response.payload_size >= expected_reply_prefix_size &&
              !memcmp(
                  fake_transmit + 1u + response.payload_offset,
                  expected_reply_prefix, expected_reply_prefix_size),
          "disconnect fixture reaches its command-specific response");
    check((!check_r0 || context.r0 == expected_r0) &&
              (!check_memory ||
               fake_target_memory[0] == expected_memory),
          "disconnect fixture applies its command-specific mutation");
    check(uvdb_state == UVDB_STATE_ERROR && uvdb_socket < 0 &&
              uvdb_has_io_failure() &&
              !uvdb_rsp_request_lifetime_is_active(
                  &uvdb_request_lifetime) &&
              __atomic_load_n(&uvdb_target_stopped,
                              __ATOMIC_ACQUIRE) == 0,
          name);
    uvdb_unlock();
}

static void test_disconnect_command_matrix(void)
{
    run_disconnect_during_command(
        "m1000,4", strlen("m1000,4"),
        "00010203", strlen("00010203"), strlen("00010203"),
        0, 0, 0, 0,
        "disconnect during m fails closed and releases the target");
    run_disconnect_during_command(
        "M1000,1:aa", strlen("M1000,1:aa"),
        "OK", 2u, 2u, 0, 0, 1, 0xaa,
        "disconnect during M fails closed and releases the target");
    run_disconnect_during_command(
        "g", 1u, "44332211", 8u,
        UVDB_RSP_CORE_PACKET_HEX_SIZE, 0, 0, 0, 0,
        "disconnect during g fails closed and releases the target");
    run_disconnect_during_command(
        "p0", 2u, "44332211", 8u, 8u, 0, 0, 0, 0,
        "disconnect during p fails closed and releases the target");

    struct uvdb_rsp_core_registers registers = {0};
    registers.r[0] = UINT32_C(0x12345678);
    registers.r[15] = UINT32_C(0x81000000);
    registers.cpsr = UINT32_C(0x60000010);
    char encoded[UVDB_RSP_CORE_PACKET_HEX_SIZE];
    size_t encoded_size = 0;
    check(uvdb_rsp_encode_register_packet(
              encoded, sizeof(encoded), &registers, NULL, 0,
              &encoded_size) == 0,
          "encode disconnect G fixture");
    char command[1u + UVDB_RSP_CORE_PACKET_HEX_SIZE];
    command[0] = 'G';
    memcpy(command + 1u, encoded, encoded_size);
    run_disconnect_during_command(
        command, sizeof(command), "OK", 2u, 2u,
        1, UINT32_C(0x12345678), 0, 0,
        "disconnect during G fails closed and releases the target");
    run_disconnect_during_command(
        "P0=78563412", strlen("P0=78563412"),
        "OK", 2u, 2u, 1, UINT32_C(0x12345678), 0, 0,
        "disconnect during P fails closed and releases the target");
}

struct fake_exception_thread {
    KuKernelExceptionContext context;
};

static void* run_fake_exception_handler(void* opaque)
{
    struct fake_exception_thread* operation = opaque;
    fake_thread = FAKE_THREAD;
    exception_handler(&operation->context);
    return NULL;
}

static void queue_rst_wait_sequence(int no_ack)
{
    queue_rsp_payload("qSupported", strlen("qSupported"), 1);
    if(no_ack)
        append_rsp_payload(
            "QStartNoAckMode", strlen("QStartNoAckMode"), 1);
    append_rsp_payload("qOffsets", strlen("qOffsets"), !no_ack);
    append_rsp_payload("g", 1u, !no_ack);
    append_rsp_payload("p0", 2u, !no_ack);
}

static void queue_clean_reconnect_sequence(void)
{
    queue_rsp_payload("qSupported", strlen("qSupported"), 1);
    append_rsp_payload("D", 1u, 1);
}

static void test_stopped_rst_reopens_listener(void)
{
    reset_core();
    uvdb_socket = -1;
    uvdb_state = UVDB_STATE_IDLE;
    __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_RELEASE);

    const uintptr_t resume = UINT32_C(0x81005678);
    const uint64_t no_trap = (uint64_t)resume << 32 | resume;
    for(unsigned int cycle = 0; cycle < 100u; ++cycle)
    {
        const int no_ack = (cycle & 1u) != 0;
        queue_rst_wait_sequence(no_ack);
        fake_accept_calls = 1;
        fake_transmit_size = 0;
        fake_connected_socket_closed = 0;
        fake_connected_socket_shutdown = 0;
        fake_require_nonblocking_receive = 1;
        fake_nonblocking_receive_violation = 0;
        fake_receive_would_block_count = 0;
        fake_receive_would_block_result =
            no_ack ? -(ssize_t)SCE_NET_EAGAIN : SCE_NET_ERROR_EAGAIN;
        fake_peer_reset = 0;

        const uint32_t failed_generation = uvdb_socket_generation + 1u;
        check(real_uvdb_enter(resume) != no_trap &&
                  uvdb_socket == FAKE_SOCKET &&
                  uvdb_socket_generation == failed_generation,
              "RST cycle admits the stopped debugger generation");

        struct fake_exception_thread operation = {
            .context = {
                .r0 = (uint32_t)resume,
                .pc = (uint32_t)uvdb_trap_address(),
                .SPSR = UINT32_C(0x60000010),
                .exceptionType =
                    KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION,
            },
        };
        pthread_t stopped_thread;
        check(pthread_create(
                  &stopped_thread, NULL,
                  run_fake_exception_handler, &operation) == 0,
              "start stopped RST packet wait");
        check(wait_for_atomic_nonzero(
                  &fake_receive_would_block_count) == 0 &&
                  __atomic_load_n(&uvdb_packet_io_active,
                                  __ATOMIC_ACQUIRE) == 1 &&
                  !uvdb_protocol_gate_is_idle(&uvdb_protocol_gate) &&
                  uvdb_candidate_socket < 0 &&
                  uvdb_console_transport_no_ack(
                      &uvdb_console_transport) == no_ack,
              "stopped packet wait publishes nonblocking I/O and ownership");
        __atomic_store_n(&fake_peer_reset, 1, __ATOMIC_RELEASE);
        check(pthread_join(stopped_thread, NULL) == 0,
              "join stopped RST cleanup");
        check(!fake_nonblocking_receive_violation &&
                  uvdb_socket < 0 &&
                  uvdb_state == UVDB_STATE_ERROR &&
                  !__atomic_load_n(&uvdb_target_stopped,
                                   __ATOMIC_ACQUIRE) &&
                  !__atomic_load_n(&uvdb_packet_io_active,
                                   __ATOMIC_ACQUIRE) &&
                  uvdb_protocol_gate_is_idle(&uvdb_protocol_gate) &&
                  uvdb_exception_guard_is_idle(&uvdb_exception_guard) &&
                  !uvdb_console_transport_no_ack(
                      &uvdb_console_transport),
              "RST read error releases stopped state and protocol ownership");

        queue_clean_reconnect_sequence();
        fake_accept_calls = 1;
        fake_transmit_size = 0;
        fake_connected_socket_closed = 0;
        fake_connected_socket_shutdown = 0;
        fake_require_nonblocking_receive = 0;
        fake_peer_reset = 0;
        const uint32_t reconnect_generation =
            uvdb_socket_generation + 1u;
        check(real_uvdb_enter(resume) != no_trap &&
                  uvdb_socket == FAKE_SOCKET &&
                  uvdb_socket_generation == reconnect_generation,
              "RST recovery reopens and admits a new listener generation");
        KuKernelExceptionContext reconnect = {
            .r0 = (uint32_t)resume,
            .pc = (uint32_t)uvdb_trap_address(),
            .SPSR = UINT32_C(0x60000010),
            .exceptionType =
                KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION,
        };
        fake_thread = FAKE_THREAD;
        exception_handler(&reconnect);
        check(uvdb_socket < 0 &&
                  uvdb_state == UVDB_STATE_IDLE &&
                  !__atomic_load_n(&uvdb_target_stopped,
                                   __ATOMIC_ACQUIRE) &&
                  uvdb_protocol_gate_is_idle(&uvdb_protocol_gate) &&
                  uvdb_exception_guard_is_idle(&uvdb_exception_guard),
              "reconnected qSupported and detach complete cleanly");
    }
}

static void test_raw_would_block_classification(void)
{
    check(uvdb_raw_io_would_block(SCE_NET_ERROR_EAGAIN),
          "encoded EAGAIN remains retryable");
    check(uvdb_raw_io_would_block(-(ssize_t)SCE_NET_EAGAIN),
          "raw negative EAGAIN is retryable");
    check(uvdb_raw_io_would_block(-(ssize_t)SCE_NET_EWOULDBLOCK),
          "raw negative EWOULDBLOCK is retryable");
    check(!uvdb_raw_io_would_block(-1),
          "generic raw -1 remains fatal");
    check(!uvdb_raw_io_would_block(
              -(ssize_t)(SCE_NET_EAGAIN + 1)),
          "unrelated raw negative remains fatal");
}

struct fake_packet_io_thread {
    int send;
    int result;
};

static void* run_blocked_packet_io(void* opaque)
{
    struct fake_packet_io_thread* operation = opaque;
    fake_thread = FAKE_OTHER_THREAD;
    uint32_t owner = uvdb_protocol_owner_for_thread(fake_thread);
    operation->result = uvdb_protocol_gate_try_acquire(
        &uvdb_protocol_gate, owner);
    if(operation->result != UVDB_PROTOCOL_GATE_ACQUIRED)
        return NULL;

    uvdb_lock();
    if(operation->send)
    {
        buffer_write(&out_buf, "blocked send", strlen("blocked send"));
        buffer_flush(&out_buf);
    }
    else
    {
        char* destination = NULL;
        (void)buffer_poll(&in_buf, &destination);
    }
    uvdb_unlock();
    operation->result = uvdb_protocol_gate_release(
        &uvdb_protocol_gate, owner);
    return NULL;
}

static void test_connected_io_cancellation_and_exclusion(void)
{
    reset_core();
    fake_block_receive = 1;
    struct fake_packet_io_thread receive = {0};
    pthread_t receive_thread;
    check(pthread_create(
              &receive_thread, NULL, run_blocked_packet_io,
              &receive) == 0,
          "start blocked connected receive");
    check(wait_for_atomic_value(&fake_receive_blocked, 1) == 0 &&
              __atomic_load_n(&uvdb_packet_io_active,
                              __ATOMIC_ACQUIRE) == 1,
          "connected receive publishes its cancellation lifetime");
    check(uvdb_protocol_gate_try_acquire(
              &uvdb_protocol_gate,
              uvdb_protocol_owner_for_thread(FAKE_THREAD)) ==
              UVDB_PROTOCOL_GATE_BUSY,
          "blocked receive excludes a second protocol owner");
    int stop_result = uvdb_stop_server();
    check(stop_result == 0,
          "connected receive cancellation completes bounded shutdown");
    check(pthread_join(receive_thread, NULL) == 0 &&
              receive.result == 0 &&
              fake_connected_socket_shutdown &&
              __atomic_load_n(&uvdb_packet_io_active,
                              __ATOMIC_ACQUIRE) == 0 &&
              uvdb_protocol_gate_is_idle(&uvdb_protocol_gate),
          "receive cancellation drains I/O and protocol ownership");

    reset_core();
    fake_require_nonblocking_send = 1;
    struct fake_packet_io_thread send = {.send = 1};
    pthread_t send_thread;
    check(pthread_create(
              &send_thread, NULL, run_blocked_packet_io, &send) == 0,
          "start blocked connected send");
    check(wait_for_atomic_value(&fake_send_blocked, 1) == 0 &&
              __atomic_load_n(&uvdb_packet_io_active,
                              __ATOMIC_ACQUIRE) == 1,
          "connected send publishes its cancellation lifetime");
    check(uvdb_protocol_gate_try_acquire(
              &uvdb_protocol_gate,
              uvdb_protocol_owner_for_thread(FAKE_THREAD)) ==
              UVDB_PROTOCOL_GATE_BUSY,
          "blocked send excludes a second protocol owner");
    stop_result = uvdb_stop_server();
    check(stop_result == 0,
          "connected send cancellation completes bounded shutdown");
    check(pthread_join(send_thread, NULL) == 0 &&
              send.result == 0 &&
              fake_connected_socket_shutdown &&
              fake_send_would_block_count > 0 &&
              !fake_nonblocking_send_violation &&
              __atomic_load_n(&uvdb_packet_io_active,
                              __ATOMIC_ACQUIRE) == 0 &&
              uvdb_protocol_gate_is_idle(&uvdb_protocol_gate),
          "send cancellation drains I/O and protocol ownership");
}

static void* run_fake_server_main(void* unused)
{
    (void)unused;
    fake_thread = FAKE_OTHER_THREAD;
    (void)uvdb_server_main(0, NULL);
    __atomic_store_n(&fake_server_main_exited, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_connected_hup_during_shutdown(void)
{
    reset_core();
    __atomic_store_n(&uvdb_target_stopped, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&uvdb_server_stop, 0, __ATOMIC_RELEASE);
    uvdb_server_thread = FAKE_OTHER_THREAD;
    fake_epoll_block = 1;
    pthread_t server;
    check(pthread_create(&server, NULL, run_fake_server_main, NULL) == 0,
          "start connected HUP server fixture");
    check(wait_for_atomic_value(&fake_epoll_wait_blocked, 1) == 0,
          "server reaches connected epoll wait");
    __atomic_store_n(&uvdb_server_stop, 1, __ATOMIC_RELEASE);
    fake_epoll_events = SCE_NET_EPOLLHUP;
    __atomic_store_n(&fake_epoll_release, 1, __ATOMIC_RELEASE);
    check(pthread_join(server, NULL) == 0 &&
              fake_server_main_exited &&
              fake_connected_socket_closed &&
              uvdb_socket < 0 &&
              uvdb_state == UVDB_STATE_IDLE &&
              __atomic_load_n(&uvdb_target_stopped,
                              __ATOMIC_ACQUIRE) == 0 &&
              fake_accept_calls == 0,
          "intentional connected HUP retires without a synthetic trap");
    uvdb_server_thread = -1;
}

static int fake_stress_critical;

static void* run_stress_protocol_faults(void* opaque)
{
    const uintptr_t index = (uintptr_t)opaque;
    fake_thread = FAKE_OTHER_THREAD + (SceUID)index;
    const uint32_t owner = uvdb_protocol_owner_for_thread(fake_thread);
    while(__atomic_load_n(&fake_stress_running, __ATOMIC_ACQUIRE))
    {
        if(uvdb_protocol_gate_try_acquire(
               &uvdb_protocol_gate, owner) ==
           UVDB_PROTOCOL_GATE_ACQUIRED)
        {
            if(__atomic_add_fetch(
                   &fake_stress_critical, 1, __ATOMIC_ACQ_REL) != 1)
                __atomic_store_n(
                    &fake_stress_gate_violation, 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(
                &fake_stress_protocol_entries, 1, __ATOMIC_RELAXED);
            int entry = uvdb_exception_guard_enter(
                &uvdb_exception_guard,
                (uint32_t)(index % 3u));
            if(entry != UVDB_EXCEPTION_GUARD_INVALID)
            {
                __atomic_add_fetch(
                    &fake_stress_fault_entries, 1, __ATOMIC_RELAXED);
                (void)uvdb_exception_guard_leave(
                    &uvdb_exception_guard,
                    (uint32_t)(index % 3u), entry);
            }
            __atomic_sub_fetch(
                &fake_stress_critical, 1, __ATOMIC_ACQ_REL);
            (void)uvdb_protocol_gate_release(
                &uvdb_protocol_gate, owner);
        }
        else
            usleep(1);
    }
    return NULL;
}

static void* run_stress_console_pressure(void* unused)
{
    (void)unused;
    static const char message[] = "deterministic console pressure\n";
    while(__atomic_load_n(&fake_stress_running, __ATOMIC_ACQUIRE))
    {
        (void)uvdb_console_capture(message, sizeof(message) - 1u);
        __atomic_add_fetch(
            &fake_stress_console_attempts, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void test_deterministic_multithread_lifecycle_stress(void)
{
    reset_core();
    uvdb_socket = -1;
    uvdb_state = UVDB_STATE_IDLE;
    uvdb_server_thread = -1;
    check(uvdb_start_server() == 0,
          "start first stress server generation");
    __atomic_store_n(&fake_stress_running, 1, __ATOMIC_RELEASE);
    pthread_t protocol_workers[3];
    pthread_t console_worker;
    for(uintptr_t i = 0; i < 3u; ++i)
        check(pthread_create(
                  &protocol_workers[i], NULL,
                  run_stress_protocol_faults,
                  (void*)(i + 1u)) == 0,
              "start stress protocol/fault worker");
    check(pthread_create(
              &console_worker, NULL, run_stress_console_pressure,
              NULL) == 0,
          "start stress console worker");
    check(wait_for_atomic_nonzero(
              &fake_stress_protocol_entries) == 0 &&
              wait_for_atomic_nonzero(
                  &fake_stress_fault_entries) == 0 &&
              wait_for_atomic_nonzero(
                  &fake_stress_console_attempts) == 0,
          "stress workers make progress before reconnect cycling");

    for(unsigned int cycle = 0; cycle < 1000u; ++cycle)
    {
        check(uvdb_publish_socket(&uvdb_socket, FAKE_SOCKET) == 0,
              "stress publishes a fresh connected socket generation");
        uvdb_state = UVDB_STATE_CONNECTED;
        __atomic_store_n(
            &fake_connected_socket_shutdown, 0, __ATOMIC_RELEASE);
        fake_connected_socket_closed = 0;
        check(uvdb_console_transport_begin_connection(
                  &uvdb_console_transport) == UVDB_CONSOLE_READY &&
                  uvdb_console_transport_enable_no_ack(
                      &uvdb_console_transport) == UVDB_CONSOLE_READY,
              "stress opens a bounded console generation");
        check(uvdb_stop_server() == 0 &&
                  uvdb_socket < 0 &&
                  uvdb_state == UVDB_STATE_IDLE &&
                  fake_connected_socket_shutdown &&
                  fake_connected_socket_closed &&
                  uvdb_protocol_gate_is_idle(&uvdb_protocol_gate) &&
                  uvdb_protocol_gate_is_closing(&uvdb_protocol_gate),
              "stress shutdown cancels, joins, and retires one generation");
        if(cycle + 1u < 1000u)
            check(uvdb_start_server() == 0 &&
                      !uvdb_protocol_gate_is_closing(
                          &uvdb_protocol_gate),
                  "stress restart reopens a quiescent protocol generation");
        usleep(1);
    }

    __atomic_store_n(&fake_stress_running, 0, __ATOMIC_RELEASE);
    for(size_t i = 0; i < 3u; ++i)
        check(pthread_join(protocol_workers[i], NULL) == 0,
              "join stress protocol/fault worker");
    check(pthread_join(console_worker, NULL) == 0,
          "join stress console worker");
    struct uvdb_console_stats console_stats;
    check(uvdb_console_get_stats(&console_stats) == 0 &&
              console_stats.accepted_records > 0 &&
              console_stats.queued_records <= UVDB_CONSOLE_QUEUE_SLOTS &&
              console_stats.dropped_full_records +
                  console_stats.dropped_disconnected_records > 0,
          "stress console pressure remains bounded with explicit loss");
    check(uvdb_protocol_gate_is_idle(&uvdb_protocol_gate) &&
              uvdb_exception_guard_is_idle(&uvdb_exception_guard) &&
              !fake_stress_gate_violation &&
              fake_stress_protocol_entries > 0 &&
              fake_stress_fault_entries > 0 &&
              fake_stress_console_attempts > 0,
          "stress preserves exclusive ownership and drains fault/console work");
}

int main(void)
{
    test_real_status_query_ordering();
    test_real_corrupt_frame_recovery();
    test_exception_lock_contention_handoff();
    test_stop_start_exception_quiescence();
    test_shutdown_does_not_synthesize_trap();
    test_server_join_timeout_is_bounded_and_retryable();
    test_non_rsp_probe_does_not_consume_session();
    test_silent_probe_timeout_is_bounded();
    test_candidate_socket_cleanup();
    test_production_frame_boundaries_and_escapes();
    test_disconnect_command_matrix();
    test_raw_would_block_classification();
    test_stopped_rst_reopens_listener();
    test_connected_io_cancellation_and_exclusion();
    test_connected_hup_during_shutdown();
    test_deterministic_multithread_lifecycle_stress();
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
    ++fake_delay_calls;
    if(__atomic_load_n(&fake_receive_blocked, __ATOMIC_ACQUIRE) ||
       __atomic_load_n(&fake_send_blocked, __ATOMIC_ACQUIRE) ||
       __atomic_load_n(&fake_stress_running, __ATOMIC_ACQUIRE))
        usleep(microseconds > 1000u ? 1000u : microseconds);
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
    (void)uid; (void)status;
    fake_join_timeout = timeout ? *(unsigned int*)timeout : 0;
    fake_join_sequence = ++fake_lifecycle_sequence;
    if(fake_wait_failures > 0)
    {
        --fake_wait_failures;
        return -1;
    }
    return 0;
}
int sceKernelDeleteThread(SceUID uid)
{
    (void)uid;
    ++fake_delete_calls;
    return 0;
}
SceUID sceKernelGetModuleIdByAddr(const void* address)
{ (void)address; return -1; }
int sceKernelGetModuleInfo(SceUID uid, SceKernelModuleInfo* info)
{ (void)uid; (void)info; return -1; }
int sceKernelGetModuleList(int flags, SceUID* modules, SceSize* count)
{ (void)flags; (void)modules; if(count) *count = 0; return 0; }

ssize_t sceNetSyscallRecvfrom(void* arguments)
{
    uvdb_net_syscall_arg* args = arguments;
    if(!args)
        return -1;
    if((int)args[0] == FAKE_SOCKET &&
       !(args[3] & MSG_PEEK) &&
       __atomic_load_n(&fake_block_receive, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&fake_receive_blocked, 1, __ATOMIC_RELEASE);
        while(!__atomic_load_n(&fake_release_receive, __ATOMIC_ACQUIRE) &&
              !__atomic_load_n(&fake_connected_socket_shutdown,
                               __ATOMIC_ACQUIRE))
            usleep(1000);
        return -1;
    }
    unsigned char* source;
    size_t* offset;
    size_t source_size;
    if((int)args[0] == 9)
    {
        source = fake_probe_receive;
        offset = &fake_probe_receive_offset;
        source_size = fake_probe_receive_size;
    }
    else if((int)args[0] == FAKE_SOCKET)
    {
        source = fake_receive;
        offset = &fake_receive_offset;
        source_size = fake_receive_size;
    }
    else
        return -1;
    if(*offset >= source_size &&
       (int)args[0] == FAKE_SOCKET &&
       __atomic_load_n(
           &fake_require_nonblocking_receive, __ATOMIC_ACQUIRE))
    {
        if(!(args[3] & MSG_DONTWAIT))
        {
            __atomic_store_n(
                &fake_nonblocking_receive_violation, 1,
                __ATOMIC_RELEASE);
            return -1;
        }
        if(!__atomic_load_n(&fake_peer_reset, __ATOMIC_ACQUIRE))
        {
            __atomic_add_fetch(
                &fake_receive_would_block_count, 1,
                __ATOMIC_RELAXED);
            return fake_receive_would_block_result;
        }
        return -1;
    }
    if(*offset >= source_size)
        return -1;
    size_t available = source_size - *offset;
    size_t requested = (size_t)args[2];
    size_t amount = available < requested ? available : requested;
    memcpy((void*)(uintptr_t)args[1],
           source + *offset, amount);
    if(!(args[3] & MSG_PEEK))
        *offset += amount;
    return (ssize_t)amount;
}

ssize_t sceNetSyscallSendto(void* arguments)
{
    uvdb_net_syscall_arg* args = arguments;
    if(!args || (int)args[0] != FAKE_SOCKET)
        return -1;
    if(__atomic_load_n(
           &fake_require_nonblocking_send, __ATOMIC_ACQUIRE))
    {
        if(!(args[3] & MSG_DONTWAIT))
        {
            __atomic_store_n(
                &fake_nonblocking_send_violation, 1,
                __ATOMIC_RELEASE);
            return -1;
        }
        __atomic_store_n(&fake_send_blocked, 1, __ATOMIC_RELEASE);
        if(!__atomic_load_n(
               &fake_connected_socket_shutdown, __ATOMIC_ACQUIRE))
        {
            __atomic_add_fetch(
                &fake_send_would_block_count, 1,
                __ATOMIC_RELAXED);
            return SCE_NET_ERROR_EAGAIN;
        }
        return -1;
    }
    if(__atomic_load_n(&fake_block_send, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&fake_send_blocked, 1, __ATOMIC_RELEASE);
        while(!__atomic_load_n(&fake_release_send, __ATOMIC_ACQUIRE) &&
              !__atomic_load_n(&fake_connected_socket_shutdown,
                               __ATOMIC_ACQUIRE))
            usleep(1000);
        return -1;
    }
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
{ (void)name; (void)domain; (void)type; (void)protocol; return 8; }
int sceNetSyscallSetsockopt(void* arguments) { (void)arguments; return 0; }
int sceNetSyscallBind(int socket, const void* address, unsigned int size)
{ (void)socket; (void)address; (void)size; return 0; }
int sceNetSyscallListen(int socket, int backlog)
{ (void)socket; (void)backlog; return 0; }
int sceNetSyscallAccept(int socket, void* address, void* address_size)
{
    (void)socket; (void)address; (void)address_size;
    return fake_accept_calls++ == 0 ? 9 : FAKE_SOCKET;
}
int sceNetSyscallShutdown(int socket, int how)
{
    (void)socket; (void)how;
    if(socket == 9)
        fake_probe_socket_shutdown = 1;
    if(socket == FAKE_SOCKET)
        __atomic_store_n(
            &fake_connected_socket_shutdown, 1, __ATOMIC_RELEASE);
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
int sceNetSyscallClose(int socket)
{
    if(socket == 9)
        fake_probe_socket_closed = 1;
    if(socket == FAKE_SOCKET)
        fake_connected_socket_closed = 1;
    return 0;
}
int* sceNetErrnoLoc(void) { static int error; return &error; }
int sceNetSend(int socket, const void* data, size_t size, int flags)
{ (void)socket; (void)data; (void)flags; return (int)size; }
int sceNetEpollCreate(const char* name, int flags)
{ (void)name; (void)flags; return 1; }
int sceNetEpollControl(
    int epoll, int operation, int socket, SceNetEpollEvent* event)
{
    (void)epoll; (void)operation; (void)event;
    fake_epoll_socket = socket;
    return 0;
}
int sceNetEpollWait(
    int epoll, SceNetEpollEvent* events, int maximum, int timeout)
{
    (void)epoll; (void)maximum; (void)timeout;
    if(__atomic_load_n(&fake_epoll_block, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&fake_epoll_wait_blocked, 1, __ATOMIC_RELEASE);
        while(!__atomic_load_n(&fake_epoll_release, __ATOMIC_ACQUIRE))
            usleep(1000);
        events[0].events = fake_epoll_events;
        events[0].data.fd = fake_epoll_socket;
        return 1;
    }
    size_t available = 0;
    if(fake_epoll_socket == 9)
        available = fake_probe_receive_size - fake_probe_receive_offset;
    else if(fake_epoll_socket == FAKE_SOCKET)
        available = fake_receive_size - fake_receive_offset;
    if(!available)
        return 0;
    events[0].events = SCE_NET_EPOLLIN;
    events[0].data.fd = fake_epoll_socket;
    return 1;
}
int sceNetEpollDestroy(int epoll) { (void)epoll; return 0; }

int kuKernelCpuUnrestrictedMemcpy(
    void* destination, const void* source, size_t size)
{
    uintptr_t address = (uintptr_t)destination;
    if(size <= sizeof(fake_target_memory) &&
       address >= UINT32_C(0x1000) &&
       address - UINT32_C(0x1000) <=
           sizeof(fake_target_memory) - size)
        memcpy(fake_target_memory + address - UINT32_C(0x1000),
               source, size);
    else
        memcpy(destination, source, size);
    return 0;
}
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
{
    if(uid < 0 || !pairs || count != 1u || !rest ||
       pairs[0].length > sizeof(fake_pipe_data))
        return -1;
    uintptr_t address = (uintptr_t)pairs[0].addr;
    if(pairs[0].length > sizeof(fake_target_memory) ||
       address < UINT32_C(0x1000) ||
       address - UINT32_C(0x1000) >
           sizeof(fake_target_memory) - pairs[0].length)
        return -1;
    fake_pipe_size = pairs[0].length;
    memcpy(fake_pipe_data,
           fake_target_memory + address - UINT32_C(0x1000),
           fake_pipe_size);
    *(size_t*)(uintptr_t)rest[1] = fake_pipe_size;
    return 0;
}
int _sceKernelReceiveMsgPipeVector(
    SceUID uid, const SceKernelAddrPair* pairs, unsigned int count,
    uvdb_net_syscall_arg* rest)
{
    if(uid < 0 || !pairs || count != 1u || !rest ||
       pairs[0].length > fake_pipe_size)
        return -1;
    memcpy((void*)(uintptr_t)pairs[0].addr,
           fake_pipe_data, pairs[0].length);
    memmove(fake_pipe_data, fake_pipe_data + pairs[0].length,
            fake_pipe_size - pairs[0].length);
    fake_pipe_size -= pairs[0].length;
    *(size_t*)(uintptr_t)rest[1] = pairs[0].length;
    return 0;
}

#ifdef UVDB_KERNEL_THREAD_CONTROL
int vdKernelGetStatus(struct vd_kernel_status* status)
{
    if(!status)
        return -1;
    *status = (struct vd_kernel_status){
        .abi_version = VD_KERNEL_ABI_VERSION,
        .capabilities =
            VD_KERNEL_REQUIRED_THREAD_CONTROL_CAPABILITIES,
        .max_threads = VD_KERNEL_MAX_THREADS,
    };
    return 0;
}

int vdKernelBeginStop(
    unsigned int lease_ms,
    SceUID exempt_user_thread,
    struct vd_kernel_stop_result* stop_result)
{
    (void)lease_ms;
    (void)exempt_user_thread;
    if(!stop_result)
        return -1;
    *stop_result = (struct vd_kernel_stop_result){
        .token = 1u,
        .suspended_count = 1,
        .failed_thread = -1,
    };
    return 0;
}

int vdKernelRenewStop(unsigned int token, unsigned int lease_ms)
{
    return token == 1u && lease_ms == 2000u ? 0 : -1;
}

int vdKernelEndStop(unsigned int token, int* resumed_count)
{
    if(token != 1u)
        return -1;
    if(resumed_count)
        *resumed_count = 1;
    return 0;
}

int vdKernelGetThreadList(
    SceUID* ids,
    int capacity,
    int* copied_count,
    int* total_count)
{
    if(!ids || capacity < 1 || !copied_count || !total_count)
        return -1;
    ids[0] = FAKE_THREAD;
    *copied_count = 1;
    *total_count = 1;
    return 0;
}

int vdKernelGetThreadRegisters(
    unsigned int token,
    SceUID target_user_thread,
    struct vd_thread_registers* registers)
{
    if(token != 1u || target_user_thread != FAKE_THREAD || !registers)
        return -1;
    memset(registers, 0, sizeof(*registers));
    registers->entry[0].r[0] = UINT32_C(0x11223344);
    registers->entry[0].sp = UINT32_C(0x8100f000);
    registers->entry[0].lr = UINT32_C(0x81000100);
    registers->entry[0].pc = UINT32_C(0x81001234);
    registers->entry[0].cpsr = UINT32_C(0x60000010);
    return 0;
}
#endif

int uvdb_stdio_is_internal_thread(int thread_id)
{ (void)thread_id; return 0; }
int uvdb_restore_stdio(void) { return 0; }
int uvdb_debugnet_stop(void) { return 0; }

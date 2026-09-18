#include "uvdb_monitor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char* message)
{
    if(!condition)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static int contains(
    const char* haystack,
    size_t haystack_size,
    const char* needle)
{
    size_t needle_size = strlen(needle);
    if(needle_size > haystack_size)
        return 0;
    for(size_t i = 0; i <= haystack_size - needle_size; ++i)
        if(!memcmp(haystack + i, needle, needle_size))
            return 1;
    return 0;
}

static void check_parse(
    const char* packet,
    int expected_result,
    enum uvdb_monitor_command expected_command,
    const char* message)
{
    enum uvdb_monitor_command command = UVDB_MONITOR_COMMAND_MODULES;
    int result = uvdb_monitor_parse_qrcmd(
        packet, packet ? strlen(packet) : 0u, &command);
    check(result == expected_result &&
          (result != UVDB_MONITOR_PARSE_OK || command == expected_command),
          message);
}

static struct uvdb_monitor_snapshot sample_snapshot(void)
{
    static struct uvdb_monitor_stop_trace stop_traces[1];
    struct uvdb_monitor_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.status.state = UVDB_MONITOR_STATE_CONNECTED;
    snapshot.status.port = 1234;
    snapshot.status.packet_size = 0x3fffcu;
    snapshot.status.thread_count = 2;
    snapshot.status.software_breakpoint_count = 1;
    snapshot.status.stopped_thread = 0x40010003;
    snapshot.status.general_thread = 0;
    snapshot.status.resume_thread = -1;
    snapshot.status.exception_thread = 0x40010003;
    snapshot.status.stop_signal = 5;
    snapshot.status.target_stopped = 1;
    snapshot.status.no_ack_mode = 1;
    snapshot.status.kernel_compiled = 1;
    snapshot.status.kernel_status_available = 1;
    snapshot.status.kernel_compatible = 1;
    snapshot.status.kernel_abi = 0x0001000bu;
    snapshot.status.kernel_capabilities = 0x3fu;
    snapshot.status.kernel_max_threads = 64;
    snapshot.status.stop_session_active = 1;
    snapshot.status.vfp_reads_enabled = 1;
    snapshot.status.last_fault_available = 1;
    snapshot.status.last_fault_type = 2;
    snapshot.status.last_fault_status = 0x12u;
    snapshot.status.last_fault_address = 0x81001234u;
    snapshot.status.last_fault_pc = 0x81005678u;
    snapshot.status.stop_trace_count = 1u;
    snapshot.status.stop_trace_dropped = 2u;
    stop_traces[0] =
        (struct uvdb_monitor_stop_trace){
            .generation = 7u,
            .thread = 0x40010003,
            .raw_pc = 0x81005678u,
            .exception_type = 2,
            .handoff_owner = 0x40010002u,
            .handoff_wait_seq = 1u,
            .handoff_done_seq = 2u,
            .handoff_wait_attempts = 4u,
            .handoff_wait_result =
                UVDB_MONITOR_STOP_TRACE_WAIT_RELEASED,
            .guard_seq = 3u,
            .guard_result = 1,
            .session_seq = 4u,
            .session_claimable = 1,
            .protocol_seq = 5u,
            .protocol_result = 0,
            .lock_seq = 6u,
            .lock_result = 0,
            .publish_seq = 7u,
            .classified_pc = 0x81005678u,
            .signal = 5,
            .breakpoint_match = 1,
            .stop_begin_seq = 8u,
            .stop_begin_result = 0,
            .stopped_operation_seq = 9u,
            .stopped_operation_result = 0,
            .main_loop_seq = 10u,
            .packet_wait_seq = 11u,
            .packet_ready_seq = 12u,
            .status_query_seq = 13u,
            .reply_attempt_seq = 14u,
            .reply_socket_poll_seq = 15u,
            .reply_socket_wake_seq = 16u,
            .reply_socket_wake_result = 1,
            .reply_result_seq = 17u,
            .reply_result = 0,
            .exit_seq = 18u,
        };
    snapshot.status.stop_traces = stop_traces;
    return snapshot;
}

static int render(
    enum uvdb_monitor_command command,
    const struct uvdb_monitor_snapshot* snapshot,
    char* output,
    size_t capacity,
    size_t* size)
{
    memset(output, 0xcc, capacity);
    return uvdb_monitor_render(command, snapshot, output, capacity, size);
}

int main(void)
{
    struct uvdb_monitor_display_stop display_stop;
    memset(&display_stop, 0xa5, sizeof(display_stop));
    uvdb_monitor_display_stop_reset(&display_stop);
    check(display_stop.phase == UVDB_MONITOR_DISPLAY_STOP_IDLE &&
          display_stop.generation == 0,
          "reset initial display stop handoff");
    check(uvdb_monitor_display_stop_begin(NULL, 1) < 0 &&
          uvdb_monitor_display_stop_begin(&display_stop, 0) < 0,
          "reject invalid initial display stop generation");
    check(uvdb_monitor_display_stop_begin(&display_stop, 7) == 0 &&
          uvdb_monitor_display_stop_pending_generation(&display_stop) == 7,
          "begin generation-tagged initial display sample");
    check(uvdb_monitor_display_stop_begin(&display_stop, 8) < 0 &&
          uvdb_monitor_display_stop_arm(&display_stop, 8) < 0 &&
          uvdb_monitor_display_stop_pending_generation(&display_stop) == 7,
          "reject overlapping begin and stale arm");
    check(uvdb_monitor_display_stop_arm(&display_stop, 7) == 0 &&
          uvdb_monitor_display_stop_pending_generation(&display_stop) == 0,
          "arm matching initial display stop");
    check(uvdb_monitor_display_stop_on_exception(&display_stop, 1) ==
              UVDB_MONITOR_DISPLAY_STOP_HANDLE &&
          display_stop.phase == UVDB_MONITOR_DISPLAY_STOP_IDLE &&
          display_stop.generation == 0,
          "matching server trap consumes armed display stop");

    check(uvdb_monitor_display_stop_begin(&display_stop, 9) == 0 &&
          uvdb_monitor_display_stop_on_exception(&display_stop, 0) ==
              UVDB_MONITOR_DISPLAY_STOP_NOT_OURS &&
          display_stop.phase == UVDB_MONITOR_DISPLAY_STOP_IDLE &&
          uvdb_monitor_display_stop_arm(&display_stop, 9) < 0,
          "real fault cancels display sampling before arm");

    check(uvdb_monitor_display_stop_begin(&display_stop, 10) == 0 &&
          uvdb_monitor_display_stop_arm(&display_stop, 10) == 0 &&
          uvdb_monitor_display_stop_on_exception(&display_stop, 0) ==
              UVDB_MONITOR_DISPLAY_STOP_NOT_OURS &&
          display_stop.phase == UVDB_MONITOR_DISPLAY_STOP_CANCELLED,
          "real fault cancels an armed synthetic display stop");
    check(uvdb_monitor_display_stop_on_exception(&display_stop, 1) ==
              UVDB_MONITOR_DISPLAY_STOP_IGNORE &&
          display_stop.phase == UVDB_MONITOR_DISPLAY_STOP_IDLE,
          "queued server trap is ignored after competing fault");
    check(uvdb_monitor_display_stop_on_exception(&display_stop, 0) ==
              UVDB_MONITOR_DISPLAY_STOP_NOT_OURS &&
          uvdb_monitor_display_stop_on_exception(&display_stop, 1) ==
              UVDB_MONITOR_DISPLAY_STOP_NOT_OURS,
          "unrelated exceptions do not belong to display handoff");

    check(uvdb_monitor_display_generation_is_current(12, 4, 12) &&
          !uvdb_monitor_display_generation_is_current(0, 4, 12) &&
          !uvdb_monitor_display_generation_is_current(11, 4, 12) &&
          !uvdb_monitor_display_generation_is_current(12, -1, 12),
          "only exact active socket generation exposes display cache");

    uint32_t millihz = 0;
    check(uvdb_monitor_ieee754_to_millihz(0x42700000u, &millihz) == 0 &&
          millihz == 60000u,
          "convert 60 Hz without floating-point execution");
    check(uvdb_monitor_ieee754_to_millihz(0x426fc28fu, &millihz) == 0 &&
          millihz == 59940u,
          "round fractional refresh rate to millihertz");
    millihz = 123u;
    check(uvdb_monitor_ieee754_to_millihz(0x7f800000u, &millihz) < 0 &&
          millihz == 123u,
          "reject infinite refresh rate without changing output");
    check(uvdb_monitor_ieee754_to_millihz(0x80000000u, &millihz) < 0,
          "reject signed refresh rate");
    check(uvdb_monitor_ieee754_to_millihz(0x44800000u, &millihz) < 0,
          "reject implausible refresh rate");
    check(uvdb_monitor_ieee754_to_millihz(0, NULL) < 0,
          "reject NULL refresh-rate output");

    check_parse("qRcmd,68656c70", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_HELP, "parse help");
    check_parse("qRcmd,737461747573", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_STATUS, "parse status");
    check_parse("qRcmd,74687265616473", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_THREADS, "parse threads");
    check_parse("qRcmd,6D6F64756C6573", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_MODULES, "parse uppercase module hex");
    check_parse("qRcmd,636f6e736f6c65", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_CONSOLE, "parse console");
    check_parse("qRcmd,646973706c6179", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_DISPLAY, "parse display");
    check_parse("qRcmd,092068656c702009", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_HELP, "trim horizontal whitespace");
    check_parse("qRcmd,", UVDB_MONITOR_PARSE_UNKNOWN,
                UVDB_MONITOR_COMMAND_NONE, "empty command is unknown");
    check_parse("qRcmd:68656c70", UVDB_MONITOR_PARSE_MALFORMED,
                UVDB_MONITOR_COMMAND_NONE, "comma is required");
    check_parse("qRcmd,68656c7", UVDB_MONITOR_PARSE_MALFORMED,
                UVDB_MONITOR_COMMAND_NONE, "reject odd hex");
    check_parse("qRcmd,68656x70", UVDB_MONITOR_PARSE_MALFORMED,
                UVDB_MONITOR_COMMAND_NONE, "reject nonhex");
    check_parse("qRcmd,0068656c70", UVDB_MONITOR_PARSE_MALFORMED,
                UVDB_MONITOR_COMMAND_NONE, "reject embedded NUL");
    check_parse("qRcmd,68656c70206e6f77", UVDB_MONITOR_PARSE_UNKNOWN,
                UVDB_MONITOR_COMMAND_NONE, "reject command arguments");
    check_parse("qRcmd,7772697465", UVDB_MONITOR_PARSE_UNKNOWN,
                UVDB_MONITOR_COMMAND_NONE, "reject unregistered command");
    check_parse(
        "qRcmd,616161616161616161616161616161616161616161616161616161616161616161",
        UVDB_MONITOR_PARSE_MALFORMED, UVDB_MONITOR_COMMAND_NONE,
        "reject oversized command");

    enum uvdb_monitor_command untouched = UVDB_MONITOR_COMMAND_STATUS;
    check(uvdb_monitor_parse_qrcmd(NULL, 0, &untouched) ==
              UVDB_MONITOR_PARSE_MALFORMED &&
          untouched == UVDB_MONITOR_COMMAND_STATUS,
          "NULL packet does not touch output");
    check(uvdb_monitor_parse_qrcmd("qRcmd,68656c70", 14, NULL) ==
              UVDB_MONITOR_PARSE_MALFORMED,
          "NULL command output rejected");

    char output[4096];
    size_t output_size = 0;
    int result = render(UVDB_MONITOR_COMMAND_HELP, NULL, output,
                        sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK,
          "help renders without snapshot");
    check(contains(output, output_size, "help") &&
          contains(output, output_size, "status") &&
          contains(output, output_size, "threads") &&
          contains(output, output_size, "modules") &&
          contains(output, output_size, "console") &&
          contains(output, output_size, "display"),
          "help lists complete registry");

    struct uvdb_monitor_snapshot snapshot = sample_snapshot();
    struct uvdb_monitor_snapshot original = snapshot;
    check(sizeof(snapshot) < 1024u,
          "status snapshot keeps retained traces out of exception stack");
    result = render(UVDB_MONITOR_COMMAND_STATUS, &snapshot, output,
                    sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK, "status renders");
    check(contains(output, output_size, "state: connected") &&
          contains(output, output_size, "target: stopped, signal=5") &&
          contains(output, output_size, "kernel: compatible") &&
          contains(output, output_size, "ABI=0x0001000b") &&
          contains(output, output_size, "active, healthy") &&
          contains(output, output_size, "undefined-instruction") &&
          contains(output, output_size,
                   "stop-trace newest-first: dropped=2") &&
          contains(output, output_size,
                   "#7 thread=0x40010003 active=0 raw-pc=0x81005678") &&
          contains(output, output_size, "handoff=1/2/3/4") &&
          contains(output, output_size,
                   "packet=11/0/0/0/12/13") &&
          contains(output, output_size,
                   "reply=14/15/16/1/17/0 exit=18"),
          "status includes debugger gates and fault state");
    check(!memcmp(&snapshot, &original, sizeof(snapshot)),
          "status rendering is read-only");

    struct uvdb_monitor_thread threads[2] = {
        {
            .id = 0x40010003,
            .flags = UVDB_MONITOR_THREAD_STOPPED |
                     UVDB_MONITOR_THREAD_GENERAL |
                     UVDB_MONITOR_THREAD_RESUME |
                     UVDB_MONITOR_THREAD_EXCEPTION,
            .name = "main thread",
        },
        {
            .id = 0x40010004,
            .name = "worker\nname",
        },
    };
    snapshot.threads = threads;
    snapshot.thread_count = 2;
    result = render(UVDB_MONITOR_COMMAND_THREADS, &snapshot, output,
                    sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK, "threads render");
    check(contains(output, output_size,
                   "0x40010003 [stopped,Hg,Hc,exception] main thread") &&
          contains(output, output_size, "worker_name"),
          "thread flags and names render safely");

    struct uvdb_monitor_module modules[1];
    memset(modules, 0, sizeof(modules));
    modules[0].id = 0x40000001u;
    memcpy(modules[0].name, "fixture\nmodule", 14);
    modules[0].segment_count = 2;
    modules[0].segments[0] = (struct uvdb_monitor_segment) {
        .address = 0x81000000u,
        .memory_size = 0x1000u,
        .permissions = 5u,
        .index = 0u,
    };
    modules[0].segments[1] = (struct uvdb_monitor_segment) {
        .address = 0x81200000u,
        .memory_size = 0x2000u,
        .permissions = 6u,
        .index = 2u,
    };
    snapshot.modules = modules;
    snapshot.module_count = 1;
    snapshot.module_reported_count = 130;
    snapshot.module_scanned_count = 128;
    snapshot.module_skipped_count = 2;
    snapshot.module_query_result = 0;
    result = render(UVDB_MONITOR_COMMAND_MODULES, &snapshot, output,
                    sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK, "modules render");
    check(contains(output, output_size,
                   "shown=1 reported=130 skipped=2 snapshot-omitted=2") &&
          contains(output, output_size, "fixture_module") &&
          contains(output, output_size,
                   "segment[2]: address=0x81200000 size=0x00002000"),
          "module bounds and segments render safely");

    snapshot.module_query_result = -42;
    snapshot.module_count = 0;
    result = render(UVDB_MONITOR_COMMAND_MODULES, &snapshot, output,
                    sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK &&
          contains(output, output_size, "unavailable: 0xffffffd6"),
          "module query failure is visible");

    snapshot.console = (struct uvdb_monitor_console) {
        .available = 1,
        .session_open = 1,
        .session_generation = 2,
        .queued_records = 3,
        .queued_bytes = 200,
        .sessions_opened = 2,
        .reconnects = 1,
        .accepted_records = 8,
        .accepted_bytes = 500,
        .sent_records = 5,
        .sent_bytes = 300,
        .dropped_disconnected_records = 1,
        .dropped_disconnected_bytes = 20,
        .dropped_contention_records = 2,
        .dropped_contention_bytes = 30,
        .dropped_full_records = 3,
        .dropped_full_bytes = 40,
        .dropped_stale_records = 4,
        .dropped_stale_bytes = 50,
        .no_ack_mode = 1,
        .frames_sent = 5,
        .frame_bytes_sent = 350,
        .would_block = 1,
        .commit_busy = 2,
        .partial_writes = 1,
        .hard_errors = 2,
        .session_errors = 3,
        .last_native_error = -77,
    };
    original = snapshot;
    result = render(UVDB_MONITOR_COMMAND_CONSOLE, &snapshot, output,
                    sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK, "console renders");
    check(contains(output, output_size,
                   "session: open, generation=2, no-ack=yes") &&
          contains(output, output_size, "queued: 3 records / 200 bytes") &&
          contains(output, output_size,
                   "dropped-total: 10 records / 140 bytes") &&
          contains(output, output_size,
                   "transport: frames=5 frame-bytes=350 would-block=1") &&
          contains(output, output_size,
                   "partial=1 hard=2 session=3 last-native=0xffffffb3"),
          "console exposes queue, loss, and transport counters");
    check(!memcmp(&snapshot, &original, sizeof(snapshot)),
          "console rendering is read-only");
    snapshot.console.available = 0;
    result = render(UVDB_MONITOR_COMMAND_CONSOLE, &snapshot, output,
                    sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK &&
          contains(output, output_size, "unavailable"),
          "unavailable console snapshot is explicit");

    snapshot.display = (struct uvdb_monitor_display) {
        .available = 1,
        .primary_head = 0,
        .vcount = 12345,
        .refresh_query_result = 0,
        .refresh_millihz = 59940,
        .maximum_query_result = 0,
        .maximum_width = 960,
        .maximum_height = 544,
        .immediate = {
            .query_result = 0,
            .address = 0x81200000u,
            .pitch = 960,
            .pixel_format = 0,
            .width = 960,
            .height = 544,
        },
        .next_frame = {
            .query_result = -7,
        },
    };
    original = snapshot;
    result = render(UVDB_MONITOR_COMMAND_DISPLAY, &snapshot, output,
                    sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK, "display renders");
    check(contains(output, output_size, "primary-head: 0") &&
          contains(output, output_size, "vcount: 12345") &&
          contains(output, output_size, "refresh-rate: 59.940 Hz") &&
          contains(output, output_size, "maximum-framebuffer: 960x544") &&
          contains(output, output_size,
                   "address=0x81200000 size=960x544 pitch=960 ") &&
          contains(output, output_size, "format=A8B8G8R8(0x00000000)") &&
          contains(output, output_size,
                   "framebuffer-next: unavailable, result=0xfffffff9"),
          "display reports bounded framebuffer and display metadata");
    check(!memcmp(&snapshot, &original, sizeof(snapshot)),
          "display rendering is read-only");

    snapshot.display.available = 0;
    result = render(UVDB_MONITOR_COMMAND_DISPLAY, &snapshot, output,
                    sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK &&
          contains(output, output_size,
                   "unavailable: no safe cached server-thread sample"),
          "display reports a missing safe cache explicitly");

    snapshot.display = (struct uvdb_monitor_display) {
        .available = 1,
        .primary_head = -3,
        .vcount = -4,
        .refresh_query_result = -5,
        .maximum_query_result = -6,
        .immediate = {.query_result = -7},
        .next_frame = {.query_result = -8},
    };
    result = render(UVDB_MONITOR_COMMAND_DISPLAY, &snapshot, output,
                    sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK &&
          contains(output, output_size,
                   "primary-head: unavailable, result=0xfffffffd") &&
          contains(output, output_size,
                   "vcount: unavailable, result=0xfffffffc") &&
          contains(output, output_size,
                   "refresh-rate: unavailable, result=0xfffffffb") &&
          contains(output, output_size,
                   "maximum-framebuffer: unavailable, result=0xfffffffa"),
          "negative display query results are explicit failures");

    struct uvdb_monitor_thread* maximum_threads = calloc(
        UVDB_MONITOR_MAX_THREADS, sizeof(*maximum_threads));
    struct uvdb_monitor_module* maximum_modules = calloc(
        UVDB_MONITOR_MAX_MODULES, sizeof(*maximum_modules));
    char* maximum_output = malloc(64u * 1024u);
    check(maximum_threads && maximum_modules && maximum_output,
          "allocate maximum snapshot fixtures");
    if(maximum_threads && maximum_modules && maximum_output)
    {
        for(size_t i = 0; i < UVDB_MONITOR_MAX_THREADS; ++i)
        {
            maximum_threads[i].id = (int32_t)(0x40010000u + i);
            snprintf(maximum_threads[i].name,
                     sizeof(maximum_threads[i].name), "thread-%u",
                     (unsigned int)i);
        }
        snapshot.threads = maximum_threads;
        snapshot.thread_count = UVDB_MONITOR_MAX_THREADS;
        result = render(UVDB_MONITOR_COMMAND_THREADS, &snapshot,
                        maximum_output, 64u * 1024u, &output_size);
        check(result == UVDB_MONITOR_RENDER_OK &&
              contains(maximum_output, output_size,
                       "0x4001003f [-] thread-63"),
              "maximum thread inventory renders completely");

        for(size_t i = 0; i < UVDB_MONITOR_MAX_MODULES; ++i)
        {
            maximum_modules[i].id = (uint32_t)(0x40020000u + i);
            snprintf(maximum_modules[i].name,
                     sizeof(maximum_modules[i].name), "module-%u",
                     (unsigned int)i);
            maximum_modules[i].segment_count = 1;
            maximum_modules[i].segments[0] =
                (struct uvdb_monitor_segment) {
                    .address = (uint32_t)(0x81000000u + i * 0x1000u),
                    .memory_size = 0x1000u,
                    .permissions = 5u,
                    .index = 0u,
                };
        }
        snapshot.modules = maximum_modules;
        snapshot.module_count = UVDB_MONITOR_MAX_MODULES;
        snapshot.module_reported_count = UVDB_MONITOR_MAX_MODULES;
        snapshot.module_scanned_count = UVDB_MONITOR_MAX_MODULES;
        snapshot.module_skipped_count = 0;
        snapshot.module_query_result = 0;
        result = render(UVDB_MONITOR_COMMAND_MODULES, &snapshot,
                        maximum_output, 64u * 1024u, &output_size);
        check(result == UVDB_MONITOR_RENDER_OK &&
              contains(maximum_output, output_size,
                       "0x4002007f module-127"),
              "maximum module snapshot renders completely");

        snapshot.thread_count = UVDB_MONITOR_MAX_THREADS + 1u;
        result = render(UVDB_MONITOR_COMMAND_THREADS, &snapshot,
                        maximum_output, 64u * 1024u, &output_size);
        check(result == UVDB_MONITOR_RENDER_INVALID && output_size == 0,
              "oversized thread inventory rejected");
        snapshot.thread_count = 0;
        snapshot.module_count = UVDB_MONITOR_MAX_MODULES + 1u;
        result = render(UVDB_MONITOR_COMMAND_MODULES, &snapshot,
                        maximum_output, 64u * 1024u, &output_size);
        check(result == UVDB_MONITOR_RENDER_INVALID && output_size == 0,
              "oversized module snapshot rejected");
    }
    free(maximum_output);
    free(maximum_modules);
    free(maximum_threads);

    result = render(UVDB_MONITOR_COMMAND_HELP, NULL, output, 64,
                    &output_size);
    check(result == UVDB_MONITOR_RENDER_TRUNCATED && output_size <= 64 &&
          output_size >= strlen("... output truncated\n") &&
          !memcmp(output + output_size - strlen("... output truncated\n"),
                  "... output truncated\n",
                  strlen("... output truncated\n")),
          "bounded renderer adds a complete truncation marker");

    size_t full_size = 0;
    result = render(UVDB_MONITOR_COMMAND_HELP, NULL, output,
                    sizeof(output), &full_size);
    check(result == UVDB_MONITOR_RENDER_OK, "measure full help");
    char* exact = malloc(full_size ? full_size : 1u);
    char* short_buffer = malloc(full_size ? full_size : 1u);
    size_t exact_size = 0;
    size_t short_size = 0;
    check(exact && short_buffer, "allocate capacity tests");
    if(exact && short_buffer)
    {
        check(uvdb_monitor_render(UVDB_MONITOR_COMMAND_HELP, NULL, exact,
                                  full_size, &exact_size) ==
                  UVDB_MONITOR_RENDER_OK && exact_size == full_size,
              "exact capacity succeeds");
        check(uvdb_monitor_render(UVDB_MONITOR_COMMAND_HELP, NULL,
                                  short_buffer, full_size - 1u,
                                  &short_size) ==
                  UVDB_MONITOR_RENDER_TRUNCATED && short_size < full_size,
              "one-short capacity truncates deliberately");
    }
    free(short_buffer);
    free(exact);

    size_t sentinel_size = 123;
    check(uvdb_monitor_render(UVDB_MONITOR_COMMAND_NONE, &snapshot, output,
                              sizeof(output), &sentinel_size) ==
              UVDB_MONITOR_RENDER_INVALID && sentinel_size == 0,
          "invalid renderer command rejected");
    check(uvdb_monitor_render(UVDB_MONITOR_COMMAND_HELP, NULL, NULL,
                              sizeof(output), &sentinel_size) ==
              UVDB_MONITOR_RENDER_INVALID,
          "NULL renderer output rejected");
    check(uvdb_monitor_render(UVDB_MONITOR_COMMAND_HELP, NULL, output,
                              sizeof(output), NULL) ==
              UVDB_MONITOR_RENDER_INVALID,
          "NULL renderer size rejected");

    if(failures)
    {
        fprintf(stderr, "%d monitor test(s) failed\n", failures);
        return 1;
    }
    puts("PASS: read-only GDB qRcmd monitor parser and renderer");
    return 0;
}

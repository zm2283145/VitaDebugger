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
    check_parse("qRcmd,68656c70", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_HELP, "parse help");
    check_parse("qRcmd,737461747573", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_STATUS, "parse status");
    check_parse("qRcmd,74687265616473", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_THREADS, "parse threads");
    check_parse("qRcmd,6D6F64756C6573", UVDB_MONITOR_PARSE_OK,
                UVDB_MONITOR_COMMAND_MODULES, "parse uppercase module hex");
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
          contains(output, output_size, "modules"),
          "help lists complete registry");

    struct uvdb_monitor_snapshot snapshot = sample_snapshot();
    struct uvdb_monitor_snapshot original = snapshot;
    result = render(UVDB_MONITOR_COMMAND_STATUS, &snapshot, output,
                    sizeof(output), &output_size);
    check(result == UVDB_MONITOR_RENDER_OK, "status renders");
    check(contains(output, output_size, "state: connected") &&
          contains(output, output_size, "target: stopped, signal=5") &&
          contains(output, output_size, "kernel: compatible") &&
          contains(output, output_size, "ABI=0x0001000b") &&
          contains(output, output_size, "active, healthy") &&
          contains(output, output_size, "undefined-instruction"),
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

#include "vitaprofiler.h"
#include "vitaprofiler_stream.h"
#include "vitaprofiler_tcp_vita.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "debugScreen.h"

#ifndef VP_TCP_GATE_HOST_A
#define VP_TCP_GATE_HOST_A 127
#endif
#ifndef VP_TCP_GATE_HOST_B
#define VP_TCP_GATE_HOST_B 0
#endif
#ifndef VP_TCP_GATE_HOST_C
#define VP_TCP_GATE_HOST_C 0
#endif
#ifndef VP_TCP_GATE_HOST_D
#define VP_TCP_GATE_HOST_D 1
#endif
#ifndef VP_TCP_GATE_PORT
#define VP_TCP_GATE_PORT 18195
#endif

#if VP_TCP_GATE_HOST_A < 0 || VP_TCP_GATE_HOST_A > 255 || \
    VP_TCP_GATE_HOST_B < 0 || VP_TCP_GATE_HOST_B > 255 || \
    VP_TCP_GATE_HOST_C < 0 || VP_TCP_GATE_HOST_C > 255 || \
    VP_TCP_GATE_HOST_D < 0 || VP_TCP_GATE_HOST_D > 255
#error "VitaProfiler TCP gate host octets must each be in [0, 255]"
#endif
#if VP_TCP_GATE_PORT < 1 || VP_TCP_GATE_PORT > 65535
#error "VitaProfiler TCP gate port must be in [1, 65535]"
#endif

#define GATE_NET_MEMORY_BYTES (1024u * 1024u)
#define GATE_RING_CAPACITY 32u
#define GATE_NAME_CAPACITY 4u
#define GATE_NAME_TEXT_BYTES 160u
#define GATE_DICTIONARY_WIRE_BYTES 1024u
#define GATE_EXPECTED_EVENTS 13u
#define GATE_DRAIN_BATCH 8u
#define GATE_CLOSE_RETRIES 4u

static uint8_t gate_net_memory[GATE_NET_MEMORY_BYTES]
    __attribute__((aligned(64)));
static struct vp_slot gate_slots[GATE_RING_CAPACITY];
static struct vp_context gate_context;
static struct vp_name_entry gate_name_entries[GATE_NAME_CAPACITY];
static char gate_name_text[GATE_NAME_TEXT_BYTES];
static struct vp_name_dictionary gate_names;
static uint8_t gate_dictionary_wire[GATE_DICTIONARY_WIRE_BYTES];
static struct vp_stream_writer gate_writer;
static struct vp_vita_tcp_sce_net_backend gate_backend =
    VP_VITA_TCP_SCE_NET_BACKEND_INITIALIZER;
static struct vp_vita_tcp_sink gate_sink;

static int gate_net_module_loaded;
static int gate_net_initialized;
static int gate_netctl_initialized;
static int gate_context_initialized;
static int gate_names_initialized;
static int gate_writer_initialized;
static int gate_sink_initialized;
static int gate_sink_released = 1;
static int gate_close_error_seen;
static int gate_network_cleanup_error_seen;
static const char* gate_first_failure_stage;
static int gate_first_failure_code;
/* Shared across UI states: a held button must not become a second edge when
 * cleanup completes and the app advances to the final results prompt. */
static uint32_t gate_previous_buttons;

static void remember_failure(const char* stage, int result)
{
    if (gate_first_failure_stage == NULL) {
        gate_first_failure_stage = stage;
        gate_first_failure_code = result;
    }
}

static void report_sce_result(const char* stage, int result)
{
    if (result < 0)
        remember_failure(stage, result);
    psvDebugScreenPrintf("[%s] %-30s 0x%08X\n",
                         result >= 0 ? " OK " : "FAIL", stage,
                         (unsigned int)result);
}

static void report_vp_result(const char* stage, int result)
{
    if (result != VP_RESULT_OK)
        remember_failure(stage, result);
    psvDebugScreenPrintf("[%s] %-30s 0x%08X\n",
                         result == VP_RESULT_OK ? " OK " : "FAIL", stage,
                         (unsigned int)result);
}

static uint32_t wait_for_cross_or_circle(void)
{
    for (;;) {
        SceCtrlData pad;
        uint32_t pressed = 0u;
        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
            pressed = pad.buttons & ~gate_previous_buttons;
            gate_previous_buttons = pad.buttons;
        }
        if ((pressed & SCE_CTRL_CIRCLE) != 0u)
            return SCE_CTRL_CIRCLE;
        if ((pressed & SCE_CTRL_CROSS) != 0u)
            return SCE_CTRL_CROSS;
        sceKernelDelayThread(16000);
    }
}

static int start_network(void)
{
    SceNetInitParam init;
    int result;

    psvDebugScreenPrintf("\nNetwork lifecycle (owned by this app)\n");
    result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    report_sce_result("load SCE_SYSMODULE_NET", result);
    if (result < 0)
        return result;
    gate_net_module_loaded = 1;

    memset(&init, 0, sizeof(init));
    init.memory = gate_net_memory;
    init.size = (int)sizeof(gate_net_memory);
    result = sceNetInit(&init);
    report_sce_result("sceNetInit", result);
    if (result < 0)
        return result;
    gate_net_initialized = 1;

    result = sceNetCtlInit();
    report_sce_result("sceNetCtlInit", result);
    if (result < 0)
        return result;
    gate_netctl_initialized = 1;
    return 0;
}

static int wait_for_network(char local_ip[16])
{
    int previous_state = -1;

    psvDebugScreenPrintf("Waiting for Vita network; Circle cancels safely.\n");
    for (;;) {
        SceCtrlData pad;
        int state = SCE_NETCTL_STATE_DISCONNECTED;
        int result;

        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
            const uint32_t pressed =
                pad.buttons & ~gate_previous_buttons;
            gate_previous_buttons = pad.buttons;
            if ((pressed & SCE_CTRL_CIRCLE) != 0u)
                return 1;
        }

        result = sceNetCtlInetGetState(&state);
        if (result < 0) {
            report_sce_result("sceNetCtlInetGetState", result);
            return result;
        }
        if (state != previous_state) {
            psvDebugScreenPrintf("  network state: %d\n", state);
            previous_state = state;
        }
        if (state == SCE_NETCTL_STATE_CONNECTED) {
            SceNetCtlInfo info;
            memset(&info, 0, sizeof(info));
            result = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS,
                                          &info);
            report_sce_result("read local IP", result);
            if (result < 0)
                return result;
            memcpy(local_ip, info.ip_address, 15u);
            local_ip[15] = '\0';
            return 0;
        }
        sceKernelDelayThread(100000);
    }
}

static int stop_network_once(void)
{
    int result;

    if (gate_netctl_initialized) {
        sceNetCtlTerm();
        gate_netctl_initialized = 0;
        report_sce_result("sceNetCtlTerm", 0);
    }
    if (gate_net_initialized) {
        result = sceNetTerm();
        report_sce_result("sceNetTerm", result);
        if (result < 0) {
            gate_network_cleanup_error_seen = 1;
            return 0;
        }
        gate_net_initialized = 0;
    }
    if (gate_net_module_loaded) {
        result = sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
        report_sce_result("unload SCE_SYSMODULE_NET", result);
        if (result < 0) {
            gate_network_cleanup_error_seen = 1;
            return 0;
        }
        gate_net_module_loaded = 0;
    }
    return 1;
}

static int initialize_capture_objects(uint32_t* zone_name,
                                      uint32_t* counter_name,
                                      uint32_t* frame_name)
{
    struct vp_name_dictionary_config name_config;
    struct vp_vita_tcp_sink_config sink_config;
    struct vp_stream_writer_config writer_config;
    int result;

    memset(&name_config, 0, sizeof(name_config));
    name_config.entries = gate_name_entries;
    name_config.entry_capacity = GATE_NAME_CAPACITY;
    name_config.text = gate_name_text;
    name_config.text_capacity = sizeof(gate_name_text);
    result = vp_name_dictionary_init(&gate_names, &name_config);
    report_vp_result("name dictionary init", result);
    if (result != VP_RESULT_OK)
        return result;
    gate_names_initialized = 1;
    result = vp_name_dictionary_register(&gate_names, "tcp_gate.zone",
                                         zone_name);
    if (result == VP_RESULT_OK)
        result = vp_name_dictionary_register(&gate_names, "tcp_gate.counter",
                                             counter_name);
    if (result == VP_RESULT_OK)
        result = vp_name_dictionary_register(&gate_names, "tcp_gate.frame",
                                             frame_name);
    if (result == VP_RESULT_OK)
        result = vp_name_dictionary_seal(&gate_names);
    report_vp_result("register and seal names", result);
    if (result != VP_RESULT_OK)
        return result;

    result = vp_vita_init(&gate_context, gate_slots, GATE_RING_CAPACITY);
    report_vp_result("profiler context init", result);
    if (result != VP_RESULT_OK)
        return result;
    gate_context_initialized = 1;

    vp_vita_tcp_sink_config_init(&sink_config);
    sink_config.endpoint.ipv4[0] = (uint8_t)VP_TCP_GATE_HOST_A;
    sink_config.endpoint.ipv4[1] = (uint8_t)VP_TCP_GATE_HOST_B;
    sink_config.endpoint.ipv4[2] = (uint8_t)VP_TCP_GATE_HOST_C;
    sink_config.endpoint.ipv4[3] = (uint8_t)VP_TCP_GATE_HOST_D;
    sink_config.endpoint.port = (uint16_t)VP_TCP_GATE_PORT;
    result = vp_vita_tcp_sce_net_ops_init(&gate_backend, &sink_config.ops);
    report_vp_result("SceNet backend init", result);
    if (result != VP_RESULT_OK)
        return result;
    sink_config.ops_user = &gate_backend;

    memset(&gate_sink, 0, sizeof(gate_sink));
    result = vp_vita_tcp_sink_init(&gate_sink, &sink_config);
    report_vp_result("TCP sink init", result);
    if (result != VP_RESULT_OK)
        return result;
    gate_sink_initialized = 1;
    gate_sink_released = 0;

    memset(&writer_config, 0, sizeof(writer_config));
    writer_config.context = &gate_context;
    writer_config.names = &gate_names;
    writer_config.write = vp_vita_tcp_sink_write;
    writer_config.write_user = &gate_sink;
    writer_config.dictionary_buffer = gate_dictionary_wire;
    writer_config.dictionary_buffer_capacity = sizeof(gate_dictionary_wire);
    memset(&gate_writer, 0, sizeof(gate_writer));
    result = vp_stream_writer_init(&gate_writer, &writer_config);
    report_vp_result("stream writer init", result);
    if (result == VP_RESULT_OK)
        gate_writer_initialized = 1;
    return result;
}

static int record_deterministic_events(uint32_t zone_name,
                                       uint32_t counter_name,
                                       uint32_t frame_name)
{
    struct vp_zone_scope zone;
    struct vp_vita_memory_snapshot memory;
    struct vp_vita_thread_snapshot thread;
    struct vp_stats stats;
    int result;

    memset(&zone, 0, sizeof(zone));
    result = vp_zone_begin(&gate_context, zone_name, &zone);
    if (result != VP_RESULT_OK)
        return result;
    sceKernelDelayThread(1000);
    result = vp_zone_end(&gate_context, &zone);
    if (result != VP_RESULT_OK)
        return result;
    result = vp_counter(&gate_context, counter_name, 314);
    if (result != VP_RESULT_OK)
        return result;
    result = vp_frame_mark(&gate_context, frame_name);
    if (result != VP_RESULT_OK)
        return result;
    sceKernelDelayThread(16667);
    result = vp_frame_mark(&gate_context, frame_name);
    if (result != VP_RESULT_OK)
        return result;
    result = vp_vita_record_memory(&gate_context, &memory);
    if (result != VP_RESULT_OK)
        return result;
    result = vp_vita_record_thread(&gate_context, 0u, &thread);
    if (result != VP_RESULT_OK)
        return result;
    result = vp_get_stats(&gate_context, &stats);
    if (result != VP_RESULT_OK)
        return result;
    if (stats.accepted != GATE_EXPECTED_EVENTS || stats.dropped != 0u ||
        stats.pending != GATE_EXPECTED_EVENTS)
        return VP_ERROR_STATE;

    psvDebugScreenPrintf("  live memory user=%u, thread=%08X\n",
                         memory.free_user_bytes, thread.thread_id);
    return VP_RESULT_OK;
}

static int release_sink_with_retries(void)
{
    uint32_t attempt;

    if (!gate_sink_initialized || gate_sink_released)
        return 1;
    for (attempt = 1u; attempt <= GATE_CLOSE_RETRIES; ++attempt) {
        const int result = vp_vita_tcp_sink_close(&gate_sink);
        psvDebugScreenPrintf("[%s] TCP close attempt %u          0x%08X\n",
                             result == VP_RESULT_OK ? " OK " : "FAIL",
                             attempt, (unsigned int)result);
        if (result == VP_RESULT_OK) {
            gate_sink_released = 1;
            return 1;
        }
        remember_failure("TCP sink close", result);
        gate_close_error_seen = 1;
        sceKernelDelayThread(100000);
    }
    return 0;
}

static const char* writer_state_name(uint32_t state)
{
    switch (state) {
    case VP_STREAM_WRITER_READY:
        return "ready";
    case VP_STREAM_WRITER_STREAMING:
        return "streaming";
    case VP_STREAM_WRITER_CLOSED:
        return "closed";
    case VP_STREAM_WRITER_FAILED:
        return "failed";
    default:
        return "uninitialized";
    }
}

static const char* sink_state_name(uint32_t state)
{
    switch (state) {
    case VP_VITA_TCP_SINK_READY:
        return "ready";
    case VP_VITA_TCP_SINK_CONNECTED:
        return "connected";
    case VP_VITA_TCP_SINK_CLOSED:
        return "closed";
    case VP_VITA_TCP_SINK_FAILED:
        return "failed";
    default:
        return "uninitialized";
    }
}

static void print_capture_stats(void)
{
    struct vp_stats core;
    struct vp_stream_writer_stats writer;
    struct vp_vita_tcp_sink_stats sink;

    psvDebugScreenPuts("\x1b[H\x1b[2J");
    psvDebugScreenPrintf("VitaProfiler TCP hardware gate\n");
    psvDebugScreenPrintf("Final statistics for %u.%u.%u.%u:%u\n\n",
                         (unsigned int)VP_TCP_GATE_HOST_A,
                         (unsigned int)VP_TCP_GATE_HOST_B,
                         (unsigned int)VP_TCP_GATE_HOST_C,
                         (unsigned int)VP_TCP_GATE_HOST_D,
                         (unsigned int)VP_TCP_GATE_PORT);
    if (gate_context_initialized &&
        vp_get_stats(&gate_context, &core) == VP_RESULT_OK) {
        psvDebugScreenPrintf("  ring: accepted=%u dropped=%u pending=%u\n",
                             core.accepted, core.dropped, core.pending);
    }
    if (gate_writer_initialized &&
        vp_stream_writer_get_stats(&gate_writer, &writer) == VP_RESULT_OK) {
        psvDebugScreenPrintf(
            "  writer: bytes=%llu events=%llu lost=%u state=%s\n",
            (unsigned long long)writer.bytes_written,
            (unsigned long long)writer.events_written,
            writer.events_lost_to_sink, writer_state_name(writer.state));
    }
    if (gate_sink_initialized &&
        vp_vita_tcp_sink_get_stats(&gate_sink, &sink) == VP_RESULT_OK) {
        psvDebugScreenPrintf(
            "  sink: bytes=%llu sends=%u waits=%u state=%s fail=%u\n",
            (unsigned long long)sink.bytes_sent, sink.send_calls,
            sink.send_waits, sink_state_name(sink.state), sink.failure);
        psvDebugScreenPrintf(
            "  TCP: connect-waits=%u partial=%u would-block=%u errors=%u\n",
            sink.connect_waits, sink.partial_sends, sink.would_blocks,
            sink.failures);
        psvDebugScreenPrintf(
            "  cleanup: close=%u errors=%u shutdown=%u native=%08X\n",
            sink.close_attempts, sink.close_errors, sink.shutdown_errors,
            (unsigned int)sink.last_native_error);
    }
    psvDebugScreenPrintf(
        "  network: netctl=%u net=%u module=%u cleanup-errors=%u\n",
        (unsigned int)gate_netctl_initialized,
        (unsigned int)gate_net_initialized,
        (unsigned int)gate_net_module_loaded,
        (unsigned int)gate_network_cleanup_error_seen);
    if (gate_first_failure_stage != NULL) {
        psvDebugScreenPrintf("  first failure: %s (0x%08X)\n",
                             gate_first_failure_stage,
                             (unsigned int)gate_first_failure_code);
    }
}

static int capture_stats_pass(void)
{
    struct vp_stats core;
    struct vp_stream_writer_stats writer;
    struct vp_vita_tcp_sink_stats sink;

    if (!gate_context_initialized || !gate_writer_initialized ||
        !gate_sink_initialized || !gate_sink_released ||
        gate_close_error_seen)
        return 0;
    if (vp_get_stats(&gate_context, &core) != VP_RESULT_OK ||
        vp_stream_writer_get_stats(&gate_writer, &writer) != VP_RESULT_OK ||
        vp_vita_tcp_sink_get_stats(&gate_sink, &sink) != VP_RESULT_OK)
        return 0;
    return core.accepted == GATE_EXPECTED_EVENTS && core.dropped == 0u &&
           core.pending == 0u &&
           writer.events_written == GATE_EXPECTED_EVENTS &&
           writer.events_lost_to_sink == 0u &&
           writer.state == VP_STREAM_WRITER_CLOSED &&
           sink.bytes_sent == writer.bytes_written &&
           sink.state == VP_VITA_TCP_SINK_CLOSED &&
           sink.failure == VP_VITA_TCP_FAILURE_NONE;
}

static void release_capture_objects(void)
{
    if (!gate_sink_released)
        return;
    if (gate_context_initialized) {
        vp_deinit(&gate_context);
        gate_context_initialized = 0;
    }
    if (gate_names_initialized) {
        vp_name_dictionary_deinit(&gate_names);
        gate_names_initialized = 0;
    }
}

static int drain_and_close_writer(size_t* total_drained)
{
    struct vp_stream_writer_stats stats;
    int result;

    *total_drained = 0u;
    if (!gate_writer_initialized)
        return 0;
    result = vp_stream_writer_get_stats(&gate_writer, &stats);
    if (result != VP_RESULT_OK) {
        report_vp_result("read writer state", result);
        return 0;
    }
    if (stats.state == VP_STREAM_WRITER_CLOSED)
        return 1;
    if (stats.state != VP_STREAM_WRITER_STREAMING)
        return 0;

    for (;;) {
        size_t drained = 0u;
        result = vp_stream_writer_drain(&gate_writer, GATE_DRAIN_BATCH,
                                        &drained);
        *total_drained += drained;
        if (result != VP_RESULT_OK || drained == 0u)
            break;
    }
    report_vp_result("drain ring completely", result);
    psvDebugScreenPrintf("  events drained: %u\n",
                         (unsigned int)*total_drained);
    if (result != VP_RESULT_OK)
        return 0;

    result = vp_stream_writer_close(&gate_writer);
    report_vp_result("close stream writer", result);
    return result == VP_RESULT_OK;
}

static int run_capture(void)
{
    uint32_t zone_name = 0u;
    uint32_t counter_name = 0u;
    uint32_t frame_name = 0u;
    size_t total_drained = 0u;
    int64_t stream_start_us;
    int events_recorded_ok = 0;
    int pipeline_ok;
    int result;

    psvDebugScreenPrintf("\nCapture pipeline\n");
    result = initialize_capture_objects(&zone_name, &counter_name,
                                        &frame_name);
    if (result != VP_RESULT_OK)
        goto cleanup;

    psvDebugScreenPrintf("Connecting to %u.%u.%u.%u:%u ...\n",
                         (unsigned int)VP_TCP_GATE_HOST_A,
                         (unsigned int)VP_TCP_GATE_HOST_B,
                         (unsigned int)VP_TCP_GATE_HOST_C,
                         (unsigned int)VP_TCP_GATE_HOST_D,
                         (unsigned int)VP_TCP_GATE_PORT);
    result = vp_vita_tcp_sink_connect(&gate_sink);
    report_vp_result("TCP connect", result);
    if (result != VP_RESULT_OK)
        goto cleanup;

    stream_start_us = (int64_t)sceKernelGetProcessTimeWide();
    if (stream_start_us < 0) {
        result = VP_ERROR_PLATFORM;
        report_vp_result("read stream start clock", result);
        goto cleanup;
    }
    result = vp_stream_writer_begin(&gate_writer,
                                    (uint64_t)stream_start_us);
    report_vp_result("send dictionary/header", result);
    if (result != VP_RESULT_OK)
        goto cleanup;

    result = record_deterministic_events(zone_name, counter_name, frame_name);
    report_vp_result("record 13 ordered events", result);
    if (result != VP_RESULT_OK)
        goto cleanup;
    events_recorded_ok = 1;

cleanup:
    pipeline_ok = drain_and_close_writer(&total_drained);
    if (!events_recorded_ok || total_drained != GATE_EXPECTED_EVENTS)
        pipeline_ok = 0;
    if (!release_sink_with_retries()) {
        psvDebugScreenPrintf(
            "[FAIL] socket retained; X/Circle retries cleanup.\n");
        pipeline_ok = 0;
    }
    if (!capture_stats_pass()) {
        remember_failure("final capture validation", VP_ERROR_STATE);
        pipeline_ok = 0;
    }
    return pipeline_ok;
}

int main(void)
{
    char local_ip[16] = "unknown";
    int cancelled = 0;
    int exit_code = 0;
    int network_result;
    int gate_ran = 0;
    int gate_passed = 0;
    int overall_pass = 0;
    uint32_t button;

    psvDebugScreenInit();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    psvDebugScreenPrintf("VitaProfiler TCP hardware gate\n");
    psvDebugScreenPrintf("Ordinary user mode; no kernel plugin calls.\n");
    psvDebugScreenPrintf("Target PC: %u.%u.%u.%u:%u\n",
                         (unsigned int)VP_TCP_GATE_HOST_A,
                         (unsigned int)VP_TCP_GATE_HOST_B,
                         (unsigned int)VP_TCP_GATE_HOST_C,
                         (unsigned int)VP_TCP_GATE_HOST_D,
                         (unsigned int)VP_TCP_GATE_PORT);

    network_result = start_network();
    if (network_result < 0) {
        exit_code = 1;
        goto shutdown;
    }
    network_result = wait_for_network(local_ip);
    if (network_result == 1) {
        cancelled = 1;
        goto shutdown;
    }
    if (network_result < 0) {
        exit_code = 1;
        goto shutdown;
    }

    psvDebugScreenPrintf("\nVita IP: %s\n", local_ip);
    psvDebugScreenPrintf("Start the PC receiver, then press X to connect.\n");
    psvDebugScreenPrintf("Circle exits without opening a TCP socket.\n");
    button = wait_for_cross_or_circle();
    if (button == SCE_CTRL_CIRCLE) {
        cancelled = 1;
        goto shutdown;
    }

    gate_ran = 1;
    gate_passed = run_capture();
    if (!gate_passed)
        exit_code = 1;

shutdown:
    while (!gate_sink_released) {
        psvDebugScreenPrintf(
            "Press X or Circle to retry retained socket cleanup.\n");
        (void)wait_for_cross_or_circle();
        (void)release_sink_with_retries();
    }
    while (!stop_network_once()) {
        exit_code = 1;
        psvDebugScreenPrintf(
            "Network cleanup retained; press Circle to retry.\n");
        do {
            button = wait_for_cross_or_circle();
        } while (button != SCE_CTRL_CIRCLE);
    }

    if (gate_ran) {
        overall_pass = gate_passed && !gate_network_cleanup_error_seen;
        if (!overall_pass)
            exit_code = 1;
        print_capture_stats();
        psvDebugScreenPrintf("\nVita-side result: %s\n",
                             overall_pass ? "PASS" : "FAIL");
        psvDebugScreenPrintf(
            "PASS includes clean TCP EOF/close and network teardown.\n");
    } else if (cancelled) {
        psvDebugScreenPrintf(
            "\nCancelled before TCP connection; cleanup is complete.\n");
    } else {
        psvDebugScreenPrintf("\nResult: FAIL\n");
    }

    release_capture_objects();
    psvDebugScreenPrintf("Press Circle to exit safely.\n");
    do {
        button = wait_for_cross_or_circle();
    } while (button != SCE_CTRL_CIRCLE);
    psvDebugScreenFinish();
    return exit_code;
}

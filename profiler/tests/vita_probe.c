#include "vitaprofiler.h"

#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <string.h>

#include "debugScreen.h"

#define FUNCTIONAL_CAPACITY 32u
#define STRESS_CAPACITY 64u
#define STRESS_PRODUCERS 4u
#define STRESS_EVENTS_PER_PRODUCER 128u
#define STRESS_NAME_BASE UINT32_C(0x70000000)

static struct vp_context functional_context;
static struct vp_slot functional_slots[FUNCTIONAL_CAPACITY];
static struct vp_event functional_events[FUNCTIONAL_CAPACITY];
static struct vp_name_dictionary functional_names;
static struct vp_name_entry functional_name_entries[8];
static char functional_name_text[128];
static uint8_t functional_name_wire[1024];

static struct vp_context stress_context;
static struct vp_slot stress_slots[STRESS_CAPACITY];
static struct vp_event stress_events[STRESS_CAPACITY];
static uint8_t stress_seen[STRESS_PRODUCERS][STRESS_EVENTS_PER_PRODUCER];

struct stress_result {
    uint32_t accepted;
    uint32_t dropped;
    uint32_t errors;
    uint32_t thread_id;
};

static struct stress_result stress_results[STRESS_PRODUCERS];
static volatile uint32_t stress_ready;
static volatile uint32_t stress_start;
static int check_count;
static int failure_count;

static void report_check(const char* name, int passed)
{
    ++check_count;
    if (!passed)
        ++failure_count;
    psvDebugScreenPrintf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
}

static uint16_t read_u16_le(const uint8_t* bytes)
{
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32_le(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64_le(const uint8_t* bytes)
{
    return (uint64_t)read_u32_le(bytes) |
           ((uint64_t)read_u32_le(bytes + 4) << 32);
}

static int encoded_event_matches(const struct vp_event* event)
{
    uint8_t encoded[VP_WIRE_EVENT_SIZE];
    if (vp_encode_event_le(event, encoded) != VP_RESULT_OK)
        return 0;
    return read_u64_le(encoded) == event->timestamp_us &&
           read_u64_le(encoded + 8) == (uint64_t)event->value &&
           read_u32_le(encoded + 16) == event->name_id &&
           read_u32_le(encoded + 20) == event->thread_id &&
           read_u32_le(encoded + 24) == event->correlation_id &&
           read_u16_le(encoded + 28) == event->type &&
           read_u16_le(encoded + 30) == event->flags;
}

static int name_view_matches(const struct vp_name_view* view,
                             const char* expected)
{
    size_t length = strlen(expected);
    return length == view->name_length &&
           memcmp(view->name, expected, length) == 0;
}

static int test_exact_wire_encoding(void)
{
    struct vp_wire_header header;
    struct vp_event event;
    uint8_t encoded_header[VP_WIRE_HEADER_SIZE];
    uint8_t encoded_event[VP_WIRE_EVENT_SIZE];
    static const uint8_t expected_header[VP_WIRE_HEADER_SIZE] = {
        0x56, 0x50, 0x52, 0x46, 0x01, 0x00, 0x20, 0x00,
        0x20, 0x00, 0x01, 0x00, 0x40, 0x42, 0x0f, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t expected_event[VP_WIRE_EVENT_SIZE] = {
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x44, 0x33, 0x22, 0x11, 0xdd, 0xcc, 0xbb, 0xaa,
        0x88, 0x77, 0x66, 0x55, 0x34, 0x12, 0x78, 0x56,
    };

    vp_wire_header_init(&header, UINT64_C(0x0102030405060708));
    if (vp_encode_wire_header_le(&header, encoded_header) != VP_RESULT_OK ||
        memcmp(encoded_header, expected_header, sizeof(expected_header)) != 0)
        return 0;

    memset(&event, 0, sizeof(event));
    event.timestamp_us = UINT64_C(0x0102030405060708);
    event.value = -2;
    event.name_id = UINT32_C(0x11223344);
    event.thread_id = UINT32_C(0xaabbccdd);
    event.correlation_id = UINT32_C(0x55667788);
    event.type = UINT16_C(0x1234);
    event.flags = UINT16_C(0x5678);
    return vp_encode_event_le(&event, encoded_event) == VP_RESULT_OK &&
           memcmp(encoded_event, expected_event, sizeof(expected_event)) == 0;
}

static void test_functional_events(void)
{
    struct vp_name_dictionary_config name_config;
    struct vp_zone_scope zone;
    struct vp_vita_memory_snapshot memory;
    struct vp_vita_thread_snapshot thread;
    struct vp_stats stats;
    uint32_t zone_name = 0u;
    uint32_t frame_name = 0u;
    uint32_t counter_name = 0u;
    size_t name_wire_size = 0u;
    size_t count;
    size_t i;
    int calls_ok = 1;
    int zone_started;
    int event_shape_ok;
    int live_wire_ok = 1;
    int names_ok;
    int names_resolve_ok = 1;

    memset(&name_config, 0, sizeof(name_config));
    memset(&zone, 0, sizeof(zone));
    memset(&memory, 0, sizeof(memory));
    memset(&thread, 0, sizeof(thread));
    name_config.entries = functional_name_entries;
    name_config.entry_capacity =
        sizeof(functional_name_entries) / sizeof(functional_name_entries[0]);
    name_config.text = functional_name_text;
    name_config.text_capacity = sizeof(functional_name_text);
    names_ok =
        vp_name_dictionary_init(&functional_names, &name_config) ==
            VP_RESULT_OK &&
        vp_name_dictionary_register(&functional_names, "probe zone",
                                    &zone_name) == VP_RESULT_OK &&
        vp_name_dictionary_register(&functional_names, "probe frame",
                                    &frame_name) == VP_RESULT_OK &&
        vp_name_dictionary_register(&functional_names, "probe counter",
                                    &counter_name) == VP_RESULT_OK &&
        vp_name_dictionary_seal(&functional_names) == VP_RESULT_OK &&
        vp_encode_name_dictionary_le(&functional_names, functional_name_wire,
                                      sizeof(functional_name_wire),
                                      &name_wire_size) == VP_RESULT_OK;
    report_check("name dictionary registers, seals and encodes", names_ok);
    if (vp_vita_init(&functional_context, functional_slots,
                     FUNCTIONAL_CAPACITY) != VP_RESULT_OK) {
        report_check("functional profiler context initialized", 0);
        vp_name_dictionary_deinit(&functional_names);
        return;
    }
    if (zone_name == 0u || frame_name == 0u || counter_name == 0u)
        calls_ok = 0;
    zone_started = vp_zone_begin(&functional_context, zone_name, &zone) ==
                   VP_RESULT_OK;
    if (!zone_started)
        calls_ok = 0;
    sceKernelDelayThread(2000);
    if (zone_started &&
        vp_zone_end(&functional_context, &zone) != VP_RESULT_OK)
        calls_ok = 0;
    if (vp_frame_mark(&functional_context, frame_name) != VP_RESULT_OK)
        calls_ok = 0;
    sceKernelDelayThread(16667);
    if (vp_frame_mark(&functional_context, frame_name) != VP_RESULT_OK)
        calls_ok = 0;
    if (vp_counter(&functional_context, counter_name, 64) != VP_RESULT_OK)
        calls_ok = 0;
    if (vp_vita_record_memory(&functional_context, &memory) != VP_RESULT_OK)
        calls_ok = 0;
    if (vp_vita_record_thread(&functional_context, 0u, &thread) !=
        VP_RESULT_OK)
        calls_ok = 0;
    report_check("zones, frames, counter and snapshots record", calls_ok);

    count = vp_drain(&functional_context, functional_events,
                     FUNCTIONAL_CAPACITY);
    event_shape_ok = count == 13u;
    if (event_shape_ok) {
        const struct vp_event* events = functional_events;
        event_shape_ok =
            events[0].type == VP_EVENT_ZONE_BEGIN &&
            events[1].type == VP_EVENT_ZONE_END &&
            events[0].name_id == zone_name &&
            events[1].name_id == zone_name &&
            events[0].correlation_id != 0u &&
            events[0].correlation_id == events[1].correlation_id &&
            events[1].value > 0 &&
            events[2].type == VP_EVENT_FRAME &&
            (events[2].flags & VP_EVENT_FLAG_FIRST) != 0u &&
            events[2].value == 0 &&
            events[3].type == VP_EVENT_FRAME && events[3].value > 0 &&
            events[4].type == VP_EVENT_COUNTER &&
            events[4].name_id == counter_name && events[4].value == 64 &&
            events[5].type == VP_EVENT_MEMORY_SAMPLE &&
            events[5].name_id == VP_METRIC_FREE_USER_BYTES &&
            events[5].value == (int64_t)memory.free_user_bytes &&
            events[6].name_id == VP_METRIC_FREE_CDRAM_BYTES &&
            events[6].value == (int64_t)memory.free_cdram_bytes &&
            events[7].name_id == VP_METRIC_FREE_PHYCONT_BYTES &&
            events[7].value == (int64_t)memory.free_phycont_bytes &&
            events[8].type == VP_EVENT_PROCESS_SAMPLE &&
            events[8].name_id == VP_METRIC_PROCESS_TIME_US &&
            events[8].value == (int64_t)memory.process_time_us &&
            events[9].type == VP_EVENT_THREAD_SAMPLE &&
            events[9].name_id == VP_METRIC_THREAD_RUN_CLOCKS &&
            events[9].value == (int64_t)thread.run_clocks &&
            events[10].name_id == VP_METRIC_THREAD_STACK_FREE_BYTES &&
            events[10].value == (int64_t)thread.stack_free_bytes &&
            events[11].name_id == VP_METRIC_THREAD_PREEMPTIONS &&
            events[11].value == (int64_t)thread.thread_preemptions &&
            events[12].name_id == VP_METRIC_INTERRUPT_PREEMPTIONS &&
            events[12].value == (int64_t)thread.interrupt_preemptions;
    }
    report_check("13 live events retain FIFO shape and values", event_shape_ok);

    for (i = 0; i < count; ++i) {
        struct vp_name_view view;
        if (!encoded_event_matches(&functional_events[i]))
            live_wire_ok = 0;
        if (!names_ok ||
            vp_name_wire_lookup_le(functional_name_wire, name_wire_size,
                                   functional_events[i].name_id, &view) !=
                VP_RESULT_OK)
            names_resolve_ok = 0;
    }
    report_check("live events survive wire encode/decode", live_wire_ok);
    if (names_resolve_ok) {
        struct vp_name_view zone_view;
        struct vp_name_view builtin_view;
        names_resolve_ok =
            vp_name_wire_lookup_le(functional_name_wire, name_wire_size,
                                   zone_name, &zone_view) == VP_RESULT_OK &&
            name_view_matches(&zone_view, "probe zone") &&
            vp_name_wire_lookup_le(functional_name_wire, name_wire_size,
                                   VP_METRIC_FREE_USER_BYTES,
                                   &builtin_view) == VP_RESULT_OK &&
            name_view_matches(&builtin_view,
                              "vita.memory.free_user_bytes");
    }
    report_check("captured event IDs resolve to useful names",
                 names_resolve_ok);
    report_check("exact wire header and event byte layout",
                 test_exact_wire_encoding());

    report_check("memory snapshot contains live Vita data",
                 memory.timestamp_us != 0u && memory.free_user_bytes != 0u);
    report_check("current-thread snapshot contains live Vita data",
                 thread.timestamp_us != 0u && thread.thread_id != 0u &&
                 thread.stack_free_bytes >= 0);
    report_check("functional ring drained without drops",
                 vp_get_stats(&functional_context, &stats) == VP_RESULT_OK &&
                 stats.accepted == 13u && stats.dropped == 0u &&
                 stats.pending == 0u);

    psvDebugScreenPrintf("  memory: user=%u CDRAM=%u phycont=%u\n",
                         memory.free_user_bytes, memory.free_cdram_bytes,
                         memory.free_phycont_bytes);
    psvDebugScreenPrintf("  thread: id=%08X stack-free=%d run=%08X%08X\n",
                         thread.thread_id, thread.stack_free_bytes,
                         (uint32_t)(thread.run_clocks >> 32),
                         (uint32_t)thread.run_clocks);
    vp_deinit(&functional_context);
    vp_name_dictionary_deinit(&functional_names);
}

static int stress_worker(SceSize args, void* argp)
{
    uint32_t producer;
    uint32_t sequence;
    struct stress_result* result;

    if (args != sizeof(producer) || argp == NULL)
        return -1;
    memcpy(&producer, argp, sizeof(producer));
    if (producer >= STRESS_PRODUCERS)
        return -1;
    result = &stress_results[producer];
    result->thread_id = (uint32_t)sceKernelGetThreadId();
    (void)__atomic_fetch_add(&stress_ready, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&stress_start, __ATOMIC_ACQUIRE) == 0u)
        sceKernelDelayThread(100);

    for (sequence = 0; sequence < STRESS_EVENTS_PER_PRODUCER; ++sequence) {
        struct vp_event event;
        int record_result;
        memset(&event, 0, sizeof(event));
        event.timestamp_us = sequence;
        event.value = (int64_t)sequence;
        event.name_id = STRESS_NAME_BASE + producer;
        event.thread_id = result->thread_id;
        event.correlation_id = producer;
        event.type = VP_EVENT_COUNTER;
        record_result = vp_record(&stress_context, &event);
        if (record_result == VP_RESULT_OK)
            ++result->accepted;
        else if (record_result == VP_RESULT_DROPPED)
            ++result->dropped;
        else
            ++result->errors;
    }
    return 0;
}

static void test_mpsc_pressure(void)
{
    SceUID workers[STRESS_PRODUCERS];
    struct vp_stats stats;
    uint32_t started = 0u;
    uint32_t accepted = 0u;
    uint32_t dropped = 0u;
    uint32_t errors = 0u;
    uint32_t producer;
    uint64_t ready_deadline;
    size_t count;
    size_t i;
    int create_ok = 1;
    int join_ok = 1;
    int contents_ok = 1;
    int reuse_ok;

    memset(workers, 0xff, sizeof(workers));
    memset(stress_results, 0, sizeof(stress_results));
    memset(stress_seen, 0, sizeof(stress_seen));
    __atomic_store_n(&stress_ready, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&stress_start, 0u, __ATOMIC_RELAXED);
    if (vp_vita_init(&stress_context, stress_slots, STRESS_CAPACITY) !=
        VP_RESULT_OK) {
        report_check("MPSC profiler context initialized", 0);
        return;
    }

    for (producer = 0; producer < STRESS_PRODUCERS; ++producer) {
        SceUID worker = sceKernelCreateThread("vp pressure producer",
                                              stress_worker, 0x10000100,
                                              16 * 1024, 0, 0, NULL);
        if (worker < 0 ||
            sceKernelStartThread(worker, sizeof(producer), &producer) < 0) {
            create_ok = 0;
            if (worker >= 0)
                sceKernelDeleteThread(worker);
            continue;
        }
        workers[producer] = worker;
        ++started;
    }

    ready_deadline = (uint64_t)sceKernelGetSystemTimeWide() + UINT64_C(2000000);
    while (__atomic_load_n(&stress_ready, __ATOMIC_ACQUIRE) < started &&
           (uint64_t)sceKernelGetSystemTimeWide() < ready_deadline)
        sceKernelDelayThread(1000);
    create_ok = create_ok && started == STRESS_PRODUCERS &&
                __atomic_load_n(&stress_ready, __ATOMIC_ACQUIRE) == started;
    __atomic_store_n(&stress_start, 1u, __ATOMIC_RELEASE);

    for (producer = 0; producer < STRESS_PRODUCERS; ++producer) {
        int status = -1;
        if (workers[producer] < 0)
            continue;
        if (sceKernelWaitThreadEnd(workers[producer], &status, NULL) < 0 ||
            status != 0)
            join_ok = 0;
        if (sceKernelDeleteThread(workers[producer]) < 0)
            join_ok = 0;
        accepted += stress_results[producer].accepted;
        dropped += stress_results[producer].dropped;
        errors += stress_results[producer].errors;
    }
    report_check("four Vita producer threads ran concurrently",
                 create_ok && join_ok);

    report_check("bounded pressure accepts capacity and drops excess",
                 errors == 0u && accepted == STRESS_CAPACITY &&
                 dropped == STRESS_PRODUCERS * STRESS_EVENTS_PER_PRODUCER -
                                STRESS_CAPACITY &&
                 vp_get_stats(&stress_context, &stats) == VP_RESULT_OK &&
                 stats.accepted == accepted && stats.dropped == dropped &&
                 stats.pending == STRESS_CAPACITY);

    count = vp_drain(&stress_context, stress_events, STRESS_CAPACITY);
    if (count != STRESS_CAPACITY)
        contents_ok = 0;
    for (i = 0; i < count; ++i) {
        const struct vp_event* event = &stress_events[i];
        uint32_t p;
        uint32_t sequence;
        if (event->name_id < STRESS_NAME_BASE)
            contents_ok = 0;
        p = event->name_id - STRESS_NAME_BASE;
        sequence = (uint32_t)event->value;
        if (p >= STRESS_PRODUCERS ||
            sequence >= STRESS_EVENTS_PER_PRODUCER ||
            event->type != VP_EVENT_COUNTER ||
            event->thread_id != stress_results[p].thread_id ||
            stress_seen[p][sequence] != 0u) {
            contents_ok = 0;
            continue;
        }
        stress_seen[p][sequence] = 1u;
    }
    report_check("pressure drain has unique complete records", contents_ok);

    reuse_ok = vp_counter(&stress_context, vp_name_id("after pressure"), 1) ==
                   VP_RESULT_OK &&
               vp_drain(&stress_context, stress_events, 1u) == 1u &&
               stress_events[0].value == 1 &&
               vp_get_stats(&stress_context, &stats) == VP_RESULT_OK &&
               stats.pending == 0u;
    report_check("ring slot reuse works after pressure drain", reuse_ok);
    psvDebugScreenPrintf("  pressure: accepted=%u dropped=%u errors=%u\n",
                         accepted, dropped, errors);
    vp_deinit(&stress_context);
}

int main(void)
{
    psvDebugScreenInit();
    psvDebugScreenPrintf("VitaProfiler user-mode self-test\n");
    psvDebugScreenPrintf("No kernel plugin calls are used.\n\n");

    test_functional_events();
    test_mpsc_pressure();

    psvDebugScreenPrintf("\nResult: %s (%d checks, %d failures)\n",
                         failure_count == 0 ? "PASS" : "FAIL",
                         check_count, failure_count);
    psvDebugScreenPrintf("\nThis diagnostic exits automatically in 5 minutes.\n");
    sceKernelDelayThread(300000000);
    return failure_count == 0 ? 0 : 1;
}

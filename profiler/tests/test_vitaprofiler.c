#include "vitaprofiler.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

static int failures;

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__);     \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

struct fake_source {
    uint64_t now;
    uint32_t thread_id;
};

static uint64_t fake_clock(void* user)
{
    return ((struct fake_source*)user)->now;
}

static uint32_t fake_thread(void* user)
{
    return ((struct fake_source*)user)->thread_id;
}

static void init_context(struct vp_context* context, struct vp_slot* slots,
                         uint32_t capacity, struct fake_source* source)
{
    struct vp_config config;
    memset(&config, 0, sizeof(config));
    config.slots = slots;
    config.capacity = capacity;
    config.clock = fake_clock;
    config.clock_user = source;
    config.thread_id = fake_thread;
    config.thread_user = source;
    CHECK(vp_init(context, &config) == VP_RESULT_OK, "initialize context");
}

static void test_validation_and_names(void)
{
    struct vp_context context;
    struct vp_slot slots[4];
    struct fake_source source = {0, 1};
    struct vp_config config;

    memset(&context, 0, sizeof(context));
    memset(&config, 0, sizeof(config));
    config.slots = slots;
    config.capacity = 3;
    config.clock = fake_clock;
    config.clock_user = &source;
    config.thread_id = fake_thread;
    config.thread_user = &source;
    CHECK(vp_init(&context, &config) == VP_ERROR_INVALID_ARGUMENT,
          "reject non-power-of-two capacity");
    config.capacity = 4;
    config.clock = NULL;
    CHECK(vp_init(&context, &config) == VP_ERROR_INVALID_ARGUMENT,
          "reject missing clock");
    CHECK(vp_record(&context, NULL) == VP_ERROR_NOT_INITIALIZED,
          "reject record before initialization");
    CHECK(vp_name_id(NULL) == 0u, "NULL name is unnamed");
    CHECK(vp_name_id("") == 0u, "empty name is unnamed");
    CHECK(vp_name_id("hello") == 0x4f9f2cabu, "stable FNV-1a name ID");
}

static void test_bounded_fifo_and_reuse(void)
{
    struct vp_context context;
    struct vp_slot slots[4];
    struct vp_event output[4];
    struct vp_stats stats;
    struct fake_source source = {100, 9};
    size_t count;
    unsigned int i;

    init_context(&context, slots, 4, &source);
    for (i = 0; i < 4; ++i) {
        source.now = 100u + i;
        CHECK(vp_counter(&context, 10u, (int64_t)i) == VP_RESULT_OK,
              "fill ring");
    }
    CHECK(vp_counter(&context, 10u, 4) == VP_RESULT_DROPPED,
          "full ring drops without overwrite");
    CHECK(vp_get_stats(&context, &stats) == VP_RESULT_OK,
          "read ring stats");
    CHECK(stats.pending == 4u && stats.accepted == 4u && stats.dropped == 1u,
          "full ring stats");

    count = vp_drain(&context, output, 2);
    CHECK(count == 2u, "partial drain count");
    CHECK(output[0].value == 0 && output[1].value == 1,
          "partial drain FIFO order");
    CHECK(vp_counter(&context, 10u, 5) == VP_RESULT_OK,
          "reuse first drained slot");
    CHECK(vp_counter(&context, 10u, 6) == VP_RESULT_OK,
          "reuse second drained slot");
    count = vp_drain(&context, output, 4);
    CHECK(count == 4u, "drain wrapped ring");
    CHECK(output[0].value == 2 && output[1].value == 3 &&
              output[2].value == 5 && output[3].value == 6,
          "wrapped ring remains FIFO");
    CHECK(vp_get_stats(&context, &stats) == VP_RESULT_OK &&
              stats.pending == 0u && stats.accepted == 6u &&
              stats.dropped == 1u,
          "post-drain stats");
    vp_deinit(&context);
    CHECK(vp_get_stats(&context, &stats) == VP_ERROR_NOT_INITIALIZED,
          "deinitialized context rejects access");
}

static void test_zones_and_frames(void)
{
    struct vp_context context;
    struct vp_slot slots[8];
    struct vp_event output[8];
    struct vp_zone_scope zone;
    struct fake_source source = {1000, 7};
    uint32_t zone_name = vp_name_id("update");
    uint32_t frame_name = vp_name_id("frame");
    size_t count;

    init_context(&context, slots, 8, &source);
    CHECK(vp_zone_begin(&context, zone_name, &zone) == VP_RESULT_OK,
          "begin zone");
    source.now = 1060;
    CHECK(vp_zone_end(&context, &zone) == VP_RESULT_OK, "end zone");
    CHECK(vp_zone_end(&context, &zone) == VP_ERROR_INVALID_ARGUMENT,
          "zone token is single-use");

    source.now = 2000;
    CHECK(vp_frame_mark(&context, frame_name) == VP_RESULT_OK,
          "first frame marker");
    source.now = 18667;
    CHECK(vp_frame_mark(&context, frame_name) == VP_RESULT_OK,
          "second frame marker");
    count = vp_drain(&context, output, 8);
    CHECK(count == 4u, "zone and frame event count");
    CHECK(output[0].type == VP_EVENT_ZONE_BEGIN &&
              output[1].type == VP_EVENT_ZONE_END,
          "zone event types");
    CHECK(output[0].correlation_id != 0u &&
              output[0].correlation_id == output[1].correlation_id,
          "zone correlation");
    CHECK(output[1].value == 60, "zone duration");
    CHECK(output[2].type == VP_EVENT_FRAME && output[2].value == 0 &&
              (output[2].flags & VP_EVENT_FLAG_FIRST) != 0u,
          "first frame semantics");
    CHECK(output[3].value == 16667 && output[3].correlation_id == 1u,
          "frame duration and sequence");

    source.now = 3000;
    source.thread_id = 7;
    CHECK(vp_zone_begin(&context, zone_name, &zone) == VP_RESULT_OK,
          "begin cross-thread zone");
    source.now = 2990;
    source.thread_id = 8;
    CHECK(vp_zone_end(&context, &zone) == VP_RESULT_OK,
          "end cross-thread zone");
    count = vp_drain(&context, output, 8);
    CHECK(count == 2u && output[1].value == 0,
          "clock regression clamps duration");
    CHECK((output[1].flags & VP_EVENT_FLAG_THREAD_MISMATCH) != 0u &&
              (output[1].flags & VP_EVENT_FLAG_CLOCK_REGRESSION) != 0u,
          "zone diagnostic flags");
}

static void test_wire_encoding(void)
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
    CHECK(vp_encode_wire_header_le(&header, encoded_header) == VP_RESULT_OK,
          "encode wire header");
    CHECK(memcmp(encoded_header, expected_header, sizeof(expected_header)) == 0,
          "wire header bytes");

    memset(&event, 0, sizeof(event));
    event.timestamp_us = UINT64_C(0x0102030405060708);
    event.value = -2;
    event.name_id = 0x11223344u;
    event.thread_id = 0xaabbccddu;
    event.correlation_id = 0x55667788u;
    event.type = 0x1234u;
    event.flags = 0x5678u;
    CHECK(vp_encode_event_le(&event, encoded_event) == VP_RESULT_OK,
          "encode wire event");
    CHECK(memcmp(encoded_event, expected_event, sizeof(expected_event)) == 0,
          "wire event bytes");
}

#define STRESS_PRODUCERS 4u
#define STRESS_EVENTS_PER_PRODUCER 512u
#define STRESS_CAPACITY (STRESS_PRODUCERS * STRESS_EVENTS_PER_PRODUCER)

struct producer_args {
    struct vp_context* context;
    uint32_t producer;
    int result;
};

static struct vp_slot stress_slots[STRESS_CAPACITY];
static struct vp_event stress_output[STRESS_CAPACITY];
static unsigned char stress_seen[STRESS_PRODUCERS]
                                [STRESS_EVENTS_PER_PRODUCER];

static void producer_body(struct producer_args* args)
{
    uint32_t i;
    for (i = 0; i < STRESS_EVENTS_PER_PRODUCER; ++i) {
        struct vp_event event;
        memset(&event, 0, sizeof(event));
        event.timestamp_us = i;
        event.value = (int64_t)i;
        event.name_id = args->producer;
        event.thread_id = args->producer + 1u;
        event.type = VP_EVENT_COUNTER;
        if (vp_record(args->context, &event) != VP_RESULT_OK) {
            args->result = -1;
            return;
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI producer_thread(LPVOID user)
{
    producer_body((struct producer_args*)user);
    return 0;
}
#else
static void* producer_thread(void* user)
{
    producer_body((struct producer_args*)user);
    return NULL;
}
#endif

static void test_multiple_producers(void)
{
    struct vp_context context;
    struct fake_source source = {0, 1};
    struct producer_args args[STRESS_PRODUCERS];
    size_t count;
    size_t i;
    unsigned int producer;
#if defined(_WIN32)
    HANDLE threads[STRESS_PRODUCERS];
#else
    pthread_t threads[STRESS_PRODUCERS];
#endif

    memset(stress_seen, 0, sizeof(stress_seen));
    init_context(&context, stress_slots, STRESS_CAPACITY, &source);
    for (producer = 0; producer < STRESS_PRODUCERS; ++producer) {
        args[producer].context = &context;
        args[producer].producer = producer;
        args[producer].result = 0;
#if defined(_WIN32)
        threads[producer] = CreateThread(NULL, 0, producer_thread,
                                         &args[producer], 0, NULL);
        CHECK(threads[producer] != NULL, "create producer thread");
#else
        CHECK(pthread_create(&threads[producer], NULL, producer_thread,
                             &args[producer]) == 0,
              "create producer thread");
#endif
    }
    for (producer = 0; producer < STRESS_PRODUCERS; ++producer) {
#if defined(_WIN32)
        if (threads[producer] != NULL) {
            CHECK(WaitForSingleObject(threads[producer], INFINITE) ==
                      WAIT_OBJECT_0,
                  "join producer thread");
            CloseHandle(threads[producer]);
        }
#else
        CHECK(pthread_join(threads[producer], NULL) == 0,
              "join producer thread");
#endif
        CHECK(args[producer].result == 0,
              "concurrent producer unexpectedly dropped");
    }

    count = vp_drain(&context, stress_output, STRESS_CAPACITY);
    CHECK(count == STRESS_CAPACITY, "drain every concurrent event");
    for (i = 0; i < count; ++i) {
        uint32_t p = stress_output[i].name_id;
        uint32_t sequence = (uint32_t)stress_output[i].value;
        CHECK(p < STRESS_PRODUCERS &&
                  sequence < STRESS_EVENTS_PER_PRODUCER,
              "concurrent event bounds");
        if (p < STRESS_PRODUCERS &&
            sequence < STRESS_EVENTS_PER_PRODUCER) {
            CHECK(stress_seen[p][sequence] == 0u,
                  "concurrent event is unique");
            stress_seen[p][sequence] = 1u;
        }
    }
    for (producer = 0; producer < STRESS_PRODUCERS; ++producer) {
        unsigned int sequence;
        for (sequence = 0; sequence < STRESS_EVENTS_PER_PRODUCER; ++sequence)
            CHECK(stress_seen[producer][sequence] != 0u,
                  "concurrent event is present");
    }
}

int main(void)
{
    test_validation_and_names();
    test_bounded_fifo_and_reuse();
    test_zones_and_frames();
    test_wire_encoding();
    test_multiple_producers();

    if (failures != 0) {
        fprintf(stderr, "%d profiler test(s) failed\n", failures);
        return 1;
    }
    puts("vitaprofiler: all native tests passed");
    return 0;
}

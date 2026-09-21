#include "vitaprofiler_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

struct legacy_stream_writer_layout {
    struct vp_context* context;
    const struct vp_name_dictionary* names;
    vp_stream_write_fn write;
    void* write_user;
    uint8_t* dictionary_buffer;
    size_t dictionary_buffer_capacity;
    uint64_t bytes_written;
    uint64_t events_written;
    uint32_t events_lost_to_sink;
    uint32_t state;
    uint32_t initialized;
};

_Static_assert(sizeof(struct vp_stream_writer) ==
                   sizeof(struct legacy_stream_writer_layout),
               "legacy stream writer layout changed");
_Static_assert(offsetof(struct vp_stream_writer, initialized) ==
                   offsetof(struct legacy_stream_writer_layout, initialized),
               "legacy stream writer field offsets changed");
_Static_assert(sizeof(struct vp_stream_writer_stats) ==
                   sizeof(uint64_t) * 2u + sizeof(uint32_t) * 2u,
               "legacy stream stats layout changed");

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

struct memory_sink {
    uint8_t data[16384];
    size_t used;
    uint32_t calls;
    uint32_t fail_call;
};

struct counting_sink {
    uint64_t bytes;
    size_t largest_write;
};

static uint64_t read_u64_le(const uint8_t* input)
{
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
        value |= (uint64_t)input[index] << (index * 8u);
    return value;
}

static uint32_t read_u32_le(const uint8_t* input)
{
    uint32_t value = 0u;
    for (uint32_t index = 0u; index < 4u; ++index)
        value |= (uint32_t)input[index] << (index * 8u);
    return value;
}

static void write_u32_le(uint8_t* output, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
        output[index] = (uint8_t)(value >> (index * 8u));
}

static void write_u64_le(uint8_t* output, uint64_t value)
{
    for (uint32_t index = 0u; index < 8u; ++index)
        output[index] = (uint8_t)(value >> (index * 8u));
}

static uint32_t test_crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0u; index < size; ++index) {
        crc ^= data[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1u) ^
                  (UINT32_C(0xedb88320) & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static void refresh_chunk_crc(uint8_t* chunk)
{
    uint32_t payload_size = read_u32_le(chunk + 12u);
    write_u32_le(
        chunk + 20u,
        test_crc32(chunk + VP_STREAM_V2_CHUNK_HEADER_SIZE, payload_size));
}

static uint64_t fake_clock(void* user)
{
    return ((struct fake_source*)user)->now;
}

static uint32_t fake_thread(void* user)
{
    return ((struct fake_source*)user)->thread_id;
}

static int memory_write(void* user, const uint8_t* data, size_t size)
{
    struct memory_sink* sink = (struct memory_sink*)user;
    ++sink->calls;
    if (sink->fail_call != 0u && sink->calls == sink->fail_call)
        return -1;
    if (size > sizeof(sink->data) - sink->used)
        return -1;
    memcpy(sink->data + sink->used, data, size);
    sink->used += size;
    return 0;
}

static int counting_write(void* user, const uint8_t* data, size_t size)
{
    struct counting_sink* sink = (struct counting_sink*)user;
    if (data == NULL && size != 0u)
        return -1;
    sink->bytes += size;
    if (size > sink->largest_write)
        sink->largest_write = size;
    return 0;
}

static void init_context(struct vp_context* context, struct vp_slot* slots,
                         struct fake_source* source)
{
    struct vp_config config;
    memset(&config, 0, sizeof(config));
    config.slots = slots;
    config.capacity = 8u;
    config.clock = fake_clock;
    config.clock_user = source;
    config.thread_id = fake_thread;
    config.thread_user = source;
    CHECK(vp_init(context, &config) == VP_RESULT_OK, "initialize context");
}

static void init_names(struct vp_name_dictionary* names,
                       struct vp_name_entry* entries, char* text,
                       uint32_t* zone_id, uint32_t* counter_id)
{
    struct vp_name_dictionary_config config;
    memset(&config, 0, sizeof(config));
    config.entries = entries;
    config.entry_capacity = 4u;
    config.text = text;
    config.text_capacity = 128u;
    CHECK(vp_name_dictionary_init(names, &config) == VP_RESULT_OK,
          "initialize dictionary");
    CHECK(vp_name_dictionary_register(names, "update", zone_id) ==
              VP_RESULT_OK,
          "register zone name");
    CHECK(vp_name_dictionary_register(names, "draw calls", counter_id) ==
              VP_RESULT_OK,
          "register counter name");
    CHECK(vp_name_dictionary_seal(names) == VP_RESULT_OK,
          "seal dictionary");
}

static void test_legacy_writer_canaries(void)
{
    struct guarded_writer {
        uint8_t before[16];
        struct vp_stream_writer value;
        uint8_t after[16];
    } writer;
    struct guarded_stats {
        uint8_t before[16];
        struct vp_stream_writer_stats value;
        uint8_t after[16];
    } stats;
    struct vp_context context;
    struct vp_slot slots[8];
    struct fake_source source = {1u, 2u};
    struct vp_name_dictionary names;
    struct vp_name_entry entries[4];
    char text[128];
    uint32_t zone_id;
    uint32_t counter_id;
    uint8_t dictionary_buffer[1024];
    struct memory_sink sink;
    struct vp_stream_writer_config config;
    uint8_t canary[16];
    memset(&writer, 0, sizeof(writer));
    memset(&stats, 0, sizeof(stats));
    memset(&sink, 0, sizeof(sink));
    memset(canary, 0xa5, sizeof(canary));
    memset(writer.before, 0xa5, sizeof(writer.before));
    memset(writer.after, 0xa5, sizeof(writer.after));
    memset(stats.before, 0xa5, sizeof(stats.before));
    memset(stats.after, 0xa5, sizeof(stats.after));
    init_context(&context, slots, &source);
    init_names(&names, entries, text, &zone_id, &counter_id);
    memset(&config, 0, sizeof(config));
    config.context = &context;
    config.names = &names;
    config.write = memory_write;
    config.write_user = &sink;
    config.dictionary_buffer = dictionary_buffer;
    config.dictionary_buffer_capacity = sizeof(dictionary_buffer);
    CHECK(vp_stream_writer_init(&writer.value, &config) == VP_RESULT_OK &&
              vp_stream_writer_begin(&writer.value, 1u) == VP_RESULT_OK &&
              vp_stream_writer_get_stats(
                  &writer.value, &stats.value) == VP_RESULT_OK &&
              vp_stream_writer_close(&writer.value) == VP_RESULT_OK,
          "legacy stream writer entry points remain functional");
    CHECK(memcmp(writer.before, canary, sizeof(canary)) == 0 &&
              memcmp(writer.after, canary, sizeof(canary)) == 0 &&
              memcmp(stats.before, canary, sizeof(canary)) == 0 &&
              memcmp(stats.after, canary, sizeof(canary)) == 0,
          "legacy stream writer and stats preserve caller canaries");
    vp_name_dictionary_deinit(&names);
    vp_deinit(&context);
}

static void test_combined_stream_round_trip(void)
{
    struct vp_context context;
    struct vp_slot slots[8];
    struct fake_source source = {1000u, 7u};
    struct vp_name_dictionary names;
    struct vp_name_entry entries[4];
    char text[128];
    uint32_t zone_id = 0u;
    uint32_t counter_id = 0u;
    uint8_t dictionary_buffer[1024];
    struct memory_sink sink;
    struct vp_stream_writer writer;
    struct vp_stream_writer_config writer_config;
    struct vp_stream_writer_stats writer_stats;
    struct vp_zone_scope zone;
    struct vp_name_wire_info name_info;
    struct vp_wire_cursor cursor;
    struct vp_wire_info wire_info;
    struct vp_event event;
    struct vp_name_view name;
    size_t drained = 0u;

    memset(&sink, 0, sizeof(sink));
    init_context(&context, slots, &source);
    init_names(&names, entries, text, &zone_id, &counter_id);
    CHECK(vp_zone_begin(&context, zone_id, &zone) == VP_RESULT_OK,
          "record zone begin");
    source.now = 1250u;
    CHECK(vp_counter(&context, counter_id, 42) == VP_RESULT_OK,
          "record named counter");
    CHECK(vp_zone_end(&context, &zone) == VP_RESULT_OK,
          "record zone end");

    memset(&writer_config, 0, sizeof(writer_config));
    writer_config.context = &context;
    writer_config.names = &names;
    writer_config.write = memory_write;
    writer_config.write_user = &sink;
    writer_config.dictionary_buffer = dictionary_buffer;
    writer_config.dictionary_buffer_capacity = 1u;
    CHECK(vp_stream_writer_init(&writer, &writer_config) ==
                  VP_ERROR_BUFFER_TOO_SMALL &&
              sink.calls == 0u && sink.used == 0u,
          "short dictionary buffer fails before sink output");
    writer_config.dictionary_buffer_capacity = sizeof(dictionary_buffer);
    CHECK(vp_stream_writer_init(&writer, &writer_config) == VP_RESULT_OK,
          "initialize stream writer");
    CHECK(vp_stream_writer_begin(&writer, 900u) == VP_RESULT_OK,
          "write dictionary and event header");
    CHECK(vp_stream_writer_close(&writer) == VP_ERROR_BUSY &&
              vp_stream_writer_get_stats(&writer, &writer_stats) ==
                  VP_RESULT_OK &&
              writer_stats.state == VP_STREAM_WRITER_STREAMING,
          "close refuses to hide queued records");
    CHECK(vp_stream_writer_drain(&writer, 2u, &drained) == VP_RESULT_OK &&
              drained == 2u,
          "bounded first drain");
    CHECK(vp_stream_writer_drain(&writer, 8u, &drained) == VP_RESULT_OK &&
              drained == 1u,
          "drain remaining event");
    CHECK(vp_stream_writer_close(&writer) == VP_RESULT_OK,
          "close complete stream");
    CHECK(vp_stream_writer_get_stats(&writer, &writer_stats) ==
                  VP_RESULT_OK &&
              writer_stats.events_written == 3u &&
              writer_stats.events_lost_to_sink == 0u &&
              writer_stats.bytes_written == sink.used,
          "writer statistics account exact output");

    CHECK(vp_name_wire_validate_le(sink.data, sink.used, &name_info) ==
              VP_RESULT_OK,
          "combined stream begins with valid dictionary");
    CHECK(vp_wire_cursor_init(&cursor, sink.data + name_info.total_size,
                              sink.used - name_info.total_size,
                              &wire_info) == VP_RESULT_OK &&
              wire_info.stream_start_us == 900u &&
              wire_info.event_count == 3u,
          "event receiver finds stream after dictionary");
    CHECK(vp_wire_cursor_next(&cursor, &event) == VP_RESULT_OK &&
              event.type == VP_EVENT_ZONE_BEGIN &&
              vp_name_wire_lookup_le(sink.data, name_info.total_size,
                                     event.name_id, &name) == VP_RESULT_OK &&
              name.name_length == 6u &&
              memcmp(name.name, "update", 6u) == 0,
          "receiver resolves named zone");
    CHECK(vp_wire_cursor_next(&cursor, &event) == VP_RESULT_OK &&
              event.type == VP_EVENT_COUNTER && event.value == 42,
          "receiver decodes named counter");
    CHECK(vp_wire_cursor_next(&cursor, &event) == VP_RESULT_OK &&
              event.type == VP_EVENT_ZONE_END && event.value == 250,
          "receiver decodes zone completion");
    CHECK(vp_wire_cursor_next(&cursor, &event) == VP_RESULT_END,
          "receiver consumes exact event count");
}

static void test_fail_closed_sink_lifecycle(void)
{
    struct vp_context context;
    struct vp_slot slots[8];
    struct fake_source source = {1u, 2u};
    struct vp_name_dictionary names;
    struct vp_name_entry entries[4];
    char text[128];
    uint32_t zone_id = 0u;
    uint32_t counter_id = 0u;
    uint8_t dictionary_buffer[1024];
    struct memory_sink sink;
    struct vp_stream_writer writer;
    struct vp_stream_writer_config config;
    struct vp_stream_writer_stats stats;
    size_t drained = 99u;

    init_context(&context, slots, &source);
    init_names(&names, entries, text, &zone_id, &counter_id);
    CHECK(vp_counter(&context, counter_id, 7) == VP_RESULT_OK,
          "queue sink-failure fixture");
    memset(&sink, 0, sizeof(sink));
    sink.fail_call = 3u; /* dictionary and VPRF header succeed; event fails. */
    memset(&config, 0, sizeof(config));
    config.context = &context;
    config.names = &names;
    config.write = memory_write;
    config.write_user = &sink;
    config.dictionary_buffer = dictionary_buffer;
    config.dictionary_buffer_capacity = sizeof(dictionary_buffer);
    CHECK(vp_stream_writer_init(&writer, &config) == VP_RESULT_OK &&
              vp_stream_writer_begin(&writer, 1u) == VP_RESULT_OK,
          "start sink-failure fixture");
    CHECK(vp_stream_writer_drain(&writer, 1u, &drained) == VP_ERROR_IO &&
              drained == 0u,
          "sink failure is returned without claiming delivery");
    CHECK(vp_stream_writer_get_stats(&writer, &stats) == VP_RESULT_OK &&
              stats.state == VP_STREAM_WRITER_FAILED &&
              stats.events_written == 0u &&
              stats.events_lost_to_sink == 1u,
          "failed drained event is explicitly accounted");
    CHECK(vp_stream_writer_drain(&writer, 1u, &drained) == VP_ERROR_IO &&
              vp_stream_writer_close(&writer) == VP_ERROR_IO,
          "failed writer cannot resume or claim clean close");
}

static void test_v2_incremental_session_round_trip(void)
{
    struct vp_context context;
    struct vp_slot slots[8];
    struct fake_source source = {1000u, 7u};
    struct vp_name_dictionary names;
    struct vp_name_entry entries[4];
    char text[128];
    uint32_t zone_id = 0u;
    uint32_t counter_id = 0u;
    uint8_t dictionary_buffer[1024];
    struct memory_sink sink;
    struct vp_stream_writer_v2 writer;
    struct vp_stream_writer_config config;
    struct vp_stream_v2_session session;
    struct vp_stream_v2_thread thread;
    struct vp_stream_v2_module module;
    struct vp_stream_v2_cursor cursor;
    struct vp_stream_v2_info info;
    struct vp_stream_v2_chunk_view chunk;
    struct vp_event event;
    struct vp_zone_scope zone;
    size_t drained = 0u;
    size_t event_chunks = 0u;
    size_t decoded_events = 0u;
    uint64_t end_bytes_written = 0u;

    memset(&sink, 0, sizeof(sink));
    init_context(&context, slots, &source);
    init_names(&names, entries, text, &zone_id, &counter_id);
    CHECK(vp_zone_begin(&context, zone_id, &zone) == VP_RESULT_OK,
          "record v2 zone begin");
    source.now = 1250u;
    CHECK(vp_counter(&context, counter_id, 42) == VP_RESULT_OK &&
              vp_zone_end(&context, &zone) == VP_RESULT_OK,
          "record v2 remaining events");

    memset(&config, 0, sizeof(config));
    config.context = &context;
    config.names = &names;
    config.write = memory_write;
    config.write_user = &sink;
    config.dictionary_buffer = dictionary_buffer;
    config.dictionary_buffer_capacity = sizeof(dictionary_buffer);
    memset(&session, 0, sizeof(session));
    session.session_id = UINT64_C(0x0102030405060708);
    session.process_id = 73u;
    session.process_identity = UINT64_C(0x1122334455667788);
    session.timer_source = "sceKernelGetProcessTimeWide";
    session.timer_unit = "microseconds";
    session.flags = VP_STREAM_V2_SESSION_PROCESS_ID |
                    VP_STREAM_V2_SESSION_PROCESS_IDENTITY |
                    VP_STREAM_V2_SESSION_TIMER_SOURCE |
                    VP_STREAM_V2_SESSION_TIMER_UNIT;
    memset(&thread, 0, sizeof(thread));
    thread.thread_id = 7u;
    thread.generation = 2u;
    thread.identity = UINT64_C(0xaabbccdd00000002);
    thread.name_id = zone_id;
    thread.flags = VP_STREAM_V2_THREAD_IDENTITY;
    memset(&module, 0, sizeof(module));
    module.module_id = 4u;
    module.generation = 1u;
    module.address_start = UINT32_C(0x81000000);
    module.address_end = UINT32_C(0x81010000);
    module.name_id = zone_id;
    module.flags = VP_STREAM_V2_MODULE_EXECUTABLE |
                   VP_STREAM_V2_MODULE_ARM |
                   VP_STREAM_V2_MODULE_THUMB;

    CHECK(vp_stream_writer_init_v2(&writer, &config) == VP_RESULT_OK &&
              vp_stream_writer_begin_v2(&writer, 900u, &session) ==
                  VP_RESULT_OK &&
              vp_stream_writer_write_thread_v2(&writer, &thread) ==
                  VP_RESULT_OK &&
              vp_stream_writer_write_module_v2(&writer, &module) ==
                  VP_RESULT_OK,
          "begin v2 session and publish supplied identity metadata");
    CHECK(vp_stream_writer_write_thread_v2(&writer, &thread) ==
              VP_ERROR_INVALID_ARGUMENT &&
              vp_stream_writer_write_module_v2(&writer, &module) ==
                  VP_ERROR_INVALID_ARGUMENT,
          "v2 writer rejects duplicate metadata generations");
    for (uint32_t index = 1u;
         index < VP_STREAM_V2_MAX_THREAD_METADATA; ++index) {
        thread.thread_id = 100u + index;
        thread.generation = index;
        CHECK(vp_stream_writer_write_thread_v2(&writer, &thread) ==
                  VP_RESULT_OK,
              "v2 writer accepts metadata within the session bound");
    }
    thread.thread_id = 1000u;
    thread.generation = 1000u;
    CHECK(vp_stream_writer_write_thread_v2(&writer, &thread) ==
              VP_ERROR_CAPACITY,
          "v2 writer enforces the thread metadata bound");
    CHECK(vp_stream_writer_drain_v2(
              &writer, 2u, &drained) == VP_RESULT_OK &&
              drained == 2u &&
              vp_stream_writer_write_stats_v2(&writer) == VP_RESULT_OK,
          "publish a bounded live v2 event chunk and stats snapshot");
    CHECK(vp_stream_writer_drain_v2(
              &writer, 8u, &drained) == VP_RESULT_OK &&
              drained == 1u &&
              vp_stream_writer_set_transport_loss_v2(
                  &writer, UINT32_MAX) == VP_RESULT_OK &&
              vp_stream_writer_write_stats_v2(&writer) == VP_RESULT_OK &&
              vp_stream_writer_set_transport_loss_v2(&writer, 2u) ==
                  VP_RESULT_OK &&
              vp_stream_writer_close_v2(&writer) == VP_RESULT_OK,
          "close v2 session after wrapping transport loss counter");

    CHECK(vp_stream_v2_cursor_init(
              &cursor, sink.data, sink.used, &info) == VP_RESULT_OK &&
              info.complete == 1u &&
              info.session_id == session.session_id &&
              info.event_count == 3u,
          "v2 receiver validates the complete lifecycle");
    while (vp_stream_v2_cursor_next(&cursor, &chunk) == VP_RESULT_OK) {
        if (chunk.header.type != VP_STREAM_V2_CHUNK_EVENTS)
        {
            if (chunk.header.type == VP_STREAM_V2_CHUNK_END)
                end_bytes_written = read_u64_le(chunk.payload + 24);
            continue;
        }
        ++event_chunks;
        for (size_t offset = 0u;
             offset < chunk.header.payload_size;
             offset += VP_WIRE_EVENT_SIZE) {
            CHECK(vp_decode_event_le(
                      chunk.payload + offset,
                      chunk.header.payload_size - offset, &event) ==
                      VP_RESULT_OK,
                  "decode event from v2 chunk");
            ++decoded_events;
        }
    }
    CHECK(event_chunks == 2u && decoded_events == 3u &&
              end_bytes_written == sink.used,
          "v2 chunks expose incremental event batches");

    {
        uint8_t damaged[16384];
        size_t chunk_offset = 0u;
        size_t dictionary_offset = 0u;
        size_t thread_offset = 0u;
        size_t stats_offset = 0u;
        size_t end_offset = 0u;
        size_t last_chunk_size =
            VP_STREAM_V2_CHUNK_HEADER_SIZE +
            VP_STREAM_V2_STATS_PAYLOAD_SIZE;
        memcpy(damaged, sink.data, sink.used);
        while (chunk_offset < sink.used) {
            uint32_t chunk_type =
                (uint32_t)damaged[chunk_offset + 8u] |
                ((uint32_t)damaged[chunk_offset + 9u] << 8u);
            if (chunk_type == VP_STREAM_V2_CHUNK_DICTIONARY)
                dictionary_offset = chunk_offset;
            if (chunk_type == VP_STREAM_V2_CHUNK_THREAD &&
                thread_offset == 0u)
                thread_offset = chunk_offset;
            if (chunk_type == VP_STREAM_V2_CHUNK_STATS &&
                stats_offset == 0u)
                stats_offset = chunk_offset;
            if (chunk_type == VP_STREAM_V2_CHUNK_END)
                end_offset = chunk_offset;
            chunk_offset += VP_STREAM_V2_CHUNK_HEADER_SIZE +
                            read_u32_le(damaged + chunk_offset + 12u);
        }
        damaged[VP_STREAM_V2_CHUNK_HEADER_SIZE + 40u] ^= 1u;
        CHECK(vp_stream_v2_cursor_init(
                  &cursor, damaged, sink.used, NULL) ==
                  VP_ERROR_MALFORMED,
              "v2 payload CRC corruption fails closed");
        memcpy(damaged, sink.data, sink.used);
        damaged[4] = (uint8_t)(VP_STREAM_V2_VERSION + 1u);
        CHECK(vp_stream_v2_cursor_init(
                  &cursor, damaged, sink.used, NULL) ==
                  VP_ERROR_UNSUPPORTED,
              "v2 chunk version mismatch fails closed");
        CHECK(vp_stream_v2_cursor_init(
                  &cursor, sink.data, sink.used - 1u, NULL) ==
                  VP_ERROR_MALFORMED &&
                  vp_stream_v2_cursor_init(
                      &cursor, sink.data,
                      sink.used - last_chunk_size, NULL) ==
                      VP_ERROR_MALFORMED,
              "truncated and incomplete v2 sessions fail closed");
        memcpy(damaged, sink.data, sink.used);
        write_u64_le(
            damaged + stats_offset + VP_STREAM_V2_CHUNK_HEADER_SIZE + 16u,
            3u);
        refresh_chunk_crc(damaged + stats_offset);
        CHECK(vp_stream_v2_cursor_init(
                  &cursor, damaged, sink.used, NULL) == VP_ERROR_MALFORMED,
              "intermediate stats cannot claim unseen events");
        memcpy(damaged, sink.data, sink.used);
        write_u64_le(
            damaged + end_offset + VP_STREAM_V2_CHUNK_HEADER_SIZE + 24u,
            sink.used - 1u);
        refresh_chunk_crc(damaged + end_offset);
        CHECK(vp_stream_v2_cursor_init(
                  &cursor, damaged, sink.used, NULL) == VP_ERROR_MALFORMED,
              "END byte count must include its own complete chunk");
        memcpy(damaged, sink.data, sink.used);
        damaged[stats_offset + 8u] = VP_STREAM_V2_CHUNK_THREAD;
        damaged[stats_offset + 9u] = 0u;
        memcpy(damaged + stats_offset + VP_STREAM_V2_CHUNK_HEADER_SIZE,
               damaged + thread_offset + VP_STREAM_V2_CHUNK_HEADER_SIZE,
               VP_STREAM_V2_THREAD_PAYLOAD_SIZE);
        refresh_chunk_crc(damaged + stats_offset);
        CHECK(vp_stream_v2_cursor_init(
                  &cursor, damaged, sink.used, NULL) == VP_ERROR_MALFORMED,
              "v2 cursor rejects duplicate metadata generations");
        memcpy(damaged, sink.data, sink.used);
        {
            uint32_t dictionary_size =
                read_u32_le(damaged + dictionary_offset + 12u);
            size_t insertion =
                dictionary_offset + VP_STREAM_V2_CHUNK_HEADER_SIZE +
                dictionary_size;
            memmove(damaged + insertion + 1u, damaged + insertion,
                    sink.used - insertion);
            damaged[insertion] = 0u;
            write_u32_le(
                damaged + dictionary_offset + 12u, dictionary_size + 1u);
            refresh_chunk_crc(damaged + dictionary_offset);
            write_u64_le(
                damaged + end_offset + 1u +
                    VP_STREAM_V2_CHUNK_HEADER_SIZE + 24u,
                sink.used + 1u);
            refresh_chunk_crc(damaged + end_offset + 1u);
            CHECK(vp_stream_v2_cursor_init(
                      &cursor, damaged, sink.used + 1u, NULL) ==
                      VP_ERROR_MALFORMED,
                  "v2 dictionary chunk rejects trailing payload bytes");
        }
    }
}

static void test_v2_drain_caps_oversized_staging_buffer(void)
{
    const uint32_t slot_count = UINT32_C(131072);
    const size_t maximum_events =
        VP_STREAM_V2_MAX_CHUNK_PAYLOAD / VP_WIRE_EVENT_SIZE;
    struct vp_slot* slots =
        (struct vp_slot*)calloc(slot_count, sizeof(*slots));
    uint8_t* staging =
        (uint8_t*)malloc(VP_STREAM_V2_MAX_CHUNK_PAYLOAD +
                         VP_WIRE_EVENT_SIZE);
    struct vp_context context;
    struct vp_config context_config;
    struct fake_source source = {1u, 2u};
    struct vp_name_dictionary names;
    struct vp_name_entry entries[4];
    char text[128];
    uint32_t zone_id = 0u;
    uint32_t counter_id = 0u;
    struct counting_sink sink;
    struct vp_stream_writer_v2 writer;
    struct vp_stream_writer_config config;
    struct vp_stream_v2_session session;
    struct vp_stats ring;
    size_t drained = 0u;

    CHECK(slots != NULL && staging != NULL,
          "allocate oversized-staging regression storage");
    if (slots == NULL || staging == NULL) {
        free(slots);
        free(staging);
        return;
    }
    memset(&sink, 0, sizeof(sink));
    memset(&context_config, 0, sizeof(context_config));
    context_config.slots = slots;
    context_config.capacity = slot_count;
    context_config.clock = fake_clock;
    context_config.clock_user = &source;
    context_config.thread_id = fake_thread;
    context_config.thread_user = &source;
    CHECK(vp_init(&context, &context_config) == VP_RESULT_OK,
          "initialize large ring for staging cap");
    init_names(&names, entries, text, &zone_id, &counter_id);
    for (size_t index = 0u; index < maximum_events + 1u; ++index)
        CHECK(vp_counter(&context, counter_id, (int64_t)index) ==
                  VP_RESULT_OK,
              "fill large ring for staging cap");

    memset(&config, 0, sizeof(config));
    config.context = &context;
    config.names = &names;
    config.write = counting_write;
    config.write_user = &sink;
    config.dictionary_buffer = staging;
    config.dictionary_buffer_capacity =
        VP_STREAM_V2_MAX_CHUNK_PAYLOAD + VP_WIRE_EVENT_SIZE;
    memset(&session, 0, sizeof(session));
    session.session_id = 1u;
    CHECK(vp_stream_writer_init_v2(&writer, &config) == VP_RESULT_OK &&
              vp_stream_writer_begin_v2(&writer, 0u, &session) ==
                  VP_RESULT_OK &&
              vp_stream_writer_drain_v2(&writer, SIZE_MAX, &drained) ==
                  VP_RESULT_OK &&
              drained == maximum_events &&
              sink.largest_write <= VP_STREAM_V2_MAX_CHUNK_PAYLOAD &&
              vp_get_stats(&context, &ring) == VP_RESULT_OK &&
              ring.pending == 1u,
          "oversized staging is capped before events leave the ring");
    CHECK(vp_stream_writer_drain_v2(&writer, SIZE_MAX, &drained) ==
                  VP_RESULT_OK &&
              drained == 1u &&
              vp_stream_writer_close_v2(&writer) == VP_RESULT_OK,
          "remaining event drains into a second valid v2 chunk");
    vp_name_dictionary_deinit(&names);
    vp_deinit(&context);
    free(staging);
    free(slots);
}

int main(void)
{
    test_legacy_writer_canaries();
    test_combined_stream_round_trip();
    test_fail_closed_sink_lifecycle();
    test_v2_incremental_session_round_trip();
    test_v2_drain_caps_oversized_staging_buffer();
    if (failures != 0) {
        fprintf(stderr, "%d profiler stream test(s) failed\n", failures);
        return 1;
    }
    puts("vitaprofiler stream: all native tests passed");
    return 0;
}

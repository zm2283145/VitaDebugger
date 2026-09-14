#include "vitaprofiler_stream.h"

#include <stdio.h>
#include <string.h>

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

struct memory_sink {
    uint8_t data[4096];
    size_t used;
    uint32_t calls;
    uint32_t fail_call;
};

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

int main(void)
{
    test_combined_stream_round_trip();
    test_fail_closed_sink_lifecycle();
    if (failures != 0) {
        fprintf(stderr, "%d profiler stream test(s) failed\n", failures);
        return 1;
    }
    puts("vitaprofiler stream: all native tests passed");
    return 0;
}

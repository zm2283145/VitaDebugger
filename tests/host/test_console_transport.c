#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uvdb_console.h"
#include "uvdb_console_transport.h"

#define CHECK(condition) do { \
    if(!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while(0)

#define FAKE_FRAME_CAPACITY 80u

enum fake_mode {
    FAKE_COMPLETE,
    FAKE_WOULD_BLOCK,
    FAKE_WOULD_BLOCK_PARTIAL,
    FAKE_PARTIAL,
    FAKE_COMPLETE_ZERO,
    FAKE_COMPLETE_OVERSIZED,
    FAKE_HARD_ERROR,
    FAKE_ERROR_PARTIAL,
    FAKE_ROTATE_SESSION,
};

struct fake_writer {
    enum fake_mode mode;
    unsigned int calls;
    unsigned int stored;
    unsigned char frame[FAKE_FRAME_CAPACITY][UVDB_CONSOLE_RSP_FRAME_MAX];
    size_t size[FAKE_FRAME_CAPACITY];
    int lock_queue_on_complete;
    struct uvdb_console_transport* rotate_transport;
};

static enum uvdb_console_write_result fake_write(
    void* context,
    const void* data,
    size_t size,
    size_t* bytes_sent,
    int* native_error)
{
    struct fake_writer* writer = context;
    writer->calls++;
    *bytes_sent = 0;
    *native_error = 0;
    if(writer->stored < FAKE_FRAME_CAPACITY)
    {
        memcpy(writer->frame[writer->stored], data, size);
        writer->size[writer->stored] = size;
        writer->stored++;
    }

    if(writer->mode == FAKE_WOULD_BLOCK)
    {
        *native_error = 0x1234;
        return UVDB_CONSOLE_WRITE_WOULD_BLOCK;
    }
    if(writer->mode == FAKE_WOULD_BLOCK_PARTIAL)
    {
        *bytes_sent = size ? 1u : 0;
        *native_error = 0x2345;
        return UVDB_CONSOLE_WRITE_WOULD_BLOCK;
    }
    if(writer->mode == FAKE_PARTIAL)
    {
        *bytes_sent = size ? size - 1u : 0;
        return UVDB_CONSOLE_WRITE_COMPLETE;
    }
    if(writer->mode == FAKE_COMPLETE_ZERO)
        return UVDB_CONSOLE_WRITE_COMPLETE;
    if(writer->mode == FAKE_COMPLETE_OVERSIZED)
    {
        *bytes_sent = size + 1u;
        return UVDB_CONSOLE_WRITE_COMPLETE;
    }
    if(writer->mode == FAKE_HARD_ERROR)
    {
        *native_error = -77;
        return UVDB_CONSOLE_WRITE_ERROR;
    }
    if(writer->mode == FAKE_ERROR_PARTIAL)
    {
        *bytes_sent = size ? 1u : 0;
        *native_error = -88;
        return UVDB_CONSOLE_WRITE_ERROR;
    }
    if(writer->mode == FAKE_ROTATE_SESSION)
    {
        struct uvdb_console_transport* transport = writer->rotate_transport;
        CHECK(transport != NULL);
        CHECK(uvdb_console_transport_end_connection(transport) ==
              UVDB_CONSOLE_READY);
        CHECK(uvdb_console_transport_begin_connection(transport) ==
              UVDB_CONSOLE_READY);
        CHECK(uvdb_console_transport_enable_no_ack(transport) ==
              UVDB_CONSOLE_READY);
        CHECK(uvdb_console_capture("new", 3) == 3);
    }
    if(writer->lock_queue_on_complete)
        CHECK(uvdb_console_test_lock_queue());
    *bytes_sent = size;
    return UVDB_CONSOLE_WRITE_COMPLETE;
}

static int hex_value(unsigned char value)
{
    if(value >= '0' && value <= '9')
        return value - '0';
    if(value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}

static int decode_frame(
    const unsigned char* frame,
    size_t frame_size,
    unsigned char* output,
    size_t capacity,
    size_t* output_size)
{
    if(!frame || !output_size || frame_size < 6 || frame[0] != '$')
        return -1;
    size_t hash = frame_size - 3u;
    if(frame[hash] != '#' || frame[1] != 'O' || ((hash - 2u) & 1u))
        return -1;

    unsigned int checksum = 0;
    for(size_t index = 1; index < hash; ++index)
        checksum += frame[index];
    int high = hex_value(frame[hash + 1u]);
    int low = hex_value(frame[hash + 2u]);
    if(high < 0 || low < 0 || ((checksum & 0xffu) !=
                               (unsigned int)((high << 4) | low)))
        return -1;

    size_t decoded = (hash - 2u) / 2u;
    *output_size = decoded;
    if(decoded > capacity || (decoded && !output))
        return -1;
    for(size_t index = 0; index < decoded; ++index)
    {
        high = hex_value(frame[2u + index * 2u]);
        low = hex_value(frame[3u + index * 2u]);
        if(high < 0 || low < 0)
            return -1;
        output[index] = (unsigned char)((high << 4) | low);
    }
    return 0;
}

static void setup(
    struct uvdb_console_transport* transport,
    struct fake_writer* writer)
{
    uvdb_console_reset();
    uvdb_console_transport_init(transport);
    memset(writer, 0, sizeof(*writer));
    writer->mode = FAKE_COMPLETE;
}

static int test_invalid_and_idle_states(void)
{
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    struct uvdb_console_transport_stats stats;
    setup(&transport, &writer);

    uvdb_console_transport_init(NULL);
    CHECK(uvdb_console_transport_begin_connection(NULL) ==
          UVDB_CONSOLE_ERROR);
    CHECK(uvdb_console_transport_enable_no_ack(NULL) ==
          UVDB_CONSOLE_ERROR);
    CHECK(uvdb_console_transport_end_connection(NULL) ==
          UVDB_CONSOLE_ERROR);
    CHECK(!uvdb_console_transport_no_ack(NULL));
    CHECK(uvdb_console_transport_pump(NULL, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_FATAL);
    CHECK(uvdb_console_transport_pump(&transport, NULL, &writer) ==
          UVDB_CONSOLE_PUMP_FATAL);
    CHECK(uvdb_console_transport_get_stats(NULL, &stats) < 0);
    CHECK(uvdb_console_transport_get_stats(&transport, NULL) < 0);
    CHECK(writer.calls == 0);

    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_IDLE);
    CHECK(writer.calls == 0);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_IDLE);
    CHECK(writer.calls == 0);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_IDLE);
    CHECK(writer.calls == 0);
    return 0;
}

static int test_golden_frame_checksum(void)
{
    static const unsigned char input[] = {'A', '\n'};
    static const unsigned char expected[] = "$O410a#45";
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    setup(&transport, &writer);

    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_capture(input, sizeof(input)) == sizeof(input));
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_SENT);
    CHECK(writer.calls == 1 && writer.stored == 1);
    CHECK(writer.size[0] == sizeof(expected) - 1u);
    CHECK(memcmp(writer.frame[0], expected, sizeof(expected) - 1u) == 0);

    struct uvdb_console_stats queue_stats;
    struct uvdb_console_transport_stats transport_stats;
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(uvdb_console_transport_get_stats(&transport, &transport_stats) == 0);
    CHECK(queue_stats.sent_records == 1);
    CHECK(queue_stats.sent_bytes == sizeof(input));
    CHECK(queue_stats.queued_records == 0);
    CHECK(transport_stats.frames_sent == 1);
    CHECK(transport_stats.frame_bytes_sent == sizeof(expected) - 1u);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    return 0;
}

static int test_session_gate(void)
{
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    setup(&transport, &writer);

    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(!uvdb_console_transport_no_ack(&transport));
    CHECK(uvdb_console_capture("before", 6) == 0);
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_IDLE);
    CHECK(writer.calls == 0);

    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_no_ack(&transport));
    uint32_t first_generation = transport.generation;
    CHECK(first_generation != 0);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(transport.generation == first_generation);
    CHECK(uvdb_console_capture("live", 4) == 4);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(!uvdb_console_transport_no_ack(&transport));
    CHECK(transport.generation == 0);
    CHECK(uvdb_console_capture("after", 5) == 0);

    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(transport.generation != first_generation);

    struct uvdb_console_stats stats;
    CHECK(uvdb_console_get_stats(&stats) == 0);
    CHECK(stats.sessions_opened == 2);
    CHECK(stats.reconnects == 1);
    CHECK(stats.accepted_records == 1 && stats.accepted_bytes == 4);
    CHECK(stats.dropped_disconnected_records == 2);
    CHECK(stats.dropped_disconnected_bytes == 11);
    CHECK(stats.dropped_stale_records == 1);
    CHECK(stats.dropped_stale_bytes == 4);
    CHECK(stats.queued_records == 0 && stats.queued_bytes == 0);
    CHECK(stats.session_open == 1);
    CHECK(stats.session_generation == transport.generation);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    return 0;
}

static int test_begin_connection_replaces_active_session(void)
{
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    setup(&transport, &writer);

    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    uint32_t first_generation = transport.generation;
    CHECK(first_generation != 0);
    CHECK(uvdb_console_capture("old", 3) == 3);

    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(!uvdb_console_transport_no_ack(&transport));
    CHECK(transport.generation == 0);
    CHECK(uvdb_console_capture("gap", 3) == 0);
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_IDLE);
    CHECK(writer.calls == 0);

    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(transport.generation != 0);
    CHECK(transport.generation != first_generation);
    CHECK(uvdb_console_capture("new", 3) == 3);
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_SENT);

    unsigned char decoded[4];
    size_t size = 0;
    CHECK(writer.stored == 1);
    CHECK(decode_frame(writer.frame[0], writer.size[0], decoded,
                       sizeof(decoded), &size) == 0);
    CHECK(size == 3 && memcmp(decoded, "new", 3) == 0);

    struct uvdb_console_stats stats;
    CHECK(uvdb_console_get_stats(&stats) == 0);
    CHECK(stats.sessions_opened == 2 && stats.reconnects == 1);
    CHECK(stats.accepted_records == 2 && stats.accepted_bytes == 6);
    CHECK(stats.sent_records == 1 && stats.sent_bytes == 3);
    CHECK(stats.dropped_disconnected_records == 1 &&
          stats.dropped_disconnected_bytes == 3);
    CHECK(stats.dropped_stale_records == 1 &&
          stats.dropped_stale_bytes == 3);
    CHECK(stats.queued_records == 0 && stats.queued_bytes == 0);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    return 0;
}

static int test_close_busy_still_closes_gate(void)
{
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    setup(&transport, &writer);
    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_capture("old", 3) == 3);
    CHECK(uvdb_console_test_lock_lifecycle());
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_BUSY);

    struct uvdb_console_stats stats;
    CHECK(uvdb_console_get_stats(&stats) == 0);
    CHECK(stats.session_open == 0);
    CHECK(stats.queued_records == 1);
    CHECK(uvdb_console_capture("closed", 6) == 0);
    uvdb_console_test_unlock_lifecycle();

    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_get_stats(&stats) == 0);
    CHECK(stats.queued_records == 0);
    CHECK(stats.dropped_stale_bytes == 3);
    CHECK(stats.dropped_disconnected_bytes == 6);
    CHECK(stats.sessions_opened == 2 && stats.reconnects == 1);
    CHECK(stats.session_open == 1);
    CHECK(stats.session_generation == transport.generation);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    return 0;
}

static int test_fifo_binary_frames(void)
{
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    setup(&transport, &writer);
    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);

    unsigned char input[UVDB_CONSOLE_RECORD_MAX * 2u + 7u];
    for(size_t index = 0; index < sizeof(input); ++index)
        input[index] = (unsigned char)(index * 73u);
    input[0] = 0;
    input[1] = '$';
    input[2] = '#';
    input[3] = '}';
    input[4] = 0x80;
    input[5] = 0xff;
    CHECK(uvdb_console_capture(input, sizeof(input)) == sizeof(input));

    for(unsigned int record = 0; record < 3; ++record)
        CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
              UVDB_CONSOLE_PUMP_SENT);
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_IDLE);
    CHECK(writer.calls == 3);
    CHECK(writer.stored == 3);

    unsigned char decoded[sizeof(input)];
    size_t decoded_total = 0;
    uint32_t frame_bytes = 0;
    for(unsigned int index = 0; index < writer.stored; ++index)
    {
        size_t chunk = 0;
        CHECK(decode_frame(writer.frame[index], writer.size[index],
                           decoded + decoded_total,
                           sizeof(decoded) - decoded_total, &chunk) == 0);
        decoded_total += chunk;
        frame_bytes += (uint32_t)writer.size[index];
    }
    CHECK(decoded_total == sizeof(input));
    CHECK(memcmp(decoded, input, sizeof(input)) == 0);
    CHECK(writer.size[0] == UVDB_CONSOLE_RSP_FRAME_MAX);

    struct uvdb_console_stats queue_stats;
    struct uvdb_console_transport_stats transport_stats;
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(uvdb_console_transport_get_stats(&transport, &transport_stats) == 0);
    CHECK(queue_stats.sent_records == 3);
    CHECK(queue_stats.sent_bytes == sizeof(input));
    CHECK(queue_stats.queued_records == 0);
    CHECK(transport_stats.frames_sent == 3);
    CHECK(transport_stats.frame_bytes_sent == frame_bytes);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    return 0;
}

static int test_one_record_per_pump_and_would_block(void)
{
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    setup(&transport, &writer);
    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_capture("one", 3) == 3);
    CHECK(uvdb_console_capture("two", 3) == 3);

    writer.mode = FAKE_WOULD_BLOCK;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_WOULD_BLOCK);
    CHECK(writer.calls == 1);
    struct uvdb_console_stats before;
    CHECK(uvdb_console_get_stats(&before) == 0);
    CHECK(before.queued_records == 2 && before.sent_records == 0);

    writer.mode = FAKE_COMPLETE;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_SENT);
    CHECK(writer.calls == 2);
    CHECK(writer.size[0] == writer.size[1]);
    CHECK(memcmp(writer.frame[0], writer.frame[1], writer.size[0]) == 0);
    struct uvdb_console_stats after;
    CHECK(uvdb_console_get_stats(&after) == 0);
    CHECK(after.queued_records == 1 && after.sent_records == 1);

    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_SENT);
    CHECK(writer.calls == 3);
    CHECK(uvdb_console_get_stats(&after) == 0);
    CHECK(after.queued_records == 0 && after.sent_records == 2);

    struct uvdb_console_transport_stats stats;
    CHECK(uvdb_console_transport_get_stats(&transport, &stats) == 0);
    CHECK(stats.would_block == 1);
    CHECK(stats.last_native_error == 0x1234);
    CHECK(stats.frames_sent == 2);
    CHECK(stats.frame_bytes_sent == writer.size[1] + writer.size[2]);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    return 0;
}

static int test_sent_commit_busy_does_not_duplicate(void)
{
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    setup(&transport, &writer);
    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_capture("first", 5) == 5);
    CHECK(uvdb_console_capture("second", 6) == 6);

    writer.lock_queue_on_complete = 1;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_SENT);
    CHECK(writer.calls == 1);
    CHECK(transport.sent_uncommitted_sequence != 0);
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_BUSY);
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_BUSY);
    CHECK(writer.calls == 1);

    struct uvdb_console_stats queue_stats;
    struct uvdb_console_transport_stats stats;
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(uvdb_console_transport_get_stats(&transport, &stats) == 0);
    CHECK(queue_stats.queued_records == 2 && queue_stats.sent_records == 0);
    CHECK(stats.commit_busy == 3);
    CHECK(stats.frames_sent == 1);

    uvdb_console_test_unlock_queue();
    writer.lock_queue_on_complete = 0;

    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_COMMITTED);
    CHECK(writer.calls == 1);
    CHECK(transport.sent_uncommitted_sequence == 0);
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(queue_stats.queued_records == 1 && queue_stats.queued_bytes == 6);
    CHECK(queue_stats.sent_records == 1 && queue_stats.sent_bytes == 5);
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_SENT);
    CHECK(writer.calls == 2);

    unsigned char decoded[16];
    size_t size = 0;
    CHECK(decode_frame(writer.frame[0], writer.size[0], decoded,
                       sizeof(decoded), &size) == 0);
    CHECK(size == 5 && memcmp(decoded, "first", 5) == 0);
    CHECK(decode_frame(writer.frame[1], writer.size[1], decoded,
                       sizeof(decoded), &size) == 0);
    CHECK(size == 6 && memcmp(decoded, "second", 6) == 0);

    CHECK(uvdb_console_transport_get_stats(&transport, &stats) == 0);
    CHECK(stats.commit_busy == 3);
    CHECK(stats.frames_sent == 2);
    CHECK(stats.frame_bytes_sent == writer.size[0] + writer.size[1]);
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(queue_stats.queued_records == 0 && queue_stats.queued_bytes == 0);
    CHECK(queue_stats.sent_records == 2 && queue_stats.sent_bytes == 11);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    return 0;
}

static int run_fatal_write_case(
    enum fake_mode mode,
    uint32_t expected_partial_writes,
    uint32_t expected_hard_errors,
    int expected_native_error)
{
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    setup(&transport, &writer);
    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_capture("x", 1) == 1);

    writer.mode = mode;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_FATAL);
    CHECK(transport.failed);
    CHECK(writer.calls == 1 && writer.stored == 1);
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_FATAL);
    CHECK(writer.calls == 1);

    struct uvdb_console_stats queue_stats;
    struct uvdb_console_transport_stats transport_stats;
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(uvdb_console_transport_get_stats(&transport, &transport_stats) == 0);
    CHECK(queue_stats.queued_records == 1 && queue_stats.queued_bytes == 1);
    CHECK(queue_stats.sent_records == 0 && queue_stats.sent_bytes == 0);
    CHECK(transport_stats.frames_sent == 0);
    CHECK(transport_stats.frame_bytes_sent == 0);
    CHECK(transport_stats.partial_writes == expected_partial_writes);
    CHECK(transport_stats.hard_errors == expected_hard_errors);
    CHECK(transport_stats.last_native_error == expected_native_error);

    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(!transport.failed);
    CHECK(!uvdb_console_transport_no_ack(&transport));
    CHECK(transport.generation == 0);
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(queue_stats.session_open == 0);
    CHECK(queue_stats.queued_records == 0 && queue_stats.queued_bytes == 0);
    CHECK(queue_stats.dropped_stale_records == 1 &&
          queue_stats.dropped_stale_bytes == 1);
    return 0;
}

static int test_invalid_write_result_matrix(void)
{
    CHECK(run_fatal_write_case(FAKE_WOULD_BLOCK_PARTIAL, 1, 0,
                               0x2345) == 0);
    CHECK(run_fatal_write_case(FAKE_PARTIAL, 1, 0, 0) == 0);
    CHECK(run_fatal_write_case(FAKE_COMPLETE_ZERO, 1, 0, 0) == 0);
    CHECK(run_fatal_write_case(FAKE_COMPLETE_OVERSIZED, 1, 0, 0) == 0);
    CHECK(run_fatal_write_case(FAKE_HARD_ERROR, 0, 1, -77) == 0);
    CHECK(run_fatal_write_case(FAKE_ERROR_PARTIAL, 1, 0, -88) == 0);
    return 0;
}

static int test_send_failures_and_reconnect(void)
{
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    setup(&transport, &writer);
    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    uint32_t old_generation = transport.generation;
    CHECK(old_generation != 0);
    CHECK(uvdb_console_capture("old-partial", 11) == 11);

    writer.mode = FAKE_PARTIAL;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_FATAL);
    unsigned int failed_calls = writer.calls;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_FATAL);
    CHECK(writer.calls == failed_calls);
    struct uvdb_console_stats queue_stats;
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(queue_stats.sent_records == 0 && queue_stats.queued_records == 1);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(queue_stats.session_open == 0);
    CHECK(queue_stats.queued_records == 0 && queue_stats.queued_bytes == 0);
    CHECK(queue_stats.dropped_stale_records == 1 &&
          queue_stats.dropped_stale_bytes == 11);

    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(transport.generation != 0);
    CHECK(transport.generation != old_generation);
    CHECK(uvdb_console_capture("new", 3) == 3);
    writer.mode = FAKE_COMPLETE;
    writer.stored = 0;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_SENT);
    unsigned char decoded[16];
    size_t size = 0;
    CHECK(decode_frame(writer.frame[0], writer.size[0], decoded,
                       sizeof(decoded), &size) == 0);
    CHECK(size == 3 && memcmp(decoded, "new", 3) == 0);
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(queue_stats.sessions_opened == 2 && queue_stats.reconnects == 1);
    CHECK(queue_stats.sent_records == 1 && queue_stats.sent_bytes == 3);
    CHECK(queue_stats.queued_records == 0 && queue_stats.queued_bytes == 0);
    CHECK(queue_stats.dropped_stale_records == 1 &&
          queue_stats.dropped_stale_bytes == 11);

    CHECK(uvdb_console_capture("hard", 4) == 4);
    writer.mode = FAKE_HARD_ERROR;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_FATAL);
    failed_calls = writer.calls;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_FATAL);
    CHECK(writer.calls == failed_calls);
    struct uvdb_console_transport_stats stats;
    CHECK(uvdb_console_transport_get_stats(&transport, &stats) == 0);
    CHECK(stats.partial_writes == 1);
    CHECK(stats.hard_errors == 1);
    CHECK(stats.last_native_error == -77);
    CHECK(stats.frames_sent == 1);
    CHECK(stats.frame_bytes_sent == writer.size[0]);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(queue_stats.session_open == 0);
    CHECK(queue_stats.queued_records == 0 && queue_stats.queued_bytes == 0);
    CHECK(queue_stats.sent_records == 1 && queue_stats.sent_bytes == 3);
    CHECK(queue_stats.dropped_stale_records == 2 &&
          queue_stats.dropped_stale_bytes == 15);
    return 0;
}

static int test_generation_change_during_send(void)
{
    struct uvdb_console_transport transport;
    struct fake_writer writer;
    setup(&transport, &writer);
    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_capture("old", 3) == 3);

    writer.mode = FAKE_ROTATE_SESSION;
    writer.rotate_transport = &transport;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_FATAL);
    CHECK(transport.sent_uncommitted_sequence == 0);
    CHECK(transport.failed);
    unsigned int failed_calls = writer.calls;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_FATAL);
    CHECK(writer.calls == failed_calls);

    struct uvdb_console_stats queue_stats;
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(queue_stats.queued_records == 1);
    CHECK(queue_stats.sent_records == 0);

    CHECK(uvdb_console_transport_begin_connection(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_transport_enable_no_ack(&transport) ==
          UVDB_CONSOLE_READY);
    CHECK(uvdb_console_capture("new", 3) == 3);
    writer.mode = FAKE_COMPLETE;
    writer.stored = 0;
    CHECK(uvdb_console_transport_pump(&transport, fake_write, &writer) ==
          UVDB_CONSOLE_PUMP_SENT);
    unsigned char decoded[8];
    size_t size = 0;
    CHECK(decode_frame(writer.frame[0], writer.size[0], decoded,
                       sizeof(decoded), &size) == 0);
    CHECK(size == 3 && memcmp(decoded, "new", 3) == 0);

    struct uvdb_console_transport_stats stats;
    CHECK(uvdb_console_transport_get_stats(&transport, &stats) == 0);
    CHECK(stats.session_errors == 1);
    CHECK(stats.frames_sent == 1);
    CHECK(uvdb_console_get_stats(&queue_stats) == 0);
    CHECK(queue_stats.sessions_opened == 3 && queue_stats.reconnects == 2);
    CHECK(queue_stats.accepted_records == 3 &&
          queue_stats.accepted_bytes == 9);
    CHECK(queue_stats.sent_records == 1 && queue_stats.sent_bytes == 3);
    CHECK(queue_stats.dropped_stale_records == 2 &&
          queue_stats.dropped_stale_bytes == 6);
    CHECK(queue_stats.queued_records == 0 && queue_stats.queued_bytes == 0);
    CHECK(uvdb_console_transport_end_connection(&transport) ==
          UVDB_CONSOLE_READY);
    return 0;
}

int main(void)
{
    CHECK(test_invalid_and_idle_states() == 0);
    CHECK(test_golden_frame_checksum() == 0);
    CHECK(test_session_gate() == 0);
    CHECK(test_begin_connection_replaces_active_session() == 0);
    CHECK(test_close_busy_still_closes_gate() == 0);
    CHECK(test_fifo_binary_frames() == 0);
    CHECK(test_one_record_per_pump_and_would_block() == 0);
    CHECK(test_sent_commit_busy_does_not_duplicate() == 0);
    CHECK(test_invalid_write_result_matrix() == 0);
    CHECK(test_send_failures_and_reconnect() == 0);
    CHECK(test_generation_change_during_send() == 0);
    puts("console transport tests passed");
    return 0;
}

#include "vitadebug_input_trace.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

struct playback_sink {
    struct vd_input_state inputs[8];
    size_t count;
    size_t fail_call;
};

static void write_u32(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static uint32_t crc_update(uint32_t crc, const uint8_t* data, size_t size)
{
    size_t index;

    for (index = 0u; index < size; ++index) {
        uint32_t bit;
        crc ^= data[index];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^
                  (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return crc;
}

static uint32_t trace_checksum(const uint8_t* data, size_t data_size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    crc = crc_update(crc, data, 72u);
    crc = crc_update(crc, data + 76u, data_size - 76u);
    return ~crc;
}

static int apply_input(void* user, const struct vd_input_state* input)
{
    struct playback_sink* sink = (struct playback_sink*)user;
    ++sink->count;
    if (sink->fail_call != 0u && sink->count == sink->fail_call)
        return -1;
    if (sink->count <= 8u)
        sink->inputs[sink->count - 1u] = *input;
    return 0;
}

static void configure(struct vd_input_trace_config* config,
                      uint8_t* buffer, size_t capacity,
                      uint32_t max_events)
{
    vd_input_trace_config_init(config);
    config->buffer = buffer;
    config->buffer_capacity = capacity;
    config->max_events = max_events;
    config->max_duration_us = 1000000u;
    config->max_scheduling_drift_us = 100u;
    memcpy(config->identity.title_id, "VDSCRN001", 10u);
    config->identity.process_id = 42u;
    config->identity.process_generation = 7u;
    config->identity.session_id = UINT64_C(0x1122334455667788);
}

static void test_round_trip_and_playback(void)
{
    uint8_t buffer[VD_INPUT_TRACE_HEADER_SIZE +
                   4u * VD_INPUT_TRACE_EVENT_SIZE];
    struct vd_input_trace_config config;
    struct vd_input_trace trace = {0};
    struct vd_input_trace_info info;
    struct vd_input_state first = {0};
    struct vd_input_state second = {0};
    struct playback_sink sink = {0};
    const uint8_t* data = NULL;
    size_t data_size = 0u;

    configure(&config, buffer, sizeof(buffer), 4u);
    CHECK(vd_input_trace_init(&trace, &config) ==
              VD_INPUT_TRACE_OK,
          "trace initializes with caller-owned bounded storage");
    CHECK(vd_input_trace_record_begin(
              &trace, UINT64_C(0x8877665544332211), 1000u) ==
              VD_INPUT_TRACE_OK,
          "recording explicitly begins");
    first.buttons = VD_INPUT_BUTTON_CROSS | VD_INPUT_BUTTON_RIGHT;
    first.left_x = -32768;
    first.left_y = 32767;
    first.right_x = -123;
    first.right_y = 456;
    first.touch_count = 2u;
    first.touches[0] =
        (struct vd_input_touch){1u, 123u, 456u, 789u};
    first.touches[1] =
        (struct vd_input_touch){2u, 321u, 654u, 987u};
    second.buttons = VD_INPUT_BUTTON_CIRCLE;
    CHECK(vd_input_trace_record_input(&trace, &first, 1000u, 10u) ==
              VD_INPUT_TRACE_OK,
          "button analog and touch state records exactly");
    CHECK(vd_input_trace_record_checkpoint(
              &trace, VD_INPUT_TRACE_CHECKPOINT_FRAME, 1500u, 11u) ==
              VD_INPUT_TRACE_OK,
          "frame checkpoint records with relative timing");
    CHECK(vd_input_trace_record_input(
              &trace, &second, 2000u, VD_INPUT_TRACE_NO_FRAME) ==
              VD_INPUT_TRACE_OK &&
              vd_input_trace_record_end(
                  &trace, VD_INPUT_TRACE_END_COMPLETE) ==
                  VD_INPUT_TRACE_OK,
          "recording closes with an explicit complete reason");
    CHECK(vd_input_trace_data(&trace, &data, &data_size) ==
              VD_INPUT_TRACE_OK &&
              data_size == VD_INPUT_TRACE_HEADER_SIZE +
                               3u * VD_INPUT_TRACE_EVENT_SIZE,
          "completed trace exposes one exact bounded blob");
    CHECK(vd_input_trace_verify(data, data_size, &config.identity,
                                &info) == VD_INPUT_TRACE_OK &&
              info.event_count == 3u && info.duration_us == 1000u &&
              info.end_reason == VD_INPUT_TRACE_END_COMPLETE,
          "trace round trip verifies identity timing and end reason");

    CHECK(vd_input_trace_playback_begin(&trace, 5000u) ==
              VD_INPUT_TRACE_OK,
          "verified complete trace starts at real-time 1x");
    CHECK(vd_input_trace_playback_tick(&trace, 5000u, apply_input,
                                       &sink) == VD_INPUT_TRACE_OK &&
              sink.count == 1u &&
              memcmp(&sink.inputs[0], &first, sizeof(first)) == 0,
          "first physical input replays exactly at relative time zero");
    CHECK(vd_input_trace_playback_tick(&trace, 5500u, apply_input,
                                       &sink) == VD_INPUT_TRACE_OK &&
              sink.count == 1u,
          "checkpoint advances without injecting synthetic input");
    CHECK(vd_input_trace_playback_tick(&trace, 6000u, apply_input,
                                       &sink) ==
              VD_INPUT_TRACE_COMPLETE &&
              sink.count == 3u &&
              memcmp(&sink.inputs[1], &second, sizeof(second)) == 0 &&
              sink.inputs[2].buttons == 0u &&
              sink.inputs[2].touch_count == 0u,
          "final non-neutral input replays then completion neutralizes");
    CHECK(vd_input_trace_get_info(&trace, &info) ==
              VD_INPUT_TRACE_OK &&
              info.max_scheduling_drift_us == 0u,
          "playback reports bounded observed scheduling drift");
}

static void test_limits_identity_and_malformed(void)
{
    uint8_t buffer[VD_INPUT_TRACE_HEADER_SIZE +
                   2u * VD_INPUT_TRACE_EVENT_SIZE];
    uint8_t copy[sizeof(buffer)];
    struct vd_input_trace_config config;
    struct vd_input_trace trace = {0};
    struct vd_input_trace_info info;
    struct vd_input_state input = {0};
    struct vd_input_trace_identity wrong;
    const uint8_t* data;
    size_t data_size;

    configure(&config, buffer, sizeof(buffer), 2u);
    CHECK(vd_input_trace_init(&trace, &config) ==
              VD_INPUT_TRACE_OK &&
              vd_input_trace_record_begin(&trace, 1u, 1000u) ==
                  VD_INPUT_TRACE_OK,
          "limit fixture starts recording");
    input.buttons = UINT32_C(0x00010000);
    CHECK(vd_input_trace_record_input(&trace, &input, 1005u, 1u) ==
              VD_INPUT_TRACE_ERROR_ARGUMENT,
          "system button bits are never recorded");
    input.buttons = VD_INPUT_BUTTON_START;
    CHECK(vd_input_trace_record_input(&trace, &input, 1010u, 1u) ==
              VD_INPUT_TRACE_OK &&
              vd_input_trace_record_input(&trace, &input, 1020u, 2u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_record_input(&trace, &input, 1030u, 3u) ==
                  VD_INPUT_TRACE_ERROR_LIMIT,
          "overflow aborts instead of dropping an event");
    CHECK(vd_input_trace_get_info(&trace, &info) ==
              VD_INPUT_TRACE_OK &&
              info.state == VD_INPUT_TRACE_STATE_READY &&
              info.event_count == 2u &&
              info.end_reason == VD_INPUT_TRACE_END_OVERFLOW,
          "overflow trace retains explicit terminal reason");
    CHECK(vd_input_trace_data(&trace, &data, &data_size) ==
              VD_INPUT_TRACE_OK,
          "aborted bounded trace remains inspectable");
    wrong = config.identity;
    ++wrong.process_generation;
    CHECK(vd_input_trace_verify(data, data_size, &wrong, NULL) ==
              VD_INPUT_TRACE_ERROR_IDENTITY,
          "wrong title generation fails closed");
    wrong = config.identity;
    memcpy(wrong.title_id, "OTHER0001", 10u);
    CHECK(vd_input_trace_verify(data, data_size, &wrong, NULL) ==
              VD_INPUT_TRACE_ERROR_IDENTITY,
          "wrong title fails closed");
    memcpy(copy, data, data_size);
    copy[VD_INPUT_TRACE_HEADER_SIZE + 24u] ^= 1u;
    CHECK(vd_input_trace_verify(copy, data_size, &config.identity,
                                NULL) ==
              VD_INPUT_TRACE_ERROR_CHECKSUM,
          "mutated event payload fails checksum");
    memcpy(copy, data, data_size);
    copy[25] = 1u;
    write_u32(copy + 72, trace_checksum(copy, data_size));
    CHECK(vd_input_trace_verify(copy, data_size, &config.identity,
                                NULL) ==
              VD_INPUT_TRACE_ERROR_FORMAT,
          "checksum-correct nonzero header reserved byte is rejected");
    memcpy(copy, data, data_size);
    copy[VD_INPUT_TRACE_HEADER_SIZE + 48u] = 1u;
    write_u32(copy + 72, trace_checksum(copy, data_size));
    CHECK(vd_input_trace_verify(copy, data_size, &config.identity,
                                NULL) ==
              VD_INPUT_TRACE_ERROR_FORMAT,
          "checksum-correct nonzero event reserved byte is rejected");
    CHECK(vd_input_trace_verify(data, data_size - 1u, &config.identity,
                                NULL) ==
              VD_INPUT_TRACE_ERROR_LIMIT,
          "truncated trace fails closed");

    CHECK(vd_input_trace_record_begin(&trace, 2u, 2000u) ==
              VD_INPUT_TRACE_OK &&
              vd_input_trace_record_input(&trace, &input, 2100u, 2u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_record_input(&trace, &input, 2099u, 3u) ==
                  VD_INPUT_TRACE_ERROR_TIME,
          "nonmonotonic recording time is rejected");
    (void)vd_input_trace_abort(&trace,
                               VD_INPUT_TRACE_END_ABORTED);
    CHECK(vd_input_trace_record_begin(&trace, 3u, 3000u) ==
              VD_INPUT_TRACE_OK &&
              vd_input_trace_record_input(
                  &trace, &input, 1003001u, 4u) ==
                  VD_INPUT_TRACE_ERROR_TIME &&
              vd_input_trace_get_info(&trace, &info) ==
                  VD_INPUT_TRACE_OK &&
              info.state == VD_INPUT_TRACE_STATE_READY &&
              info.end_reason == VD_INPUT_TRACE_END_TIMEOUT,
          "duration overflow terminates direct recording as timeout");
}

static void test_drift_callback_cancel_and_pause(void)
{
    uint8_t buffer[VD_INPUT_TRACE_HEADER_SIZE +
                   2u * VD_INPUT_TRACE_EVENT_SIZE];
    struct vd_input_trace_config config;
    struct vd_input_trace trace = {0};
    struct vd_input_state input = {0};
    struct playback_sink sink = {0};

    configure(&config, buffer, sizeof(buffer), 2u);
    CHECK(vd_input_trace_init(&trace, &config) ==
              VD_INPUT_TRACE_OK &&
              vd_input_trace_record_begin(&trace, 3u, 0u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_record_input(&trace, &input, 10u, 1u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_record_end(
                  &trace, VD_INPUT_TRACE_END_COMPLETE) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_playback_begin(&trace, 1000u) ==
                  VD_INPUT_TRACE_OK,
          "drift fixture prepares complete trace");
    CHECK(vd_input_trace_pause(&trace) ==
              VD_INPUT_TRACE_ERROR_UNSUPPORTED,
          "pause is explicitly unsupported rather than ambiguous");
    CHECK(vd_input_trace_playback_tick(&trace, 1111u, apply_input,
                                       &sink) ==
              VD_INPUT_TRACE_ERROR_TIME &&
              trace.state == VD_INPUT_TRACE_STATE_FAILED &&
              trace.end_reason == VD_INPUT_TRACE_END_TIMEOUT,
          "excess scheduling drift terminates playback");

    CHECK(vd_input_trace_init(&trace, &config) ==
              VD_INPUT_TRACE_OK &&
              vd_input_trace_record_begin(&trace, 4u, 0u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_record_input(&trace, &input, 0u, 1u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_record_end(
                  &trace, VD_INPUT_TRACE_END_COMPLETE) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_playback_begin(&trace, 2000u) ==
                  VD_INPUT_TRACE_OK,
          "callback fixture prepares trace");
    sink.fail_call = sink.count + 1u;
    CHECK(vd_input_trace_playback_tick(&trace, 2000u, apply_input,
                                       &sink) ==
              VD_INPUT_TRACE_ERROR_CALLBACK &&
              trace.end_reason ==
                  VD_INPUT_TRACE_END_CALLBACK_FAILURE,
          "callback failure terminates playback explicitly");

    sink.fail_call = 0u;
    CHECK(vd_input_trace_init(&trace, &config) ==
              VD_INPUT_TRACE_OK &&
              vd_input_trace_record_begin(&trace, 5u, 0u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_record_input(&trace, &input, 0u, 1u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_record_end(
                  &trace, VD_INPUT_TRACE_END_COMPLETE) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_playback_begin(&trace, 3000u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_abort(
                  &trace, VD_INPUT_TRACE_END_CANCELLED) ==
                  VD_INPUT_TRACE_OK &&
              trace.state == VD_INPUT_TRACE_STATE_READY &&
              trace.end_reason == VD_INPUT_TRACE_END_CANCELLED,
          "playback cancel is immediate and explicit");

    sink.fail_call = 0u;
    sink.count = 0u;
    input.buttons = VD_INPUT_BUTTON_CROSS;
    CHECK(vd_input_trace_init(&trace, &config) ==
              VD_INPUT_TRACE_OK &&
              vd_input_trace_record_begin(&trace, 6u, 0u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_record_input(&trace, &input, 0u, 1u) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_record_end(
                  &trace, VD_INPUT_TRACE_END_COMPLETE) ==
                  VD_INPUT_TRACE_OK &&
              vd_input_trace_playback_begin(&trace, 4000u) ==
                  VD_INPUT_TRACE_OK,
          "completion-neutral failure fixture prepares trace");
    sink.fail_call = 2u;
    CHECK(vd_input_trace_playback_tick(&trace, 4000u, apply_input,
                                       &sink) ==
              VD_INPUT_TRACE_ERROR_CALLBACK &&
              sink.count == 2u &&
              trace.state == VD_INPUT_TRACE_STATE_FAILED &&
              trace.end_reason ==
                  VD_INPUT_TRACE_END_CALLBACK_FAILURE,
          "direct playback exposes completion neutral failure");
}

int main(void)
{
    test_round_trip_and_playback();
    test_limits_identity_and_malformed();
    test_drift_callback_cancel_and_pause();
    if (failures != 0) {
        fprintf(stderr, "%d input trace test(s) failed\n", failures);
        return 1;
    }
    puts("input trace tests passed");
    return 0;
}

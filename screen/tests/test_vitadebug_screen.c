#include "vitadebug_screen.h"

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

struct memory_sink {
    uint8_t data[1024];
    size_t size;
    size_t fail_call;
    size_t calls;
};

static int write_memory(void* user, const uint8_t* data, size_t size)
{
    struct memory_sink* sink = (struct memory_sink*)user;
    ++sink->calls;
    if (sink->fail_call != 0u && sink->calls == sink->fail_call)
        return -1;
    if (size > sizeof(sink->data) - sink->size)
        return -1;
    memcpy(sink->data + sink->size, data, size);
    sink->size += size;
    return 0;
}

static uint32_t read_u32_be(const uint8_t* input)
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | input[3];
}

static uint64_t read_u64_be(const uint8_t* input)
{
    return ((uint64_t)read_u32_be(input) << 32) |
           read_u32_be(input + 4);
}

static void configure(struct vd_screen_config* config,
                      struct vd_screen_source* source,
                      struct memory_sink* sink, uint8_t* pixels)
{
    size_t index;
    vd_screen_config_init(config);
    source->base = pixels;
    source->capacity = 16u;
    config->explicit_consent = VD_SCREEN_EXPLICIT_CONSENT;
    config->write = write_memory;
    config->write_user = sink;
    config->sources = source;
    config->source_count = 1u;
    config->min_frame_interval_us = 100000u;
    memcpy(config->title_id, "VDSCRN001", 10u);
    config->process_id = 42u;
    config->process_generation = 7u;
    config->session_id = UINT64_C(0x1122334455667788);
    for (index = 0u; index < VD_SCREEN_AUTH_TOKEN_SIZE; ++index)
        config->auth_token[index] = (uint8_t)(index + 1u);
}

static void test_opt_in_and_wire(void)
{
    uint8_t pixels[16] = {
        255u, 0u, 0u, 255u, 0u, 255u, 0u, 255u,
        0u, 0u, 255u, 255u, 255u, 255u, 255u, 255u,
    };
    struct vd_screen_source source;
    struct vd_screen_config config;
    struct vd_screen_stream stream;
    struct vd_screen_frame frame;
    struct vd_screen_stats stats;
    struct memory_sink sink;

    memset(&sink, 0, sizeof(sink));
    memset(&stream, 0, sizeof(stream));
    configure(&config, &source, &sink, pixels);
    CHECK(config.min_frame_interval_us ==
              VD_SCREEN_DEFAULT_MIN_FRAME_INTERVAL_US,
          "configuration defaults to a bounded ten frames per second");
    config.explicit_consent = 0u;
    CHECK(vd_screen_stream_init(&stream, &config) ==
              VD_SCREEN_ERROR_DISABLED,
          "stream initialization requires explicit consent");

    configure(&config, &source, &sink, pixels);
    CHECK(vd_screen_stream_init(&stream, &config) == VD_SCREEN_OK,
          "valid source-owned configuration initializes");
    CHECK(vd_screen_stream_begin(&stream) == VD_SCREEN_OK &&
              sink.size == VD_SCREEN_AUTH_SIZE &&
              memcmp(sink.data, "VDSA", 4u) == 0 &&
              read_u32_be(sink.data + 4) == 0x00010048u &&
              memcmp(sink.data + 40u, "VDSCRN001", 9u) == 0 &&
              read_u32_be(sink.data + 52u) == 42u &&
              read_u64_be(sink.data + 56u) == 7u &&
              read_u64_be(sink.data + 64u) ==
                  UINT64_C(0x1122334455667788),
          "begin writes the fixed authentication preface");
    CHECK(vd_screen_stream_init(&stream, &config) ==
              VD_SCREEN_ERROR_STATE,
          "active stream cannot be reinitialized past its lifecycle");

    memset(&frame, 0, sizeof(frame));
    frame.pixels = pixels;
    frame.width = 2u;
    frame.height = 2u;
    frame.stride_bytes = 8u;
    frame.pixel_format = VD_SCREEN_PIXEL_RGBA8888;
    frame.timestamp_us = 1000000u;
    CHECK(vd_screen_submit_displayed_frame(&stream, &frame) == VD_SCREEN_OK,
          "registered displayed frame is serialized");
    CHECK(sink.size == VD_SCREEN_AUTH_SIZE + VD_SCREEN_FRAME_HEADER_SIZE +
                           sizeof(pixels) &&
              memcmp(sink.data + VD_SCREEN_AUTH_SIZE, "VDSC", 4u) == 0 &&
              read_u64_be(sink.data + VD_SCREEN_AUTH_SIZE + 8u) == 0u &&
              read_u32_be(sink.data + VD_SCREEN_AUTH_SIZE + 36u) ==
                  sizeof(pixels) &&
              read_u32_be(sink.data + VD_SCREEN_AUTH_SIZE + 44u) ==
                  VD_SCREEN_FRAME_FLAG_SOURCE_OWNED &&
              read_u64_be(sink.data + VD_SCREEN_AUTH_SIZE + 48u) ==
                  UINT64_C(0x1122334455667788),
          "frame header carries sequence, bounds, ownership, and session");
    CHECK(memcmp(sink.data + VD_SCREEN_AUTH_SIZE +
                     VD_SCREEN_FRAME_HEADER_SIZE,
                 pixels, sizeof(pixels)) == 0,
          "frame payload is exact");

    frame.timestamp_us += 1u;
    CHECK(vd_screen_submit_displayed_frame(&stream, &frame) ==
              VD_SCREEN_DROPPED,
          "producer rate gate drops rather than queues");
    frame.timestamp_us += 100000u;
    CHECK(vd_screen_submit_displayed_frame(&stream, &frame) == VD_SCREEN_OK,
          "later frame passes producer rate gate");
    CHECK(vd_screen_stream_get_stats(&stream, &stats) == VD_SCREEN_OK &&
              stats.frames_sent == 2u && stats.frames_dropped == 1u &&
              stats.next_sequence == 2u,
          "producer statistics account sent and dropped frames");
}

static void test_source_bounds_and_failure(void)
{
    uint8_t pixels[16] = {0};
    uint8_t foreign[16] = {0};
    struct vd_screen_source source;
    struct vd_screen_config config;
    struct vd_screen_stream stream;
    struct vd_screen_frame frame;
    struct memory_sink sink;

    memset(&sink, 0, sizeof(sink));
    memset(&stream, 0, sizeof(stream));
    configure(&config, &source, &sink, pixels);
    CHECK(vd_screen_stream_init(&stream, &config) == VD_SCREEN_OK &&
              vd_screen_stream_begin(&stream) == VD_SCREEN_OK,
          "source validation fixture starts");
    memset(&frame, 0, sizeof(frame));
    frame.pixels = foreign;
    frame.width = 2u;
    frame.height = 2u;
    frame.stride_bytes = 8u;
    frame.pixel_format = VD_SCREEN_PIXEL_RGBA8888;
    CHECK(vd_screen_submit_displayed_frame(&stream, &frame) ==
              VD_SCREEN_ERROR_SOURCE,
          "foreign pointer is rejected");
    frame.pixels = pixels;
    frame.stride_bytes = 12u;
    CHECK(vd_screen_submit_displayed_frame(&stream, &frame) ==
              VD_SCREEN_ERROR_SOURCE,
          "payload beyond the registered source extent is rejected");
    frame.stride_bytes = 8u;
    frame.pixel_format = 99u;
    CHECK(vd_screen_submit_displayed_frame(&stream, &frame) ==
              VD_SCREEN_ERROR_FORMAT,
          "unknown format fails closed");

    memset(&sink, 0, sizeof(sink));
    memset(&stream, 0, sizeof(stream));
    configure(&config, &source, &sink, pixels);
    sink.fail_call = 2u;
    CHECK(vd_screen_stream_init(&stream, &config) == VD_SCREEN_OK &&
              vd_screen_stream_begin(&stream) == VD_SCREEN_OK,
          "failure fixture authenticates");
    frame.pixel_format = VD_SCREEN_PIXEL_RGBA8888;
    CHECK(vd_screen_submit_displayed_frame(&stream, &frame) ==
              VD_SCREEN_ERROR_IO,
          "partial-frame write failure is terminal");
    CHECK(vd_screen_submit_displayed_frame(&stream, &frame) ==
              VD_SCREEN_ERROR_IO,
          "failed stream cannot resume ambiguously");
    CHECK(vd_screen_stream_close(&stream) == VD_SCREEN_OK &&
              vd_screen_stream_get_stats(&stream, &(struct vd_screen_stats){0}) ==
                  VD_SCREEN_OK &&
              stream.state == VD_SCREEN_STATE_FAILED,
          "cleanup preserves terminal failure state");
}

int main(void)
{
    test_opt_in_and_wire();
    test_source_bounds_and_failure();
    if (failures != 0) {
        fprintf(stderr, "%d screen producer test(s) failed\n", failures);
        return 1;
    }
    puts("screen producer tests passed");
    return 0;
}

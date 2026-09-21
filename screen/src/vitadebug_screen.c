#include "vitadebug_screen.h"

#include <limits.h>
#include <string.h>

#define VD_SCREEN_STREAM_MAGIC UINT32_C(0x56445343)
#define VD_SCREEN_AUTH_MAGIC UINT32_C(0x56445341)
#define VD_SCREEN_FRAME_MAGIC UINT32_C(0x56445343)

static void vd_write_u16_be(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void vd_write_u32_be(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void vd_write_u64_be(uint8_t* output, uint64_t value)
{
    vd_write_u32_be(output, (uint32_t)(value >> 32));
    vd_write_u32_be(output + 4, (uint32_t)value);
}

static uint32_t vd_crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t index;

    for (index = 0u; index < size; ++index) {
        uint32_t bit;
        crc ^= data[index];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^
                  (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int vd_token_is_nonzero(
    const uint8_t token[VD_SCREEN_AUTH_TOKEN_SIZE])
{
    size_t index;
    uint8_t aggregate = 0u;

    for (index = 0u; index < VD_SCREEN_AUTH_TOKEN_SIZE; ++index)
        aggregate |= token[index];
    return aggregate != 0u;
}

static int vd_title_id_is_valid(
    const char title_id[VD_SCREEN_TITLE_ID_SIZE + 1u])
{
    size_t index;

    for (index = 0u; index < VD_SCREEN_TITLE_ID_SIZE; ++index) {
        const char value = title_id[index];
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9')))
            return 0;
    }
    return title_id[VD_SCREEN_TITLE_ID_SIZE] == '\0';
}

static uint32_t vd_bytes_per_pixel(uint32_t pixel_format)
{
    switch (pixel_format) {
        case VD_SCREEN_PIXEL_RGBA8888:
        case VD_SCREEN_PIXEL_BGRA8888:
            return 4u;
        case VD_SCREEN_PIXEL_RGB565_LE:
            return 2u;
        default:
            return 0u;
    }
}

static int vd_stream_is_valid(const struct vd_screen_stream* stream)
{
    return stream != NULL &&
           stream->initialized == VD_SCREEN_STREAM_MAGIC &&
           stream->write != NULL;
}

static int vd_stream_write(struct vd_screen_stream* stream,
                           const uint8_t* data, size_t size)
{
    if (stream->write(stream->write_user, data, size) != 0) {
        stream->state = VD_SCREEN_STATE_FAILED;
        return VD_SCREEN_ERROR_IO;
    }
    if (UINT64_MAX - stream->bytes_sent < (uint64_t)size)
        stream->bytes_sent = UINT64_MAX;
    else
        stream->bytes_sent += (uint64_t)size;
    return VD_SCREEN_OK;
}

void vd_screen_config_init(struct vd_screen_config* config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->max_width = VD_SCREEN_DEFAULT_MAX_WIDTH;
    config->max_height = VD_SCREEN_DEFAULT_MAX_HEIGHT;
    config->max_payload_bytes = VD_SCREEN_DEFAULT_MAX_PAYLOAD;
    config->min_frame_interval_us =
        VD_SCREEN_DEFAULT_MIN_FRAME_INTERVAL_US;
}

int vd_screen_stream_init(struct vd_screen_stream* stream,
                          const struct vd_screen_config* config)
{
    size_t index;

    if (stream == NULL || config == NULL)
        return VD_SCREEN_ERROR_INVALID_ARGUMENT;
    if (stream->initialized == VD_SCREEN_STREAM_MAGIC &&
        (stream->state == VD_SCREEN_STATE_STREAMING ||
         stream->state == VD_SCREEN_STATE_FAILED))
        return VD_SCREEN_ERROR_STATE;
    if (config->explicit_consent != VD_SCREEN_EXPLICIT_CONSENT)
        return VD_SCREEN_ERROR_DISABLED;
    if (config->write == NULL || config->sources == NULL ||
        config->source_count == 0u ||
        config->source_count > VD_SCREEN_MAX_SOURCES ||
        config->max_width == 0u || config->max_height == 0u ||
        config->max_width > UINT16_MAX ||
        config->max_height > UINT16_MAX ||
        config->max_payload_bytes == 0u ||
        config->min_frame_interval_us <
            VD_SCREEN_MIN_FRAME_INTERVAL_US ||
        !vd_title_id_is_valid(config->title_id) ||
        config->process_id == 0u ||
        config->process_generation == 0u ||
        config->session_id == 0u ||
        !vd_token_is_nonzero(config->auth_token))
        return VD_SCREEN_ERROR_INVALID_ARGUMENT;
    for (index = 0u; index < config->source_count; ++index) {
        if (config->sources[index].base == NULL ||
            config->sources[index].capacity == 0u ||
            config->sources[index].capacity >
                (size_t)config->max_payload_bytes)
            return VD_SCREEN_ERROR_INVALID_ARGUMENT;
    }

    memset(stream, 0, sizeof(*stream));
    stream->write = config->write;
    stream->write_user = config->write_user;
    memcpy(stream->sources, config->sources,
           config->source_count * sizeof(config->sources[0]));
    memcpy(stream->auth_token, config->auth_token,
           VD_SCREEN_AUTH_TOKEN_SIZE);
    memcpy(stream->title_id, config->title_id, sizeof(stream->title_id));
    stream->process_id = config->process_id;
    stream->process_generation = config->process_generation;
    stream->session_id = config->session_id;
    stream->source_count = (uint32_t)config->source_count;
    stream->max_width = config->max_width;
    stream->max_height = config->max_height;
    stream->max_payload_bytes = config->max_payload_bytes;
    stream->min_frame_interval_us = config->min_frame_interval_us;
    stream->state = VD_SCREEN_STATE_READY;
    stream->initialized = VD_SCREEN_STREAM_MAGIC;
    return VD_SCREEN_OK;
}

int vd_screen_stream_begin(struct vd_screen_stream* stream)
{
    uint8_t auth[VD_SCREEN_AUTH_SIZE];
    int result;

    if (!vd_stream_is_valid(stream))
        return VD_SCREEN_ERROR_STATE;
    if (stream->state != VD_SCREEN_STATE_READY)
        return VD_SCREEN_ERROR_STATE;

    memset(auth, 0, sizeof(auth));
    vd_write_u32_be(auth, VD_SCREEN_AUTH_MAGIC);
    vd_write_u16_be(auth + 4, VD_SCREEN_PROTOCOL_VERSION);
    vd_write_u16_be(auth + 6, VD_SCREEN_AUTH_SIZE);
    memcpy(auth + 8, stream->auth_token, VD_SCREEN_AUTH_TOKEN_SIZE);
    memcpy(auth + 40, stream->title_id, VD_SCREEN_TITLE_ID_SIZE);
    vd_write_u32_be(auth + 52, stream->process_id);
    vd_write_u64_be(auth + 56, stream->process_generation);
    vd_write_u64_be(auth + 64, stream->session_id);
    result = vd_stream_write(stream, auth, sizeof(auth));
    memset(stream->auth_token, 0, sizeof(stream->auth_token));
    if (result != VD_SCREEN_OK)
        return result;
    stream->state = VD_SCREEN_STATE_STREAMING;
    return VD_SCREEN_OK;
}

int vd_screen_submit_displayed_frame(struct vd_screen_stream* stream,
                                     const struct vd_screen_frame* frame)
{
    uint8_t header[VD_SCREEN_FRAME_HEADER_SIZE];
    uint32_t bytes_per_pixel;
    uint64_t payload_size;
    size_t source_index;
    int source_found = 0;
    int result;

    if (!vd_stream_is_valid(stream) || frame == NULL)
        return VD_SCREEN_ERROR_INVALID_ARGUMENT;
    if (stream->state == VD_SCREEN_STATE_FAILED)
        return VD_SCREEN_ERROR_IO;
    if (stream->state != VD_SCREEN_STATE_STREAMING)
        return VD_SCREEN_ERROR_STATE;
    if (stream->next_sequence == UINT64_MAX)
        return VD_SCREEN_ERROR_STATE;

    bytes_per_pixel = vd_bytes_per_pixel(frame->pixel_format);
    if (bytes_per_pixel == 0u)
        return VD_SCREEN_ERROR_FORMAT;
    if (frame->width == 0u || frame->height == 0u ||
        frame->width > stream->max_width ||
        frame->height > stream->max_height ||
        frame->width > UINT32_MAX / bytes_per_pixel ||
        frame->stride_bytes < frame->width * bytes_per_pixel)
        return VD_SCREEN_ERROR_BOUNDS;

    payload_size = (uint64_t)frame->stride_bytes * frame->height;
    if (payload_size == 0u ||
        payload_size > stream->max_payload_bytes ||
        payload_size > SIZE_MAX)
        return VD_SCREEN_ERROR_BOUNDS;
    for (source_index = 0u; source_index < stream->source_count;
         ++source_index) {
        if (frame->pixels == stream->sources[source_index].base &&
            payload_size <= stream->sources[source_index].capacity) {
            source_found = 1;
            break;
        }
    }
    if (!source_found)
        return VD_SCREEN_ERROR_SOURCE;

    if (stream->has_timestamp != 0u) {
        uint64_t elapsed;
        if (frame->timestamp_us < stream->last_timestamp_us)
            return VD_SCREEN_ERROR_TIMESTAMP;
        elapsed = frame->timestamp_us - stream->last_timestamp_us;
        if (elapsed < stream->min_frame_interval_us) {
            if (stream->frames_dropped != UINT64_MAX)
                ++stream->frames_dropped;
            return VD_SCREEN_DROPPED;
        }
    }

    memset(header, 0, sizeof(header));
    vd_write_u32_be(header, VD_SCREEN_FRAME_MAGIC);
    vd_write_u16_be(header + 4, VD_SCREEN_PROTOCOL_VERSION);
    vd_write_u16_be(header + 6, VD_SCREEN_FRAME_HEADER_SIZE);
    vd_write_u64_be(header + 8, stream->next_sequence);
    vd_write_u64_be(header + 16, frame->timestamp_us);
    vd_write_u16_be(header + 24, (uint16_t)frame->width);
    vd_write_u16_be(header + 26, (uint16_t)frame->height);
    vd_write_u32_be(header + 28, frame->stride_bytes);
    vd_write_u32_be(header + 32, frame->pixel_format);
    vd_write_u32_be(header + 36, (uint32_t)payload_size);
    vd_write_u32_be(
        header + 40,
        vd_crc32((const uint8_t*)frame->pixels, (size_t)payload_size));
    vd_write_u32_be(header + 44, VD_SCREEN_FRAME_FLAG_SOURCE_OWNED);
    vd_write_u64_be(header + 48, stream->session_id);

    result = vd_stream_write(stream, header, sizeof(header));
    if (result != VD_SCREEN_OK)
        return result;
    result = vd_stream_write(stream, (const uint8_t*)frame->pixels,
                             (size_t)payload_size);
    if (result != VD_SCREEN_OK)
        return result;

    stream->last_timestamp_us = frame->timestamp_us;
    stream->has_timestamp = 1u;
    ++stream->next_sequence;
    if (stream->frames_sent != UINT64_MAX)
        ++stream->frames_sent;
    return VD_SCREEN_OK;
}

int vd_screen_stream_close(struct vd_screen_stream* stream)
{
    if (!vd_stream_is_valid(stream))
        return VD_SCREEN_ERROR_STATE;
    memset(stream->auth_token, 0, sizeof(stream->auth_token));
    if (stream->state == VD_SCREEN_STATE_CLOSED)
        return VD_SCREEN_OK;
    if (stream->state == VD_SCREEN_STATE_UNINITIALIZED)
        return VD_SCREEN_ERROR_STATE;
    if (stream->state == VD_SCREEN_STATE_FAILED)
        return VD_SCREEN_OK;
    stream->state = VD_SCREEN_STATE_CLOSED;
    return VD_SCREEN_OK;
}

int vd_screen_stream_get_stats(const struct vd_screen_stream* stream,
                               struct vd_screen_stats* stats)
{
    if (!vd_stream_is_valid(stream) || stats == NULL)
        return VD_SCREEN_ERROR_INVALID_ARGUMENT;
    stats->frames_sent = stream->frames_sent;
    stats->frames_dropped = stream->frames_dropped;
    stats->bytes_sent = stream->bytes_sent;
    stats->next_sequence = stream->next_sequence;
    stats->state = stream->state;
    return VD_SCREEN_OK;
}

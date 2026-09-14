#include "vitaprofiler_stream.h"

#include <string.h>

#define VP_STREAM_WRITER_MAGIC UINT32_C(0x56505357)

static int vp_stream_writer_is_valid(const struct vp_stream_writer* writer)
{
    return writer != NULL && writer->initialized == VP_STREAM_WRITER_MAGIC &&
           writer->context != NULL && writer->names != NULL &&
           writer->write != NULL;
}

static int vp_stream_write(struct vp_stream_writer* writer,
                           const uint8_t* data, size_t size)
{
    if (writer->write(writer->write_user, data, size) != 0) {
        writer->state = VP_STREAM_WRITER_FAILED;
        return VP_ERROR_IO;
    }
    writer->bytes_written += size;
    return VP_RESULT_OK;
}

int vp_stream_writer_init(struct vp_stream_writer* writer,
                          const struct vp_stream_writer_config* config)
{
    struct vp_name_dictionary_stats name_stats;
    struct vp_stats ring_stats;
    size_t required = 0u;
    int result;

    if (writer == NULL || config == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    memset(writer, 0, sizeof(*writer));
    if (config->context == NULL || config->names == NULL ||
        config->write == NULL || config->dictionary_buffer == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    result = vp_get_stats(config->context, &ring_stats);
    if (result != VP_RESULT_OK)
        return result;
    result = vp_name_dictionary_get_stats(config->names, &name_stats);
    if (result != VP_RESULT_OK)
        return result;
    if (name_stats.sealed == 0u)
        return VP_ERROR_SEALED;
    result = vp_name_dictionary_wire_size(config->names, &required);
    if (result != VP_RESULT_OK)
        return result;
    if (config->dictionary_buffer_capacity < required)
        return VP_ERROR_BUFFER_TOO_SMALL;

    writer->context = config->context;
    writer->names = config->names;
    writer->write = config->write;
    writer->write_user = config->write_user;
    writer->dictionary_buffer = config->dictionary_buffer;
    writer->dictionary_buffer_capacity = config->dictionary_buffer_capacity;
    writer->state = VP_STREAM_WRITER_READY;
    writer->initialized = VP_STREAM_WRITER_MAGIC;
    return VP_RESULT_OK;
}

int vp_stream_writer_begin(struct vp_stream_writer* writer,
                           uint64_t stream_start_us)
{
    struct vp_wire_header header;
    uint8_t encoded_header[VP_WIRE_HEADER_SIZE];
    size_t dictionary_size = 0u;
    int result;

    if (!vp_stream_writer_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (writer->state != VP_STREAM_WRITER_READY)
        return VP_ERROR_STATE;

    result = vp_encode_name_dictionary_le(
        writer->names, writer->dictionary_buffer,
        writer->dictionary_buffer_capacity, &dictionary_size);
    if (result != VP_RESULT_OK)
        return result;
    vp_wire_header_init(&header, stream_start_us);
    result = vp_encode_wire_header_le(&header, encoded_header);
    if (result != VP_RESULT_OK)
        return result;

    result = vp_stream_write(writer, writer->dictionary_buffer,
                             dictionary_size);
    if (result != VP_RESULT_OK)
        return result;
    result = vp_stream_write(writer, encoded_header, sizeof(encoded_header));
    if (result != VP_RESULT_OK)
        return result;
    writer->state = VP_STREAM_WRITER_STREAMING;
    return VP_RESULT_OK;
}

int vp_stream_writer_drain(struct vp_stream_writer* writer,
                           size_t max_events, size_t* events_written)
{
    struct vp_event event;
    uint8_t encoded[VP_WIRE_EVENT_SIZE];
    size_t count = 0u;

    if (events_written != NULL)
        *events_written = 0u;
    if (!vp_stream_writer_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (events_written == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (writer->state == VP_STREAM_WRITER_FAILED)
        return VP_ERROR_IO;
    if (writer->state != VP_STREAM_WRITER_STREAMING)
        return VP_ERROR_STATE;

    while (count < max_events &&
           vp_drain(writer->context, &event, 1u) == 1u) {
        int result = vp_encode_event_le(&event, encoded);
        if (result != VP_RESULT_OK)
            return result;
        result = vp_stream_write(writer, encoded, sizeof(encoded));
        if (result != VP_RESULT_OK) {
            ++writer->events_lost_to_sink;
            *events_written = count;
            return result;
        }
        ++count;
        ++writer->events_written;
    }
    *events_written = count;
    return VP_RESULT_OK;
}

int vp_stream_writer_close(struct vp_stream_writer* writer)
{
    struct vp_stats ring_stats;
    int result;

    if (!vp_stream_writer_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (writer->state == VP_STREAM_WRITER_FAILED)
        return VP_ERROR_IO;
    if (writer->state != VP_STREAM_WRITER_STREAMING)
        return VP_ERROR_STATE;
    result = vp_get_stats(writer->context, &ring_stats);
    if (result != VP_RESULT_OK)
        return result;
    if (ring_stats.pending != 0u)
        return VP_ERROR_BUSY;
    writer->state = VP_STREAM_WRITER_CLOSED;
    return VP_RESULT_OK;
}

int vp_stream_writer_get_stats(const struct vp_stream_writer* writer,
                               struct vp_stream_writer_stats* stats)
{
    if (!vp_stream_writer_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (stats == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    stats->bytes_written = writer->bytes_written;
    stats->events_written = writer->events_written;
    stats->events_lost_to_sink = writer->events_lost_to_sink;
    stats->state = writer->state;
    return VP_RESULT_OK;
}

#include "vitaprofiler_stream.h"

#include <string.h>

#define VP_STREAM_WRITER_MAGIC UINT32_C(0x56505357)
#define VP_STREAM_V2_SESSION_FLAGS_ALL                                  \
    (VP_STREAM_V2_SESSION_PROCESS_ID |                                  \
     VP_STREAM_V2_SESSION_PROCESS_IDENTITY |                             \
     VP_STREAM_V2_SESSION_TIMER_SOURCE | VP_STREAM_V2_SESSION_TIMER_UNIT)
#define VP_STREAM_V2_THREAD_FLAGS_ALL VP_STREAM_V2_THREAD_IDENTITY
#define VP_STREAM_V2_MODULE_FLAGS_ALL                                   \
    (VP_STREAM_V2_MODULE_EXECUTABLE | VP_STREAM_V2_MODULE_ARM |          \
     VP_STREAM_V2_MODULE_THUMB)

static void vp_stream_write_u16_le(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void vp_stream_write_u32_le(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static void vp_stream_write_u64_le(uint8_t* output, uint64_t value)
{
    vp_stream_write_u32_le(output, (uint32_t)value);
    vp_stream_write_u32_le(output + 4, (uint32_t)(value >> 32));
}

static uint16_t vp_stream_read_u16_le(const uint8_t* input)
{
    return (uint16_t)((uint16_t)input[0] |
                      ((uint16_t)input[1] << 8));
}

static uint32_t vp_stream_read_u32_le(const uint8_t* input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

static uint64_t vp_stream_read_u64_le(const uint8_t* input)
{
    return (uint64_t)vp_stream_read_u32_le(input) |
           ((uint64_t)vp_stream_read_u32_le(input + 4) << 32);
}

static uint32_t vp_stream_crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    for (i = 0u; i < size; ++i) {
        uint32_t bit;
        crc ^= data[i];
        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc >> 1) ^
                  (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int vp_stream_add_metadata_key(
    uint64_t* keys, uint32_t* count, uint32_t capacity, uint64_t key)
{
    uint32_t index;
    for (index = 0u; index < *count; ++index) {
        if (keys[index] == key)
            return VP_ERROR_INVALID_ARGUMENT;
    }
    if (*count == capacity)
        return VP_ERROR_CAPACITY;
    keys[*count] = key;
    ++*count;
    return VP_RESULT_OK;
}

static size_t vp_stream_bounded_text_length(const char* text,
                                            size_t maximum)
{
    size_t length = 0u;
    if (text == NULL)
        return 0u;
    while (length <= maximum && text[length] != '\0')
        ++length;
    return length;
}

static int vp_stream_writer_is_valid(const struct vp_stream_writer* writer)
{
    return writer != NULL && writer->initialized == VP_STREAM_WRITER_MAGIC &&
           writer->context != NULL && writer->names != NULL &&
           writer->write != NULL;
}

static int vp_stream_writer_v2_is_valid(
    const struct vp_stream_writer_v2* writer)
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

static int vp_stream_write_v2(struct vp_stream_writer_v2* writer,
                              const uint8_t* data, size_t size)
{
    if (writer->write(writer->write_user, data, size) != 0) {
        writer->state = VP_STREAM_WRITER_FAILED;
        return VP_ERROR_IO;
    }
    writer->bytes_written += size;
    return VP_RESULT_OK;
}

static int vp_stream_write_v2_chunk(struct vp_stream_writer_v2* writer,
                                    uint16_t type,
                                    const uint8_t* payload,
                                    size_t payload_size)
{
    uint8_t header[VP_STREAM_V2_CHUNK_HEADER_SIZE];
    int result;
    if (payload_size > VP_STREAM_V2_MAX_CHUNK_PAYLOAD ||
        (payload_size != 0u && payload == NULL))
        return VP_ERROR_INVALID_ARGUMENT;
    memset(header, 0, sizeof(header));
    vp_stream_write_u32_le(header, VP_STREAM_V2_CHUNK_MAGIC);
    vp_stream_write_u16_le(header + 4, VP_STREAM_V2_VERSION);
    vp_stream_write_u16_le(header + 6, VP_STREAM_V2_CHUNK_HEADER_SIZE);
    vp_stream_write_u16_le(header + 8, type);
    vp_stream_write_u32_le(header + 12, (uint32_t)payload_size);
    vp_stream_write_u32_le(header + 16, writer->chunk_sequence);
    vp_stream_write_u32_le(header + 20,
                           vp_stream_crc32(payload, payload_size));
    result = vp_stream_write_v2(writer, header, sizeof(header));
    if (result != VP_RESULT_OK)
        return result;
    if (payload_size != 0u) {
        result = vp_stream_write_v2(writer, payload, payload_size);
        if (result != VP_RESULT_OK)
            return result;
    }
    ++writer->chunk_sequence;
    return VP_RESULT_OK;
}

static void vp_stream_encode_stats_v2(
    const struct vp_stream_writer_v2* writer, const struct vp_stats* ring,
    uint8_t output[VP_STREAM_V2_STATS_PAYLOAD_SIZE])
{
    memset(output, 0, VP_STREAM_V2_STATS_PAYLOAD_SIZE);
    vp_stream_write_u32_le(output, ring->accepted);
    vp_stream_write_u32_le(output + 4, ring->dropped);
    vp_stream_write_u32_le(output + 8, writer->transport_events_lost);
    vp_stream_write_u32_le(output + 12, writer->events_lost_to_sink);
    vp_stream_write_u64_le(output + 16, writer->events_written);
    vp_stream_write_u64_le(output + 24, writer->bytes_written);
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

int vp_stream_writer_init_v2(
    struct vp_stream_writer_v2* writer,
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

int vp_stream_writer_begin_v2(
    struct vp_stream_writer_v2* writer, uint64_t stream_start_us,
    const struct vp_stream_v2_session* session)
{
    uint8_t payload[VP_STREAM_V2_SESSION_PAYLOAD_SIZE];
    size_t dictionary_size = 0u;
    size_t source_length;
    size_t unit_length;
    int result;

    if (!vp_stream_writer_v2_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (writer->state != VP_STREAM_WRITER_READY)
        return VP_ERROR_STATE;
    if (session == NULL || session->session_id == 0u ||
        (session->flags & ~VP_STREAM_V2_SESSION_FLAGS_ALL) != 0u)
        return VP_ERROR_INVALID_ARGUMENT;
    source_length = vp_stream_bounded_text_length(
        session->timer_source, VP_STREAM_V2_TIMER_SOURCE_MAX);
    unit_length = vp_stream_bounded_text_length(
        session->timer_unit, VP_STREAM_V2_TIMER_UNIT_MAX);
    if (source_length > VP_STREAM_V2_TIMER_SOURCE_MAX ||
        unit_length > VP_STREAM_V2_TIMER_UNIT_MAX ||
        (((session->flags & VP_STREAM_V2_SESSION_TIMER_SOURCE) != 0u) !=
         (source_length != 0u)) ||
        (((session->flags & VP_STREAM_V2_SESSION_TIMER_UNIT) != 0u) !=
         (unit_length != 0u)) ||
        ((session->flags & VP_STREAM_V2_SESSION_PROCESS_ID) == 0u &&
         session->process_id != 0u) ||
        ((session->flags & VP_STREAM_V2_SESSION_PROCESS_IDENTITY) == 0u &&
         session->process_identity != 0u))
        return VP_ERROR_INVALID_ARGUMENT;

    result = vp_encode_name_dictionary_le(
        writer->names, writer->dictionary_buffer,
        writer->dictionary_buffer_capacity, &dictionary_size);
    if (result != VP_RESULT_OK)
        return result;
    if (dictionary_size > VP_STREAM_V2_MAX_CHUNK_PAYLOAD)
        return VP_ERROR_CAPACITY;

    memset(payload, 0, sizeof(payload));
    vp_stream_write_u64_le(payload, session->session_id);
    vp_stream_write_u64_le(payload + 8, session->process_identity);
    vp_stream_write_u64_le(payload + 16, stream_start_us);
    vp_stream_write_u32_le(payload + 24, session->process_id);
    vp_stream_write_u32_le(payload + 28, session->flags);
    vp_stream_write_u16_le(payload + 32, (uint16_t)source_length);
    vp_stream_write_u16_le(payload + 34, (uint16_t)unit_length);
    if (source_length != 0u)
        memcpy(payload + 40, session->timer_source, source_length);
    if (unit_length != 0u)
        memcpy(payload + 80, session->timer_unit, unit_length);

    result = vp_stream_write_v2_chunk(
        writer, VP_STREAM_V2_CHUNK_SESSION, payload, sizeof(payload));
    if (result != VP_RESULT_OK)
        return result;
    result = vp_stream_write_v2_chunk(
        writer, VP_STREAM_V2_CHUNK_DICTIONARY,
        writer->dictionary_buffer, dictionary_size);
    if (result != VP_RESULT_OK)
        return result;
    writer->state = VP_STREAM_WRITER_STREAMING;
    return VP_RESULT_OK;
}

int vp_stream_writer_write_thread_v2(
    struct vp_stream_writer_v2* writer,
    const struct vp_stream_v2_thread* thread)
{
    uint8_t payload[VP_STREAM_V2_THREAD_PAYLOAD_SIZE];
    uint64_t key;
    int result;
    if (!vp_stream_writer_v2_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (writer->state == VP_STREAM_WRITER_FAILED)
        return VP_ERROR_IO;
    if (writer->state != VP_STREAM_WRITER_STREAMING)
        return VP_ERROR_STATE;
    if (thread == NULL || thread->thread_id == 0u ||
        (thread->flags & ~VP_STREAM_V2_THREAD_FLAGS_ALL) != 0u ||
        (((thread->flags & VP_STREAM_V2_THREAD_IDENTITY) != 0u) !=
         (thread->identity != 0u)))
        return VP_ERROR_INVALID_ARGUMENT;
    key = ((uint64_t)thread->thread_id << 32u) | thread->generation;
    for (uint32_t index = 0u;
         index < writer->thread_metadata_count; ++index) {
        if (writer->thread_metadata_keys[index] == key)
            return VP_ERROR_INVALID_ARGUMENT;
    }
    if (writer->thread_metadata_count ==
        VP_STREAM_V2_MAX_THREAD_METADATA)
        return VP_ERROR_CAPACITY;
    memset(payload, 0, sizeof(payload));
    vp_stream_write_u64_le(payload, thread->identity);
    vp_stream_write_u32_le(payload + 8, thread->thread_id);
    vp_stream_write_u32_le(payload + 12, thread->generation);
    vp_stream_write_u32_le(payload + 16, thread->name_id);
    vp_stream_write_u32_le(payload + 20, thread->flags);
    result = vp_stream_write_v2_chunk(
        writer, VP_STREAM_V2_CHUNK_THREAD, payload, sizeof(payload));
    if (result != VP_RESULT_OK)
        return result;
    return vp_stream_add_metadata_key(
        writer->thread_metadata_keys, &writer->thread_metadata_count,
        VP_STREAM_V2_MAX_THREAD_METADATA, key);
}

int vp_stream_writer_write_module_v2(
    struct vp_stream_writer_v2* writer,
    const struct vp_stream_v2_module* module)
{
    uint8_t payload[VP_STREAM_V2_MODULE_PAYLOAD_SIZE];
    uint64_t key;
    int result;
    if (!vp_stream_writer_v2_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (writer->state == VP_STREAM_WRITER_FAILED)
        return VP_ERROR_IO;
    if (writer->state != VP_STREAM_WRITER_STREAMING)
        return VP_ERROR_STATE;
    if (module == NULL || module->module_id == 0u ||
        module->address_start >= module->address_end ||
        (module->flags & ~VP_STREAM_V2_MODULE_FLAGS_ALL) != 0u ||
        (module->flags &
         (VP_STREAM_V2_MODULE_ARM | VP_STREAM_V2_MODULE_THUMB)) == 0u)
        return VP_ERROR_INVALID_ARGUMENT;
    key = ((uint64_t)module->module_id << 32u) | module->generation;
    for (uint32_t index = 0u;
         index < writer->module_metadata_count; ++index) {
        if (writer->module_metadata_keys[index] == key)
            return VP_ERROR_INVALID_ARGUMENT;
    }
    if (writer->module_metadata_count ==
        VP_STREAM_V2_MAX_MODULE_METADATA)
        return VP_ERROR_CAPACITY;
    memset(payload, 0, sizeof(payload));
    vp_stream_write_u32_le(payload, module->module_id);
    vp_stream_write_u32_le(payload + 4, module->generation);
    vp_stream_write_u32_le(payload + 8, module->address_start);
    vp_stream_write_u32_le(payload + 12, module->address_end);
    vp_stream_write_u32_le(payload + 16, module->name_id);
    vp_stream_write_u32_le(payload + 20, module->flags);
    result = vp_stream_write_v2_chunk(
        writer, VP_STREAM_V2_CHUNK_MODULE, payload, sizeof(payload));
    if (result != VP_RESULT_OK)
        return result;
    return vp_stream_add_metadata_key(
        writer->module_metadata_keys, &writer->module_metadata_count,
        VP_STREAM_V2_MAX_MODULE_METADATA, key);
}

int vp_stream_writer_write_stats_v2(struct vp_stream_writer_v2* writer)
{
    struct vp_stats ring;
    uint8_t payload[VP_STREAM_V2_STATS_PAYLOAD_SIZE];
    int result;
    if (!vp_stream_writer_v2_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (writer->state == VP_STREAM_WRITER_FAILED)
        return VP_ERROR_IO;
    if (writer->state != VP_STREAM_WRITER_STREAMING)
        return VP_ERROR_STATE;
    result = vp_get_stats(writer->context, &ring);
    if (result != VP_RESULT_OK)
        return result;
    vp_stream_encode_stats_v2(writer, &ring, payload);
    return vp_stream_write_v2_chunk(
        writer, VP_STREAM_V2_CHUNK_STATS, payload, sizeof(payload));
}

int vp_stream_writer_set_transport_loss_v2(
    struct vp_stream_writer_v2* writer, uint32_t events_lost)
{
    if (!vp_stream_writer_v2_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (writer->state == VP_STREAM_WRITER_FAILED)
        return VP_ERROR_IO;
    if (writer->state != VP_STREAM_WRITER_STREAMING)
        return VP_ERROR_STATE;
    writer->transport_events_lost = events_lost;
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

int vp_stream_writer_drain_v2(
    struct vp_stream_writer_v2* writer,
    size_t max_events, size_t* events_written)
{
    struct vp_event event;
    size_t capacity;
    size_t limit;
    size_t count = 0u;
    if (events_written != NULL)
        *events_written = 0u;
    if (!vp_stream_writer_v2_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (events_written == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (writer->state == VP_STREAM_WRITER_FAILED)
        return VP_ERROR_IO;
    if (writer->state != VP_STREAM_WRITER_STREAMING)
        return VP_ERROR_STATE;
    capacity = writer->dictionary_buffer_capacity / VP_WIRE_EVENT_SIZE;
    if (capacity >
        VP_STREAM_V2_MAX_CHUNK_PAYLOAD / VP_WIRE_EVENT_SIZE)
        capacity =
            VP_STREAM_V2_MAX_CHUNK_PAYLOAD / VP_WIRE_EVENT_SIZE;
    limit = max_events < capacity ? max_events : capacity;
    while (count < limit &&
           vp_drain(writer->context, &event, 1u) == 1u) {
        int result = vp_encode_event_le(
            &event, writer->dictionary_buffer +
                        count * VP_WIRE_EVENT_SIZE);
        if (result != VP_RESULT_OK)
            return result;
        ++count;
    }
    if (count != 0u) {
        int result = vp_stream_write_v2_chunk(
            writer, VP_STREAM_V2_CHUNK_EVENTS,
            writer->dictionary_buffer, count * VP_WIRE_EVENT_SIZE);
        if (result != VP_RESULT_OK) {
            writer->events_lost_to_sink += (uint32_t)count;
            return result;
        }
        writer->events_written += count;
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

int vp_stream_writer_close_v2(struct vp_stream_writer_v2* writer)
{
    struct vp_stats ring_stats;
    uint8_t payload[VP_STREAM_V2_STATS_PAYLOAD_SIZE];
    int result;
    if (!vp_stream_writer_v2_is_valid(writer))
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
    vp_stream_encode_stats_v2(writer, &ring_stats, payload);
    result = vp_stream_write_v2_chunk(
        writer, VP_STREAM_V2_CHUNK_STATS, payload, sizeof(payload));
    if (result != VP_RESULT_OK)
        return result;
    if (writer->bytes_written >
        UINT64_MAX - (VP_STREAM_V2_CHUNK_HEADER_SIZE +
                      VP_STREAM_V2_STATS_PAYLOAD_SIZE))
        return VP_ERROR_CAPACITY;
    vp_stream_encode_stats_v2(writer, &ring_stats, payload);
    vp_stream_write_u64_le(
        payload + 24,
        writer->bytes_written + VP_STREAM_V2_CHUNK_HEADER_SIZE +
            VP_STREAM_V2_STATS_PAYLOAD_SIZE);
    result = vp_stream_write_v2_chunk(
        writer, VP_STREAM_V2_CHUNK_END, payload, sizeof(payload));
    if (result != VP_RESULT_OK)
        return result;
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

int vp_stream_writer_get_stats_v2(
    const struct vp_stream_writer_v2* writer,
    struct vp_stream_writer_stats_v2* stats)
{
    if (!vp_stream_writer_v2_is_valid(writer))
        return VP_ERROR_NOT_INITIALIZED;
    if (stats == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    stats->bytes_written = writer->bytes_written;
    stats->events_written = writer->events_written;
    stats->events_lost_to_sink = writer->events_lost_to_sink;
    stats->state = writer->state;
    stats->transport_events_lost = writer->transport_events_lost;
    stats->wire_version = VP_STREAM_V2_VERSION;
    return VP_RESULT_OK;
}

int vp_stream_v2_decode_chunk_le(
    const uint8_t* input, size_t input_size,
    struct vp_stream_v2_chunk_view* chunk)
{
    struct vp_stream_v2_chunk_view decoded;
    size_t total_size;
    if (chunk != NULL)
        memset(chunk, 0, sizeof(*chunk));
    if (input == NULL || chunk == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (input_size < VP_STREAM_V2_CHUNK_HEADER_SIZE)
        return VP_ERROR_MALFORMED;
    memset(&decoded, 0, sizeof(decoded));
    decoded.header.magic = vp_stream_read_u32_le(input);
    decoded.header.version = vp_stream_read_u16_le(input + 4);
    decoded.header.header_size = vp_stream_read_u16_le(input + 6);
    decoded.header.type = vp_stream_read_u16_le(input + 8);
    decoded.header.flags = vp_stream_read_u16_le(input + 10);
    decoded.header.payload_size = vp_stream_read_u32_le(input + 12);
    decoded.header.sequence = vp_stream_read_u32_le(input + 16);
    decoded.header.payload_crc32 = vp_stream_read_u32_le(input + 20);
    decoded.header.reserved = vp_stream_read_u64_le(input + 24);
    if (decoded.header.magic != VP_STREAM_V2_CHUNK_MAGIC)
        return VP_ERROR_MALFORMED;
    if (decoded.header.version != VP_STREAM_V2_VERSION ||
        decoded.header.header_size != VP_STREAM_V2_CHUNK_HEADER_SIZE)
        return VP_ERROR_UNSUPPORTED;
    if (decoded.header.type < VP_STREAM_V2_CHUNK_SESSION ||
        decoded.header.type > VP_STREAM_V2_CHUNK_END ||
        decoded.header.flags != 0u || decoded.header.reserved != 0u ||
        decoded.header.payload_size > VP_STREAM_V2_MAX_CHUNK_PAYLOAD)
        return VP_ERROR_MALFORMED;
    total_size = VP_STREAM_V2_CHUNK_HEADER_SIZE +
                 (size_t)decoded.header.payload_size;
    if (total_size < decoded.header.payload_size || input_size < total_size)
        return VP_ERROR_MALFORMED;
    decoded.payload = input + VP_STREAM_V2_CHUNK_HEADER_SIZE;
    if (vp_stream_crc32(decoded.payload, decoded.header.payload_size) !=
        decoded.header.payload_crc32)
        return VP_ERROR_MALFORMED;
    *chunk = decoded;
    return VP_RESULT_OK;
}

static int vp_stream_v2_validate_session(
    const struct vp_stream_v2_chunk_view* chunk, uint64_t* session_id)
{
    const uint8_t* payload = chunk->payload;
    uint64_t identity;
    uint32_t process_id;
    uint32_t flags;
    uint16_t source_length;
    uint16_t unit_length;
    size_t i;
    if (chunk->header.payload_size != VP_STREAM_V2_SESSION_PAYLOAD_SIZE)
        return VP_ERROR_MALFORMED;
    *session_id = vp_stream_read_u64_le(payload);
    identity = vp_stream_read_u64_le(payload + 8);
    process_id = vp_stream_read_u32_le(payload + 24);
    flags = vp_stream_read_u32_le(payload + 28);
    source_length = vp_stream_read_u16_le(payload + 32);
    unit_length = vp_stream_read_u16_le(payload + 34);
    if (*session_id == 0u ||
        (flags & ~VP_STREAM_V2_SESSION_FLAGS_ALL) != 0u ||
        source_length > VP_STREAM_V2_TIMER_SOURCE_MAX ||
        unit_length > VP_STREAM_V2_TIMER_UNIT_MAX ||
        vp_stream_read_u32_le(payload + 36) != 0u ||
        (((flags & VP_STREAM_V2_SESSION_TIMER_SOURCE) != 0u) !=
         (source_length != 0u)) ||
        (((flags & VP_STREAM_V2_SESSION_TIMER_UNIT) != 0u) !=
         (unit_length != 0u)) ||
        ((flags & VP_STREAM_V2_SESSION_PROCESS_ID) == 0u &&
         process_id != 0u) ||
        ((flags & VP_STREAM_V2_SESSION_PROCESS_IDENTITY) == 0u &&
         identity != 0u))
        return VP_ERROR_MALFORMED;
    for (i = source_length; i < VP_STREAM_V2_TIMER_SOURCE_MAX; ++i) {
        if (payload[40u + i] != 0u)
            return VP_ERROR_MALFORMED;
    }
    for (i = unit_length; i < VP_STREAM_V2_TIMER_UNIT_MAX; ++i) {
        if (payload[80u + i] != 0u)
            return VP_ERROR_MALFORMED;
    }
    return VP_RESULT_OK;
}

static int vp_stream_v2_validate_payload(
    const struct vp_stream_v2_chunk_view* chunk)
{
    const uint8_t* payload = chunk->payload;
    switch (chunk->header.type) {
    case VP_STREAM_V2_CHUNK_DICTIONARY: {
        struct vp_name_wire_info info;
        int result = vp_name_wire_validate_le(
            payload, chunk->header.payload_size, &info);
        return result == VP_RESULT_OK &&
                       info.total_size == chunk->header.payload_size
                   ? VP_RESULT_OK
                   : (result == VP_RESULT_OK ? VP_ERROR_MALFORMED : result);
    }
    case VP_STREAM_V2_CHUNK_EVENTS:
        return (chunk->header.payload_size != 0u &&
                chunk->header.payload_size % VP_WIRE_EVENT_SIZE == 0u)
                   ? VP_RESULT_OK
                   : VP_ERROR_MALFORMED;
    case VP_STREAM_V2_CHUNK_THREAD: {
        uint64_t identity;
        uint32_t flags;
        if (chunk->header.payload_size != VP_STREAM_V2_THREAD_PAYLOAD_SIZE)
            return VP_ERROR_MALFORMED;
        identity = vp_stream_read_u64_le(payload);
        flags = vp_stream_read_u32_le(payload + 20);
        if (vp_stream_read_u32_le(payload + 8) == 0u ||
            (flags & ~VP_STREAM_V2_THREAD_FLAGS_ALL) != 0u ||
            (((flags & VP_STREAM_V2_THREAD_IDENTITY) != 0u) !=
             (identity != 0u)) ||
            vp_stream_read_u64_le(payload + 24) != 0u)
            return VP_ERROR_MALFORMED;
        return VP_RESULT_OK;
    }
    case VP_STREAM_V2_CHUNK_MODULE: {
        uint32_t start;
        uint32_t end;
        uint32_t flags;
        if (chunk->header.payload_size != VP_STREAM_V2_MODULE_PAYLOAD_SIZE)
            return VP_ERROR_MALFORMED;
        start = vp_stream_read_u32_le(payload + 8);
        end = vp_stream_read_u32_le(payload + 12);
        flags = vp_stream_read_u32_le(payload + 20);
        if (vp_stream_read_u32_le(payload) == 0u || start >= end ||
            (flags & ~VP_STREAM_V2_MODULE_FLAGS_ALL) != 0u ||
            (flags & (VP_STREAM_V2_MODULE_ARM |
                      VP_STREAM_V2_MODULE_THUMB)) == 0u ||
            vp_stream_read_u64_le(payload + 24) != 0u)
            return VP_ERROR_MALFORMED;
        return VP_RESULT_OK;
    }
    case VP_STREAM_V2_CHUNK_STATS:
    case VP_STREAM_V2_CHUNK_END:
        return chunk->header.payload_size == VP_STREAM_V2_STATS_PAYLOAD_SIZE
                   ? VP_RESULT_OK
                   : VP_ERROR_MALFORMED;
    default:
        return VP_ERROR_MALFORMED;
    }
}

int vp_stream_v2_cursor_init(
    struct vp_stream_v2_cursor* cursor, const uint8_t* input,
    size_t input_size, struct vp_stream_v2_info* info)
{
    struct vp_stream_v2_info parsed;
    size_t offset = 0u;
    uint32_t expected_sequence = 0u;
    uint32_t saw_end = 0u;
    uint32_t saw_stats = 0u;
    uint64_t thread_keys[VP_STREAM_V2_MAX_THREAD_METADATA];
    uint64_t module_keys[VP_STREAM_V2_MAX_MODULE_METADATA];
    uint32_t thread_count = 0u;
    uint32_t module_count = 0u;
    uint64_t previous_events_written = 0u;
    uint64_t previous_bytes_written = 0u;
    if (cursor == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    memset(cursor, 0, sizeof(*cursor));
    memset(&parsed, 0, sizeof(parsed));
    if (input == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    while (offset < input_size) {
        struct vp_stream_v2_chunk_view chunk;
        size_t chunk_size;
        int result = vp_stream_v2_decode_chunk_le(
            input + offset, input_size - offset, &chunk);
        if (result != VP_RESULT_OK)
            return result;
        if (chunk.header.sequence != expected_sequence || saw_end != 0u)
            return VP_ERROR_MALFORMED;
        if (expected_sequence == 0u) {
            if (chunk.header.type != VP_STREAM_V2_CHUNK_SESSION)
                return VP_ERROR_MALFORMED;
            result = vp_stream_v2_validate_session(
                &chunk, &parsed.session_id);
        } else if (expected_sequence == 1u) {
            if (chunk.header.type != VP_STREAM_V2_CHUNK_DICTIONARY)
                return VP_ERROR_MALFORMED;
            result = vp_stream_v2_validate_payload(&chunk);
        } else {
            if (chunk.header.type == VP_STREAM_V2_CHUNK_SESSION ||
                chunk.header.type == VP_STREAM_V2_CHUNK_DICTIONARY)
                return VP_ERROR_MALFORMED;
            result = vp_stream_v2_validate_payload(&chunk);
        }
        if (result != VP_RESULT_OK)
            return result;
        if (chunk.header.type == VP_STREAM_V2_CHUNK_THREAD) {
            uint64_t key =
                ((uint64_t)vp_stream_read_u32_le(chunk.payload + 8) << 32u) |
                vp_stream_read_u32_le(chunk.payload + 12);
            result = vp_stream_add_metadata_key(
                thread_keys, &thread_count,
                VP_STREAM_V2_MAX_THREAD_METADATA, key);
            if (result != VP_RESULT_OK)
                return VP_ERROR_MALFORMED;
        }
        if (chunk.header.type == VP_STREAM_V2_CHUNK_MODULE) {
            uint64_t key =
                ((uint64_t)vp_stream_read_u32_le(chunk.payload) << 32u) |
                vp_stream_read_u32_le(chunk.payload + 4);
            result = vp_stream_add_metadata_key(
                module_keys, &module_count,
                VP_STREAM_V2_MAX_MODULE_METADATA, key);
            if (result != VP_RESULT_OK)
                return VP_ERROR_MALFORMED;
        }
        if (chunk.header.type == VP_STREAM_V2_CHUNK_EVENTS)
            parsed.event_count +=
                chunk.header.payload_size / VP_WIRE_EVENT_SIZE;
        if (chunk.header.type == VP_STREAM_V2_CHUNK_STATS ||
            chunk.header.type == VP_STREAM_V2_CHUNK_END) {
            uint64_t events_written =
                vp_stream_read_u64_le(chunk.payload + 16);
            uint64_t bytes_written =
                vp_stream_read_u64_le(chunk.payload + 24);
            if (events_written > parsed.event_count ||
                (saw_stats != 0u &&
                 (events_written < previous_events_written ||
                  bytes_written < previous_bytes_written)))
                return VP_ERROR_MALFORMED;
            previous_events_written = events_written;
            previous_bytes_written = bytes_written;
            saw_stats = 1u;
        }
        if (chunk.header.type == VP_STREAM_V2_CHUNK_END) {
            if (vp_stream_read_u64_le(chunk.payload + 16) !=
                    parsed.event_count ||
                vp_stream_read_u32_le(chunk.payload + 12) != 0u ||
                vp_stream_read_u64_le(chunk.payload + 24) !=
                    offset + VP_STREAM_V2_CHUNK_HEADER_SIZE +
                        (size_t)chunk.header.payload_size)
                return VP_ERROR_MALFORMED;
            saw_end = 1u;
        }
        chunk_size = VP_STREAM_V2_CHUNK_HEADER_SIZE +
                     (size_t)chunk.header.payload_size;
        offset += chunk_size;
        ++expected_sequence;
        ++parsed.chunk_count;
    }
    if (offset != input_size || expected_sequence < 3u || saw_end == 0u)
        return VP_ERROR_MALFORMED;
    parsed.complete = 1u;
    parsed.total_size = input_size;
    cursor->data = input;
    cursor->total_size = input_size;
    if (info != NULL)
        *info = parsed;
    return VP_RESULT_OK;
}

int vp_stream_v2_cursor_next(
    struct vp_stream_v2_cursor* cursor,
    struct vp_stream_v2_chunk_view* chunk)
{
    int result;
    if (chunk != NULL)
        memset(chunk, 0, sizeof(*chunk));
    if (cursor == NULL || chunk == NULL || cursor->data == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (cursor->offset == cursor->total_size)
        return VP_RESULT_END;
    result = vp_stream_v2_decode_chunk_le(
        cursor->data + cursor->offset,
        cursor->total_size - cursor->offset, chunk);
    if (result != VP_RESULT_OK)
        return result;
    if (chunk->header.sequence != cursor->next_sequence)
        return VP_ERROR_MALFORMED;
    cursor->offset += VP_STREAM_V2_CHUNK_HEADER_SIZE +
                      (size_t)chunk->header.payload_size;
    ++cursor->next_sequence;
    return VP_RESULT_OK;
}

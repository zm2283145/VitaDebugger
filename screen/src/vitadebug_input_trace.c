#include "vitadebug_input_trace.h"

#include <string.h>

#define VD_INPUT_TRACE_MAGIC UINT32_C(0x56445452)
#define VD_INPUT_TRACE_INSTANCE_MAGIC UINT32_C(0x56445449)

static uint16_t vd_trace_read_u16(const uint8_t* input)
{
    return (uint16_t)(((uint16_t)input[0] << 8) | input[1]);
}

static uint32_t vd_trace_read_u32(const uint8_t* input)
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | input[3];
}

static uint64_t vd_trace_read_u64(const uint8_t* input)
{
    return ((uint64_t)vd_trace_read_u32(input) << 32) |
           vd_trace_read_u32(input + 4);
}

static void vd_trace_write_u16(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void vd_trace_write_u32(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void vd_trace_write_u64(uint8_t* output, uint64_t value)
{
    vd_trace_write_u32(output, (uint32_t)(value >> 32));
    vd_trace_write_u32(output + 4, (uint32_t)value);
}

static uint32_t vd_trace_crc32_update(uint32_t crc, const uint8_t* data,
                                      size_t size)
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

static uint32_t vd_trace_checksum(const uint8_t* data, size_t payload_size)
{
    uint32_t crc = UINT32_C(0xffffffff);

    crc = vd_trace_crc32_update(crc, data, 72u);
    crc = vd_trace_crc32_update(crc, data + 76u, 20u);
    crc = vd_trace_crc32_update(
        crc, data + VD_INPUT_TRACE_HEADER_SIZE, payload_size);
    return ~crc;
}

static int vd_trace_title_valid(const char title_id[10])
{
    size_t index;

    if (title_id == NULL || title_id[9] != '\0')
        return 0;
    for (index = 0u; index < 9u; ++index) {
        const char value = title_id[index];
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9')))
            return 0;
    }
    return 1;
}

static int vd_trace_identity_valid(
    const struct vd_input_trace_identity* identity)
{
    return identity != NULL && vd_trace_title_valid(identity->title_id) &&
           identity->process_id != 0u &&
           identity->process_generation != 0u &&
           identity->session_id != 0u;
}

static int vd_trace_valid(const struct vd_input_trace* trace)
{
    return trace != NULL &&
           trace->initialized == VD_INPUT_TRACE_INSTANCE_MAGIC &&
           trace->buffer != NULL;
}

static int vd_trace_input_valid(const struct vd_input_state* input)
{
    return input != NULL &&
           (input->buttons & ~VD_INPUT_BUTTON_ALLOWED_MASK) == 0u &&
           input->touch_count <= 2u;
}

static int vd_trace_reason_valid(uint32_t reason)
{
    return reason >= VD_INPUT_TRACE_END_COMPLETE &&
           reason <= VD_INPUT_TRACE_END_CANCELLED;
}

static void vd_trace_write_header(struct vd_input_trace* trace)
{
    uint8_t* header = trace->buffer;
    const size_t payload_size =
        (size_t)trace->event_count * VD_INPUT_TRACE_EVENT_SIZE;

    memset(header, 0, VD_INPUT_TRACE_HEADER_SIZE);
    vd_trace_write_u32(header, VD_INPUT_TRACE_MAGIC);
    vd_trace_write_u16(header + 4, VD_INPUT_TRACE_VERSION);
    vd_trace_write_u16(header + 6, VD_INPUT_TRACE_HEADER_SIZE);
    vd_trace_write_u16(header + 8, VD_INPUT_TRACE_EVENT_SIZE);
    vd_trace_write_u16(header + 10, (uint16_t)trace->end_reason);
    vd_trace_write_u32(header + 12, 1u);
    memcpy(header + 16, trace->identity.title_id, 9u);
    vd_trace_write_u32(header + 28, trace->identity.process_id);
    vd_trace_write_u64(header + 32,
                       trace->identity.process_generation);
    vd_trace_write_u64(header + 40, trace->identity.session_id);
    vd_trace_write_u64(header + 48, trace->trace_id);
    vd_trace_write_u32(header + 56, trace->event_count);
    vd_trace_write_u32(
        header + 60,
        (uint32_t)(VD_INPUT_TRACE_HEADER_SIZE + payload_size));
    vd_trace_write_u64(header + 64, trace->last_relative_us);
    vd_trace_write_u32(
        header + 72,
        vd_trace_checksum(header, payload_size));
    trace->data_size = VD_INPUT_TRACE_HEADER_SIZE + payload_size;
}

static int vd_trace_append(struct vd_input_trace* trace, uint32_t kind,
                           uint32_t marker,
                           const struct vd_input_state* input,
                           uint64_t now_us, uint64_t frame_index)
{
    uint8_t* event;
    uint64_t relative_us;
    size_t offset;

    if (!vd_trace_valid(trace) ||
        trace->state != VD_INPUT_TRACE_STATE_RECORDING)
        return VD_INPUT_TRACE_ERROR_STATE;
    if (now_us < trace->started_us)
        return VD_INPUT_TRACE_ERROR_TIME;
    relative_us = now_us - trace->started_us;
    if (trace->has_event != 0u &&
        relative_us < trace->last_relative_us)
        return VD_INPUT_TRACE_ERROR_TIME;
    if (relative_us > trace->max_duration_us) {
        (void)vd_input_trace_record_end(
            trace, VD_INPUT_TRACE_END_TIMEOUT);
        return VD_INPUT_TRACE_ERROR_TIME;
    }
    if (frame_index != VD_INPUT_TRACE_NO_FRAME &&
        trace->has_frame != 0u &&
        frame_index < trace->last_frame_index)
        return VD_INPUT_TRACE_ERROR_TIME;
    if (trace->event_count >= trace->max_events ||
        VD_INPUT_TRACE_HEADER_SIZE +
                ((size_t)trace->event_count + 1u) *
                    VD_INPUT_TRACE_EVENT_SIZE >
            trace->buffer_capacity) {
        (void)vd_input_trace_record_end(
            trace, VD_INPUT_TRACE_END_OVERFLOW);
        return VD_INPUT_TRACE_ERROR_LIMIT;
    }
    if (kind == VD_INPUT_TRACE_EVENT_INPUT) {
        if (marker != 0u || !vd_trace_input_valid(input))
            return VD_INPUT_TRACE_ERROR_ARGUMENT;
    } else if (kind == VD_INPUT_TRACE_EVENT_CHECKPOINT) {
        if (marker < VD_INPUT_TRACE_CHECKPOINT_FRAME ||
            marker > VD_INPUT_TRACE_CHECKPOINT_USER || input != NULL)
            return VD_INPUT_TRACE_ERROR_ARGUMENT;
    } else {
        return VD_INPUT_TRACE_ERROR_ARGUMENT;
    }

    offset = VD_INPUT_TRACE_HEADER_SIZE +
             (size_t)trace->event_count * VD_INPUT_TRACE_EVENT_SIZE;
    event = trace->buffer + offset;
    memset(event, 0, VD_INPUT_TRACE_EVENT_SIZE);
    vd_trace_write_u32(event, trace->event_count + 1u);
    vd_trace_write_u16(event + 4, (uint16_t)kind);
    vd_trace_write_u16(event + 6, VD_INPUT_TRACE_EVENT_SIZE);
    vd_trace_write_u64(event + 8, relative_us);
    vd_trace_write_u64(event + 16, frame_index);
    if (input != NULL) {
        vd_trace_write_u32(event + 24, input->buttons);
        vd_trace_write_u16(event + 28, (uint16_t)input->left_x);
        vd_trace_write_u16(event + 30, (uint16_t)input->left_y);
        vd_trace_write_u16(event + 32, (uint16_t)input->right_x);
        vd_trace_write_u16(event + 34, (uint16_t)input->right_y);
        event[36] = input->touch_count;
        if (input->touch_count > 0u) {
            vd_trace_write_u16(event + 40, input->touches[0].id);
            vd_trace_write_u16(event + 42, input->touches[0].x);
            vd_trace_write_u16(event + 44, input->touches[0].y);
            vd_trace_write_u16(event + 46, input->touches[0].force);
        }
        if (input->touch_count > 1u) {
            vd_trace_write_u16(event + 52, input->touches[1].id);
            vd_trace_write_u16(event + 54, input->touches[1].x);
            vd_trace_write_u16(event + 56, input->touches[1].y);
            vd_trace_write_u16(event + 58, input->touches[1].force);
        }
    }
    event[37] = (uint8_t)marker;
    ++trace->event_count;
    trace->last_relative_us = relative_us;
    trace->has_event = 1u;
    if (frame_index != VD_INPUT_TRACE_NO_FRAME) {
        trace->last_frame_index = frame_index;
        trace->has_frame = 1u;
    }
    return VD_INPUT_TRACE_OK;
}

void vd_input_trace_config_init(struct vd_input_trace_config* config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->max_events = VD_INPUT_TRACE_MAX_EVENTS;
    config->max_duration_us = VD_INPUT_TRACE_MAX_DURATION_US;
    config->max_scheduling_drift_us =
        VD_INPUT_TRACE_DEFAULT_MAX_DRIFT_US;
}

int vd_input_trace_init(struct vd_input_trace* trace,
                        const struct vd_input_trace_config* config)
{
    size_t capacity_events;

    if (trace == NULL || config == NULL || config->buffer == NULL ||
        config->buffer_capacity <
            VD_INPUT_TRACE_HEADER_SIZE + VD_INPUT_TRACE_EVENT_SIZE ||
        !vd_trace_identity_valid(&config->identity) ||
        config->max_events == 0u ||
        config->max_events > VD_INPUT_TRACE_MAX_EVENTS ||
        config->max_duration_us == 0u ||
        config->max_duration_us > VD_INPUT_TRACE_MAX_DURATION_US ||
        config->max_scheduling_drift_us == 0u ||
        config->max_scheduling_drift_us >
            VD_INPUT_TRACE_MAX_DRIFT_US)
        return VD_INPUT_TRACE_ERROR_ARGUMENT;
    capacity_events =
        (config->buffer_capacity - VD_INPUT_TRACE_HEADER_SIZE) /
        VD_INPUT_TRACE_EVENT_SIZE;
    if (capacity_events < config->max_events)
        return VD_INPUT_TRACE_ERROR_LIMIT;
    memset(trace, 0, sizeof(*trace));
    trace->buffer = config->buffer;
    trace->buffer_capacity = config->buffer_capacity;
    trace->identity = config->identity;
    trace->max_events = config->max_events;
    trace->max_duration_us = config->max_duration_us;
    trace->max_scheduling_drift_us =
        config->max_scheduling_drift_us;
    trace->state = VD_INPUT_TRACE_STATE_IDLE;
    trace->initialized = VD_INPUT_TRACE_INSTANCE_MAGIC;
    return VD_INPUT_TRACE_OK;
}

int vd_input_trace_record_begin(struct vd_input_trace* trace,
                                uint64_t trace_id, uint64_t now_us)
{
    if (!vd_trace_valid(trace) ||
        (trace->state != VD_INPUT_TRACE_STATE_IDLE &&
         trace->state != VD_INPUT_TRACE_STATE_READY) ||
        trace_id == 0u)
        return VD_INPUT_TRACE_ERROR_STATE;
    memset(trace->buffer, 0, trace->buffer_capacity);
    trace->started_us = now_us;
    trace->last_relative_us = 0u;
    trace->last_frame_index = 0u;
    trace->trace_id = trace_id;
    trace->data_size = 0u;
    trace->event_count = 0u;
    trace->playback_index = 0u;
    trace->end_reason = 0u;
    trace->observed_max_drift_us = 0u;
    trace->has_event = 0u;
    trace->has_frame = 0u;
    trace->state = VD_INPUT_TRACE_STATE_RECORDING;
    return VD_INPUT_TRACE_OK;
}

int vd_input_trace_record_input(struct vd_input_trace* trace,
                                const struct vd_input_state* input,
                                uint64_t now_us, uint64_t frame_index)
{
    return vd_trace_append(trace, VD_INPUT_TRACE_EVENT_INPUT, 0u, input,
                           now_us, frame_index);
}

int vd_input_trace_record_checkpoint(struct vd_input_trace* trace,
                                     uint32_t marker, uint64_t now_us,
                                     uint64_t frame_index)
{
    return vd_trace_append(trace, VD_INPUT_TRACE_EVENT_CHECKPOINT, marker,
                           NULL, now_us, frame_index);
}

int vd_input_trace_record_end(struct vd_input_trace* trace,
                              uint32_t end_reason)
{
    if (!vd_trace_valid(trace) ||
        trace->state != VD_INPUT_TRACE_STATE_RECORDING ||
        !vd_trace_reason_valid(end_reason))
        return VD_INPUT_TRACE_ERROR_STATE;
    trace->end_reason = end_reason;
    vd_trace_write_header(trace);
    trace->state = VD_INPUT_TRACE_STATE_READY;
    return VD_INPUT_TRACE_OK;
}

int vd_input_trace_verify(const uint8_t* data, size_t data_size,
                          const struct vd_input_trace_identity* expected,
                          struct vd_input_trace_info* info)
{
    uint32_t event_count;
    uint32_t index;
    uint64_t last_time = 0u;
    uint64_t last_frame = 0u;
    int has_frame = 0;

    if (info != NULL)
        memset(info, 0, sizeof(*info));
    if (data == NULL || data_size < VD_INPUT_TRACE_HEADER_SIZE ||
        data_size > VD_INPUT_TRACE_HEADER_SIZE +
                        (size_t)VD_INPUT_TRACE_MAX_EVENTS *
                            VD_INPUT_TRACE_EVENT_SIZE ||
        vd_trace_read_u32(data) != VD_INPUT_TRACE_MAGIC ||
        vd_trace_read_u16(data + 4) != VD_INPUT_TRACE_VERSION ||
        vd_trace_read_u16(data + 6) != VD_INPUT_TRACE_HEADER_SIZE ||
        vd_trace_read_u16(data + 8) != VD_INPUT_TRACE_EVENT_SIZE ||
        !vd_trace_reason_valid(vd_trace_read_u16(data + 10)) ||
        vd_trace_read_u32(data + 12) != 1u ||
        !vd_trace_title_valid((const char*)data + 16) ||
        vd_trace_read_u32(data + 28) == 0u ||
        vd_trace_read_u64(data + 32) == 0u ||
        vd_trace_read_u64(data + 40) == 0u ||
        vd_trace_read_u64(data + 48) == 0u)
        return VD_INPUT_TRACE_ERROR_FORMAT;
    for (index = 25u; index < 28u; ++index) {
        if (data[index] != 0u)
            return VD_INPUT_TRACE_ERROR_FORMAT;
    }
    for (index = 76u; index < VD_INPUT_TRACE_HEADER_SIZE; ++index) {
        if (data[index] != 0u)
            return VD_INPUT_TRACE_ERROR_FORMAT;
    }
    event_count = vd_trace_read_u32(data + 56);
    if (event_count > VD_INPUT_TRACE_MAX_EVENTS ||
        vd_trace_read_u32(data + 60) != data_size ||
        data_size != VD_INPUT_TRACE_HEADER_SIZE +
                         (size_t)event_count *
                             VD_INPUT_TRACE_EVENT_SIZE ||
        vd_trace_read_u64(data + 64) > VD_INPUT_TRACE_MAX_DURATION_US)
        return VD_INPUT_TRACE_ERROR_LIMIT;
    if (vd_trace_read_u32(data + 72) !=
        vd_trace_checksum(data,
                          data_size - VD_INPUT_TRACE_HEADER_SIZE))
        return VD_INPUT_TRACE_ERROR_CHECKSUM;
    if (expected != NULL &&
        (memcmp(data + 16, expected->title_id, 9u) != 0 ||
         vd_trace_read_u32(data + 28) != expected->process_id ||
         vd_trace_read_u64(data + 32) !=
             expected->process_generation ||
         vd_trace_read_u64(data + 40) != expected->session_id))
        return VD_INPUT_TRACE_ERROR_IDENTITY;

    for (index = 0u; index < event_count; ++index) {
        const uint8_t* event =
            data + VD_INPUT_TRACE_HEADER_SIZE +
            (size_t)index * VD_INPUT_TRACE_EVENT_SIZE;
        const uint16_t kind = vd_trace_read_u16(event + 4);
        const uint64_t relative_us = vd_trace_read_u64(event + 8);
        const uint64_t frame_index = vd_trace_read_u64(event + 16);
        uint32_t reserved;
        if (vd_trace_read_u32(event) != index + 1u ||
            vd_trace_read_u16(event + 6) != VD_INPUT_TRACE_EVENT_SIZE ||
            (index != 0u && relative_us < last_time) ||
            relative_us > vd_trace_read_u64(data + 64) ||
            event[38] != 0u || event[39] != 0u)
            return VD_INPUT_TRACE_ERROR_FORMAT;
        reserved = vd_trace_read_u32(event + 48) |
                   vd_trace_read_u32(event + 60);
        if (reserved != 0u)
            return VD_INPUT_TRACE_ERROR_FORMAT;
        if (frame_index != VD_INPUT_TRACE_NO_FRAME) {
            if (has_frame && frame_index < last_frame)
                return VD_INPUT_TRACE_ERROR_FORMAT;
            last_frame = frame_index;
            has_frame = 1;
        }
        if (kind == VD_INPUT_TRACE_EVENT_INPUT) {
            if (event[37] != 0u || event[36] > 2u ||
                (vd_trace_read_u32(event + 24) &
                 ~VD_INPUT_BUTTON_ALLOWED_MASK) != 0u)
                return VD_INPUT_TRACE_ERROR_FORMAT;
            if (event[36] == 0u &&
                (vd_trace_read_u64(event + 40) != 0u ||
                 vd_trace_read_u64(event + 52) != 0u))
                return VD_INPUT_TRACE_ERROR_FORMAT;
            if (event[36] == 1u &&
                vd_trace_read_u64(event + 52) != 0u)
                return VD_INPUT_TRACE_ERROR_FORMAT;
        } else if (kind == VD_INPUT_TRACE_EVENT_CHECKPOINT) {
            size_t byte;
            if (event[37] < VD_INPUT_TRACE_CHECKPOINT_FRAME ||
                event[37] > VD_INPUT_TRACE_CHECKPOINT_USER)
                return VD_INPUT_TRACE_ERROR_FORMAT;
            for (byte = 24u; byte < VD_INPUT_TRACE_EVENT_SIZE; ++byte) {
                if (byte != 37u && event[byte] != 0u)
                    return VD_INPUT_TRACE_ERROR_FORMAT;
            }
        } else {
            return VD_INPUT_TRACE_ERROR_FORMAT;
        }
        last_time = relative_us;
    }
    if ((event_count != 0u &&
         last_time != vd_trace_read_u64(data + 64)) ||
        (event_count == 0u && vd_trace_read_u64(data + 64) != 0u))
        return VD_INPUT_TRACE_ERROR_FORMAT;
    if (info != NULL) {
        info->state = VD_INPUT_TRACE_STATE_READY;
        info->event_count = event_count;
        info->end_reason = vd_trace_read_u16(data + 10);
        info->trace_id = vd_trace_read_u64(data + 48);
        info->duration_us = vd_trace_read_u64(data + 64);
        info->data_size = data_size;
    }
    return VD_INPUT_TRACE_OK;
}

int vd_input_trace_import(struct vd_input_trace* trace,
                          const uint8_t* data, size_t data_size)
{
    struct vd_input_trace_info info;
    int result;

    if (!vd_trace_valid(trace) ||
        (trace->state != VD_INPUT_TRACE_STATE_IDLE &&
         trace->state != VD_INPUT_TRACE_STATE_READY))
        return VD_INPUT_TRACE_ERROR_STATE;
    if (data_size > trace->buffer_capacity)
        return VD_INPUT_TRACE_ERROR_LIMIT;
    result = vd_input_trace_verify(data, data_size, &trace->identity, &info);
    if (result != VD_INPUT_TRACE_OK)
        return result;
    if (info.event_count > trace->max_events ||
        info.duration_us > trace->max_duration_us)
        return VD_INPUT_TRACE_ERROR_LIMIT;
    memmove(trace->buffer, data, data_size);
    trace->trace_id = info.trace_id;
    trace->event_count = info.event_count;
    trace->end_reason = info.end_reason;
    trace->last_relative_us = info.duration_us;
    trace->data_size = info.data_size;
    trace->state = VD_INPUT_TRACE_STATE_READY;
    return VD_INPUT_TRACE_OK;
}

int vd_input_trace_playback_begin(struct vd_input_trace* trace,
                                  uint64_t now_us)
{
    struct vd_input_trace_info info;
    int result;

    if (!vd_trace_valid(trace) ||
        trace->state != VD_INPUT_TRACE_STATE_READY)
        return VD_INPUT_TRACE_ERROR_STATE;
    result = vd_input_trace_verify(trace->buffer, trace->data_size,
                                   &trace->identity, &info);
    if (result != VD_INPUT_TRACE_OK)
        return result;
    if (info.end_reason != VD_INPUT_TRACE_END_COMPLETE)
        return VD_INPUT_TRACE_ERROR_STATE;
    trace->playback_started_us = now_us;
    trace->playback_index = 0u;
    trace->observed_max_drift_us = 0u;
    trace->state = VD_INPUT_TRACE_STATE_PLAYING;
    return VD_INPUT_TRACE_OK;
}

int vd_input_trace_playback_tick(struct vd_input_trace* trace,
                                 uint64_t now_us,
                                 vd_input_trace_apply_fn apply,
                                 void* apply_user)
{
    uint32_t dispatched = 0u;

    if (!vd_trace_valid(trace) ||
        trace->state != VD_INPUT_TRACE_STATE_PLAYING || apply == NULL)
        return VD_INPUT_TRACE_ERROR_STATE;
    if (now_us < trace->playback_started_us)
        return VD_INPUT_TRACE_ERROR_TIME;
    while (trace->playback_index < trace->event_count &&
           dispatched < VD_INPUT_TRACE_MAX_EVENTS_PER_TICK) {
        const uint8_t* event =
            trace->buffer + VD_INPUT_TRACE_HEADER_SIZE +
            (size_t)trace->playback_index * VD_INPUT_TRACE_EVENT_SIZE;
        const uint64_t relative_us = vd_trace_read_u64(event + 8);
        uint64_t due_us;
        uint64_t drift_us;
        if (relative_us > UINT64_MAX - trace->playback_started_us) {
            trace->state = VD_INPUT_TRACE_STATE_FAILED;
            return VD_INPUT_TRACE_ERROR_TIME;
        }
        due_us = trace->playback_started_us + relative_us;
        if (now_us < due_us)
            break;
        drift_us = now_us - due_us;
        if (drift_us > trace->observed_max_drift_us)
            trace->observed_max_drift_us = drift_us;
        if (drift_us > trace->max_scheduling_drift_us) {
            trace->state = VD_INPUT_TRACE_STATE_FAILED;
            trace->end_reason = VD_INPUT_TRACE_END_TIMEOUT;
            return VD_INPUT_TRACE_ERROR_TIME;
        }
        if (vd_trace_read_u16(event + 4) ==
            VD_INPUT_TRACE_EVENT_INPUT) {
            struct vd_input_state input;
            memset(&input, 0, sizeof(input));
            input.buttons = vd_trace_read_u32(event + 24);
            input.left_x = (int16_t)vd_trace_read_u16(event + 28);
            input.left_y = (int16_t)vd_trace_read_u16(event + 30);
            input.right_x = (int16_t)vd_trace_read_u16(event + 32);
            input.right_y = (int16_t)vd_trace_read_u16(event + 34);
            input.touch_count = event[36];
            if (input.touch_count > 0u) {
                input.touches[0].id = vd_trace_read_u16(event + 40);
                input.touches[0].x = vd_trace_read_u16(event + 42);
                input.touches[0].y = vd_trace_read_u16(event + 44);
                input.touches[0].force = vd_trace_read_u16(event + 46);
            }
            if (input.touch_count > 1u) {
                input.touches[1].id = vd_trace_read_u16(event + 52);
                input.touches[1].x = vd_trace_read_u16(event + 54);
                input.touches[1].y = vd_trace_read_u16(event + 56);
                input.touches[1].force = vd_trace_read_u16(event + 58);
            }
            if (apply(apply_user, &input) != 0) {
                trace->state = VD_INPUT_TRACE_STATE_FAILED;
                trace->end_reason =
                    VD_INPUT_TRACE_END_CALLBACK_FAILURE;
                return VD_INPUT_TRACE_ERROR_CALLBACK;
            }
        }
        ++trace->playback_index;
        ++dispatched;
    }
    if (trace->playback_index == trace->event_count) {
        const struct vd_input_state neutral = {0};
        if (apply(apply_user, &neutral) != 0) {
            trace->state = VD_INPUT_TRACE_STATE_FAILED;
            trace->end_reason =
                VD_INPUT_TRACE_END_CALLBACK_FAILURE;
            return VD_INPUT_TRACE_ERROR_CALLBACK;
        }
        trace->state = VD_INPUT_TRACE_STATE_READY;
        return VD_INPUT_TRACE_COMPLETE;
    }
    return VD_INPUT_TRACE_OK;
}

int vd_input_trace_abort(struct vd_input_trace* trace, uint32_t end_reason)
{
    if (!vd_trace_valid(trace) || !vd_trace_reason_valid(end_reason))
        return VD_INPUT_TRACE_ERROR_ARGUMENT;
    if (trace->state == VD_INPUT_TRACE_STATE_RECORDING)
        return vd_input_trace_record_end(trace, end_reason);
    if (trace->state == VD_INPUT_TRACE_STATE_PLAYING) {
        trace->state = VD_INPUT_TRACE_STATE_READY;
        trace->end_reason = end_reason;
        return VD_INPUT_TRACE_OK;
    }
    return VD_INPUT_TRACE_ERROR_STATE;
}

int vd_input_trace_pause(struct vd_input_trace* trace)
{
    (void)trace;
    return VD_INPUT_TRACE_ERROR_UNSUPPORTED;
}

int vd_input_trace_get_info(const struct vd_input_trace* trace,
                            struct vd_input_trace_info* info)
{
    if (!vd_trace_valid(trace) || info == NULL)
        return VD_INPUT_TRACE_ERROR_ARGUMENT;
    memset(info, 0, sizeof(*info));
    info->state = trace->state;
    info->event_count = trace->event_count;
    info->end_reason = trace->end_reason;
    info->trace_id = trace->trace_id;
    info->duration_us = trace->last_relative_us;
    info->max_scheduling_drift_us =
        trace->observed_max_drift_us;
    info->data_size = trace->data_size;
    return VD_INPUT_TRACE_OK;
}

int vd_input_trace_data(const struct vd_input_trace* trace,
                        const uint8_t** data, size_t* data_size)
{
    if (!vd_trace_valid(trace) || data == NULL || data_size == NULL ||
        trace->state != VD_INPUT_TRACE_STATE_READY ||
        trace->data_size < VD_INPUT_TRACE_HEADER_SIZE)
        return VD_INPUT_TRACE_ERROR_STATE;
    *data = trace->buffer;
    *data_size = trace->data_size;
    return VD_INPUT_TRACE_OK;
}

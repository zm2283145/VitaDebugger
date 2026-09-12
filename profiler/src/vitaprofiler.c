#include "vitaprofiler.h"

#include <limits.h>
#include <string.h>

#define VP_CONTEXT_MAGIC 0x56505246u
#define VP_MAX_CAPACITY (1u << 30)

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_InterlockedCompareExchange)
#pragma intrinsic(_InterlockedExchange)
#pragma intrinsic(_InterlockedExchangeAdd)

static uint32_t vp_atomic_load_relaxed(const volatile uint32_t* value)
{
    return (uint32_t)_InterlockedCompareExchange(
        (volatile long*)(uintptr_t)value, 0, 0);
}

static uint32_t vp_atomic_load_acquire(const volatile uint32_t* value)
{
    return vp_atomic_load_relaxed(value);
}

static void vp_atomic_store_relaxed(volatile uint32_t* value,
                                    uint32_t replacement)
{
    (void)_InterlockedExchange((volatile long*)value, (long)replacement);
}

static void vp_atomic_store_release(volatile uint32_t* value,
                                    uint32_t replacement)
{
    vp_atomic_store_relaxed(value, replacement);
}

static int vp_atomic_compare_exchange_relaxed(volatile uint32_t* value,
                                              uint32_t* expected,
                                              uint32_t replacement)
{
    const long observed = _InterlockedCompareExchange(
        (volatile long*)value, (long)replacement, (long)*expected);
    if ((uint32_t)observed == *expected)
        return 1;
    *expected = (uint32_t)observed;
    return 0;
}

static uint32_t vp_atomic_fetch_add_relaxed(volatile uint32_t* value,
                                           uint32_t increment)
{
    return (uint32_t)_InterlockedExchangeAdd((volatile long*)value,
                                             (long)increment);
}
#else
static uint32_t vp_atomic_load_relaxed(const volatile uint32_t* value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static uint32_t vp_atomic_load_acquire(const volatile uint32_t* value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void vp_atomic_store_relaxed(volatile uint32_t* value,
                                    uint32_t replacement)
{
    __atomic_store_n(value, replacement, __ATOMIC_RELAXED);
}

static void vp_atomic_store_release(volatile uint32_t* value,
                                    uint32_t replacement)
{
    __atomic_store_n(value, replacement, __ATOMIC_RELEASE);
}

static int vp_atomic_compare_exchange_relaxed(volatile uint32_t* value,
                                              uint32_t* expected,
                                              uint32_t replacement)
{
    return __atomic_compare_exchange_n(value, expected, replacement, 1,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static uint32_t vp_atomic_fetch_add_relaxed(volatile uint32_t* value,
                                           uint32_t increment)
{
    return __atomic_fetch_add(value, increment, __ATOMIC_RELAXED);
}
#endif

static int vp_context_is_valid(const struct vp_context* context)
{
    return context != NULL && context->initialized == VP_CONTEXT_MAGIC &&
           context->slots != NULL && context->capacity >= 2u;
}

static int64_t vp_elapsed_us(uint64_t begin, uint64_t end, uint16_t* flags)
{
    uint64_t elapsed;
    if (end < begin) {
        *flags = (uint16_t)(*flags | VP_EVENT_FLAG_CLOCK_REGRESSION);
        return 0;
    }
    elapsed = end - begin;
    if (elapsed > (uint64_t)INT64_MAX)
        return INT64_MAX;
    return (int64_t)elapsed;
}

static void vp_write_u16_le(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void vp_write_u32_le(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static void vp_write_u64_le(uint8_t* output, uint64_t value)
{
    vp_write_u32_le(output, (uint32_t)value);
    vp_write_u32_le(output + 4, (uint32_t)(value >> 32));
}

int vp_init(struct vp_context* context, const struct vp_config* config)
{
    uint32_t i;
    if (context == NULL || config == NULL || config->slots == NULL ||
        config->clock == NULL || config->thread_id == NULL ||
        config->capacity < 2u || config->capacity > VP_MAX_CAPACITY ||
        (config->capacity & (config->capacity - 1u)) != 0u)
        return VP_ERROR_INVALID_ARGUMENT;

    memset(context, 0, sizeof(*context));
    context->slots = config->slots;
    context->capacity = config->capacity;
    context->capacity_mask = config->capacity - 1u;
    context->clock = config->clock;
    context->clock_user = config->clock_user;
    context->thread_id = config->thread_id;
    context->thread_user = config->thread_user;
    vp_atomic_store_relaxed(&context->next_correlation_id, 1u);

    for (i = 0; i < config->capacity; ++i) {
        memset(&context->slots[i].event, 0,
               sizeof(context->slots[i].event));
        context->slots[i].reserved = 0;
        vp_atomic_store_relaxed(&context->slots[i].sequence, i);
    }

    context->initialized = VP_CONTEXT_MAGIC;
    return VP_RESULT_OK;
}

void vp_deinit(struct vp_context* context)
{
    if (context != NULL)
        memset(context, 0, sizeof(*context));
}

int vp_record(struct vp_context* context, const struct vp_event* event)
{
    uint32_t position;
    struct vp_slot* slot;

    if (!vp_context_is_valid(context))
        return VP_ERROR_NOT_INITIALIZED;
    if (event == NULL || event->type == 0u)
        return VP_ERROR_INVALID_ARGUMENT;

    position = vp_atomic_load_relaxed(&context->enqueue_pos);
    for (;;) {
        uint32_t sequence;
        int32_t difference;
        uint32_t expected;

        slot = &context->slots[position & context->capacity_mask];
        sequence = vp_atomic_load_acquire(&slot->sequence);
        difference = (int32_t)(sequence - position);

        if (difference == 0) {
            expected = position;
            if (vp_atomic_compare_exchange_relaxed(&context->enqueue_pos,
                                                   &expected,
                                                   position + 1u))
                break;
            position = expected;
        } else if (difference < 0) {
            (void)vp_atomic_fetch_add_relaxed(&context->dropped, 1u);
            return VP_RESULT_DROPPED;
        } else {
            position = vp_atomic_load_relaxed(&context->enqueue_pos);
        }
    }

    slot->event = *event;
    vp_atomic_store_release(&slot->sequence, position + 1u);
    (void)vp_atomic_fetch_add_relaxed(&context->accepted, 1u);
    return VP_RESULT_OK;
}

int vp_emit(struct vp_context* context, uint16_t type, uint16_t flags,
            uint32_t name_id, int64_t value, uint32_t correlation_id)
{
    struct vp_event event;
    if (!vp_context_is_valid(context))
        return VP_ERROR_NOT_INITIALIZED;
    if (type == 0u)
        return VP_ERROR_INVALID_ARGUMENT;

    event.timestamp_us = context->clock(context->clock_user);
    event.value = value;
    event.name_id = name_id;
    event.thread_id = context->thread_id(context->thread_user);
    event.correlation_id = correlation_id;
    event.type = type;
    event.flags = flags;
    return vp_record(context, &event);
}

int vp_counter(struct vp_context* context, uint32_t name_id, int64_t value)
{
    return vp_emit(context, VP_EVENT_COUNTER, VP_EVENT_FLAG_NONE, name_id,
                   value, 0u);
}

int vp_zone_begin(struct vp_context* context, uint32_t name_id,
                  struct vp_zone_scope* scope)
{
    struct vp_event event;
    uint32_t correlation_id;
    int result;

    if (scope == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    memset(scope, 0, sizeof(*scope));
    if (!vp_context_is_valid(context))
        return VP_ERROR_NOT_INITIALIZED;

    correlation_id = vp_atomic_fetch_add_relaxed(
        &context->next_correlation_id, 1u);
    if (correlation_id == 0u)
        correlation_id = vp_atomic_fetch_add_relaxed(
            &context->next_correlation_id, 1u);

    event.timestamp_us = context->clock(context->clock_user);
    event.value = 0;
    event.name_id = name_id;
    event.thread_id = context->thread_id(context->thread_user);
    event.correlation_id = correlation_id;
    event.type = VP_EVENT_ZONE_BEGIN;
    event.flags = VP_EVENT_FLAG_NONE;
    result = vp_record(context, &event);
    if (result != VP_RESULT_OK)
        return result;

    scope->begin_timestamp_us = event.timestamp_us;
    scope->name_id = name_id;
    scope->thread_id = event.thread_id;
    scope->correlation_id = correlation_id;
    scope->active = 1u;
    return VP_RESULT_OK;
}

int vp_zone_end(struct vp_context* context, struct vp_zone_scope* scope)
{
    struct vp_event event;
    uint64_t end;

    if (!vp_context_is_valid(context))
        return VP_ERROR_NOT_INITIALIZED;
    if (scope == NULL || scope->active == 0u)
        return VP_ERROR_INVALID_ARGUMENT;

    end = context->clock(context->clock_user);
    event.timestamp_us = end;
    event.name_id = scope->name_id;
    event.thread_id = context->thread_id(context->thread_user);
    event.correlation_id = scope->correlation_id;
    event.type = VP_EVENT_ZONE_END;
    event.flags = event.thread_id == scope->thread_id
                      ? VP_EVENT_FLAG_NONE
                      : VP_EVENT_FLAG_THREAD_MISMATCH;
    event.value = vp_elapsed_us(scope->begin_timestamp_us, end,
                                &event.flags);
    scope->active = 0u;
    return vp_record(context, &event);
}

int vp_frame_mark(struct vp_context* context, uint32_t name_id)
{
    struct vp_event event;
    uint64_t now;

    if (!vp_context_is_valid(context))
        return VP_ERROR_NOT_INITIALIZED;

    now = context->clock(context->clock_user);
    event.timestamp_us = now;
    event.name_id = name_id;
    event.thread_id = context->thread_id(context->thread_user);
    event.correlation_id = context->next_frame_id++;
    event.type = VP_EVENT_FRAME;
    event.flags = VP_EVENT_FLAG_NONE;
    if (context->has_frame_timestamp == 0u) {
        event.flags = VP_EVENT_FLAG_FIRST;
        event.value = 0;
        context->has_frame_timestamp = 1u;
    } else {
        event.value = vp_elapsed_us(context->last_frame_timestamp_us, now,
                                    &event.flags);
    }
    context->last_frame_timestamp_us = now;
    return vp_record(context, &event);
}

size_t vp_drain(struct vp_context* context, struct vp_event* events,
                size_t max_events)
{
    size_t copied = 0;
    uint32_t position;

    if (!vp_context_is_valid(context) ||
        (events == NULL && max_events != 0u))
        return 0;

    position = vp_atomic_load_relaxed(&context->dequeue_pos);
    while (copied < max_events) {
        struct vp_slot* slot =
            &context->slots[position & context->capacity_mask];
        uint32_t sequence = vp_atomic_load_acquire(&slot->sequence);
        int32_t difference = (int32_t)(sequence - (position + 1u));

        if (difference != 0)
            break;

        events[copied++] = slot->event;
        vp_atomic_store_release(&slot->sequence,
                                position + context->capacity);
        ++position;
        vp_atomic_store_relaxed(&context->dequeue_pos, position);
    }
    return copied;
}

int vp_get_stats(const struct vp_context* context, struct vp_stats* stats)
{
    uint32_t enqueue;
    uint32_t dequeue;
    if (!vp_context_is_valid(context))
        return VP_ERROR_NOT_INITIALIZED;
    if (stats == NULL)
        return VP_ERROR_INVALID_ARGUMENT;

    enqueue = vp_atomic_load_relaxed(&context->enqueue_pos);
    dequeue = vp_atomic_load_relaxed(&context->dequeue_pos);
    stats->capacity = context->capacity;
    stats->pending = enqueue - dequeue;
    if (stats->pending > stats->capacity)
        stats->pending = stats->capacity;
    stats->accepted = vp_atomic_load_relaxed(&context->accepted);
    stats->dropped = vp_atomic_load_relaxed(&context->dropped);
    return VP_RESULT_OK;
}

uint32_t vp_name_id(const char* name)
{
    const unsigned char* cursor = (const unsigned char*)name;
    uint32_t hash = 2166136261u;
    if (name == NULL || *name == '\0')
        return 0u;
    while (*cursor != 0u) {
        hash ^= *cursor++;
        hash *= 16777619u;
    }
    return hash == 0u ? 1u : hash;
}

void vp_wire_header_init(struct vp_wire_header* header,
                         uint64_t stream_start_us)
{
    if (header == NULL)
        return;
    memset(header, 0, sizeof(*header));
    header->magic = VP_WIRE_MAGIC;
    header->version = VP_WIRE_VERSION;
    header->header_size = VP_WIRE_HEADER_SIZE;
    header->event_size = VP_WIRE_EVENT_SIZE;
    header->flags = VP_WIRE_FLAG_LITTLE_ENDIAN;
    header->clock_hz = VP_CLOCK_HZ;
    header->stream_start_us = stream_start_us;
}

int vp_encode_wire_header_le(const struct vp_wire_header* header,
                             uint8_t output[VP_WIRE_HEADER_SIZE])
{
    if (header == NULL || output == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    vp_write_u32_le(output, header->magic);
    vp_write_u16_le(output + 4, header->version);
    vp_write_u16_le(output + 6, header->header_size);
    vp_write_u16_le(output + 8, header->event_size);
    vp_write_u16_le(output + 10, header->flags);
    vp_write_u32_le(output + 12, header->clock_hz);
    vp_write_u64_le(output + 16, header->stream_start_us);
    vp_write_u64_le(output + 24, header->reserved);
    return VP_RESULT_OK;
}

int vp_encode_event_le(const struct vp_event* event,
                       uint8_t output[VP_WIRE_EVENT_SIZE])
{
    if (event == NULL || output == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    vp_write_u64_le(output, event->timestamp_us);
    vp_write_u64_le(output + 8, (uint64_t)event->value);
    vp_write_u32_le(output + 16, event->name_id);
    vp_write_u32_le(output + 20, event->thread_id);
    vp_write_u32_le(output + 24, event->correlation_id);
    vp_write_u16_le(output + 28, event->type);
    vp_write_u16_le(output + 30, event->flags);
    return VP_RESULT_OK;
}

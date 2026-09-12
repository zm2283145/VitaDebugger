#ifndef VITAPROFILER_H
#define VITAPROFILER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VP_WIRE_MAGIC 0x46525056u /* "VPRF" in little-endian byte order. */
#define VP_WIRE_VERSION 1u
#define VP_WIRE_HEADER_SIZE 32u
#define VP_WIRE_EVENT_SIZE 32u
#define VP_CLOCK_HZ 1000000u
#define VP_WIRE_FLAG_LITTLE_ENDIAN 1u

enum vp_result {
    VP_RESULT_OK = 0,
    VP_RESULT_DROPPED = 1,
    VP_ERROR_INVALID_ARGUMENT = -1,
    VP_ERROR_NOT_INITIALIZED = -2,
    VP_ERROR_PLATFORM = -3,
};

enum vp_event_type {
    VP_EVENT_ZONE_BEGIN = 1,
    VP_EVENT_ZONE_END = 2,
    VP_EVENT_COUNTER = 3,
    VP_EVENT_FRAME = 4,
    VP_EVENT_MEMORY_SAMPLE = 5,
    VP_EVENT_THREAD_SAMPLE = 6,
    VP_EVENT_PROCESS_SAMPLE = 7,
    VP_EVENT_CUSTOM = 0x8000,
};

enum vp_event_flags {
    VP_EVENT_FLAG_NONE = 0,
    VP_EVENT_FLAG_FIRST = 1u << 0,
    VP_EVENT_FLAG_THREAD_MISMATCH = 1u << 1,
    VP_EVENT_FLAG_CLOCK_REGRESSION = 1u << 2,
    VP_EVENT_FLAG_RAW_VALUE = 1u << 3,
};

/* Stable IDs for samples emitted by the Vita adapter. Application-provided
 * names use vp_name_id() and should not use this reserved high range. Macros
 * avoid relying on implementation-defined enum widths in C11. */
#define VP_METRIC_FREE_USER_BYTES UINT32_C(0xfff00001)
#define VP_METRIC_FREE_CDRAM_BYTES UINT32_C(0xfff00002)
#define VP_METRIC_FREE_PHYCONT_BYTES UINT32_C(0xfff00003)
#define VP_METRIC_PROCESS_TIME_US UINT32_C(0xfff00004)
#define VP_METRIC_THREAD_RUN_CLOCKS UINT32_C(0xfff00005)
#define VP_METRIC_THREAD_STACK_FREE_BYTES UINT32_C(0xfff00006)
#define VP_METRIC_THREAD_PREEMPTIONS UINT32_C(0xfff00007)
#define VP_METRIC_INTERRUPT_PREEMPTIONS UINT32_C(0xfff00008)

/* Fixed-width in-memory event and versioned wire header. The wire encoder
 * always writes little endian and must be used instead of dumping native
 * structs when exporting data. */
struct vp_event {
    uint64_t timestamp_us;
    int64_t value;
    uint32_t name_id;
    uint32_t thread_id;
    uint32_t correlation_id;
    uint16_t type;
    uint16_t flags;
};

struct vp_wire_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t event_size;
    uint16_t flags;
    uint32_t clock_hz;
    uint64_t stream_start_us;
    uint64_t reserved;
};

/* One slot is caller-owned storage, not part of the export format. reserved
 * keeps the event naturally aligned on both Vita ARM EABI and common hosts. */
struct vp_slot {
    volatile uint32_t sequence;
    uint32_t reserved;
    struct vp_event event;
};

/* Producers may invoke both callbacks concurrently. clock must be reentrant and
 * return a monotonic timestamp in microseconds. thread_id must be reentrant and
 * identify the calling producer thread. Both user pointers must remain valid
 * until every producer has stopped and the context is deinitialized. */
typedef uint64_t (*vp_clock_fn)(void* user);
typedef uint32_t (*vp_thread_id_fn)(void* user);

struct vp_config {
    struct vp_slot* slots;
    uint32_t capacity;
    vp_clock_fn clock;
    void* clock_user;
    vp_thread_id_fn thread_id;
    void* thread_user;
};

/* The context and slots are caller-owned. Treat the fields as private after
 * vp_init(); they are public only so applications can allocate them statically. */
struct vp_context {
    struct vp_slot* slots;
    uint32_t capacity;
    uint32_t capacity_mask;
    volatile uint32_t enqueue_pos;
    volatile uint32_t dequeue_pos;
    volatile uint32_t accepted;
    volatile uint32_t dropped;
    volatile uint32_t next_correlation_id;
    uint32_t initialized;
    vp_clock_fn clock;
    void* clock_user;
    vp_thread_id_fn thread_id;
    void* thread_user;
    uint64_t last_frame_timestamp_us;
    uint32_t next_frame_id;
    uint32_t has_frame_timestamp;
};

struct vp_zone_scope {
    uint64_t begin_timestamp_us;
    uint32_t name_id;
    uint32_t thread_id;
    uint32_t correlation_id;
    uint32_t active;
};

struct vp_stats {
    uint32_t capacity;
    uint32_t pending;
    uint32_t accepted;
    uint32_t dropped;
};

struct vp_vita_memory_snapshot {
    uint64_t timestamp_us;
    uint64_t process_time_us;
    uint32_t free_user_bytes;
    uint32_t free_cdram_bytes;
    uint32_t free_phycont_bytes;
    uint32_t reserved;
};

struct vp_vita_thread_snapshot {
    uint64_t timestamp_us;
    uint64_t run_clocks;
    uint32_t thread_id;
    int32_t stack_free_bytes;
    uint32_t thread_preemptions;
    uint32_t interrupt_preemptions;
};

/* capacity must be a power of two in [2, 2^30]. Initialization and teardown
 * are quiescent operations; producers and the consumer must not be active. */
int vp_init(struct vp_context* context, const struct vp_config* config);
void vp_deinit(struct vp_context* context);

/* Multi-producer safe recording primitives. The supplied record is copied.
 * A full ring increments dropped and returns VP_RESULT_DROPPED without waiting.
 * A producer must be allowed to return from a record call: asynchronously
 * terminating it after slot reservation can leave the consumer unable to pass
 * that unpublished slot. */
int vp_record(struct vp_context* context, const struct vp_event* event);
int vp_emit(struct vp_context* context, uint16_t type, uint16_t flags,
            uint32_t name_id, int64_t value, uint32_t correlation_id);
int vp_counter(struct vp_context* context, uint32_t name_id, int64_t value);
int vp_zone_begin(struct vp_context* context, uint32_t name_id,
                  struct vp_zone_scope* scope);
int vp_zone_end(struct vp_context* context, struct vp_zone_scope* scope);

/* Frame markers keep one small piece of non-atomic state. Call vp_frame_mark()
 * from one designated frame thread only. value is the elapsed microseconds
 * since the previous marker, or zero with FIRST set for the initial marker. */
int vp_frame_mark(struct vp_context* context, uint32_t name_id);

/* Exactly one consumer may drain a context. Producers may record concurrently.
 * The function never waits; it returns the number of complete events copied. */
size_t vp_drain(struct vp_context* context, struct vp_event* events,
                size_t max_events);
int vp_get_stats(const struct vp_context* context, struct vp_stats* stats);

/* Stable 32-bit FNV-1a name ID. Zero is reserved for unnamed events. */
uint32_t vp_name_id(const char* name);

void vp_wire_header_init(struct vp_wire_header* header,
                         uint64_t stream_start_us);
int vp_encode_wire_header_le(const struct vp_wire_header* header,
                             uint8_t output[VP_WIRE_HEADER_SIZE]);
int vp_encode_event_le(const struct vp_event* event,
                       uint8_t output[VP_WIRE_EVENT_SIZE]);

/* User-mode Vita adapter. No kernel companion is required. thread_id == 0
 * selects the caller. Other IDs must already be known to the application.
 * The record helpers expand one captured snapshot into several independent
 * ring records. They are non-transactional: a full ring can accept a prefix,
 * drop the rest, and return VP_RESULT_DROPPED. Do not blindly retry, because
 * that can duplicate the accepted prefix. Atomic batch reservation and a
 * snapshot correlation ID are future wire revisions. */
int vp_vita_init(struct vp_context* context, struct vp_slot* slots,
                 uint32_t capacity);
int vp_vita_capture_memory(struct vp_vita_memory_snapshot* snapshot);
int vp_vita_record_memory(struct vp_context* context,
                          struct vp_vita_memory_snapshot* snapshot);
int vp_vita_capture_thread(uint32_t thread_id,
                           struct vp_vita_thread_snapshot* snapshot);
int vp_vita_record_thread(struct vp_context* context, uint32_t thread_id,
                          struct vp_vita_thread_snapshot* snapshot);

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
#define VP_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define VP_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

VP_STATIC_ASSERT(sizeof(struct vp_event) == VP_WIRE_EVENT_SIZE,
                 "vp_event must remain 32 bytes");
VP_STATIC_ASSERT(offsetof(struct vp_event, timestamp_us) == 0,
                 "vp_event timestamp offset changed");
VP_STATIC_ASSERT(offsetof(struct vp_event, value) == 8,
                 "vp_event value offset changed");
VP_STATIC_ASSERT(offsetof(struct vp_event, name_id) == 16,
                 "vp_event name offset changed");
VP_STATIC_ASSERT(offsetof(struct vp_event, thread_id) == 20,
                 "vp_event thread offset changed");
VP_STATIC_ASSERT(offsetof(struct vp_event, correlation_id) == 24,
                 "vp_event correlation offset changed");
VP_STATIC_ASSERT(offsetof(struct vp_event, type) == 28,
                 "vp_event type offset changed");
VP_STATIC_ASSERT(offsetof(struct vp_event, flags) == 30,
                 "vp_event flags offset changed");
VP_STATIC_ASSERT(sizeof(struct vp_wire_header) == VP_WIRE_HEADER_SIZE,
                 "vp_wire_header must remain 32 bytes");
VP_STATIC_ASSERT(offsetof(struct vp_wire_header, stream_start_us) == 16,
                 "vp_wire_header timestamp offset changed");
VP_STATIC_ASSERT(offsetof(struct vp_wire_header, reserved) == 24,
                 "vp_wire_header reserved offset changed");
VP_STATIC_ASSERT(sizeof(struct vp_slot) == 40,
                 "vp_slot ABI changed");
VP_STATIC_ASSERT(sizeof(struct vp_vita_memory_snapshot) == 32,
                 "memory snapshot ABI changed");
VP_STATIC_ASSERT(sizeof(struct vp_vita_thread_snapshot) == 32,
                 "thread snapshot ABI changed");

#undef VP_STATIC_ASSERT

#endif

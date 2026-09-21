#ifndef VITAPROFILER_STREAM_H
#define VITAPROFILER_STREAM_H

#include "vitaprofiler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A stream sink must consume the complete buffer before returning zero. It is
 * called only by the single profiler consumer. A nonzero return permanently
 * fails that writer instance because bytes may already be visible to the peer. */
typedef int (*vp_stream_write_fn)(void* user, const uint8_t* data,
                                  size_t size);

#define VP_STREAM_V2_CHUNK_MAGIC UINT32_C(0x32435056)
#define VP_STREAM_V2_VERSION 2u
#define VP_STREAM_V2_CHUNK_HEADER_SIZE 32u
#define VP_STREAM_V2_SESSION_PAYLOAD_SIZE 96u
#define VP_STREAM_V2_THREAD_PAYLOAD_SIZE 32u
#define VP_STREAM_V2_MODULE_PAYLOAD_SIZE 32u
#define VP_STREAM_V2_STATS_PAYLOAD_SIZE 32u
#define VP_STREAM_V2_TIMER_SOURCE_MAX 40u
#define VP_STREAM_V2_TIMER_UNIT_MAX 16u
#define VP_STREAM_V2_MAX_CHUNK_PAYLOAD (2u * 1024u * 1024u)

enum vp_stream_v2_chunk_type {
    VP_STREAM_V2_CHUNK_SESSION = 1,
    VP_STREAM_V2_CHUNK_DICTIONARY = 2,
    VP_STREAM_V2_CHUNK_EVENTS = 3,
    VP_STREAM_V2_CHUNK_THREAD = 4,
    VP_STREAM_V2_CHUNK_MODULE = 5,
    VP_STREAM_V2_CHUNK_STATS = 6,
    VP_STREAM_V2_CHUNK_END = 7,
};

enum vp_stream_v2_session_flags {
    VP_STREAM_V2_SESSION_PROCESS_ID = 1u << 0,
    VP_STREAM_V2_SESSION_PROCESS_IDENTITY = 1u << 1,
    VP_STREAM_V2_SESSION_TIMER_SOURCE = 1u << 2,
    VP_STREAM_V2_SESSION_TIMER_UNIT = 1u << 3,
};

enum vp_stream_v2_thread_flags {
    VP_STREAM_V2_THREAD_IDENTITY = 1u << 0,
};

enum vp_stream_v2_module_flags {
    VP_STREAM_V2_MODULE_EXECUTABLE = 1u << 0,
    VP_STREAM_V2_MODULE_ARM = 1u << 1,
    VP_STREAM_V2_MODULE_THUMB = 1u << 2,
};

struct vp_stream_v2_session {
    uint64_t session_id;
    uint64_t process_identity;
    uint32_t process_id;
    uint32_t flags;
    const char* timer_source;
    const char* timer_unit;
};

struct vp_stream_v2_thread {
    uint64_t identity;
    uint32_t thread_id;
    uint32_t generation;
    uint32_t name_id;
    uint32_t flags;
};

struct vp_stream_v2_module {
    uint32_t module_id;
    uint32_t generation;
    uint32_t address_start;
    uint32_t address_end;
    uint32_t name_id;
    uint32_t flags;
};

struct vp_stream_v2_chunk_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t type;
    uint16_t flags;
    uint32_t payload_size;
    uint32_t sequence;
    uint32_t payload_crc32;
    uint64_t reserved;
};

struct vp_stream_v2_chunk_view {
    struct vp_stream_v2_chunk_header header;
    const uint8_t* payload;
};

struct vp_stream_v2_info {
    uint64_t session_id;
    size_t chunk_count;
    size_t event_count;
    size_t total_size;
    uint32_t complete;
};

struct vp_stream_v2_cursor {
    const uint8_t* data;
    size_t total_size;
    size_t offset;
    uint32_t next_sequence;
};

enum vp_stream_writer_state {
    VP_STREAM_WRITER_UNINITIALIZED = 0,
    VP_STREAM_WRITER_READY = 1,
    VP_STREAM_WRITER_STREAMING = 2,
    VP_STREAM_WRITER_CLOSED = 3,
    VP_STREAM_WRITER_FAILED = 4,
};

struct vp_stream_writer_config {
    struct vp_context* context;
    const struct vp_name_dictionary* names;
    vp_stream_write_fn write;
    void* write_user;

    /* vp_encode_name_dictionary_le() needs one contiguous caller-owned block.
     * Query vp_name_dictionary_wire_size() when sizing this buffer. */
    uint8_t* dictionary_buffer;
    size_t dictionary_buffer_capacity;
};

/* Fields are public only to permit static allocation. Treat them as private
 * after vp_stream_writer_init(). */
struct vp_stream_writer {
    struct vp_context* context;
    const struct vp_name_dictionary* names;
    vp_stream_write_fn write;
    void* write_user;
    uint8_t* dictionary_buffer;
    size_t dictionary_buffer_capacity;
    uint64_t bytes_written;
    uint64_t events_written;
    uint32_t events_lost_to_sink;
    uint32_t transport_events_lost;
    uint32_t chunk_sequence;
    uint32_t wire_version;
    uint32_t state;
    uint32_t initialized;
};

struct vp_stream_writer_stats {
    uint64_t bytes_written;
    uint64_t events_written;
    uint32_t events_lost_to_sink;
    uint32_t transport_events_lost;
    uint32_t wire_version;
    uint32_t state;
};

/* Initialization performs all capacity and sealed-dictionary checks before a
 * byte reaches the sink. begin writes VPNM followed immediately by VPRF.
 * drain removes up to max_events complete records from the ring and serializes
 * them. A sink failure after removal accounts the one undelivered record in
 * events_lost_to_sink and makes the writer permanently failed. */
int vp_stream_writer_init(struct vp_stream_writer* writer,
                          const struct vp_stream_writer_config* config);
int vp_stream_writer_begin(struct vp_stream_writer* writer,
                           uint64_t stream_start_us);
/* Version 2 wraps the unchanged VPNM dictionary and 32-byte event records in
 * CRC-checked, sequence-numbered chunks. The caller-owned dictionary buffer is
 * reused as bounded event-chunk staging after begin. */
int vp_stream_writer_begin_v2(
    struct vp_stream_writer* writer, uint64_t stream_start_us,
    const struct vp_stream_v2_session* session);
int vp_stream_writer_write_thread_v2(
    struct vp_stream_writer* writer,
    const struct vp_stream_v2_thread* thread);
int vp_stream_writer_write_module_v2(
    struct vp_stream_writer* writer,
    const struct vp_stream_v2_module* module);
int vp_stream_writer_write_stats_v2(struct vp_stream_writer* writer);
int vp_stream_writer_set_transport_loss_v2(
    struct vp_stream_writer* writer, uint32_t events_lost);
int vp_stream_writer_drain(struct vp_stream_writer* writer,
                           size_t max_events, size_t* events_written);
/* Stop producers first, drain until the ring is empty, then close. Close
 * returns VP_ERROR_BUSY and leaves the writer streaming while records remain;
 * it never silently labels an incomplete capture as cleanly closed. */
int vp_stream_writer_close(struct vp_stream_writer* writer);
int vp_stream_writer_get_stats(const struct vp_stream_writer* writer,
                               struct vp_stream_writer_stats* stats);

/* Allocation-free receiver helpers for one complete v2 session. Cursor
 * initialization validates every chunk, CRC, sequence, payload shape, and the
 * required SESSION -> DICTIONARY -> ... -> END lifecycle before exposing data.
 * An absent END chunk is malformed, never silently treated as complete. */
int vp_stream_v2_decode_chunk_le(
    const uint8_t* input, size_t input_size,
    struct vp_stream_v2_chunk_view* chunk);
int vp_stream_v2_cursor_init(
    struct vp_stream_v2_cursor* cursor, const uint8_t* input,
    size_t input_size, struct vp_stream_v2_info* info);
int vp_stream_v2_cursor_next(
    struct vp_stream_v2_cursor* cursor,
    struct vp_stream_v2_chunk_view* chunk);

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
static_assert(sizeof(struct vp_stream_v2_chunk_header) ==
                  VP_STREAM_V2_CHUNK_HEADER_SIZE,
              "v2 chunk header must remain 32 bytes");
#else
_Static_assert(sizeof(struct vp_stream_v2_chunk_header) ==
                   VP_STREAM_V2_CHUNK_HEADER_SIZE,
               "v2 chunk header must remain 32 bytes");
#endif

#endif

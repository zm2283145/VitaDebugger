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
    uint32_t state;
    uint32_t initialized;
};

struct vp_stream_writer_stats {
    uint64_t bytes_written;
    uint64_t events_written;
    uint32_t events_lost_to_sink;
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
int vp_stream_writer_drain(struct vp_stream_writer* writer,
                           size_t max_events, size_t* events_written);
/* Stop producers first, drain until the ring is empty, then close. Close
 * returns VP_ERROR_BUSY and leaves the writer streaming while records remain;
 * it never silently labels an incomplete capture as cleanly closed. */
int vp_stream_writer_close(struct vp_stream_writer* writer);
int vp_stream_writer_get_stats(const struct vp_stream_writer* writer,
                               struct vp_stream_writer_stats* stats);

#ifdef __cplusplus
}
#endif

#endif

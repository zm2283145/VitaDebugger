#ifndef VITADEBUG_SCREEN_H
#define VITADEBUG_SCREEN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_SCREEN_PROTOCOL_VERSION 1u
#define VD_SCREEN_AUTH_SIZE 72u
#define VD_SCREEN_FRAME_HEADER_SIZE 64u
#define VD_SCREEN_AUTH_TOKEN_SIZE 32u
#define VD_SCREEN_TITLE_ID_SIZE 9u
#define VD_SCREEN_MAX_SOURCES 3u
#define VD_SCREEN_DEFAULT_MAX_WIDTH 960u
#define VD_SCREEN_DEFAULT_MAX_HEIGHT 544u
#define VD_SCREEN_DEFAULT_MAX_PAYLOAD (4u * 1024u * 1024u)
#define VD_SCREEN_DEFAULT_MIN_FRAME_INTERVAL_US UINT64_C(100000)
#define VD_SCREEN_MIN_FRAME_INTERVAL_US UINT64_C(16667)
#define VD_SCREEN_EXPLICIT_CONSENT UINT32_C(0x5343524e)

#define VD_SCREEN_FRAME_FLAG_SOURCE_OWNED UINT32_C(1)

enum vd_screen_result {
    VD_SCREEN_OK = 0,
    VD_SCREEN_DROPPED = 1,
    VD_SCREEN_ERROR_INVALID_ARGUMENT = -1,
    VD_SCREEN_ERROR_DISABLED = -2,
    VD_SCREEN_ERROR_STATE = -3,
    VD_SCREEN_ERROR_FORMAT = -4,
    VD_SCREEN_ERROR_BOUNDS = -5,
    VD_SCREEN_ERROR_SOURCE = -6,
    VD_SCREEN_ERROR_TIMESTAMP = -7,
    VD_SCREEN_ERROR_IO = -8,
};

enum vd_screen_pixel_format {
    VD_SCREEN_PIXEL_RGBA8888 = 1,
    VD_SCREEN_PIXEL_BGRA8888 = 2,
    VD_SCREEN_PIXEL_RGB565_LE = 3,
};

enum vd_screen_state {
    VD_SCREEN_STATE_UNINITIALIZED = 0,
    VD_SCREEN_STATE_READY = 1,
    VD_SCREEN_STATE_STREAMING = 2,
    VD_SCREEN_STATE_CLOSED = 3,
    VD_SCREEN_STATE_FAILED = 4,
};

typedef int (*vd_screen_write_fn)(void* user, const uint8_t* data,
                                  size_t size);

struct vd_screen_source {
    const void* base;
    size_t capacity;
};

struct vd_screen_config {
    uint32_t explicit_consent;
    vd_screen_write_fn write;
    void* write_user;
    const struct vd_screen_source* sources;
    size_t source_count;
    uint8_t auth_token[VD_SCREEN_AUTH_TOKEN_SIZE];
    char title_id[VD_SCREEN_TITLE_ID_SIZE + 1u];
    uint32_t process_id;
    uint64_t process_generation;
    uint64_t session_id;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_payload_bytes;
    uint64_t min_frame_interval_us;
};

struct vd_screen_frame {
    const void* pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t pixel_format;
    uint64_t timestamp_us;
};

/* Public only for caller-owned/static allocation. Zero-initialize before the
 * first init and treat fields as private thereafter. */
struct vd_screen_stream {
    vd_screen_write_fn write;
    void* write_user;
    struct vd_screen_source sources[VD_SCREEN_MAX_SOURCES];
    uint8_t auth_token[VD_SCREEN_AUTH_TOKEN_SIZE];
    char title_id[VD_SCREEN_TITLE_ID_SIZE + 1u];
    uint32_t process_id;
    uint64_t process_generation;
    uint64_t session_id;
    uint64_t next_sequence;
    uint64_t last_timestamp_us;
    uint64_t min_frame_interval_us;
    uint64_t frames_sent;
    uint64_t frames_dropped;
    uint64_t bytes_sent;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_payload_bytes;
    uint32_t source_count;
    uint32_t state;
    uint32_t initialized;
    uint32_t has_timestamp;
};

struct vd_screen_stats {
    uint64_t frames_sent;
    uint64_t frames_dropped;
    uint64_t bytes_sent;
    uint64_t next_sequence;
    uint32_t state;
};

void vd_screen_config_init(struct vd_screen_config* config);
int vd_screen_stream_init(struct vd_screen_stream* stream,
                          const struct vd_screen_config* config);
int vd_screen_stream_begin(struct vd_screen_stream* stream);

/* The pixels pointer must exactly match a source registered at init and the
 * complete stride * height range must fit that source. The application calls
 * this only after it has selected that same buffer for display. There is no
 * display lookup, process lookup, arbitrary address, or kernel path. */
int vd_screen_submit_displayed_frame(struct vd_screen_stream* stream,
                                     const struct vd_screen_frame* frame);

int vd_screen_stream_close(struct vd_screen_stream* stream);
int vd_screen_stream_get_stats(const struct vd_screen_stream* stream,
                               struct vd_screen_stats* stats);

#ifdef __cplusplus
}
#endif

#endif

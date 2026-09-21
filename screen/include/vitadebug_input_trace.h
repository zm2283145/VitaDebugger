#ifndef VITADEBUG_INPUT_TRACE_H
#define VITADEBUG_INPUT_TRACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_INPUT_TRACE_VERSION 1u
#define VD_INPUT_TRACE_HEADER_SIZE 96u
#define VD_INPUT_TRACE_EVENT_SIZE 64u
#define VD_INPUT_TRACE_MAX_EVENTS 4096u
#define VD_INPUT_TRACE_MAX_DURATION_US UINT64_C(3600000000)
#define VD_INPUT_TRACE_DEFAULT_MAX_DRIFT_US UINT64_C(100000)
#define VD_INPUT_TRACE_MAX_DRIFT_US UINT64_C(1000000)
#define VD_INPUT_TRACE_MAX_EVENTS_PER_TICK 32u
#define VD_INPUT_TRACE_NO_FRAME UINT64_MAX
#define VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT UINT32_C(0x56445243)
#define VD_INPUT_TRACE_EXPLICIT_PLAYBACK_CONSENT UINT32_C(0x56445042)

#define VD_INPUT_BUTTON_SELECT UINT32_C(0x00000001)
#define VD_INPUT_BUTTON_START UINT32_C(0x00000008)
#define VD_INPUT_BUTTON_UP UINT32_C(0x00000010)
#define VD_INPUT_BUTTON_RIGHT UINT32_C(0x00000020)
#define VD_INPUT_BUTTON_DOWN UINT32_C(0x00000040)
#define VD_INPUT_BUTTON_LEFT UINT32_C(0x00000080)
#define VD_INPUT_BUTTON_LEFT_TRIGGER UINT32_C(0x00000100)
#define VD_INPUT_BUTTON_RIGHT_TRIGGER UINT32_C(0x00000200)
#define VD_INPUT_BUTTON_TRIANGLE UINT32_C(0x00001000)
#define VD_INPUT_BUTTON_CIRCLE UINT32_C(0x00002000)
#define VD_INPUT_BUTTON_CROSS UINT32_C(0x00004000)
#define VD_INPUT_BUTTON_SQUARE UINT32_C(0x00008000)
#define VD_INPUT_BUTTON_ALLOWED_MASK UINT32_C(0x0000f3f9)

enum vd_input_trace_result {
    VD_INPUT_TRACE_OK = 0,
    VD_INPUT_TRACE_COMPLETE = 1,
    VD_INPUT_TRACE_ERROR_ARGUMENT = -1,
    VD_INPUT_TRACE_ERROR_STATE = -2,
    VD_INPUT_TRACE_ERROR_IDENTITY = -3,
    VD_INPUT_TRACE_ERROR_FORMAT = -4,
    VD_INPUT_TRACE_ERROR_CHECKSUM = -5,
    VD_INPUT_TRACE_ERROR_LIMIT = -6,
    VD_INPUT_TRACE_ERROR_TIME = -7,
    VD_INPUT_TRACE_ERROR_CALLBACK = -8,
    VD_INPUT_TRACE_ERROR_UNSUPPORTED = -9,
};

enum vd_input_trace_state {
    VD_INPUT_TRACE_STATE_DISABLED = 0,
    VD_INPUT_TRACE_STATE_IDLE = 1,
    VD_INPUT_TRACE_STATE_RECORDING = 2,
    VD_INPUT_TRACE_STATE_READY = 3,
    VD_INPUT_TRACE_STATE_PLAYING = 4,
    VD_INPUT_TRACE_STATE_FAILED = 5,
};

enum vd_input_trace_event_kind {
    VD_INPUT_TRACE_EVENT_INPUT = 1,
    VD_INPUT_TRACE_EVENT_CHECKPOINT = 2,
};

enum vd_input_trace_checkpoint {
    VD_INPUT_TRACE_CHECKPOINT_FRAME = 1,
    VD_INPUT_TRACE_CHECKPOINT_ASSERTION = 2,
    VD_INPUT_TRACE_CHECKPOINT_USER = 3,
};

enum vd_input_trace_end_reason {
    VD_INPUT_TRACE_END_COMPLETE = 1,
    VD_INPUT_TRACE_END_ABORTED = 2,
    VD_INPUT_TRACE_END_OVERFLOW = 3,
    VD_INPUT_TRACE_END_DISCONNECT = 4,
    VD_INPUT_TRACE_END_TIMEOUT = 5,
    VD_INPUT_TRACE_END_CALLBACK_FAILURE = 6,
    VD_INPUT_TRACE_END_SHUTDOWN = 7,
    VD_INPUT_TRACE_END_IDENTITY_CHANGE = 8,
    VD_INPUT_TRACE_END_CANCELLED = 9,
};

struct vd_input_touch {
    uint16_t id;
    uint16_t x;
    uint16_t y;
    uint16_t force;
};

struct vd_input_state {
    uint32_t buttons;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
    uint8_t touch_count;
    struct vd_input_touch touches[2];
};

struct vd_input_trace_identity {
    char title_id[10];
    uint32_t process_id;
    uint64_t process_generation;
    uint64_t session_id;
};

struct vd_input_trace_config {
    uint8_t* buffer;
    size_t buffer_capacity;
    uint32_t max_events;
    uint64_t max_duration_us;
    uint64_t max_scheduling_drift_us;
    struct vd_input_trace_identity identity;
};

struct vd_input_trace_info {
    uint32_t state;
    uint32_t event_count;
    uint32_t end_reason;
    uint64_t trace_id;
    uint64_t duration_us;
    uint64_t max_scheduling_drift_us;
    size_t data_size;
};

typedef int (*vd_input_trace_apply_fn)(
    void* user, const struct vd_input_state* input);

/* Public for caller-owned/static allocation. Treat fields as private. */
struct vd_input_trace {
    uint8_t* buffer;
    size_t buffer_capacity;
    struct vd_input_trace_identity identity;
    uint64_t started_us;
    uint64_t last_relative_us;
    uint64_t last_frame_index;
    uint64_t playback_started_us;
    uint64_t max_duration_us;
    uint64_t max_scheduling_drift_us;
    uint64_t observed_max_drift_us;
    uint64_t trace_id;
    size_t data_size;
    uint32_t max_events;
    uint32_t event_count;
    uint32_t playback_index;
    uint32_t end_reason;
    uint32_t state;
    uint32_t initialized;
    uint32_t has_event;
    uint32_t has_frame;
};

void vd_input_trace_config_init(struct vd_input_trace_config* config);
int vd_input_trace_init(struct vd_input_trace* trace,
                        const struct vd_input_trace_config* config);
int vd_input_trace_record_begin(struct vd_input_trace* trace,
                                uint64_t trace_id, uint64_t now_us);
int vd_input_trace_record_input(struct vd_input_trace* trace,
                                const struct vd_input_state* input,
                                uint64_t now_us, uint64_t frame_index);
int vd_input_trace_record_checkpoint(struct vd_input_trace* trace,
                                     uint32_t marker, uint64_t now_us,
                                     uint64_t frame_index);
int vd_input_trace_record_end(struct vd_input_trace* trace,
                              uint32_t end_reason);
int vd_input_trace_import(struct vd_input_trace* trace,
                          const uint8_t* data, size_t data_size);
int vd_input_trace_playback_begin(struct vd_input_trace* trace,
                                  uint64_t now_us);
int vd_input_trace_playback_tick(struct vd_input_trace* trace,
                                 uint64_t now_us,
                                 vd_input_trace_apply_fn apply,
                                 void* apply_user);
int vd_input_trace_abort(struct vd_input_trace* trace, uint32_t end_reason);
int vd_input_trace_pause(struct vd_input_trace* trace);
int vd_input_trace_get_info(const struct vd_input_trace* trace,
                            struct vd_input_trace_info* info);
int vd_input_trace_data(const struct vd_input_trace* trace,
                        const uint8_t** data, size_t* data_size);
int vd_input_trace_verify(const uint8_t* data, size_t data_size,
                          const struct vd_input_trace_identity* expected,
                          struct vd_input_trace_info* info);

#ifdef __cplusplus
}
#endif

#endif

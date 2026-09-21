#ifndef VITADEBUG_COMPANION_H
#define VITADEBUG_COMPANION_H

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_input_trace.h"
#include "vitadebug_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VD_COMPANION_PROTOCOL_VERSION 1u
#define VD_COMPANION_HEADER_SIZE 80u
#define VD_COMPANION_TAG_SIZE 16u
#define VD_COMPANION_SECRET_SIZE 32u
#define VD_COMPANION_MAX_PAYLOAD 4096u
#define VD_COMPANION_MAX_RECORD \
    (VD_COMPANION_HEADER_SIZE + VD_COMPANION_MAX_PAYLOAD)
#define VD_COMPANION_MAX_PATH 255u
#define VD_COMPANION_MAX_NAME 63u
#define VD_COMPANION_MAX_LIST_ENTRIES 32u
#define VD_COMPANION_MAX_READ_BYTES 2048u
#define VD_COMPANION_MAX_TOTAL_READ_BYTES (1024u * 1024u)
#define VD_COMPANION_DEFAULT_SCREEN_PORT 18197u
#define VD_COMPANION_DEFAULT_CONTROL_PORT 18198u
#define VD_COMPANION_MIN_PORT 18000u
#define VD_COMPANION_MAX_PORT 18999u
#define VD_COMPANION_MAX_DEADLINE_MS 5000u
#define VD_COMPANION_MIN_INPUT_LEASE_MS 50u
#define VD_COMPANION_MAX_INPUT_LEASE_MS 1000u
#define VD_COMPANION_EXPLICIT_CONSENT UINT32_C(0x56444350)
#define VD_COMPANION_LAN_CONSENT UINT32_C(0x56444c41)
#define VD_COMPANION_MUTATION_CONSENT UINT32_C(0x56444d55)

#define VD_COMPANION_CAP_STATUS UINT64_C(1)
#define VD_COMPANION_CAP_APP_INPUT UINT64_C(2)
#define VD_COMPANION_CAP_DEBUG_FILES UINT64_C(4)
#define VD_COMPANION_CAP_SCREEN UINT64_C(8)
#define VD_COMPANION_CAP_INPUT_RECORD UINT64_C(16)
#define VD_COMPANION_CAP_INPUT_PLAYBACK UINT64_C(32)
#define VD_COMPANION_CAP_TITLE_INVENTORY UINT64_C(64)
#define VD_COMPANION_CAP_TITLE_LAUNCH UINT64_C(128)
#define VD_COMPANION_CAP_SUPPORTED                                      \
    (VD_COMPANION_CAP_STATUS | VD_COMPANION_CAP_APP_INPUT |            \
     VD_COMPANION_CAP_DEBUG_FILES | VD_COMPANION_CAP_SCREEN |           \
     VD_COMPANION_CAP_INPUT_RECORD | VD_COMPANION_CAP_INPUT_PLAYBACK)

#define VD_COMPANION_FS_INSIDE_ROOT UINT32_C(1)
#define VD_COMPANION_FS_NO_SYMLINKS UINT32_C(2)
#define VD_COMPANION_FS_REGULAR UINT32_C(4)
#define VD_COMPANION_FS_DIRECTORY UINT32_C(8)
#define VD_COMPANION_FS_REQUIRED_REGULAR                                \
    (VD_COMPANION_FS_INSIDE_ROOT | VD_COMPANION_FS_NO_SYMLINKS |       \
     VD_COMPANION_FS_REGULAR)
#define VD_COMPANION_FS_REQUIRED_DIRECTORY                              \
    (VD_COMPANION_FS_INSIDE_ROOT | VD_COMPANION_FS_NO_SYMLINKS |       \
     VD_COMPANION_FS_DIRECTORY)

enum vd_companion_result {
    VD_COMPANION_OK = 0,
    VD_COMPANION_ERROR_INVALID_ARGUMENT = -1,
    VD_COMPANION_ERROR_DISABLED = -2,
    VD_COMPANION_ERROR_STATE = -3,
    VD_COMPANION_ERROR_PORT = -4,
    VD_COMPANION_ERROR_BIND = -5,
    VD_COMPANION_ERROR_AUTH = -6,
    VD_COMPANION_ERROR_PROTOCOL = -7,
    VD_COMPANION_ERROR_REPLAY = -8,
    VD_COMPANION_ERROR_DEADLINE = -9,
    VD_COMPANION_ERROR_UNSUPPORTED = -10,
    VD_COMPANION_ERROR_CAPABILITY = -11,
    VD_COMPANION_ERROR_INPUT = -12,
    VD_COMPANION_ERROR_PATH = -13,
    VD_COMPANION_ERROR_FILESYSTEM = -14,
    VD_COMPANION_ERROR_LIMIT = -15,
    VD_COMPANION_ERROR_IO = -16,
};

enum vd_companion_state {
    VD_COMPANION_STATE_UNINITIALIZED = 0,
    VD_COMPANION_STATE_LISTENING = 1,
    VD_COMPANION_STATE_PAIRED = 2,
    VD_COMPANION_STATE_CLOSED = 3,
    VD_COMPANION_STATE_FAILED = 4,
};

enum vd_companion_network_scope {
    VD_COMPANION_NETWORK_LOOPBACK = 1,
    VD_COMPANION_NETWORK_PRIVATE_LAN = 2,
};

enum vd_companion_message_type {
    VD_COMPANION_MESSAGE_HELLO = 1,
    VD_COMPANION_MESSAGE_STATUS = 2,
    VD_COMPANION_MESSAGE_INPUT = 3,
    VD_COMPANION_MESSAGE_FILE_LIST = 4,
    VD_COMPANION_MESSAGE_FILE_READ = 5,
    VD_COMPANION_MESSAGE_TITLE_INVENTORY = 6,
    VD_COMPANION_MESSAGE_TITLE_LAUNCH = 7,
    VD_COMPANION_MESSAGE_RESPONSE = 0x8000,
};

struct vd_companion_input {
    uint32_t buttons;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
    uint8_t touch_count;
    struct vd_input_touch touches[2];
};

struct vd_companion_file_entry {
    char name[VD_COMPANION_MAX_NAME + 1u];
    uint64_t size;
    uint32_t flags;
};

typedef int (*vd_companion_bind_fn)(void* user, uint32_t network_scope,
                                    uint16_t port);
typedef int (*vd_companion_send_fn)(void* user, const uint8_t* record,
                                    size_t record_size);
typedef int (*vd_companion_close_fn)(void* user);
typedef int (*vd_companion_input_fn)(
    void* user, const struct vd_companion_input* input);

/*
 * bind must attempt exactly the supplied scope/port and clean up its own
 * partial resources on failure. The transport accepts one client per service
 * initialization, reads at most VD_COMPANION_MAX_RECORD under an absolute
 * deadline, and passes only one complete record to service_process. On EOF,
 * partial read, timeout, or processing error it calls service_disconnect.
 */

/*
 * Filesystem callbacks must resolve and operate relative to the configured
 * application-owned debug root. They return the required INSIDE_ROOT and
 * NO_SYMLINKS flags only after rejecting links, devices, mount escapes, and
 * type changes. read_regular must perform those checks on the opened object,
 * not only on an earlier pathname lookup.
 */
typedef int (*vd_companion_fs_resolve_fn)(
    void* user, const char* relative_path, char* canonical_path,
    size_t canonical_capacity, uint32_t* flags, uint64_t* size);
typedef int (*vd_companion_fs_list_fn)(
    void* user, const char* canonical_directory,
    struct vd_companion_file_entry* entries, size_t entry_capacity,
    size_t* entry_count);
typedef int (*vd_companion_fs_read_fn)(
    void* user, const char* canonical_file, uint64_t offset, uint8_t* output,
    size_t output_capacity, size_t* output_size, uint32_t* flags,
    uint64_t* file_size);

struct vd_companion_filesystem {
    void* user;
    vd_companion_fs_resolve_fn resolve;
    vd_companion_fs_list_fn list;
    vd_companion_fs_read_fn read_regular;
};

struct vd_companion_screen_config {
    vd_screen_write_fn write;
    void* write_user;
    const struct vd_screen_source* sources;
    size_t source_count;
    uint8_t auth_token[VD_SCREEN_AUTH_TOKEN_SIZE];
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_payload_bytes;
    uint64_t min_frame_interval_us;
};

struct vd_companion_config {
    uint32_t explicit_consent;
    uint32_t lan_consent;
    uint32_t mutation_consent;
    uint32_t record_consent;
    uint32_t playback_consent;
    uint32_t network_scope;
    uint16_t screen_port;
    uint16_t control_port;
    uint64_t enabled_capabilities;
    uint8_t secret[VD_COMPANION_SECRET_SIZE];
    char title_id[VD_SCREEN_TITLE_ID_SIZE + 1u];
    uint32_t process_id;
    uint64_t process_generation;
    uint64_t session_id;
    const char* debug_root;
    vd_companion_bind_fn bind;
    vd_companion_send_fn send;
    vd_companion_close_fn close;
    void* transport_user;
    vd_companion_input_fn apply_input;
    void* input_user;
    struct vd_companion_filesystem filesystem;
    const struct vd_companion_screen_config* screen;
    uint8_t* trace_buffer;
    size_t trace_buffer_capacity;
    uint32_t trace_max_events;
    uint64_t trace_max_duration_us;
    uint64_t trace_max_scheduling_drift_us;
};

/* Public for caller-owned/static allocation. Zero-initialize before the first
 * init and treat fields as private thereafter. */
struct vd_companion_service {
    uint8_t secret[VD_COMPANION_SECRET_SIZE];
    char title_id[VD_SCREEN_TITLE_ID_SIZE + 1u];
    char debug_root[VD_COMPANION_MAX_PATH + 1u];
    uint32_t process_id;
    uint32_t network_scope;
    uint16_t screen_port;
    uint16_t control_port;
    uint64_t process_generation;
    uint64_t session_id;
    uint64_t enabled_capabilities;
    uint64_t negotiated_capabilities;
    uint64_t last_sequence;
    uint64_t input_lease_expires_ms;
    uint64_t total_file_bytes;
    uint32_t state;
    uint32_t initialized;
    uint32_t bound;
    uint32_t input_active;
    vd_companion_send_fn send;
    vd_companion_close_fn close;
    void* transport_user;
    vd_companion_input_fn apply_input;
    void* input_user;
    struct vd_companion_filesystem filesystem;
    struct vd_screen_stream screen;
    uint32_t screen_initialized;
    struct vd_input_trace trace;
    uint32_t trace_initialized;
    uint32_t record_consent;
    uint32_t playback_consent;
};

void vd_companion_config_init(struct vd_companion_config* config);
int vd_companion_validate_screen_port(uint16_t port);
int vd_companion_validate_control_port(uint16_t port);
int vd_companion_service_init(struct vd_companion_service* service,
                              const struct vd_companion_config* config);
int vd_companion_service_process(struct vd_companion_service* service,
                                 const uint8_t* record, size_t record_size,
                                 uint64_t now_ms);
int vd_companion_service_tick(struct vd_companion_service* service,
                              uint64_t now_ms);
int vd_companion_service_disconnect(struct vd_companion_service* service);
int vd_companion_service_identity_changed(
    struct vd_companion_service* service);
int vd_companion_service_close(struct vd_companion_service* service);

int vd_companion_screen_begin(struct vd_companion_service* service);
int vd_companion_submit_displayed_frame(
    struct vd_companion_service* service,
    const struct vd_screen_frame* frame);

int vd_companion_input_record_begin(struct vd_companion_service* service,
                                    uint64_t trace_id, uint64_t now_us);
int vd_companion_input_record_physical(
    struct vd_companion_service* service,
    const struct vd_companion_input* physical_input, uint64_t now_us,
    uint64_t frame_index);
int vd_companion_input_record_checkpoint(
    struct vd_companion_service* service, uint32_t marker,
    uint64_t now_us, uint64_t frame_index);
int vd_companion_input_record_end(struct vd_companion_service* service,
                                  uint32_t end_reason);
int vd_companion_input_trace_import(struct vd_companion_service* service,
                                    const uint8_t* data,
                                    size_t data_size);
int vd_companion_input_playback_begin(
    struct vd_companion_service* service, uint64_t now_us);
int vd_companion_input_playback_tick(
    struct vd_companion_service* service, uint64_t now_us);
int vd_companion_input_trace_abort(
    struct vd_companion_service* service, uint32_t end_reason);
int vd_companion_input_trace_pause(
    struct vd_companion_service* service);
int vd_companion_input_trace_get_info(
    const struct vd_companion_service* service,
    struct vd_input_trace_info* info);
int vd_companion_input_trace_data(
    const struct vd_companion_service* service, const uint8_t** data,
    size_t* data_size);

/* Test/host tooling helper for exact version-1 record construction. */
int vd_companion_encode_record(
    const uint8_t secret[VD_COMPANION_SECRET_SIZE], uint16_t message_type,
    uint16_t status, uint64_t sequence, uint64_t deadline_ms,
    uint64_t session_id, uint64_t process_generation,
    uint64_t capabilities, const uint8_t* payload, size_t payload_size,
    uint8_t* output, size_t output_capacity, size_t* output_size);

#ifdef __cplusplus
}
#endif

#endif

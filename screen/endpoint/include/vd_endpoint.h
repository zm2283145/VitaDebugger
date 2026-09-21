#ifndef VD_ENDPOINT_H
#define VD_ENDPOINT_H

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_companion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VD_ENDPOINT_CONFIG_SIZE 128u
#define VD_ENDPOINT_CONFIG_VERSION 1u
#define VD_ENDPOINT_CONFIG_PATH \
    "ux0:data/VitaDebuggerCompanion/config.bin"
#define VD_ENDPOINT_DEBUG_ROOT \
    "ux0:data/VitaDebuggerCompanion/debug"
#define VD_ENDPOINT_RECEIVE_DEADLINE_MS 5000u
#define VD_ENDPOINT_STATUS_SIZE 64u

#define VD_ENDPOINT_IO_OK 0
#define VD_ENDPOINT_IO_AGAIN 1
#define VD_ENDPOINT_IO_EOF 2
#define VD_ENDPOINT_IO_ERROR (-1)

#define VD_ENDPOINT_POLL_IDLE 0
#define VD_ENDPOINT_POLL_RECORD 1
#define VD_ENDPOINT_ERROR_ARGUMENT (-1)
#define VD_ENDPOINT_ERROR_CONFIG (-2)
#define VD_ENDPOINT_ERROR_LIMIT (-3)
#define VD_ENDPOINT_ERROR_DEADLINE (-4)
#define VD_ENDPOINT_ERROR_IO (-5)

struct vd_endpoint_config {
    uint32_t network_scope;
    uint64_t enabled_capabilities;
    uint8_t host_ipv4[4];
    uint8_t bind_ipv4[4];
    uint16_t control_port;
    uint16_t screen_port;
    uint8_t control_secret[VD_COMPANION_SECRET_SIZE];
    uint8_t screen_secret[VD_SCREEN_AUTH_TOKEN_SIZE];
    uint32_t mutation_consent;
    uint32_t record_consent;
    uint32_t playback_consent;
};

typedef int (*vd_endpoint_recv_fn)(void* user, uint8_t* output,
                                   size_t capacity, size_t* received);
typedef int (*vd_endpoint_send_fn)(void* user, const uint8_t* input,
                                   size_t size, size_t* sent);
typedef uint64_t (*vd_endpoint_clock_fn)(void* user);
typedef void (*vd_endpoint_yield_fn)(void* user);

struct vd_endpoint_io {
    void* user;
    vd_endpoint_recv_fn receive;
    vd_endpoint_send_fn send;
    vd_endpoint_clock_fn now_ms;
    vd_endpoint_yield_fn yield;
};

struct vd_endpoint_receiver {
    uint8_t record[VD_COMPANION_MAX_RECORD];
    size_t offset;
    size_t expected;
    uint64_t deadline_ms;
    uint32_t started;
};

struct vd_endpoint_virtual_fs {
    const struct vd_companion_service* service;
};

void vd_endpoint_config_init(struct vd_endpoint_config* config);
int vd_endpoint_config_parse(struct vd_endpoint_config* config,
                             const uint8_t* data, size_t size);

void vd_endpoint_receiver_init(struct vd_endpoint_receiver* receiver);
int vd_endpoint_receiver_arm(struct vd_endpoint_receiver* receiver,
                             uint64_t now_ms, uint32_t deadline_ms);
int vd_endpoint_receiver_poll(struct vd_endpoint_receiver* receiver,
                              const struct vd_endpoint_io* io,
                              const uint8_t** record, size_t* record_size);
int vd_endpoint_send_all(const struct vd_endpoint_io* io,
                         const uint8_t* data, size_t size,
                         uint32_t deadline_ms);

void vd_endpoint_virtual_fs_init(
    struct vd_endpoint_virtual_fs* filesystem,
    const struct vd_companion_service* service);
int vd_endpoint_fs_resolve(void* user, const char* relative_path,
                           char* canonical_path, size_t canonical_capacity,
                           uint32_t* flags, uint64_t* size);
int vd_endpoint_fs_list(void* user, const char* canonical_directory,
                        struct vd_companion_file_entry* entries,
                        size_t entry_capacity, size_t* entry_count);
int vd_endpoint_fs_read(void* user, const char* canonical_file,
                        uint64_t offset, uint8_t* output,
                        size_t output_capacity, size_t* output_size,
                        uint32_t* flags, uint64_t* file_size);

#ifdef __cplusplus
}
#endif

#endif

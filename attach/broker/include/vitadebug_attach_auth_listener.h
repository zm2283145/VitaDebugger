#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_attach_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VD_ATTACH_AUTH_LISTENER_DEFAULT_PORT 18195u
#define VD_ATTACH_AUTH_LISTENER_MIN_PORT 18000u
#define VD_ATTACH_AUTH_LISTENER_MAX_PORT 18999u
#define VD_ATTACH_AUTH_LISTENER_MIN_DEADLINE_MS 100u
#define VD_ATTACH_AUTH_LISTENER_MAX_DEADLINE_MS 30000u
#define VD_ATTACH_AUTH_LISTENER_ACCEPT_SLICE_MS 100u
#define VD_ATTACH_AUTH_LISTENER_MAX_REPLAYS 128u
#define VD_ATTACH_AUTH_LISTENER_MAX_FAILURES 32u
#define VD_ATTACH_AUTH_LISTENER_MIN_BACKOFF_MS 250u
#define VD_ATTACH_AUTH_LISTENER_MAX_BACKOFF_MS 8000u

enum {
    VD_ATTACH_AUTH_LISTENER_OK = 0,
    VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT = -100,
    VD_ATTACH_AUTH_LISTENER_ERROR_STATE = -101,
    VD_ATTACH_AUTH_LISTENER_ERROR_IO = -102,
    VD_ATTACH_AUTH_LISTENER_ERROR_PROTOCOL = -103,
    VD_ATTACH_AUTH_LISTENER_ERROR_PEER = -104,
    VD_ATTACH_AUTH_LISTENER_ERROR_RATE_LIMIT = -105,
    VD_ATTACH_AUTH_LISTENER_ERROR_REPLAY = -106,
    VD_ATTACH_AUTH_LISTENER_ERROR_ENTROPY = -107,
    VD_ATTACH_AUTH_LISTENER_ERROR_SHUTDOWN = -108,
    VD_ATTACH_AUTH_LISTENER_ERROR_RESOURCE = -109,
    VD_ATTACH_AUTH_LISTENER_ERROR_STORAGE = -110,
    VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT = -111,
};

typedef enum VdAttachAuthListenerState {
    VD_ATTACH_AUTH_LISTENER_STOPPED = 0,
    VD_ATTACH_AUTH_LISTENER_STARTING = 1,
    VD_ATTACH_AUTH_LISTENER_RUNNING = 2,
    VD_ATTACH_AUTH_LISTENER_STOPPING = 3,
} VdAttachAuthListenerState;

typedef struct VdAttachAuthSocketOps {
    void *context;
    uint64_t (*now_ms)(void *context);
    int (*entropy)(void *context, uint8_t *output, size_t size);
    int (*listen_open)(void *context,
                       uint32_t bind_ipv4,
                       uint16_t port,
                       int *socket_out);
    int (*accept)(void *context,
                  int listen_socket,
                  int *socket_out,
                  uint32_t *peer_ipv4,
                  uint64_t deadline_ms);
    int (*read)(void *context,
                int socket,
                void *output,
                size_t size,
                uint64_t deadline_ms);
    int (*write)(void *context,
                 int socket,
                 const void *data,
                 size_t size,
                 uint64_t deadline_ms);
    int (*shutdown)(void *context, int socket);
    int (*close)(void *context, int socket);
} VdAttachAuthSocketOps;

typedef int (*VdAttachAuthReserveGenerationFn)(
    void *context,
    uint64_t *service_generation);

typedef struct VdAttachAuthListenerConfig {
    VdAttachAuthSocketOps socket_ops;
    const VdAttachAuthKeyStorage *key_storage;
    void *generation_context;
    VdAttachAuthReserveGenerationFn reserve_service_generation;
    uint32_t bind_ipv4;
    uint32_t peer_network;
    uint32_t peer_netmask;
    uint16_t port;
    uint32_t handshake_deadline_ms;
} VdAttachAuthListenerConfig;

typedef struct VdAttachAuthFailureSlot {
    int active;
    uint32_t peer_ipv4;
    uint64_t host_key_id;
    uint32_t failures;
    uint64_t blocked_until_ms;
} VdAttachAuthFailureSlot;

typedef struct VdAttachAuthListenerStats {
    uint32_t accepted;
    uint32_t authenticated;
    uint32_t denied;
    uint32_t rate_limited;
    uint32_t replay_rejected;
    uint32_t malformed;
    uint32_t peer_rejected;
    uint32_t io_failures;
    uint32_t shutdowns;
    uint32_t close_failures;
} VdAttachAuthListenerStats;

typedef struct VdAttachAuthListener {
    VdAttachAuthListenerConfig config;
    uint64_t service_generation;
    uint64_t next_session_id;
    uint32_t connection_generation;
    int state;
    int listen_socket;
    int active_socket;
    uint8_t replay_nonces[VD_ATTACH_AUTH_LISTENER_MAX_REPLAYS][32];
    uint32_t replay_count;
    VdAttachAuthFailureSlot failures[VD_ATTACH_AUTH_LISTENER_MAX_FAILURES];
    int failure_table_exhausted;
    VdAttachAuthListenerStats stats;
} VdAttachAuthListener;

void vd_attach_auth_listener_config_init(
    VdAttachAuthListenerConfig *config);

int vd_attach_auth_listener_init(
    VdAttachAuthListener *listener,
    const VdAttachAuthListenerConfig *config);

/*
 * Reserves and durably persists a new service generation before opening the
 * endpoint. start may be called again only after run has returned from a
 * completed shutdown.
 */
int vd_attach_auth_listener_start(VdAttachAuthListener *listener);

/*
 * Owns accept and every accepted socket. The loop is serialized: at most one
 * handshake is active. A separate owner thread may call shutdown.
 */
int vd_attach_auth_listener_run(VdAttachAuthListener *listener);

/*
 * Idempotently rejects new work and closes both registered sockets so blocked
 * accept/read/write calls terminate. The worker must be joined before unload.
 */
int vd_attach_auth_listener_shutdown(VdAttachAuthListener *listener);

int vd_attach_auth_listener_get_stats(
    const VdAttachAuthListener *listener,
    VdAttachAuthListenerStats *stats);

VdAttachAuthListenerState vd_attach_auth_listener_state(
    const VdAttachAuthListener *listener);

#ifdef __cplusplus
}
#endif

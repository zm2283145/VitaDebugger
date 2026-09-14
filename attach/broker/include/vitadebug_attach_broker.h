#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_attach_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Protocol-v1 is read-only.  Keep these limits fixed and small so a broker can
 * run as a long-lived Vita service without peer-controlled allocation.
 */
#define VD_ATTACH_BROKER_MAX_SESSIONS 4u
#define VD_ATTACH_BROKER_MAX_REQUEST_IDS 64u
#define VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES 10u
#define VD_ATTACH_BROKER_TOKEN_BYTES 32u
#define VD_ATTACH_BROKER_TOKEN_HEX_BYTES 65u

#define VD_ATTACH_BROKER_MIN_LEASE_MS 250u
#define VD_ATTACH_BROKER_MAX_LEASE_MS 60000u
#define VD_ATTACH_BROKER_MIN_IO_TIMEOUT_MS 100u
#define VD_ATTACH_BROKER_MAX_IO_TIMEOUT_MS 30000u

enum {
    VD_ATTACH_BROKER_OK = 0,
    VD_ATTACH_BROKER_CLOSED = 1,
    VD_ATTACH_BROKER_ERROR_ARGUMENT = -1,
    VD_ATTACH_BROKER_ERROR_STATE = -2,
    VD_ATTACH_BROKER_ERROR_PROTOCOL = -3,
    VD_ATTACH_BROKER_ERROR_FRAME = -4,
    VD_ATTACH_BROKER_ERROR_IO = -5,
    VD_ATTACH_BROKER_ERROR_ENTROPY = -6,
    VD_ATTACH_BROKER_ERROR_REPLAY = -7,
    VD_ATTACH_BROKER_ERROR_PEER = -8,
    VD_ATTACH_BROKER_ERROR_LIMIT = -9,
    VD_ATTACH_BROKER_ERROR_SHUTDOWN = -10,
};

typedef enum VdAttachInventoryResult {
    VD_ATTACH_INVENTORY_FOUND = 0,
    VD_ATTACH_INVENTORY_NOT_FOUND = 1,
    VD_ATTACH_INVENTORY_DENIED = 2,
    VD_ATTACH_INVENTORY_CHANGED = 3,
    VD_ATTACH_INVENTORY_ERROR = 4,
    VD_ATTACH_INVENTORY_UNAVAILABLE = 5,
} VdAttachInventoryResult;

typedef struct VdAttachTargetIdentity {
    char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES];
    uint32_t pid;
    uint32_t main_modid;
    uint32_t main_fingerprint;
    uint64_t target_generation;
} VdAttachTargetIdentity;

/*
 * discover_exact must apply the Vita-side allowlist, resolve one exact title,
 * reverse-map the PID to the same title, and return an identity snapshot that
 * was revalidated by a trusted provider.  The broker calls it again at
 * RELEASE and compares every field.  Returning UNAVAILABLE keeps the adapter
 * fail-closed.
 */
typedef VdAttachInventoryResult (*VdAttachDiscoverExactFn)(
    void *context,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity,
    uint64_t deadline_ms);

/* entropy must provide cryptographically secure random bytes on Vita. */
typedef int (*VdAttachEntropyFn)(void *context, uint8_t *output, size_t size);
typedef uint64_t (*VdAttachNowMsFn)(void *context);

typedef struct VdAttachBrokerConfig {
    void *callback_context;
    VdAttachDiscoverExactFn discover_exact;
    VdAttachEntropyFn entropy;
    VdAttachNowMsFn now_ms;
    uint32_t attach_caps;
    uint32_t kernel_abi;
    uint32_t kernel_caps;
    uint32_t ticket_lease_ms;
    uint32_t io_timeout_ms;
    int service_ready;
} VdAttachBrokerConfig;

/*
 * Transport callbacks receive one absolute monotonic deadline.  They may
 * return a short positive transfer, zero for orderly peer close, or negative
 * for an error/timeout.  The broker never requests more than 4096 bytes.
 */
typedef int (*VdAttachTransportReadFn)(
    void *context, void *output, size_t size, uint64_t deadline_ms);
typedef int (*VdAttachTransportWriteFn)(
    void *context, const void *data, size_t size, uint64_t deadline_ms);
typedef void (*VdAttachTransportCloseFn)(void *context);

typedef struct VdAttachTransport {
    void *context;
    VdAttachTransportReadFn read;
    VdAttachTransportWriteFn write;
    VdAttachTransportCloseFn close;
    uint64_t peer_id;
} VdAttachTransport;

typedef struct VdAttachBrokerTicket {
    int active;
    char token[VD_ATTACH_BROKER_TOKEN_HEX_BYTES];
    uint64_t expires_at_ms;
    VdAttachTargetIdentity identity;
} VdAttachBrokerTicket;

typedef struct VdAttachBrokerSession {
    int active;
    int hello_complete;
    uint64_t session_id;
    uint64_t peer_id;
    char server_nonce[VD_ATTACH_BROKER_TOKEN_HEX_BYTES];
    char request_ids[VD_ATTACH_BROKER_MAX_REQUEST_IDS][33];
    uint32_t request_id_count;
    VdAttachBrokerTicket ticket;
} VdAttachBrokerSession;

typedef struct VdAttachBroker {
    VdAttachBrokerConfig config;
    uint64_t service_generation;
    int initialized;
    int shutting_down;
    VdAttachBrokerSession sessions[VD_ATTACH_BROKER_MAX_SESSIONS];
} VdAttachBroker;

int vd_attach_broker_init(VdAttachBroker *broker,
                          const VdAttachBrokerConfig *config);

int vd_attach_broker_open_session(VdAttachBroker *broker,
                                  uint64_t peer_id,
                                  uint64_t *session_id);

void vd_attach_broker_close_session(VdAttachBroker *broker,
                                    uint64_t session_id,
                                    uint64_t peer_id);

/*
 * Processes one unframed canonical protocol record.  On success, response_len
 * receives one unframed response record.  A negative result is fail-closed and
 * produces no response; a transport should close that connection.
 */
int vd_attach_broker_handle_record(VdAttachBroker *broker,
                                   uint64_t session_id,
                                   uint64_t peer_id,
                                   const uint8_t *request,
                                   size_t request_size,
                                   uint8_t *response,
                                   size_t response_capacity,
                                   size_t *response_len);

/* Serve one connection until peer close, protocol failure, or shutdown. */
int vd_attach_broker_serve(VdAttachBroker *broker,
                           const VdAttachTransport *transport);

/* Invalidates every session/ticket. No target state exists to restore in v1. */
void vd_attach_broker_shutdown(VdAttachBroker *broker);

#ifdef __cplusplus
}
#endif

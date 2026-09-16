#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vitadebug_attach_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocation-free wire boundary for the authenticated control model.
 *
 * This is not a socket listener.  It owns no key material and performs no
 * Vita lifecycle call.  A future resident service can inject a bounded
 * transport and use this dispatcher without giving its parser access to a
 * module path, PID, or arbitrary address.
 */
#define VD_ATTACH_LISTENER_VERSION 2u
#define VD_ATTACH_LISTENER_MAX_FRAME_SIZE 512u
#define VD_ATTACH_LISTENER_REQUEST_ID_BYTES 16u
#define VD_ATTACH_LISTENER_MAX_REQUEST_IDS 64u
#define VD_ATTACH_LISTENER_CHALLENGE_BYTES 104u
#define VD_ATTACH_LISTENER_AUTH_REQUEST_BYTES 152u
#define VD_ATTACH_LISTENER_OPERATION_REQUEST_BYTES 165u
#define VD_ATTACH_LISTENER_RESPONSE_BYTES 76u

enum {
    VD_ATTACH_LISTENER_OK = 0,
    VD_ATTACH_LISTENER_CLOSED = 1,
    VD_ATTACH_LISTENER_RECOVERY_REQUIRED = 2,
    VD_ATTACH_LISTENER_ERROR_ARGUMENT = -1,
    VD_ATTACH_LISTENER_ERROR_STATE = -2,
    VD_ATTACH_LISTENER_ERROR_PROTOCOL = -3,
    VD_ATTACH_LISTENER_ERROR_FRAME = -4,
    VD_ATTACH_LISTENER_ERROR_IO = -5,
    VD_ATTACH_LISTENER_ERROR_REPLAY = -6,
    VD_ATTACH_LISTENER_ERROR_PEER = -7,
    VD_ATTACH_LISTENER_ERROR_LIMIT = -8,
    VD_ATTACH_LISTENER_ERROR_SHUTDOWN = -9,
};

typedef enum VdAttachListenerRequestType {
    VD_ATTACH_LISTENER_REQUEST_AUTHENTICATE = 1,
    VD_ATTACH_LISTENER_REQUEST_ATTACH = 2,
    VD_ATTACH_LISTENER_REQUEST_DETACH = 3,
    VD_ATTACH_LISTENER_REQUEST_RECOVER = 4,
} VdAttachListenerRequestType;

/* Stable unsigned result values used on the wire. */
typedef enum VdAttachListenerResultCode {
    VD_ATTACH_LISTENER_RESULT_OK = 0,
    VD_ATTACH_LISTENER_RESULT_RECOVERY_REQUIRED = 1,
    VD_ATTACH_LISTENER_RESULT_ARGUMENT = 2,
    VD_ATTACH_LISTENER_RESULT_STATE = 3,
    VD_ATTACH_LISTENER_RESULT_AUTH = 4,
    VD_ATTACH_LISTENER_RESULT_EXPIRED = 5,
    VD_ATTACH_LISTENER_RESULT_REPLAY = 6,
    VD_ATTACH_LISTENER_RESULT_TARGET = 7,
    VD_ATTACH_LISTENER_RESULT_STALE_TARGET = 8,
    VD_ATTACH_LISTENER_RESULT_BUSY = 9,
    VD_ATTACH_LISTENER_RESULT_BACKEND = 10,
    VD_ATTACH_LISTENER_RESULT_ENTROPY = 11,
    VD_ATTACH_LISTENER_RESULT_SHUTDOWN = 12,
    VD_ATTACH_LISTENER_RESULT_LIMIT = 13,
    VD_ATTACH_LISTENER_RESULT_PEER = 14,
    VD_ATTACH_LISTENER_RESULT_ROLLOVER_REQUIRED = 15,
    VD_ATTACH_LISTENER_RESULT_INTERNAL = 16,
} VdAttachListenerResultCode;

typedef struct VdAttachListenerRequest {
    uint32_t type;
    uint8_t request_id[VD_ATTACH_LISTENER_REQUEST_ID_BYTES];
    uint64_t session_id;
    VdAttachControlPeerProof peer_proof;
    VdAttachControlOperationProof operation_proof;
} VdAttachListenerRequest;

typedef struct VdAttachListenerChallengeFrame {
    uint64_t session_id;
    VdAttachControlChallenge challenge;
} VdAttachListenerChallengeFrame;

typedef struct VdAttachListenerResponse {
    uint32_t request_type;
    uint8_t request_id[VD_ATTACH_LISTENER_REQUEST_ID_BYTES];
    uint64_t session_id;
    uint32_t result_code;
    uint32_t lease_state;
    uint64_t lease_id;
    uint64_t lease_expires_at_ms;
    uint32_t injected_module_uid;
    uint64_t target_generation;
} VdAttachListenerResponse;

typedef struct VdAttachListenerConfig {
    VdAttachControl *control;
    void *clock_context;
    VdAttachNowMsFn now_ms;
    uint32_t io_timeout_ms;
} VdAttachListenerConfig;

typedef struct VdAttachListenerConnection {
    int active;
    int authenticated;
    int terminal;
    uint32_t auth_attempts;
    uint64_t session_id;
    uint64_t peer_id;
    uint64_t host_key_id;
    uint8_t request_ids[VD_ATTACH_LISTENER_MAX_REQUEST_IDS]
                       [VD_ATTACH_LISTENER_REQUEST_ID_BYTES];
    uint32_t request_id_count;
} VdAttachListenerConnection;

typedef struct VdAttachListener {
    VdAttachListenerConfig config;
    int initialized;
    int shutting_down;
    VdAttachListenerConnection
        connections[VD_ATTACH_CONTROL_MAX_SESSIONS];
} VdAttachListener;

/* Storage must be zero-initialized before the first init call. */
int vd_attach_listener_init(VdAttachListener *listener,
                            const VdAttachListenerConfig *config);

int vd_attach_listener_open_connection(
    VdAttachListener *listener, uint64_t peer_id,
    VdAttachListenerChallengeFrame *challenge);

int vd_attach_listener_close_connection(VdAttachListener *listener,
                                        uint64_t session_id,
                                        uint64_t peer_id);

/*
 * Decode, replay-claim, and dispatch one payload (without its uint32 length
 * prefix).  Canonical framing errors and replay are fatal to the connection
 * and intentionally produce no response.
 */
int vd_attach_listener_handle_record(
    VdAttachListener *listener, uint64_t session_id, uint64_t peer_id,
    const uint8_t *request, size_t request_size, uint8_t *response,
    size_t response_capacity, size_t *response_size);

/*
 * Host/fixture encoders and decoders for the fixed network-byte-order wire
 * records.  Signatures remain the canonical control signatures; request IDs
 * are the first 16 bytes of the signed client/operation nonce. This binds the
 * at-most-once framing key without creating a second signing format.
 */
int vd_attach_listener_encode_request(
    const VdAttachListenerRequest *request, uint8_t *output,
    size_t output_capacity, size_t *output_size);
int vd_attach_listener_encode_challenge(
    const VdAttachListenerChallengeFrame *challenge, uint8_t *output,
    size_t output_capacity, size_t *output_size);
int vd_attach_listener_decode_challenge(
    const uint8_t *input, size_t input_size,
    VdAttachListenerChallengeFrame *challenge);
int vd_attach_listener_decode_response(
    const uint8_t *input, size_t input_size,
    VdAttachListenerResponse *response);

/*
 * Serve one injected transport.  A challenge frame is written first. Every
 * subsequent request/response exchange shares one absolute I/O deadline.
 * No TCP accept/bind code is included.
 */
int vd_attach_listener_serve(VdAttachListener *listener,
                             const VdAttachTransport *transport);

/* Prevent new work and ask the control core to perform shutdown cleanup. */
int vd_attach_listener_shutdown(VdAttachListener *listener);

#ifdef __cplusplus
}
#endif

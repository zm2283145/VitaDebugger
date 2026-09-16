#include "vitadebug_attach_listener.h"

#include <limits.h>
#include <string.h>

#define VD_LISTENER_COMMON_BYTES 40u
#define VD_LISTENER_CHALLENGE_KIND 0u

static const uint8_t g_request_magic[8] = {
    'V', 'D', 'C', 'T', 'L', '2', 'R', 'Q'
};
static const uint8_t g_challenge_magic[8] = {
    'V', 'D', 'C', 'T', 'L', '2', 'C', 'H'
};
static const uint8_t g_response_magic[8] = {
    'V', 'D', 'C', 'T', 'L', '2', 'R', 'S'
};

_Static_assert(VD_ATTACH_LISTENER_CHALLENGE_BYTES ==
                   VD_LISTENER_COMMON_BYTES + 4u * 8u +
                       VD_ATTACH_CONTROL_NONCE_BYTES,
               "listener challenge length drifted");
_Static_assert(VD_ATTACH_LISTENER_AUTH_REQUEST_BYTES ==
                   VD_LISTENER_COMMON_BYTES + 2u * 8u +
                       VD_ATTACH_CONTROL_NONCE_BYTES +
                       VD_ATTACH_CONTROL_SIGNATURE_BYTES,
               "listener auth request length drifted");
_Static_assert(VD_ATTACH_LISTENER_OPERATION_REQUEST_BYTES ==
                   VD_LISTENER_COMMON_BYTES + VD_ATTACH_TITLE_ID_LENGTH +
                       8u + 4u + 8u +
                       VD_ATTACH_CONTROL_NONCE_BYTES +
                       VD_ATTACH_CONTROL_SIGNATURE_BYTES,
               "listener operation request length drifted");
_Static_assert(VD_ATTACH_LISTENER_RESPONSE_BYTES ==
                   VD_LISTENER_COMMON_BYTES + 3u * 4u + 3u * 8u,
               "listener response length drifted");
_Static_assert(VD_ATTACH_LISTENER_MAX_REQUEST_IDS ==
                   VD_ATTACH_CONTROL_MAX_REPLAY_NONCES,
               "listener and operation rollover limits drifted");

typedef struct VdListenerWriter {
    uint8_t *bytes;
    size_t offset;
} VdListenerWriter;

typedef struct VdListenerReader {
    const uint8_t *bytes;
    size_t offset;
} VdListenerReader;

static void vd_listener_put_bytes(VdListenerWriter *writer,
                                  const void *bytes, size_t size) {
    memcpy(writer->bytes + writer->offset, bytes, size);
    writer->offset += size;
}

static void vd_listener_put_u32(VdListenerWriter *writer, uint32_t value) {
    writer->bytes[writer->offset++] = (uint8_t)(value >> 24);
    writer->bytes[writer->offset++] = (uint8_t)(value >> 16);
    writer->bytes[writer->offset++] = (uint8_t)(value >> 8);
    writer->bytes[writer->offset++] = (uint8_t)value;
}

static void vd_listener_put_u64(VdListenerWriter *writer, uint64_t value) {
    writer->bytes[writer->offset++] = (uint8_t)(value >> 56);
    writer->bytes[writer->offset++] = (uint8_t)(value >> 48);
    writer->bytes[writer->offset++] = (uint8_t)(value >> 40);
    writer->bytes[writer->offset++] = (uint8_t)(value >> 32);
    writer->bytes[writer->offset++] = (uint8_t)(value >> 24);
    writer->bytes[writer->offset++] = (uint8_t)(value >> 16);
    writer->bytes[writer->offset++] = (uint8_t)(value >> 8);
    writer->bytes[writer->offset++] = (uint8_t)value;
}

static void vd_listener_get_bytes(VdListenerReader *reader, void *bytes,
                                  size_t size) {
    memcpy(bytes, reader->bytes + reader->offset, size);
    reader->offset += size;
}

static uint32_t vd_listener_get_u32(VdListenerReader *reader) {
    uint32_t value = (uint32_t)reader->bytes[reader->offset] << 24;
    value |= (uint32_t)reader->bytes[reader->offset + 1u] << 16;
    value |= (uint32_t)reader->bytes[reader->offset + 2u] << 8;
    value |= (uint32_t)reader->bytes[reader->offset + 3u];
    reader->offset += 4u;
    return value;
}

static uint64_t vd_listener_get_u64(VdListenerReader *reader) {
    uint64_t value = (uint64_t)reader->bytes[reader->offset] << 56;
    value |= (uint64_t)reader->bytes[reader->offset + 1u] << 48;
    value |= (uint64_t)reader->bytes[reader->offset + 2u] << 40;
    value |= (uint64_t)reader->bytes[reader->offset + 3u] << 32;
    value |= (uint64_t)reader->bytes[reader->offset + 4u] << 24;
    value |= (uint64_t)reader->bytes[reader->offset + 5u] << 16;
    value |= (uint64_t)reader->bytes[reader->offset + 6u] << 8;
    value |= (uint64_t)reader->bytes[reader->offset + 7u];
    reader->offset += 8u;
    return value;
}

static int vd_listener_bytes_nonzero(const uint8_t *bytes, size_t size) {
    size_t i;
    uint8_t combined = 0u;
    for (i = 0u; i < size; ++i) {
        combined |= bytes[i];
    }
    return combined != 0u;
}

static int vd_listener_type_valid(uint32_t type) {
    return type >= VD_ATTACH_LISTENER_REQUEST_AUTHENTICATE &&
           type <= VD_ATTACH_LISTENER_REQUEST_RECOVER;
}

static int vd_listener_title_valid(
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES]) {
    size_t i;
    if (title_id == NULL ||
        title_id[VD_ATTACH_TITLE_ID_LENGTH] != '\0') {
        return 0;
    }
    for (i = 0u; i < VD_ATTACH_TITLE_ID_LENGTH; ++i) {
        const char value = title_id[i];
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9'))) {
            return 0;
        }
    }
    return 1;
}

static size_t vd_listener_request_size(uint32_t type) {
    return type == VD_ATTACH_LISTENER_REQUEST_AUTHENTICATE
               ? VD_ATTACH_LISTENER_AUTH_REQUEST_BYTES
               : VD_ATTACH_LISTENER_OPERATION_REQUEST_BYTES;
}

static int vd_listener_request_valid(
    const VdAttachListenerRequest *request) {
    const int is_attach =
        request != NULL &&
        request->type == VD_ATTACH_LISTENER_REQUEST_ATTACH;
    if (request == NULL || !vd_listener_type_valid(request->type) ||
        request->session_id == 0u ||
        !vd_listener_bytes_nonzero(request->request_id,
                                   sizeof(request->request_id))) {
        return 0;
    }
    if (request->type == VD_ATTACH_LISTENER_REQUEST_AUTHENTICATE) {
        return request->peer_proof.host_key_id != 0u &&
               request->peer_proof.expires_at_ms != 0u &&
               memcmp(request->request_id,
                      request->peer_proof.client_nonce,
                      VD_ATTACH_LISTENER_REQUEST_ID_BYTES) == 0 &&
               vd_listener_bytes_nonzero(
                   request->peer_proof.client_nonce,
                   sizeof(request->peer_proof.client_nonce)) &&
               vd_listener_bytes_nonzero(
                   request->peer_proof.signature,
                   sizeof(request->peer_proof.signature));
    }
    if (!vd_listener_title_valid(
            request->operation_proof.target_title_id) ||
        request->operation_proof.expected_target_generation == 0u ||
        request->operation_proof.expires_at_ms == 0u ||
        !vd_listener_bytes_nonzero(
            request->operation_proof.request_nonce,
            sizeof(request->operation_proof.request_nonce)) ||
        !vd_listener_bytes_nonzero(
            request->operation_proof.signature,
            sizeof(request->operation_proof.signature))) {
        return 0;
    }
    if (memcmp(request->request_id,
               request->operation_proof.request_nonce,
               VD_ATTACH_LISTENER_REQUEST_ID_BYTES) != 0) {
        return 0;
    }
    if (is_attach) {
        return request->operation_proof.requested_lease_ms >=
                   VD_ATTACH_CONTROL_MIN_LEASE_MS &&
               request->operation_proof.requested_lease_ms <=
                   VD_ATTACH_CONTROL_MAX_LEASE_MS;
    }
    return request->operation_proof.requested_lease_ms == 0u;
}

static void vd_listener_put_common(VdListenerWriter *writer,
                                   const uint8_t magic[8],
                                   uint32_t type,
                                   const uint8_t request_id[16],
                                   uint64_t session_id) {
    vd_listener_put_bytes(writer, magic, 8u);
    vd_listener_put_u32(writer, VD_ATTACH_LISTENER_VERSION);
    vd_listener_put_u32(writer, type);
    vd_listener_put_bytes(writer, request_id,
                          VD_ATTACH_LISTENER_REQUEST_ID_BYTES);
    vd_listener_put_u64(writer, session_id);
}

int vd_attach_listener_encode_request(
    const VdAttachListenerRequest *request, uint8_t *output,
    size_t output_capacity, size_t *output_size) {
    VdListenerWriter writer;
    size_t required;

    if (output_size != NULL) {
        *output_size = 0u;
    }
    if (!vd_listener_request_valid(request) || output == NULL ||
        output_size == NULL) {
        return VD_ATTACH_LISTENER_ERROR_ARGUMENT;
    }
    required = vd_listener_request_size(request->type);
    if (output_capacity < required) {
        return VD_ATTACH_LISTENER_ERROR_ARGUMENT;
    }
    writer.bytes = output;
    writer.offset = 0u;
    vd_listener_put_common(&writer, g_request_magic, request->type,
                           request->request_id, request->session_id);
    if (request->type == VD_ATTACH_LISTENER_REQUEST_AUTHENTICATE) {
        vd_listener_put_u64(&writer, request->peer_proof.host_key_id);
        vd_listener_put_u64(&writer, request->peer_proof.expires_at_ms);
        vd_listener_put_bytes(&writer,
                              request->peer_proof.client_nonce,
                              sizeof(request->peer_proof.client_nonce));
        vd_listener_put_bytes(&writer, request->peer_proof.signature,
                              sizeof(request->peer_proof.signature));
    } else {
        vd_listener_put_bytes(
            &writer, request->operation_proof.target_title_id,
            VD_ATTACH_TITLE_ID_LENGTH);
        vd_listener_put_u64(
            &writer,
            request->operation_proof.expected_target_generation);
        vd_listener_put_u32(
            &writer, request->operation_proof.requested_lease_ms);
        vd_listener_put_u64(&writer,
                            request->operation_proof.expires_at_ms);
        vd_listener_put_bytes(
            &writer, request->operation_proof.request_nonce,
            sizeof(request->operation_proof.request_nonce));
        vd_listener_put_bytes(
            &writer, request->operation_proof.signature,
            sizeof(request->operation_proof.signature));
    }
    if (writer.offset != required) {
        memset(output, 0, output_capacity);
        return VD_ATTACH_LISTENER_ERROR_STATE;
    }
    *output_size = writer.offset;
    return VD_ATTACH_LISTENER_OK;
}

static int vd_listener_decode_request(const uint8_t *input,
                                      size_t input_size,
                                      VdAttachListenerRequest *request) {
    VdListenerReader reader;
    uint32_t version;

    if (input == NULL || request == NULL ||
        input_size < VD_LISTENER_COMMON_BYTES ||
        memcmp(input, g_request_magic, sizeof(g_request_magic)) != 0) {
        return VD_ATTACH_LISTENER_ERROR_PROTOCOL;
    }
    memset(request, 0, sizeof(*request));
    reader.bytes = input;
    reader.offset = 8u;
    version = vd_listener_get_u32(&reader);
    request->type = vd_listener_get_u32(&reader);
    vd_listener_get_bytes(&reader, request->request_id,
                          sizeof(request->request_id));
    request->session_id = vd_listener_get_u64(&reader);
    if (version != VD_ATTACH_LISTENER_VERSION ||
        !vd_listener_type_valid(request->type) ||
        input_size != vd_listener_request_size(request->type)) {
        memset(request, 0, sizeof(*request));
        return VD_ATTACH_LISTENER_ERROR_PROTOCOL;
    }
    if (request->type == VD_ATTACH_LISTENER_REQUEST_AUTHENTICATE) {
        request->peer_proof.host_key_id = vd_listener_get_u64(&reader);
        request->peer_proof.expires_at_ms = vd_listener_get_u64(&reader);
        vd_listener_get_bytes(&reader,
                              request->peer_proof.client_nonce,
                              sizeof(request->peer_proof.client_nonce));
        vd_listener_get_bytes(&reader, request->peer_proof.signature,
                              sizeof(request->peer_proof.signature));
    } else {
        vd_listener_get_bytes(
            &reader, request->operation_proof.target_title_id,
            VD_ATTACH_TITLE_ID_LENGTH);
        request->operation_proof
            .target_title_id[VD_ATTACH_TITLE_ID_LENGTH] = '\0';
        request->operation_proof.expected_target_generation =
            vd_listener_get_u64(&reader);
        request->operation_proof.requested_lease_ms =
            vd_listener_get_u32(&reader);
        request->operation_proof.expires_at_ms =
            vd_listener_get_u64(&reader);
        vd_listener_get_bytes(
            &reader, request->operation_proof.request_nonce,
            sizeof(request->operation_proof.request_nonce));
        vd_listener_get_bytes(
            &reader, request->operation_proof.signature,
            sizeof(request->operation_proof.signature));
    }
    if (reader.offset != input_size ||
        !vd_listener_request_valid(request)) {
        memset(request, 0, sizeof(*request));
        return VD_ATTACH_LISTENER_ERROR_PROTOCOL;
    }
    return VD_ATTACH_LISTENER_OK;
}

int vd_attach_listener_encode_challenge(
    const VdAttachListenerChallengeFrame *challenge, uint8_t *output,
    size_t output_capacity, size_t *output_size) {
    static const uint8_t zero_request_id[16] = {0};
    VdListenerWriter writer;
    if (output_size != NULL) {
        *output_size = 0u;
    }
    if (challenge == NULL || output == NULL || output_size == NULL ||
        output_capacity < VD_ATTACH_LISTENER_CHALLENGE_BYTES ||
        challenge->session_id == 0u ||
        challenge->challenge.session_id != challenge->session_id ||
        challenge->challenge.service_generation == 0u ||
        challenge->challenge.transport_binding == 0u ||
        challenge->challenge.server_time_ms >=
            challenge->challenge.challenge_expires_at_ms ||
        !vd_listener_bytes_nonzero(
            challenge->challenge.server_nonce,
            sizeof(challenge->challenge.server_nonce))) {
        return VD_ATTACH_LISTENER_ERROR_ARGUMENT;
    }
    writer.bytes = output;
    writer.offset = 0u;
    vd_listener_put_common(&writer, g_challenge_magic,
                           VD_LISTENER_CHALLENGE_KIND, zero_request_id,
                           challenge->session_id);
    vd_listener_put_u64(&writer,
                        challenge->challenge.service_generation);
    vd_listener_put_u64(&writer,
                        challenge->challenge.transport_binding);
    vd_listener_put_u64(&writer, challenge->challenge.server_time_ms);
    vd_listener_put_u64(
        &writer, challenge->challenge.challenge_expires_at_ms);
    vd_listener_put_bytes(&writer, challenge->challenge.server_nonce,
                          sizeof(challenge->challenge.server_nonce));
    if (writer.offset != VD_ATTACH_LISTENER_CHALLENGE_BYTES) {
        memset(output, 0, output_capacity);
        return VD_ATTACH_LISTENER_ERROR_STATE;
    }
    *output_size = writer.offset;
    return VD_ATTACH_LISTENER_OK;
}

int vd_attach_listener_decode_challenge(
    const uint8_t *input, size_t input_size,
    VdAttachListenerChallengeFrame *challenge) {
    static const uint8_t zero_request_id[16] = {0};
    VdListenerReader reader;
    uint32_t version;
    uint32_t kind;
    uint8_t request_id[16];

    if (input == NULL || challenge == NULL ||
        input_size != VD_ATTACH_LISTENER_CHALLENGE_BYTES ||
        memcmp(input, g_challenge_magic, sizeof(g_challenge_magic)) != 0) {
        return VD_ATTACH_LISTENER_ERROR_PROTOCOL;
    }
    memset(challenge, 0, sizeof(*challenge));
    reader.bytes = input;
    reader.offset = 8u;
    version = vd_listener_get_u32(&reader);
    kind = vd_listener_get_u32(&reader);
    vd_listener_get_bytes(&reader, request_id, sizeof(request_id));
    challenge->session_id = vd_listener_get_u64(&reader);
    challenge->challenge.service_generation =
        vd_listener_get_u64(&reader);
    challenge->challenge.session_id = challenge->session_id;
    challenge->challenge.transport_binding =
        vd_listener_get_u64(&reader);
    challenge->challenge.server_time_ms = vd_listener_get_u64(&reader);
    challenge->challenge.challenge_expires_at_ms =
        vd_listener_get_u64(&reader);
    vd_listener_get_bytes(&reader, challenge->challenge.server_nonce,
                          sizeof(challenge->challenge.server_nonce));
    if (version != VD_ATTACH_LISTENER_VERSION ||
        kind != VD_LISTENER_CHALLENGE_KIND ||
        memcmp(request_id, zero_request_id, sizeof(request_id)) != 0 ||
        reader.offset != input_size || challenge->session_id == 0u ||
        challenge->challenge.service_generation == 0u ||
        challenge->challenge.transport_binding == 0u ||
        challenge->challenge.server_time_ms >=
            challenge->challenge.challenge_expires_at_ms ||
        !vd_listener_bytes_nonzero(
            challenge->challenge.server_nonce,
            sizeof(challenge->challenge.server_nonce))) {
        memset(challenge, 0, sizeof(*challenge));
        memset(request_id, 0, sizeof(request_id));
        return VD_ATTACH_LISTENER_ERROR_PROTOCOL;
    }
    memset(request_id, 0, sizeof(request_id));
    return VD_ATTACH_LISTENER_OK;
}

static uint32_t vd_listener_result_code(int result) {
    switch (result) {
        case VD_ATTACH_CONTROL_OK:
            return VD_ATTACH_LISTENER_RESULT_OK;
        case VD_ATTACH_CONTROL_RECOVERY_REQUIRED:
            return VD_ATTACH_LISTENER_RESULT_RECOVERY_REQUIRED;
        case VD_ATTACH_CONTROL_ERROR_ARGUMENT:
            return VD_ATTACH_LISTENER_RESULT_ARGUMENT;
        case VD_ATTACH_CONTROL_ERROR_STATE:
            return VD_ATTACH_LISTENER_RESULT_STATE;
        case VD_ATTACH_CONTROL_ERROR_AUTH:
            return VD_ATTACH_LISTENER_RESULT_AUTH;
        case VD_ATTACH_CONTROL_ERROR_EXPIRED:
            return VD_ATTACH_LISTENER_RESULT_EXPIRED;
        case VD_ATTACH_CONTROL_ERROR_REPLAY:
            return VD_ATTACH_LISTENER_RESULT_REPLAY;
        case VD_ATTACH_CONTROL_ERROR_TARGET:
            return VD_ATTACH_LISTENER_RESULT_TARGET;
        case VD_ATTACH_CONTROL_ERROR_STALE_TARGET:
            return VD_ATTACH_LISTENER_RESULT_STALE_TARGET;
        case VD_ATTACH_CONTROL_ERROR_BUSY:
            return VD_ATTACH_LISTENER_RESULT_BUSY;
        case VD_ATTACH_CONTROL_ERROR_BACKEND:
            return VD_ATTACH_LISTENER_RESULT_BACKEND;
        case VD_ATTACH_CONTROL_ERROR_ENTROPY:
            return VD_ATTACH_LISTENER_RESULT_ENTROPY;
        case VD_ATTACH_CONTROL_ERROR_SHUTDOWN:
            return VD_ATTACH_LISTENER_RESULT_SHUTDOWN;
        case VD_ATTACH_CONTROL_ERROR_LIMIT:
            return VD_ATTACH_LISTENER_RESULT_LIMIT;
        case VD_ATTACH_CONTROL_ERROR_PEER:
            return VD_ATTACH_LISTENER_RESULT_PEER;
        case VD_ATTACH_CONTROL_ERROR_ROLLOVER_REQUIRED:
            return VD_ATTACH_LISTENER_RESULT_ROLLOVER_REQUIRED;
        default:
            return VD_ATTACH_LISTENER_RESULT_INTERNAL;
    }
}

static int vd_listener_response_valid(
    const VdAttachListenerResponse *response) {
    if (response == NULL ||
        !vd_listener_type_valid(response->request_type) ||
        response->session_id == 0u ||
        !vd_listener_bytes_nonzero(response->request_id,
                                   sizeof(response->request_id)) ||
        response->result_code > VD_ATTACH_LISTENER_RESULT_INTERNAL ||
        response->lease_state > VD_ATTACH_CONTROL_LEASE_RECOVERY) {
        return 0;
    }
    if (response->lease_state == VD_ATTACH_CONTROL_LEASE_IDLE) {
        return response->lease_id == 0u &&
               response->lease_expires_at_ms == 0u &&
               response->injected_module_uid == 0u &&
               response->target_generation == 0u;
    }
    return response->lease_id != 0u &&
           response->lease_expires_at_ms != 0u &&
           response->injected_module_uid > 0u &&
           response->injected_module_uid <= (uint32_t)INT32_MAX &&
           response->target_generation != 0u;
}

static int vd_listener_encode_response(
    const VdAttachListenerResponse *response, uint8_t *output,
    size_t output_capacity, size_t *output_size) {
    VdListenerWriter writer;
    if (output_size != NULL) {
        *output_size = 0u;
    }
    if (output == NULL || output_size == NULL ||
        output_capacity < VD_ATTACH_LISTENER_RESPONSE_BYTES ||
        !vd_listener_response_valid(response)) {
        return VD_ATTACH_LISTENER_ERROR_ARGUMENT;
    }
    writer.bytes = output;
    writer.offset = 0u;
    vd_listener_put_common(&writer, g_response_magic,
                           response->request_type, response->request_id,
                           response->session_id);
    vd_listener_put_u32(&writer, response->result_code);
    vd_listener_put_u32(&writer, response->lease_state);
    vd_listener_put_u64(&writer, response->lease_id);
    vd_listener_put_u64(&writer, response->lease_expires_at_ms);
    vd_listener_put_u32(&writer, response->injected_module_uid);
    vd_listener_put_u64(&writer, response->target_generation);
    if (writer.offset != VD_ATTACH_LISTENER_RESPONSE_BYTES) {
        memset(output, 0, output_capacity);
        return VD_ATTACH_LISTENER_ERROR_STATE;
    }
    *output_size = writer.offset;
    return VD_ATTACH_LISTENER_OK;
}

int vd_attach_listener_decode_response(
    const uint8_t *input, size_t input_size,
    VdAttachListenerResponse *response) {
    VdListenerReader reader;
    uint32_t version;
    if (input == NULL || response == NULL ||
        input_size != VD_ATTACH_LISTENER_RESPONSE_BYTES ||
        memcmp(input, g_response_magic, sizeof(g_response_magic)) != 0) {
        return VD_ATTACH_LISTENER_ERROR_PROTOCOL;
    }
    memset(response, 0, sizeof(*response));
    reader.bytes = input;
    reader.offset = 8u;
    version = vd_listener_get_u32(&reader);
    response->request_type = vd_listener_get_u32(&reader);
    vd_listener_get_bytes(&reader, response->request_id,
                          sizeof(response->request_id));
    response->session_id = vd_listener_get_u64(&reader);
    response->result_code = vd_listener_get_u32(&reader);
    response->lease_state = vd_listener_get_u32(&reader);
    response->lease_id = vd_listener_get_u64(&reader);
    response->lease_expires_at_ms = vd_listener_get_u64(&reader);
    response->injected_module_uid = vd_listener_get_u32(&reader);
    response->target_generation = vd_listener_get_u64(&reader);
    if (version != VD_ATTACH_LISTENER_VERSION ||
        reader.offset != input_size ||
        !vd_listener_response_valid(response)) {
        memset(response, 0, sizeof(*response));
        return VD_ATTACH_LISTENER_ERROR_PROTOCOL;
    }
    return VD_ATTACH_LISTENER_OK;
}

static VdAttachListenerConnection *vd_listener_find_connection(
    VdAttachListener *listener, uint64_t session_id, uint64_t peer_id,
    int *error) {
    size_t i;
    for (i = 0u; i < VD_ATTACH_CONTROL_MAX_SESSIONS; ++i) {
        VdAttachListenerConnection *connection =
            &listener->connections[i];
        if (connection->active && connection->session_id == session_id) {
            if (connection->peer_id != peer_id) {
                *error = VD_ATTACH_LISTENER_ERROR_PEER;
                return NULL;
            }
            *error = VD_ATTACH_LISTENER_OK;
            return connection;
        }
    }
    *error = VD_ATTACH_LISTENER_ERROR_STATE;
    return NULL;
}

static int vd_listener_claim_request_id(
    VdAttachListenerConnection *connection,
    const uint8_t request_id[VD_ATTACH_LISTENER_REQUEST_ID_BYTES]) {
    uint32_t i;
    for (i = 0u; i < connection->request_id_count; ++i) {
        if (memcmp(connection->request_ids[i], request_id,
                   VD_ATTACH_LISTENER_REQUEST_ID_BYTES) == 0) {
            return VD_ATTACH_LISTENER_ERROR_REPLAY;
        }
    }
    if (connection->request_id_count >=
        VD_ATTACH_LISTENER_MAX_REQUEST_IDS) {
        return VD_ATTACH_LISTENER_ERROR_LIMIT;
    }
    memcpy(connection->request_ids[connection->request_id_count],
           request_id, VD_ATTACH_LISTENER_REQUEST_ID_BYTES);
    ++connection->request_id_count;
    return VD_ATTACH_LISTENER_OK;
}

int vd_attach_listener_init(VdAttachListener *listener,
                            const VdAttachListenerConfig *config) {
    if (listener == NULL || config == NULL || config->control == NULL ||
        !config->control->initialized || config->now_ms == NULL ||
        config->io_timeout_ms < VD_ATTACH_BROKER_MIN_IO_TIMEOUT_MS ||
        config->io_timeout_ms > VD_ATTACH_BROKER_MAX_IO_TIMEOUT_MS) {
        return VD_ATTACH_LISTENER_ERROR_ARGUMENT;
    }
    if (listener->initialized) {
        return VD_ATTACH_LISTENER_ERROR_STATE;
    }
    memset(listener, 0, sizeof(*listener));
    listener->config = *config;
    listener->initialized = 1;
    return VD_ATTACH_LISTENER_OK;
}

int vd_attach_listener_open_connection(
    VdAttachListener *listener, uint64_t peer_id,
    VdAttachListenerChallengeFrame *challenge) {
    VdAttachListenerConnection *free_connection = NULL;
    size_t i;
    int result;
    if (listener == NULL || challenge == NULL || peer_id == 0u ||
        !listener->initialized) {
        return VD_ATTACH_LISTENER_ERROR_ARGUMENT;
    }
    if (listener->shutting_down) {
        return VD_ATTACH_LISTENER_ERROR_SHUTDOWN;
    }
    for (i = 0u; i < VD_ATTACH_CONTROL_MAX_SESSIONS; ++i) {
        VdAttachListenerConnection *connection =
            &listener->connections[i];
        if (connection->active && connection->peer_id == peer_id) {
            return VD_ATTACH_LISTENER_ERROR_PEER;
        }
        if (!connection->active && free_connection == NULL) {
            free_connection = connection;
        }
    }
    if (free_connection == NULL) {
        return VD_ATTACH_LISTENER_ERROR_LIMIT;
    }
    memset(challenge, 0, sizeof(*challenge));
    result = vd_attach_control_open_session(listener->config.control,
                                            peer_id,
                                            &challenge->challenge);
    if (result != VD_ATTACH_CONTROL_OK) {
        return result == VD_ATTACH_CONTROL_ERROR_SHUTDOWN
                   ? VD_ATTACH_LISTENER_ERROR_SHUTDOWN
                   : VD_ATTACH_LISTENER_ERROR_STATE;
    }
    challenge->session_id = challenge->challenge.session_id;
    memset(free_connection, 0, sizeof(*free_connection));
    free_connection->active = 1;
    free_connection->session_id = challenge->session_id;
    free_connection->peer_id = peer_id;
    return VD_ATTACH_LISTENER_OK;
}

int vd_attach_listener_close_connection(VdAttachListener *listener,
                                        uint64_t session_id,
                                        uint64_t peer_id) {
    VdAttachListenerConnection *connection;
    int error;
    int result;
    if (listener == NULL || !listener->initialized) {
        return VD_ATTACH_LISTENER_ERROR_ARGUMENT;
    }
    connection = vd_listener_find_connection(listener, session_id, peer_id,
                                             &error);
    if (connection == NULL) {
        return error;
    }
    result = vd_attach_control_close_session(listener->config.control,
                                             session_id, peer_id);
    memset(connection, 0, sizeof(*connection));
    if (result == VD_ATTACH_CONTROL_OK) {
        return VD_ATTACH_LISTENER_OK;
    }
    return result == VD_ATTACH_CONTROL_RECOVERY_REQUIRED
               ? VD_ATTACH_LISTENER_RECOVERY_REQUIRED
               : VD_ATTACH_LISTENER_ERROR_STATE;
}

int vd_attach_listener_handle_record(
    VdAttachListener *listener, uint64_t session_id, uint64_t peer_id,
    const uint8_t *request_bytes, size_t request_size, uint8_t *response_bytes,
    size_t response_capacity, size_t *response_size) {
    VdAttachListenerConnection *connection;
    VdAttachListenerRequest request;
    VdAttachListenerResponse response;
    VdAttachControlLeaseSnapshot snapshot;
    int error;
    int result;

    if (response_size != NULL) {
        *response_size = 0u;
    }
    if (listener == NULL || request_bytes == NULL || response_bytes == NULL ||
        response_size == NULL ||
        response_capacity < VD_ATTACH_LISTENER_RESPONSE_BYTES ||
        !listener->initialized) {
        return VD_ATTACH_LISTENER_ERROR_ARGUMENT;
    }
    if (listener->shutting_down) {
        return VD_ATTACH_LISTENER_ERROR_SHUTDOWN;
    }
    connection = vd_listener_find_connection(listener, session_id, peer_id,
                                             &error);
    if (connection == NULL) {
        return error;
    }
    if (connection->terminal) {
        return VD_ATTACH_LISTENER_ERROR_STATE;
    }
    result = vd_listener_decode_request(request_bytes, request_size,
                                        &request);
    if (result != VD_ATTACH_LISTENER_OK ||
        request.session_id != session_id) {
        connection->terminal = 1;
        memset(&request, 0, sizeof(request));
        return VD_ATTACH_LISTENER_ERROR_PROTOCOL;
    }
    result = vd_listener_claim_request_id(connection, request.request_id);
    if (result != VD_ATTACH_LISTENER_OK) {
        connection->terminal = 1;
        memset(&request, 0, sizeof(request));
        return result;
    }

    if (request.type == VD_ATTACH_LISTENER_REQUEST_AUTHENTICATE) {
        ++connection->auth_attempts;
        result = vd_attach_control_authenticate(
            listener->config.control, session_id, peer_id,
            &request.peer_proof);
        if (result == VD_ATTACH_CONTROL_OK) {
            connection->authenticated = 1;
            connection->host_key_id = request.peer_proof.host_key_id;
        } else if (connection->auth_attempts >=
                   VD_ATTACH_CONTROL_MAX_AUTH_ATTEMPTS) {
            connection->terminal = 1;
            memset(&request, 0, sizeof(request));
            return VD_ATTACH_LISTENER_ERROR_STATE;
        }
    } else if (!connection->authenticated) {
        result = VD_ATTACH_CONTROL_ERROR_AUTH;
    } else if (request.type == VD_ATTACH_LISTENER_REQUEST_ATTACH) {
        result = vd_attach_control_begin_attach(
            listener->config.control, session_id, peer_id,
            &request.operation_proof, &snapshot);
    } else if (request.type == VD_ATTACH_LISTENER_REQUEST_DETACH) {
        result = vd_attach_control_detach(
            listener->config.control, session_id, peer_id,
            &request.operation_proof);
    } else {
        result = vd_attach_control_recover(
            listener->config.control, session_id, peer_id,
            &request.operation_proof);
    }

    /* Cryptographic operation replay and rollover are fatal to this channel. */
    if (result == VD_ATTACH_CONTROL_ERROR_REPLAY) {
        connection->terminal = 1;
        memset(&request, 0, sizeof(request));
        return VD_ATTACH_LISTENER_ERROR_REPLAY;
    }
    if (result == VD_ATTACH_CONTROL_ERROR_ROLLOVER_REQUIRED) {
        connection->terminal = 1;
        memset(&request, 0, sizeof(request));
        return VD_ATTACH_LISTENER_ERROR_LIMIT;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    if (vd_attach_control_snapshot(listener->config.control, &snapshot) !=
        VD_ATTACH_CONTROL_OK) {
        memset(&request, 0, sizeof(request));
        return VD_ATTACH_LISTENER_ERROR_STATE;
    }
    memset(&response, 0, sizeof(response));
    response.request_type = request.type;
    memcpy(response.request_id, request.request_id,
           sizeof(response.request_id));
    response.session_id = request.session_id;
    response.result_code = vd_listener_result_code(result);
    response.lease_state = snapshot.state;
    if (snapshot.state != VD_ATTACH_CONTROL_LEASE_IDLE &&
        connection->authenticated && connection->host_key_id != 0u &&
        connection->host_key_id == snapshot.owner_host_key_id) {
        response.lease_id = snapshot.lease_id;
        response.lease_expires_at_ms = snapshot.expires_at_ms;
        response.injected_module_uid = snapshot.injected_module_uid;
        response.target_generation = snapshot.target.target_generation;
    } else if (snapshot.state != VD_ATTACH_CONTROL_LEASE_IDLE) {
        /* Do not disclose another host's lease metadata. */
        response.lease_state = VD_ATTACH_CONTROL_LEASE_IDLE;
    }
    result = vd_listener_encode_response(
        &response, response_bytes, response_capacity, response_size);
    if (result != VD_ATTACH_LISTENER_OK) {
        connection->terminal = 1;
    }
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    memset(&snapshot, 0, sizeof(snapshot));
    return result;
}

static uint64_t vd_listener_add_ms(uint64_t now_ms, uint32_t delta_ms) {
    if (UINT64_MAX - now_ms < (uint64_t)delta_ms) {
        return UINT64_MAX;
    }
    return now_ms + (uint64_t)delta_ms;
}

static uint64_t vd_listener_exchange_deadline(
    VdAttachListener *listener) {
    VdAttachControlLeaseSnapshot snapshot;
    const uint64_t now_ms =
        listener->config.now_ms(listener->config.clock_context);
    uint64_t deadline_ms = vd_listener_add_ms(
        now_ms, listener->config.io_timeout_ms);
    memset(&snapshot, 0, sizeof(snapshot));
    if (vd_attach_control_snapshot(listener->config.control, &snapshot) ==
            VD_ATTACH_CONTROL_OK &&
        snapshot.state == VD_ATTACH_CONTROL_LEASE_ACTIVE &&
        snapshot.expires_at_ms < deadline_ms) {
        deadline_ms = snapshot.expires_at_ms;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    return deadline_ms;
}

static int vd_listener_read_exact(const VdAttachTransport *transport,
                                  void *output, size_t size,
                                  uint64_t deadline_ms,
                                  int initial_prefix) {
    size_t offset = 0u;
    while (offset < size) {
        int count = transport->read(transport->context,
                                    (uint8_t *)output + offset,
                                    size - offset, deadline_ms);
        if (count == 0) {
            return initial_prefix && offset == 0u
                       ? VD_ATTACH_LISTENER_CLOSED
                       : VD_ATTACH_LISTENER_ERROR_IO;
        }
        if (count < 0 || (size_t)count > size - offset) {
            return VD_ATTACH_LISTENER_ERROR_IO;
        }
        offset += (size_t)count;
    }
    return VD_ATTACH_LISTENER_OK;
}

static int vd_listener_write_exact(const VdAttachTransport *transport,
                                   const void *input, size_t size,
                                   uint64_t deadline_ms) {
    size_t offset = 0u;
    while (offset < size) {
        int count = transport->write(transport->context,
                                     (const uint8_t *)input + offset,
                                     size - offset, deadline_ms);
        if (count <= 0 || (size_t)count > size - offset) {
            return VD_ATTACH_LISTENER_ERROR_IO;
        }
        offset += (size_t)count;
    }
    return VD_ATTACH_LISTENER_OK;
}

static int vd_listener_write_frame(const VdAttachTransport *transport,
                                   const uint8_t *payload,
                                   size_t payload_size,
                                   uint64_t deadline_ms) {
    uint8_t prefix[4];
    int result;
    prefix[0] = (uint8_t)(payload_size >> 24);
    prefix[1] = (uint8_t)(payload_size >> 16);
    prefix[2] = (uint8_t)(payload_size >> 8);
    prefix[3] = (uint8_t)payload_size;
    result = vd_listener_write_exact(transport, prefix, sizeof(prefix),
                                     deadline_ms);
    if (result != VD_ATTACH_LISTENER_OK) {
        return result;
    }
    return vd_listener_write_exact(transport, payload, payload_size,
                                   deadline_ms);
}

int vd_attach_listener_serve(VdAttachListener *listener,
                             const VdAttachTransport *transport) {
    VdAttachListenerChallengeFrame challenge;
    uint8_t prefix[4];
    uint8_t request[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    uint8_t response[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    size_t challenge_size = 0u;
    uint64_t session_id = 0u;
    int result;
    int close_result;

    if (listener == NULL || transport == NULL || transport->read == NULL ||
        transport->write == NULL || transport->close == NULL ||
        transport->peer_id == 0u || !listener->initialized) {
        return VD_ATTACH_LISTENER_ERROR_ARGUMENT;
    }
    result = vd_attach_listener_open_connection(listener,
                                                transport->peer_id,
                                                &challenge);
    if (result != VD_ATTACH_LISTENER_OK) {
        transport->close(transport->context);
        return result;
    }
    session_id = challenge.session_id;
    result = vd_attach_listener_encode_challenge(
        &challenge, response, sizeof(response), &challenge_size);
    memset(&challenge, 0, sizeof(challenge));
    if (result == VD_ATTACH_LISTENER_OK) {
        const uint64_t deadline_ms =
            vd_listener_exchange_deadline(listener);
        result = vd_listener_write_frame(transport, response,
                                         challenge_size, deadline_ms);
    }

    while (result == VD_ATTACH_LISTENER_OK) {
        uint64_t deadline_ms;
        uint64_t response_deadline_ms;
        uint32_t request_size;
        size_t response_size = 0u;
        if (listener->shutting_down) {
            result = VD_ATTACH_LISTENER_ERROR_SHUTDOWN;
            break;
        }
        (void)vd_attach_control_service(listener->config.control);
        deadline_ms = vd_listener_exchange_deadline(listener);
        result = vd_listener_read_exact(transport, prefix, sizeof(prefix),
                                        deadline_ms, 1);
        if (result != VD_ATTACH_LISTENER_OK) {
            break;
        }
        request_size = ((uint32_t)prefix[0] << 24) |
                       ((uint32_t)prefix[1] << 16) |
                       ((uint32_t)prefix[2] << 8) |
                       (uint32_t)prefix[3];
        if (request_size == 0u ||
            request_size > VD_ATTACH_LISTENER_MAX_FRAME_SIZE) {
            result = VD_ATTACH_LISTENER_ERROR_FRAME;
            break;
        }
        result = vd_listener_read_exact(transport, request, request_size,
                                        deadline_ms, 0);
        if (result != VD_ATTACH_LISTENER_OK) {
            break;
        }
        result = vd_attach_listener_handle_record(
            listener, session_id, transport->peer_id, request,
            request_size, response, sizeof(response), &response_size);
        if (result != VD_ATTACH_LISTENER_OK) {
            break;
        }
        response_deadline_ms = vd_listener_exchange_deadline(listener);
        if (response_deadline_ms > deadline_ms) {
            response_deadline_ms = deadline_ms;
        }
        result = vd_listener_write_frame(transport, response,
                                         response_size,
                                         response_deadline_ms);
    }

    close_result = vd_attach_listener_close_connection(
        listener, session_id, transport->peer_id);
    transport->close(transport->context);
    memset(prefix, 0, sizeof(prefix));
    memset(request, 0, sizeof(request));
    memset(response, 0, sizeof(response));
    if (close_result == VD_ATTACH_LISTENER_RECOVERY_REQUIRED) {
        return close_result;
    }
    return result;
}

int vd_attach_listener_shutdown(VdAttachListener *listener) {
    int result;
    if (listener == NULL || !listener->initialized) {
        return VD_ATTACH_LISTENER_ERROR_ARGUMENT;
    }
    result = vd_attach_control_shutdown(listener->config.control);
    listener->shutting_down = 1;
    memset(listener->connections, 0, sizeof(listener->connections));
    if (result == VD_ATTACH_CONTROL_OK) {
        return VD_ATTACH_LISTENER_OK;
    }
    return result == VD_ATTACH_CONTROL_RECOVERY_REQUIRED
               ? VD_ATTACH_LISTENER_RECOVERY_REQUIRED
               : VD_ATTACH_LISTENER_ERROR_STATE;
}

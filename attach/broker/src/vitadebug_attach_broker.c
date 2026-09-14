#include "vitadebug_attach_broker.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct VdParsedRecord {
    char storage[VD_ATTACH_MAX_FRAME_SIZE + 1u];
    char *values[11];
} VdParsedRecord;

typedef struct VdHelloRequest {
    char request_id[33];
    char client_nonce[65];
    uint32_t expected_kernel_abi;
    uint32_t required_caps;
} VdHelloRequest;

typedef struct VdDiscoverRequest {
    char request_id[33];
    char server_nonce[65];
    uint64_t service_generation;
    char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES];
} VdDiscoverRequest;

typedef struct VdReleaseRequest {
    char request_id[33];
    char server_nonce[65];
    uint64_t service_generation;
    char ticket[65];
} VdReleaseRequest;

typedef struct VdWriter {
    uint8_t *data;
    size_t capacity;
    size_t size;
} VdWriter;

static const char *const g_hello_keys[] = {
    "request_id",
    "client_nonce",
    "mode",
    "expected_kernel_abi",
    "required_caps",
};

static const char *const g_discover_keys[] = {
    "request_id",
    "server_nonce",
    "service_generation",
    "target_title_id",
    "mode",
};

static const char *const g_release_keys[] = {
    "request_id",
    "server_nonce",
    "service_generation",
    "target_ticket",
};

static int vd_is_lower_hex(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f');
}

static int vd_token_is_valid(const char *value, size_t digits, int allow_zero) {
    size_t i;
    int any_nonzero = 0;

    if (value == NULL || strlen(value) != digits) {
        return 0;
    }
    for (i = 0; i < digits; ++i) {
        if (!vd_is_lower_hex(value[i])) {
            return 0;
        }
        if (value[i] != '0') {
            any_nonzero = 1;
        }
    }
    return allow_zero || any_nonzero;
}

static int vd_parse_hex_u64(const char *value,
                            size_t digits,
                            int allow_zero,
                            uint64_t *output) {
    size_t i;
    uint64_t result = 0;

    if (output == NULL || digits > 16u ||
        !vd_token_is_valid(value, digits, allow_zero)) {
        return VD_ATTACH_BROKER_ERROR_PROTOCOL;
    }
    for (i = 0; i < digits; ++i) {
        unsigned int nibble;
        if (value[i] <= '9') {
            nibble = (unsigned int)(value[i] - '0');
        } else {
            nibble = (unsigned int)(value[i] - 'a') + 10u;
        }
        result = (result << 4) | nibble;
    }
    *output = result;
    return VD_ATTACH_BROKER_OK;
}

static int vd_title_is_valid(const char *title_id) {
    size_t i;

    if (title_id == NULL || strlen(title_id) != VD_ATTACH_TITLE_ID_LENGTH) {
        return 0;
    }
    for (i = 0; i < VD_ATTACH_TITLE_ID_LENGTH; ++i) {
        char value = title_id[i];
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9'))) {
            return 0;
        }
    }
    return 1;
}

static int vd_parse_record(const uint8_t *request,
                           size_t request_size,
                           const char *header,
                           const char *const *keys,
                           size_t key_count,
                           VdParsedRecord *parsed) {
    size_t i;
    char *cursor;
    char *newline;

    if (request == NULL || header == NULL || keys == NULL || parsed == NULL ||
        request_size == 0u || request_size > VD_ATTACH_MAX_FRAME_SIZE ||
        key_count > sizeof(parsed->values) / sizeof(parsed->values[0]) ||
        request[request_size - 1u] != '\n') {
        return VD_ATTACH_BROKER_ERROR_PROTOCOL;
    }
    for (i = 0; i < request_size; ++i) {
        uint8_t value = request[i];
        if (value != '\n' && (value < 0x20u || value > 0x7eu)) {
            return VD_ATTACH_BROKER_ERROR_PROTOCOL;
        }
    }

    memcpy(parsed->storage, request, request_size);
    parsed->storage[request_size] = '\0';
    cursor = parsed->storage;
    newline = strchr(cursor, '\n');
    if (newline == NULL) {
        return VD_ATTACH_BROKER_ERROR_PROTOCOL;
    }
    *newline = '\0';
    if (strcmp(cursor, header) != 0) {
        return VD_ATTACH_BROKER_ERROR_PROTOCOL;
    }
    cursor = newline + 1;

    for (i = 0; i < key_count; ++i) {
        size_t key_size = strlen(keys[i]);
        newline = strchr(cursor, '\n');
        if (newline == NULL ||
            (size_t)(newline - cursor) <= key_size ||
            memcmp(cursor, keys[i], key_size) != 0 ||
            cursor[key_size] != '=') {
            return VD_ATTACH_BROKER_ERROR_PROTOCOL;
        }
        *newline = '\0';
        parsed->values[i] = cursor + key_size + 1u;
        if (parsed->values[i][0] == '\0') {
            return VD_ATTACH_BROKER_ERROR_PROTOCOL;
        }
        cursor = newline + 1;
    }
    if (*cursor != '\0') {
        return VD_ATTACH_BROKER_ERROR_PROTOCOL;
    }
    return VD_ATTACH_BROKER_OK;
}

static int vd_parse_hello(const uint8_t *request,
                          size_t request_size,
                          VdHelloRequest *hello) {
    VdParsedRecord parsed;
    uint64_t value;
    int result;

    if (hello == NULL) {
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    result = vd_parse_record(request, request_size,
                             VD_ATTACH_HELLO_HEADER,
                             g_hello_keys,
                             sizeof(g_hello_keys) / sizeof(g_hello_keys[0]),
                             &parsed);
    if (result < 0 ||
        !vd_token_is_valid(parsed.values[0], 32u, 0) ||
        !vd_token_is_valid(parsed.values[1], 64u, 0) ||
        strcmp(parsed.values[2], VD_ATTACH_MODE_OBSERVE) != 0 ||
        vd_parse_hex_u64(parsed.values[3], 8u, 0, &value) < 0) {
        return VD_ATTACH_BROKER_ERROR_PROTOCOL;
    }
    hello->expected_kernel_abi = (uint32_t)value;
    if (vd_parse_hex_u64(parsed.values[4], 8u, 1, &value) < 0 ||
        (((uint32_t)value) & ~VD_ATTACH_READ_ONLY_CAPABILITIES) != 0u) {
        return VD_ATTACH_BROKER_ERROR_PROTOCOL;
    }
    hello->required_caps = (uint32_t)value;
    memcpy(hello->request_id, parsed.values[0], sizeof(hello->request_id));
    memcpy(hello->client_nonce, parsed.values[1], sizeof(hello->client_nonce));
    return VD_ATTACH_BROKER_OK;
}

static int vd_parse_discover(const uint8_t *request,
                             size_t request_size,
                             VdDiscoverRequest *discover) {
    VdParsedRecord parsed;
    int result;

    if (discover == NULL) {
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    result = vd_parse_record(request, request_size,
                             VD_ATTACH_DISCOVER_HEADER,
                             g_discover_keys,
                             sizeof(g_discover_keys) /
                                 sizeof(g_discover_keys[0]),
                             &parsed);
    if (result < 0 ||
        !vd_token_is_valid(parsed.values[0], 32u, 0) ||
        !vd_token_is_valid(parsed.values[1], 64u, 0) ||
        vd_parse_hex_u64(parsed.values[2], 16u, 0,
                         &discover->service_generation) < 0 ||
        !vd_title_is_valid(parsed.values[3]) ||
        strcmp(parsed.values[4], VD_ATTACH_MODE_OBSERVE) != 0) {
        return VD_ATTACH_BROKER_ERROR_PROTOCOL;
    }
    memcpy(discover->request_id, parsed.values[0],
           sizeof(discover->request_id));
    memcpy(discover->server_nonce, parsed.values[1],
           sizeof(discover->server_nonce));
    memcpy(discover->title_id, parsed.values[3],
           sizeof(discover->title_id));
    return VD_ATTACH_BROKER_OK;
}

static int vd_parse_release(const uint8_t *request,
                            size_t request_size,
                            VdReleaseRequest *release) {
    VdParsedRecord parsed;
    int result;

    if (release == NULL) {
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    result = vd_parse_record(request, request_size,
                             VD_ATTACH_RELEASE_HEADER,
                             g_release_keys,
                             sizeof(g_release_keys) /
                                 sizeof(g_release_keys[0]),
                             &parsed);
    if (result < 0 ||
        !vd_token_is_valid(parsed.values[0], 32u, 0) ||
        !vd_token_is_valid(parsed.values[1], 64u, 0) ||
        vd_parse_hex_u64(parsed.values[2], 16u, 0,
                         &release->service_generation) < 0 ||
        !vd_token_is_valid(parsed.values[3], 64u, 0)) {
        return VD_ATTACH_BROKER_ERROR_PROTOCOL;
    }
    memcpy(release->request_id, parsed.values[0],
           sizeof(release->request_id));
    memcpy(release->server_nonce, parsed.values[1],
           sizeof(release->server_nonce));
    memcpy(release->ticket, parsed.values[3], sizeof(release->ticket));
    return VD_ATTACH_BROKER_OK;
}

static int vd_writer_append(VdWriter *writer, const char *format, ...) {
    va_list args;
    int count;
    size_t remaining;

    if (writer == NULL || writer->data == NULL || format == NULL ||
        writer->size >= writer->capacity) {
        return VD_ATTACH_BROKER_ERROR_FRAME;
    }
    remaining = writer->capacity - writer->size;
    va_start(args, format);
    count = vsnprintf((char *)writer->data + writer->size, remaining,
                      format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= remaining) {
        return VD_ATTACH_BROKER_ERROR_FRAME;
    }
    writer->size += (size_t)count;
    if (writer->size > VD_ATTACH_MAX_FRAME_SIZE) {
        return VD_ATTACH_BROKER_ERROR_FRAME;
    }
    return VD_ATTACH_BROKER_OK;
}

static int vd_is_percent_unreserved(uint8_t value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' ||
           value == '_' || value == '~';
}

static int vd_percent_encode_message(const char *message,
                                     char *output,
                                     size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    size_t input_index;
    size_t output_index = 0u;

    if (message == NULL || output == NULL || capacity == 0u) {
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    if (message[0] == '\0') {
        if (capacity < 4u) {
            return VD_ATTACH_BROKER_ERROR_FRAME;
        }
        memcpy(output, "%00", 4u);
        return VD_ATTACH_BROKER_OK;
    }
    for (input_index = 0u; message[input_index] != '\0'; ++input_index) {
        uint8_t value = (uint8_t)message[input_index];
        if (value < 0x20u || value > 0x7eu) {
            return VD_ATTACH_BROKER_ERROR_PROTOCOL;
        }
        if (vd_is_percent_unreserved(value)) {
            if (output_index + 1u >= capacity) {
                return VD_ATTACH_BROKER_ERROR_FRAME;
            }
            output[output_index++] = (char)value;
        } else {
            if (output_index + 3u >= capacity) {
                return VD_ATTACH_BROKER_ERROR_FRAME;
            }
            output[output_index++] = '%';
            output[output_index++] = hex[value >> 4];
            output[output_index++] = hex[value & 0x0fu];
        }
    }
    output[output_index] = '\0';
    return VD_ATTACH_BROKER_OK;
}

static int vd_write_hello_result(const VdAttachBroker *broker,
                                 const VdAttachBrokerSession *session,
                                 const VdHelloRequest *request,
                                 uint8_t *response,
                                 size_t response_capacity,
                                 size_t *response_len) {
    VdWriter writer = {response, response_capacity, 0u};
    char message[96];
    int ready = broker->config.service_ready != 0;
    int result;

    result = vd_percent_encode_message(ready ? "" : "service unavailable",
                                       message, sizeof(message));
    if (result < 0) {
        return result;
    }
    result = vd_writer_append(
        &writer,
        VD_ATTACH_HELLO_RESULT_HEADER "\n"
        "request_id=%s\n"
        "client_nonce=%s\n"
        "server_nonce=%s\n"
        "service_generation=%016" PRIx64 "\n"
        "state=%s\n"
        "attach_caps=%08" PRIx32 "\n"
        "kernel_abi=%08" PRIx32 "\n"
        "kernel_caps=%08" PRIx32 "\n"
        "ticket_lease_ms=%" PRIu32 "\n"
        "control_policy=" VD_ATTACH_CONTROL_POLICY_DISABLED "\n"
        "message=%s\n",
        request->request_id, request->client_nonce, session->server_nonce,
        broker->service_generation, ready ? "ready" : "unavailable",
        broker->config.attach_caps, broker->config.kernel_abi,
        broker->config.kernel_caps, broker->config.ticket_lease_ms, message);
    if (result == VD_ATTACH_BROKER_OK) {
        *response_len = writer.size;
    }
    return result;
}

static int vd_write_discover_result(const VdAttachBroker *broker,
                                    const VdAttachBrokerSession *session,
                                    const VdDiscoverRequest *request,
                                    const char *state,
                                    const VdAttachTargetIdentity *identity,
                                    const char *ticket,
                                    const char *plain_message,
                                    uint8_t *response,
                                    size_t response_capacity,
                                    size_t *response_len) {
    VdWriter writer = {response, response_capacity, 0u};
    VdAttachTargetIdentity zero_identity;
    char zero_ticket[65];
    char message[128];
    int result;

    memset(&zero_identity, 0, sizeof(zero_identity));
    memset(zero_ticket, '0', 64u);
    zero_ticket[64] = '\0';
    if (identity == NULL) {
        identity = &zero_identity;
    }
    if (ticket == NULL) {
        ticket = zero_ticket;
    }
    result = vd_percent_encode_message(plain_message, message,
                                       sizeof(message));
    if (result < 0) {
        return result;
    }
    result = vd_writer_append(
        &writer,
        VD_ATTACH_DISCOVER_RESULT_HEADER "\n"
        "request_id=%s\n"
        "server_nonce=%s\n"
        "service_generation=%016" PRIx64 "\n"
        "state=%s\n"
        "target_title_id=%s\n"
        "pid=%08" PRIx32 "\n"
        "main_modid=%08" PRIx32 "\n"
        "main_fingerprint=%08" PRIx32 "\n"
        "target_generation=%016" PRIx64 "\n"
        "target_ticket=%s\n"
        "message=%s\n",
        request->request_id, session->server_nonce,
        broker->service_generation, state, request->title_id, identity->pid,
        identity->main_modid, identity->main_fingerprint,
        identity->target_generation, ticket, message);
    if (result == VD_ATTACH_BROKER_OK) {
        *response_len = writer.size;
    }
    return result;
}

static int vd_write_release_result(const VdAttachBroker *broker,
                                   const VdAttachBrokerSession *session,
                                   const VdReleaseRequest *request,
                                   const char *state,
                                   const char *plain_message,
                                   uint8_t *response,
                                   size_t response_capacity,
                                   size_t *response_len) {
    VdWriter writer = {response, response_capacity, 0u};
    char message[128];
    int result = vd_percent_encode_message(plain_message, message,
                                           sizeof(message));
    if (result < 0) {
        return result;
    }
    result = vd_writer_append(
        &writer,
        VD_ATTACH_RELEASE_RESULT_HEADER "\n"
        "request_id=%s\n"
        "server_nonce=%s\n"
        "service_generation=%016" PRIx64 "\n"
        "target_ticket=%s\n"
        "state=%s\n"
        "message=%s\n",
        request->request_id, session->server_nonce,
        broker->service_generation, request->ticket, state, message);
    if (result == VD_ATTACH_BROKER_OK) {
        *response_len = writer.size;
    }
    return result;
}

static int vd_fill_random(VdAttachBroker *broker,
                          uint8_t *output,
                          size_t size) {
    unsigned int attempt;

    for (attempt = 0u; attempt < 4u; ++attempt) {
        size_t i;
        int nonzero = 0;
        if (broker->config.entropy(broker->config.callback_context,
                                   output, size) != 0) {
            return VD_ATTACH_BROKER_ERROR_ENTROPY;
        }
        for (i = 0u; i < size; ++i) {
            nonzero |= output[i] != 0u;
        }
        if (nonzero) {
            return VD_ATTACH_BROKER_OK;
        }
    }
    return VD_ATTACH_BROKER_ERROR_ENTROPY;
}

static void vd_hex_encode(const uint8_t *input, size_t size, char *output) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0u; i < size; ++i) {
        output[i * 2u] = hex[input[i] >> 4];
        output[i * 2u + 1u] = hex[input[i] & 0x0fu];
    }
    output[size * 2u] = '\0';
}

static int vd_make_token(VdAttachBroker *broker, char output[65]) {
    uint8_t bytes[VD_ATTACH_BROKER_TOKEN_BYTES];
    int result = vd_fill_random(broker, bytes, sizeof(bytes));
    if (result == VD_ATTACH_BROKER_OK) {
        vd_hex_encode(bytes, sizeof(bytes), output);
    }
    memset(bytes, 0, sizeof(bytes));
    return result;
}

static int vd_make_u64(VdAttachBroker *broker, uint64_t *output) {
    uint8_t bytes[8];
    uint64_t value = 0u;
    size_t i;
    int result;

    result = vd_fill_random(broker, bytes, sizeof(bytes));
    if (result < 0) {
        return result;
    }
    for (i = 0u; i < sizeof(bytes); ++i) {
        value = (value << 8) | bytes[i];
    }
    memset(bytes, 0, sizeof(bytes));
    if (value == 0u) {
        return VD_ATTACH_BROKER_ERROR_ENTROPY;
    }
    *output = value;
    return VD_ATTACH_BROKER_OK;
}

static VdAttachBrokerSession *vd_find_session(VdAttachBroker *broker,
                                              uint64_t session_id,
                                              uint64_t peer_id,
                                              int *error) {
    size_t i;
    for (i = 0u; i < VD_ATTACH_BROKER_MAX_SESSIONS; ++i) {
        VdAttachBrokerSession *session = &broker->sessions[i];
        if (session->active && session->session_id == session_id) {
            if (session->peer_id != peer_id) {
                *error = VD_ATTACH_BROKER_ERROR_PEER;
                return NULL;
            }
            *error = VD_ATTACH_BROKER_OK;
            return session;
        }
    }
    *error = VD_ATTACH_BROKER_ERROR_STATE;
    return NULL;
}

static int vd_request_id_claim(VdAttachBrokerSession *session,
                               const char request_id[33]) {
    uint32_t i;
    for (i = 0u; i < session->request_id_count; ++i) {
        if (memcmp(session->request_ids[i], request_id, 33u) == 0) {
            return VD_ATTACH_BROKER_ERROR_REPLAY;
        }
    }
    if (session->request_id_count >= VD_ATTACH_BROKER_MAX_REQUEST_IDS) {
        return VD_ATTACH_BROKER_ERROR_LIMIT;
    }
    memcpy(session->request_ids[session->request_id_count], request_id, 33u);
    ++session->request_id_count;
    return VD_ATTACH_BROKER_OK;
}

static int vd_identity_is_valid(const VdAttachTargetIdentity *identity,
                                const char *expected_title) {
    return identity != NULL &&
           identity->title_id[VD_ATTACH_TITLE_ID_LENGTH] == '\0' &&
           vd_title_is_valid(identity->title_id) &&
           strcmp(identity->title_id, expected_title) == 0 &&
           identity->pid > 0u && identity->pid <= 0x7fffffffu &&
           identity->main_modid > 0u &&
           identity->main_modid <= 0x7fffffffu &&
           identity->main_fingerprint != 0u &&
           identity->target_generation != 0u;
}

static int vd_identity_matches(const VdAttachTargetIdentity *left,
                               const VdAttachTargetIdentity *right) {
    return strcmp(left->title_id, right->title_id) == 0 &&
           left->pid == right->pid &&
           left->main_modid == right->main_modid &&
           left->main_fingerprint == right->main_fingerprint &&
           left->target_generation == right->target_generation;
}

static uint64_t vd_deadline_after(uint64_t now, uint32_t interval) {
    uint64_t result = now + (uint64_t)interval;
    return result < now ? UINT64_MAX : result;
}

static void vd_expire_tickets(VdAttachBroker *broker, uint64_t now) {
    size_t i;
    for (i = 0u; i < VD_ATTACH_BROKER_MAX_SESSIONS; ++i) {
        VdAttachBrokerTicket *ticket = &broker->sessions[i].ticket;
        if (broker->sessions[i].active && ticket->active &&
            now >= ticket->expires_at_ms) {
            memset(ticket, 0, sizeof(*ticket));
        }
    }
}

static int vd_ticket_exists_elsewhere(const VdAttachBroker *broker,
                                      const VdAttachBrokerSession *owner,
                                      const char *ticket) {
    size_t i;
    for (i = 0u; i < VD_ATTACH_BROKER_MAX_SESSIONS; ++i) {
        const VdAttachBrokerSession *candidate = &broker->sessions[i];
        if (candidate != owner && candidate->active &&
            candidate->ticket.active &&
            strcmp(candidate->ticket.token, ticket) == 0) {
            return 1;
        }
    }
    return 0;
}

static int vd_ticket_token_in_use(const VdAttachBroker *broker,
                                  const char *token) {
    size_t i;
    for (i = 0u; i < VD_ATTACH_BROKER_MAX_SESSIONS; ++i) {
        const VdAttachBrokerSession *session = &broker->sessions[i];
        if (session->active && session->ticket.active &&
            strcmp(session->ticket.token, token) == 0) {
            return 1;
        }
    }
    return 0;
}

static int vd_make_unique_ticket(VdAttachBroker *broker, char output[65]) {
    unsigned int attempt;
    for (attempt = 0u; attempt < 4u; ++attempt) {
        int result = vd_make_token(broker, output);
        if (result < 0) {
            return result;
        }
        if (!vd_ticket_token_in_use(broker, output)) {
            return VD_ATTACH_BROKER_OK;
        }
    }
    memset(output, 0, 65u);
    return VD_ATTACH_BROKER_ERROR_ENTROPY;
}

static int vd_server_nonce_in_use(const VdAttachBroker *broker,
                                  const VdAttachBrokerSession *owner,
                                  const char *nonce) {
    size_t i;
    for (i = 0u; i < VD_ATTACH_BROKER_MAX_SESSIONS; ++i) {
        const VdAttachBrokerSession *session = &broker->sessions[i];
        if (session != owner && session->active &&
            strcmp(session->server_nonce, nonce) == 0) {
            return 1;
        }
    }
    return 0;
}

static int vd_handle_hello(VdAttachBroker *broker,
                           VdAttachBrokerSession *session,
                           const uint8_t *request,
                           size_t request_size,
                           uint8_t *response,
                           size_t response_capacity,
                           size_t *response_len) {
    VdHelloRequest hello;
    int result;

    if (session->hello_complete) {
        return VD_ATTACH_BROKER_ERROR_STATE;
    }
    result = vd_parse_hello(request, request_size, &hello);
    if (result < 0) {
        return result;
    }
    result = vd_request_id_claim(session, hello.request_id);
    if (result < 0) {
        return result;
    }
    session->hello_complete = 1;
    return vd_write_hello_result(broker, session, &hello, response,
                                 response_capacity, response_len);
}

static const char *vd_inventory_state(VdAttachInventoryResult result) {
    switch (result) {
        case VD_ATTACH_INVENTORY_NOT_FOUND:
            return "not_found";
        case VD_ATTACH_INVENTORY_DENIED:
            return "denied";
        case VD_ATTACH_INVENTORY_CHANGED:
            return "changed";
        case VD_ATTACH_INVENTORY_ERROR:
        case VD_ATTACH_INVENTORY_UNAVAILABLE:
        default:
            return "error";
    }
}

static int vd_handle_discover(VdAttachBroker *broker,
                              VdAttachBrokerSession *session,
                              const uint8_t *request,
                              size_t request_size,
                              uint8_t *response,
                              size_t response_capacity,
                              size_t *response_len,
                              uint64_t deadline_ms) {
    VdDiscoverRequest discover;
    VdAttachTargetIdentity identity;
    VdAttachInventoryResult inventory_result;
    int result;

    if (!session->hello_complete) {
        return VD_ATTACH_BROKER_ERROR_STATE;
    }
    result = vd_parse_discover(request, request_size, &discover);
    if (result < 0) {
        return result;
    }
    if (strcmp(discover.server_nonce, session->server_nonce) != 0 ||
        discover.service_generation != broker->service_generation) {
        return VD_ATTACH_BROKER_ERROR_STATE;
    }
    result = vd_request_id_claim(session, discover.request_id);
    if (result < 0) {
        return result;
    }
    if (session->ticket.active) {
        return vd_write_discover_result(
            broker, session, &discover, "error", NULL, NULL,
            "release active ticket first", response, response_capacity,
            response_len);
    }
    if (!broker->config.service_ready ||
        broker->config.discover_exact == NULL ||
        (broker->config.attach_caps &
         (VD_ATTACH_CAP_EXACT_TITLE_DISCOVERY |
          VD_ATTACH_CAP_IDENTITY_TICKET |
          VD_ATTACH_CAP_TICKET_RELEASE)) !=
            (VD_ATTACH_CAP_EXACT_TITLE_DISCOVERY |
             VD_ATTACH_CAP_IDENTITY_TICKET |
             VD_ATTACH_CAP_TICKET_RELEASE)) {
        return vd_write_discover_result(
            broker, session, &discover, "error", NULL, NULL,
            "service unavailable", response, response_capacity,
            response_len);
    }

    memset(&identity, 0, sizeof(identity));
    inventory_result = broker->config.discover_exact(
        broker->config.callback_context, discover.title_id, &identity,
        deadline_ms);
    if (inventory_result != VD_ATTACH_INVENTORY_FOUND) {
        return vd_write_discover_result(
            broker, session, &discover, vd_inventory_state(inventory_result),
            NULL, NULL,
            inventory_result == VD_ATTACH_INVENTORY_UNAVAILABLE
                ? "identity provider unavailable"
                : "",
            response, response_capacity, response_len);
    }
    if (!vd_identity_is_valid(&identity, discover.title_id)) {
        return vd_write_discover_result(
            broker, session, &discover, "error", NULL, NULL,
            "invalid identity snapshot", response, response_capacity,
            response_len);
    }

    memset(&session->ticket, 0, sizeof(session->ticket));
    result = vd_make_unique_ticket(broker, session->ticket.token);
    if (result < 0) {
        return vd_write_discover_result(
            broker, session, &discover, "error", NULL, NULL,
            "ticket entropy unavailable", response, response_capacity,
            response_len);
    }
    session->ticket.identity = identity;
    session->ticket.expires_at_ms = vd_deadline_after(
        broker->config.now_ms(broker->config.callback_context),
        broker->config.ticket_lease_ms);
    session->ticket.active = 1;
    return vd_write_discover_result(
        broker, session, &discover, "found", &session->ticket.identity,
        session->ticket.token, "", response, response_capacity,
        response_len);
}

static int vd_handle_release(VdAttachBroker *broker,
                             VdAttachBrokerSession *session,
                             const uint8_t *request,
                             size_t request_size,
                             uint8_t *response,
                             size_t response_capacity,
                             size_t *response_len,
                             uint64_t deadline_ms) {
    VdReleaseRequest release;
    VdAttachTargetIdentity expected;
    VdAttachTargetIdentity current;
    VdAttachInventoryResult inventory_result;
    int result;

    if (!session->hello_complete) {
        return VD_ATTACH_BROKER_ERROR_STATE;
    }
    result = vd_parse_release(request, request_size, &release);
    if (result < 0) {
        return result;
    }
    if (strcmp(release.server_nonce, session->server_nonce) != 0 ||
        release.service_generation != broker->service_generation) {
        return VD_ATTACH_BROKER_ERROR_STATE;
    }
    result = vd_request_id_claim(session, release.request_id);
    if (result < 0) {
        return result;
    }
    if (!session->ticket.active ||
        strcmp(session->ticket.token, release.ticket) != 0) {
        const char *state = vd_ticket_exists_elsewhere(
                                broker, session, release.ticket)
                                ? "changed"
                                : "missing";
        return vd_write_release_result(broker, session, &release, state, "",
                                       response, response_capacity,
                                       response_len);
    }

    /* Consume before revalidation: every owner-side release attempt is one-use. */
    expected = session->ticket.identity;
    memset(&session->ticket, 0, sizeof(session->ticket));
    if (broker->config.discover_exact == NULL) {
        return vd_write_release_result(
            broker, session, &release, "error",
            "identity provider unavailable", response, response_capacity,
            response_len);
    }
    memset(&current, 0, sizeof(current));
    inventory_result = broker->config.discover_exact(
        broker->config.callback_context, expected.title_id, &current,
        deadline_ms);
    if (inventory_result == VD_ATTACH_INVENTORY_FOUND) {
        if (!vd_identity_is_valid(&current, expected.title_id)) {
            return vd_write_release_result(
                broker, session, &release, "error",
                "invalid identity snapshot", response, response_capacity,
                response_len);
        }
        if (vd_identity_matches(&current, &expected)) {
            return vd_write_release_result(
                broker, session, &release, "released", "", response,
                response_capacity, response_len);
        }
        return vd_write_release_result(broker, session, &release, "changed",
                                       "", response, response_capacity,
                                       response_len);
    }
    if (inventory_result == VD_ATTACH_INVENTORY_ERROR ||
        inventory_result == VD_ATTACH_INVENTORY_UNAVAILABLE) {
        return vd_write_release_result(
            broker, session, &release, "error",
            "identity revalidation unavailable", response,
            response_capacity, response_len);
    }
    return vd_write_release_result(broker, session, &release, "changed", "",
                                   response, response_capacity, response_len);
}

int vd_attach_broker_init(VdAttachBroker *broker,
                          const VdAttachBrokerConfig *config) {
    const uint32_t discovery_caps =
        VD_ATTACH_CAP_EXACT_TITLE_DISCOVERY |
        VD_ATTACH_CAP_IDENTITY_TICKET |
        VD_ATTACH_CAP_TICKET_RELEASE;
    int result;

    if (broker == NULL || config == NULL || config->entropy == NULL ||
        config->now_ms == NULL ||
        (config->attach_caps & ~VD_ATTACH_READ_ONLY_CAPABILITIES) != 0u ||
        config->ticket_lease_ms < VD_ATTACH_BROKER_MIN_LEASE_MS ||
        config->ticket_lease_ms > VD_ATTACH_BROKER_MAX_LEASE_MS ||
        config->io_timeout_ms < VD_ATTACH_BROKER_MIN_IO_TIMEOUT_MS ||
        config->io_timeout_ms > VD_ATTACH_BROKER_MAX_IO_TIMEOUT_MS) {
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    if ((config->attach_caps & discovery_caps) != 0u &&
        (config->attach_caps & discovery_caps) != discovery_caps) {
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    if (config->service_ready &&
        ((config->attach_caps & VD_ATTACH_CAP_STATUS) == 0u ||
         ((config->attach_caps & discovery_caps) != 0u &&
          config->discover_exact == NULL))) {
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    memset(broker, 0, sizeof(*broker));
    broker->config = *config;
    result = vd_make_u64(broker, &broker->service_generation);
    if (result < 0) {
        memset(broker, 0, sizeof(*broker));
        return result;
    }
    broker->initialized = 1;
    return VD_ATTACH_BROKER_OK;
}

int vd_attach_broker_open_session(VdAttachBroker *broker,
                                  uint64_t peer_id,
                                  uint64_t *session_id) {
    size_t i;
    uint64_t candidate;
    int result;

    if (broker == NULL || session_id == NULL || peer_id == 0u ||
        !broker->initialized) {
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    if (broker->shutting_down) {
        return VD_ATTACH_BROKER_ERROR_SHUTDOWN;
    }
    vd_expire_tickets(
        broker, broker->config.now_ms(broker->config.callback_context));
    for (i = 0u; i < VD_ATTACH_BROKER_MAX_SESSIONS; ++i) {
        if (!broker->sessions[i].active) {
            unsigned int attempts;
            VdAttachBrokerSession *session = &broker->sessions[i];
            memset(session, 0, sizeof(*session));
            for (attempts = 0u; attempts < 4u; ++attempts) {
                size_t j;
                int duplicate = 0;
                result = vd_make_u64(broker, &candidate);
                if (result < 0) {
                    return result;
                }
                for (j = 0u; j < VD_ATTACH_BROKER_MAX_SESSIONS; ++j) {
                    if (broker->sessions[j].active &&
                        broker->sessions[j].session_id == candidate) {
                        duplicate = 1;
                        break;
                    }
                }
                if (!duplicate) {
                    break;
                }
            }
            if (attempts == 4u) {
                return VD_ATTACH_BROKER_ERROR_ENTROPY;
            }
            for (attempts = 0u; attempts < 4u; ++attempts) {
                result = vd_make_token(broker, session->server_nonce);
                if (result < 0) {
                    memset(session, 0, sizeof(*session));
                    return result;
                }
                if (!vd_server_nonce_in_use(broker, session,
                                            session->server_nonce)) {
                    break;
                }
            }
            if (attempts == 4u) {
                memset(session, 0, sizeof(*session));
                return VD_ATTACH_BROKER_ERROR_ENTROPY;
            }
            session->session_id = candidate;
            session->peer_id = peer_id;
            session->active = 1;
            *session_id = candidate;
            return VD_ATTACH_BROKER_OK;
        }
    }
    return VD_ATTACH_BROKER_ERROR_LIMIT;
}

void vd_attach_broker_close_session(VdAttachBroker *broker,
                                    uint64_t session_id,
                                    uint64_t peer_id) {
    int error;
    VdAttachBrokerSession *session;
    if (broker == NULL || !broker->initialized) {
        return;
    }
    session = vd_find_session(broker, session_id, peer_id, &error);
    if (session != NULL) {
        memset(session, 0, sizeof(*session));
    }
}

static int vd_handle_record_with_deadline(VdAttachBroker *broker,
                                          uint64_t session_id,
                                          uint64_t peer_id,
                                          const uint8_t *request,
                                          size_t request_size,
                                          uint8_t *response,
                                          size_t response_capacity,
                                          size_t *response_len,
                                          uint64_t deadline_ms) {
    VdAttachBrokerSession *session;
    int error;

    if (response_len != NULL) {
        *response_len = 0u;
    }
    if (broker == NULL || request == NULL || response == NULL ||
        response_len == NULL || response_capacity == 0u ||
        !broker->initialized) {
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    if (broker->shutting_down) {
        return VD_ATTACH_BROKER_ERROR_SHUTDOWN;
    }
    session = vd_find_session(broker, session_id, peer_id, &error);
    if (session == NULL) {
        return error;
    }
    vd_expire_tickets(
        broker, broker->config.now_ms(broker->config.callback_context));

    if (request_size > strlen(VD_ATTACH_HELLO_HEADER) &&
        memcmp(request, VD_ATTACH_HELLO_HEADER,
               strlen(VD_ATTACH_HELLO_HEADER)) == 0 &&
        request[strlen(VD_ATTACH_HELLO_HEADER)] == '\n') {
        return vd_handle_hello(broker, session, request, request_size,
                               response, response_capacity, response_len);
    }
    if (request_size > strlen(VD_ATTACH_DISCOVER_HEADER) &&
        memcmp(request, VD_ATTACH_DISCOVER_HEADER,
               strlen(VD_ATTACH_DISCOVER_HEADER)) == 0 &&
        request[strlen(VD_ATTACH_DISCOVER_HEADER)] == '\n') {
        return vd_handle_discover(broker, session, request, request_size,
                                  response, response_capacity, response_len,
                                  deadline_ms);
    }
    if (request_size > strlen(VD_ATTACH_RELEASE_HEADER) &&
        memcmp(request, VD_ATTACH_RELEASE_HEADER,
               strlen(VD_ATTACH_RELEASE_HEADER)) == 0 &&
        request[strlen(VD_ATTACH_RELEASE_HEADER)] == '\n') {
        return vd_handle_release(broker, session, request, request_size,
                                 response, response_capacity, response_len,
                                 deadline_ms);
    }
    return VD_ATTACH_BROKER_ERROR_PROTOCOL;
}

int vd_attach_broker_handle_record(VdAttachBroker *broker,
                                   uint64_t session_id,
                                   uint64_t peer_id,
                                   const uint8_t *request,
                                   size_t request_size,
                                   uint8_t *response,
                                   size_t response_capacity,
                                   size_t *response_len) {
    uint64_t deadline_ms;
    if (broker == NULL || !broker->initialized || broker->config.now_ms == NULL) {
        if (response_len != NULL) {
            *response_len = 0u;
        }
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    deadline_ms = vd_deadline_after(
        broker->config.now_ms(broker->config.callback_context),
        broker->config.io_timeout_ms);
    return vd_handle_record_with_deadline(
        broker, session_id, peer_id, request, request_size, response,
        response_capacity, response_len, deadline_ms);
}

static int vd_transport_read_exact(const VdAttachTransport *transport,
                                   void *output,
                                   size_t size,
                                   uint64_t deadline,
                                   int initial_prefix) {
    size_t offset = 0u;
    while (offset < size) {
        int count = transport->read(transport->context,
                                    (uint8_t *)output + offset,
                                    size - offset, deadline);
        if (count == 0) {
            return initial_prefix && offset == 0u
                       ? VD_ATTACH_BROKER_CLOSED
                       : VD_ATTACH_BROKER_ERROR_IO;
        }
        if (count < 0 || (size_t)count > size - offset) {
            return VD_ATTACH_BROKER_ERROR_IO;
        }
        offset += (size_t)count;
    }
    return VD_ATTACH_BROKER_OK;
}

static int vd_transport_write_exact(const VdAttachTransport *transport,
                                    const void *data,
                                    size_t size,
                                    uint64_t deadline) {
    size_t offset = 0u;
    while (offset < size) {
        int count = transport->write(transport->context,
                                     (const uint8_t *)data + offset,
                                     size - offset, deadline);
        if (count <= 0 || (size_t)count > size - offset) {
            return VD_ATTACH_BROKER_ERROR_IO;
        }
        offset += (size_t)count;
    }
    return VD_ATTACH_BROKER_OK;
}

int vd_attach_broker_serve(VdAttachBroker *broker,
                           const VdAttachTransport *transport) {
    uint8_t prefix[4];
    uint8_t request[VD_ATTACH_MAX_FRAME_SIZE];
    uint8_t response[VD_ATTACH_MAX_FRAME_SIZE];
    uint64_t session_id = 0u;
    int result;

    if (broker == NULL || transport == NULL || transport->read == NULL ||
        transport->write == NULL || transport->close == NULL ||
        transport->peer_id == 0u) {
        return VD_ATTACH_BROKER_ERROR_ARGUMENT;
    }
    result = vd_attach_broker_open_session(broker, transport->peer_id,
                                           &session_id);
    if (result < 0) {
        transport->close(transport->context);
        return result;
    }

    for (;;) {
        uint64_t deadline;
        uint32_t request_size;
        size_t response_size = 0u;

        if (broker->shutting_down) {
            result = VD_ATTACH_BROKER_ERROR_SHUTDOWN;
            break;
        }
        deadline = vd_deadline_after(
            broker->config.now_ms(broker->config.callback_context),
            broker->config.io_timeout_ms);
        result = vd_transport_read_exact(transport, prefix, sizeof(prefix),
                                         deadline, 1);
        if (result != VD_ATTACH_BROKER_OK) {
            break;
        }
        request_size = ((uint32_t)prefix[0] << 24) |
                       ((uint32_t)prefix[1] << 16) |
                       ((uint32_t)prefix[2] << 8) | (uint32_t)prefix[3];
        if (request_size == 0u || request_size > VD_ATTACH_MAX_FRAME_SIZE) {
            result = VD_ATTACH_BROKER_ERROR_FRAME;
            break;
        }
        result = vd_transport_read_exact(transport, request, request_size,
                                         deadline, 0);
        if (result != VD_ATTACH_BROKER_OK) {
            break;
        }
        result = vd_handle_record_with_deadline(
            broker, session_id, transport->peer_id, request, request_size,
            response, sizeof(response), &response_size, deadline);
        if (result != VD_ATTACH_BROKER_OK) {
            break;
        }
        prefix[0] = (uint8_t)(response_size >> 24);
        prefix[1] = (uint8_t)(response_size >> 16);
        prefix[2] = (uint8_t)(response_size >> 8);
        prefix[3] = (uint8_t)response_size;
        result = vd_transport_write_exact(transport, prefix, sizeof(prefix),
                                          deadline);
        if (result != VD_ATTACH_BROKER_OK) {
            break;
        }
        result = vd_transport_write_exact(transport, response, response_size,
                                          deadline);
        if (result != VD_ATTACH_BROKER_OK) {
            break;
        }
    }

    vd_attach_broker_close_session(broker, session_id, transport->peer_id);
    transport->close(transport->context);
    return result;
}

void vd_attach_broker_shutdown(VdAttachBroker *broker) {
    if (broker == NULL || !broker->initialized) {
        return;
    }
    broker->shutting_down = 1;
    memset(broker->sessions, 0, sizeof(broker->sessions));
}

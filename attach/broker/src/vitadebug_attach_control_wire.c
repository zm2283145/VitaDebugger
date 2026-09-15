#include "vitadebug_attach_control_wire.h"

#include <limits.h>
#include <string.h>

_Static_assert(sizeof(VD_ATTACH_CONTROL_PEER_AUTH_DOMAIN) - 1u ==
                   VD_ATTACH_CONTROL_PEER_AUTH_DOMAIN_BYTES,
               "peer signing domain length drifted");
_Static_assert(sizeof(VD_ATTACH_CONTROL_OPERATION_AUTH_DOMAIN) - 1u ==
                   VD_ATTACH_CONTROL_OPERATION_AUTH_DOMAIN_BYTES,
               "operation signing domain length drifted");
_Static_assert(VD_ATTACH_CONTROL_PEER_SIGNED_BYTES ==
                   VD_ATTACH_CONTROL_PEER_AUTH_DOMAIN_BYTES + 4u +
                       7u * 8u + 2u * VD_ATTACH_CONTROL_NONCE_BYTES,
               "peer signing transcript length drifted");
_Static_assert(VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES ==
                   VD_ATTACH_CONTROL_OPERATION_AUTH_DOMAIN_BYTES +
                       8u * 4u + 9u * 8u +
                       VD_ATTACH_TITLE_ID_LENGTH +
                       3u * VD_ATTACH_CONTROL_NONCE_BYTES,
               "operation signing transcript length drifted");

typedef struct VdAttachControlWireWriter {
    uint8_t *output;
    size_t offset;
} VdAttachControlWireWriter;

static int vd_control_wire_bytes_nonzero(const uint8_t *bytes, size_t size) {
    size_t i;
    uint8_t combined = 0u;
    if (bytes == NULL) {
        return 0;
    }
    for (i = 0u; i < size; ++i) {
        combined |= bytes[i];
    }
    return combined != 0u;
}

static int vd_control_wire_title_valid(
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

static int vd_control_wire_target_valid(
    const VdAttachTargetIdentity *target) {
    return target != NULL && vd_control_wire_title_valid(target->title_id) &&
           target->pid > 0u && target->pid <= (uint32_t)INT32_MAX &&
           target->main_modid > 0u &&
           target->main_modid <= (uint32_t)INT32_MAX &&
           target->main_fingerprint != 0u &&
           target->target_generation != 0u;
}

static void vd_control_wire_put_bytes(VdAttachControlWireWriter *writer,
                                      const void *input,
                                      size_t size) {
    memcpy(writer->output + writer->offset, input, size);
    writer->offset += size;
}

static void vd_control_wire_put_u32(VdAttachControlWireWriter *writer,
                                    uint32_t value) {
    writer->output[writer->offset++] = (uint8_t)(value >> 24);
    writer->output[writer->offset++] = (uint8_t)(value >> 16);
    writer->output[writer->offset++] = (uint8_t)(value >> 8);
    writer->output[writer->offset++] = (uint8_t)value;
}

static void vd_control_wire_put_u64(VdAttachControlWireWriter *writer,
                                    uint64_t value) {
    writer->output[writer->offset++] = (uint8_t)(value >> 56);
    writer->output[writer->offset++] = (uint8_t)(value >> 48);
    writer->output[writer->offset++] = (uint8_t)(value >> 40);
    writer->output[writer->offset++] = (uint8_t)(value >> 32);
    writer->output[writer->offset++] = (uint8_t)(value >> 24);
    writer->output[writer->offset++] = (uint8_t)(value >> 16);
    writer->output[writer->offset++] = (uint8_t)(value >> 8);
    writer->output[writer->offset++] = (uint8_t)value;
}

static int vd_control_wire_peer_valid(
    const VdAttachControlPeerTranscript *transcript) {
    return transcript != NULL &&
           transcript->version == VD_ATTACH_CONTROL_VERSION &&
           transcript->service_generation != 0u &&
           transcript->session_id != 0u &&
           transcript->transport_binding != 0u &&
           transcript->host_key_id != 0u &&
           transcript->server_time_ms <
               transcript->challenge_expires_at_ms &&
           transcript->server_time_ms < transcript->expires_at_ms &&
           vd_control_wire_bytes_nonzero(
               transcript->server_nonce, sizeof(transcript->server_nonce)) &&
           vd_control_wire_bytes_nonzero(
               transcript->client_nonce, sizeof(transcript->client_nonce));
}

static int vd_control_wire_authorization_valid(
    const VdAttachControlAuthorization *authorization) {
    const int is_attach = authorization != NULL &&
        authorization->operation == VD_ATTACH_CONTROL_OPERATION_ATTACH;
    const int is_cleanup = authorization != NULL &&
        (authorization->operation == VD_ATTACH_CONTROL_OPERATION_DETACH ||
         authorization->operation == VD_ATTACH_CONTROL_OPERATION_RECOVER);

    if (authorization == NULL ||
        authorization->version != VD_ATTACH_CONTROL_VERSION ||
        (!is_attach && !is_cleanup) ||
        authorization->fixed_module_slot !=
            VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT ||
        authorization->service_generation == 0u ||
        authorization->session_id == 0u ||
        authorization->transport_binding == 0u ||
        authorization->host_key_id == 0u ||
        authorization->expires_at_ms == 0u ||
        authorization->session_expires_at_ms == 0u ||
        authorization->expires_at_ms >
            authorization->session_expires_at_ms ||
        !vd_control_wire_target_valid(&authorization->target) ||
        !vd_control_wire_bytes_nonzero(
            authorization->server_nonce,
            sizeof(authorization->server_nonce)) ||
        !vd_control_wire_bytes_nonzero(
            authorization->client_nonce,
            sizeof(authorization->client_nonce)) ||
        !vd_control_wire_bytes_nonzero(
            authorization->request_nonce,
            sizeof(authorization->request_nonce))) {
        return 0;
    }
    if (is_attach) {
        return authorization->requested_lease_ms >=
                   VD_ATTACH_CONTROL_MIN_LEASE_MS &&
               authorization->requested_lease_ms <=
                   VD_ATTACH_CONTROL_MAX_LEASE_MS &&
               authorization->lease_id == 0u &&
               authorization->lease_expires_at_ms == 0u &&
               authorization->injected_module_uid == 0u;
    }
    return authorization->requested_lease_ms == 0u &&
           authorization->lease_id != 0u &&
           authorization->lease_expires_at_ms != 0u &&
           authorization->injected_module_uid > 0u &&
           authorization->injected_module_uid <= (uint32_t)INT32_MAX;
}

int vd_attach_control_encode_peer_transcript(
    const VdAttachControlPeerTranscript *transcript,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size) {
    VdAttachControlWireWriter writer;

    if (output_size != NULL) {
        *output_size = 0u;
    }
    if (output == NULL || output_size == NULL ||
        output_capacity < VD_ATTACH_CONTROL_PEER_SIGNED_BYTES ||
        !vd_control_wire_peer_valid(transcript)) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    writer.output = output;
    writer.offset = 0u;
    vd_control_wire_put_bytes(
        &writer, VD_ATTACH_CONTROL_PEER_AUTH_DOMAIN,
        VD_ATTACH_CONTROL_PEER_AUTH_DOMAIN_BYTES);
    vd_control_wire_put_u32(&writer, transcript->version);
    vd_control_wire_put_u64(&writer, transcript->service_generation);
    vd_control_wire_put_u64(&writer, transcript->session_id);
    vd_control_wire_put_u64(&writer, transcript->transport_binding);
    vd_control_wire_put_u64(&writer, transcript->host_key_id);
    vd_control_wire_put_u64(&writer, transcript->server_time_ms);
    vd_control_wire_put_u64(&writer, transcript->challenge_expires_at_ms);
    vd_control_wire_put_u64(&writer, transcript->expires_at_ms);
    vd_control_wire_put_bytes(&writer, transcript->server_nonce,
                              sizeof(transcript->server_nonce));
    vd_control_wire_put_bytes(&writer, transcript->client_nonce,
                              sizeof(transcript->client_nonce));
    if (writer.offset != VD_ATTACH_CONTROL_PEER_SIGNED_BYTES) {
        memset(output, 0, output_capacity);
        return VD_ATTACH_CONTROL_ERROR_STATE;
    }
    *output_size = writer.offset;
    return VD_ATTACH_CONTROL_OK;
}

int vd_attach_control_encode_operation_authorization(
    const VdAttachControlAuthorization *authorization,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size) {
    VdAttachControlWireWriter writer;

    if (output_size != NULL) {
        *output_size = 0u;
    }
    if (output == NULL || output_size == NULL ||
        output_capacity < VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES ||
        !vd_control_wire_authorization_valid(authorization)) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    writer.output = output;
    writer.offset = 0u;
    vd_control_wire_put_bytes(
        &writer, VD_ATTACH_CONTROL_OPERATION_AUTH_DOMAIN,
        VD_ATTACH_CONTROL_OPERATION_AUTH_DOMAIN_BYTES);
    vd_control_wire_put_u32(&writer, authorization->version);
    vd_control_wire_put_u32(&writer, authorization->operation);
    vd_control_wire_put_u32(&writer, authorization->fixed_module_slot);
    vd_control_wire_put_u64(&writer, authorization->service_generation);
    vd_control_wire_put_u64(&writer, authorization->session_id);
    vd_control_wire_put_u64(&writer, authorization->transport_binding);
    vd_control_wire_put_u64(&writer, authorization->host_key_id);
    vd_control_wire_put_u64(&writer, authorization->expires_at_ms);
    vd_control_wire_put_u64(&writer,
                            authorization->session_expires_at_ms);
    vd_control_wire_put_u32(&writer, authorization->requested_lease_ms);
    vd_control_wire_put_u64(&writer, authorization->lease_id);
    vd_control_wire_put_u64(&writer, authorization->lease_expires_at_ms);
    vd_control_wire_put_u32(&writer, authorization->injected_module_uid);
    vd_control_wire_put_bytes(&writer, authorization->target.title_id,
                              VD_ATTACH_TITLE_ID_LENGTH);
    vd_control_wire_put_u32(&writer, authorization->target.pid);
    vd_control_wire_put_u32(&writer, authorization->target.main_modid);
    vd_control_wire_put_u32(&writer,
                            authorization->target.main_fingerprint);
    vd_control_wire_put_u64(&writer,
                            authorization->target.target_generation);
    vd_control_wire_put_bytes(&writer, authorization->server_nonce,
                              sizeof(authorization->server_nonce));
    vd_control_wire_put_bytes(&writer, authorization->client_nonce,
                              sizeof(authorization->client_nonce));
    vd_control_wire_put_bytes(&writer, authorization->request_nonce,
                              sizeof(authorization->request_nonce));
    if (writer.offset != VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES) {
        memset(output, 0, output_capacity);
        return VD_ATTACH_CONTROL_ERROR_STATE;
    }
    *output_size = writer.offset;
    return VD_ATTACH_CONTROL_OK;
}

#include "vitadebug_attach_auth_listener.h"

#include <limits.h>
#include <string.h>

#include <monocypher.h>

#define VD_AUTH_SOCKET_INVALID (-1)
#define VD_AUTH_SOCKET_CLOSING (-2)
#define VD_AUTH_HEADER_SIZE 8u
#define VD_AUTH_FRAME_PREFIX_SIZE 4u
#define VD_AUTH_TYPE_HELLO 1u
#define VD_AUTH_TYPE_CHALLENGE 2u
#define VD_AUTH_TYPE_PROOF 3u
#define VD_AUTH_TYPE_RESULT 4u
#define VD_AUTH_STATUS_OK 0u
#define VD_AUTH_STATUS_DENIED 1u

int vd_attach_auth_network_config_validate(
    const VdAttachAuthNetworkConfig *config) {
    if (config == NULL) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    if (config->mode == VD_ATTACH_AUTH_NETWORK_STANDALONE_OWNED) {
        return config->network_memory != NULL &&
                       config->network_memory_size >=
                           VD_ATTACH_AUTH_NETWORK_MEMORY_MIN &&
                       config->owns_network_module == 1 &&
                       config->owns_network_initialization == 1
                   ? VD_ATTACH_AUTH_LISTENER_OK
                   : VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    if (config->mode == VD_ATTACH_AUTH_NETWORK_SHELL_BORROWED) {
        return config->network_memory == NULL &&
                       config->network_memory_size == 0u &&
                       config->owns_network_module == 0 &&
                       config->owns_network_initialization == 0
                   ? VD_ATTACH_AUTH_LISTENER_OK
                   : VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
}

static uint16_t vd_auth_get_u16(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t vd_auth_get_u32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static uint64_t vd_auth_get_u64(const uint8_t *data) {
    return ((uint64_t)vd_auth_get_u32(data) << 32) |
           vd_auth_get_u32(data + 4u);
}

static void vd_auth_put_u16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void vd_auth_put_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static void vd_auth_put_u64(uint8_t *data, uint64_t value) {
    vd_auth_put_u32(data, (uint32_t)(value >> 32));
    vd_auth_put_u32(data + 4u, (uint32_t)value);
}

static int vd_auth_nonzero(const uint8_t *data, size_t size) {
    size_t index;
    uint8_t combined = 0u;
    for (index = 0u; index < size; ++index) {
        combined |= data[index];
    }
    return combined != 0u;
}

static int vd_auth_private_ipv4(uint32_t address) {
    return (address & UINT32_C(0xff000000)) == UINT32_C(0x0a000000) ||
           (address & UINT32_C(0xfff00000)) == UINT32_C(0xac100000) ||
           (address & UINT32_C(0xffff0000)) == UINT32_C(0xc0a80000);
}

static int vd_auth_mask_contiguous(uint32_t mask) {
    uint32_t inverse;
    if (mask == 0u || mask == UINT32_MAX) {
        return 0;
    }
    inverse = ~mask;
    return (inverse & (inverse + 1u)) == 0u;
}

static int vd_auth_peer_allowed(const VdAttachAuthListener *listener,
                                uint32_t peer_ipv4) {
    uint32_t host_mask = ~listener->config.peer_netmask;
    uint32_t host = peer_ipv4 & host_mask;
    return vd_auth_private_ipv4(peer_ipv4) &&
           (peer_ipv4 & listener->config.peer_netmask) ==
               listener->config.peer_network &&
           host != 0u && host != host_mask;
}

static void vd_auth_increment(uint32_t *value) {
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

static int vd_auth_ops_valid(const VdAttachAuthSocketOps *ops) {
    return ops != NULL && ops->now_ms != NULL && ops->entropy != NULL &&
           ops->listen_open != NULL && ops->accept != NULL &&
           ops->read != NULL && ops->write != NULL &&
           ops->shutdown != NULL && ops->close != NULL;
}

static int vd_auth_close_registered(VdAttachAuthListener *listener,
                                    int *slot) {
    int socket;
    for (;;) {
        int expected;
        socket = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
        if (socket == VD_AUTH_SOCKET_INVALID) {
            return VD_ATTACH_AUTH_LISTENER_OK;
        }
        if (socket == VD_AUTH_SOCKET_CLOSING) {
            return VD_ATTACH_AUTH_LISTENER_ERROR_STATE;
        }
        expected = socket;
        if (__atomic_compare_exchange_n(
                slot, &expected, VD_AUTH_SOCKET_CLOSING, 0,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            int close_result;
            (void)listener->config.socket_ops.shutdown(
                listener->config.socket_ops.context, socket);
            close_result = listener->config.socket_ops.close(
                listener->config.socket_ops.context, socket);
            if (close_result < 0) {
                vd_auth_increment(&listener->stats.close_failures);
                __atomic_store_n(slot, socket, __ATOMIC_RELEASE);
                return VD_ATTACH_AUTH_LISTENER_ERROR_IO;
            }
            __atomic_store_n(slot, VD_AUTH_SOCKET_INVALID,
                             __ATOMIC_RELEASE);
            return VD_ATTACH_AUTH_LISTENER_OK;
        }
    }
}

static int vd_auth_exact_read(VdAttachAuthListener *listener,
                              int socket,
                              uint8_t *output,
                              size_t size,
                              uint64_t deadline_ms) {
    size_t offset = 0u;
    while (offset < size) {
        int result;
        if (__atomic_load_n(&listener->state, __ATOMIC_ACQUIRE) ==
            VD_ATTACH_AUTH_LISTENER_STOPPING) {
            return VD_ATTACH_AUTH_LISTENER_ERROR_SHUTDOWN;
        }
        result = listener->config.socket_ops.read(
            listener->config.socket_ops.context, socket, output + offset,
            size - offset, deadline_ms);
        if (result == 0) {
            return VD_ATTACH_AUTH_LISTENER_ERROR_IO;
        }
        if (result <= 0 || (size_t)result > size - offset) {
            return result == VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT
                       ? VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT
                       : VD_ATTACH_AUTH_LISTENER_ERROR_IO;
        }
        offset += (size_t)result;
    }
    return VD_ATTACH_AUTH_LISTENER_OK;
}

static int vd_auth_exact_write(VdAttachAuthListener *listener,
                               int socket,
                               const uint8_t *data,
                               size_t size,
                               uint64_t deadline_ms) {
    size_t offset = 0u;
    while (offset < size) {
        int result;
        if (__atomic_load_n(&listener->state, __ATOMIC_ACQUIRE) ==
            VD_ATTACH_AUTH_LISTENER_STOPPING) {
            return VD_ATTACH_AUTH_LISTENER_ERROR_SHUTDOWN;
        }
        result = listener->config.socket_ops.write(
            listener->config.socket_ops.context, socket, data + offset,
            size - offset, deadline_ms);
        if (result <= 0 || (size_t)result > size - offset) {
            return result == VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT
                       ? VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT
                       : VD_ATTACH_AUTH_LISTENER_ERROR_IO;
        }
        offset += (size_t)result;
    }
    return VD_ATTACH_AUTH_LISTENER_OK;
}

static int vd_auth_receive_frame(VdAttachAuthListener *listener,
                                 int socket,
                                 uint8_t *payload,
                                 size_t capacity,
                                 size_t expected_size,
                                 uint64_t deadline_ms) {
    uint8_t prefix[VD_AUTH_FRAME_PREFIX_SIZE];
    uint32_t payload_size;
    int result = vd_auth_exact_read(listener, socket, prefix,
                                    sizeof(prefix), deadline_ms);
    if (result != VD_ATTACH_AUTH_LISTENER_OK) {
        return result;
    }
    payload_size = vd_auth_get_u32(prefix);
    if (payload_size == 0u ||
        payload_size > VD_ATTACH_AUTH_FRAME_MAX_BYTES ||
        payload_size > capacity || payload_size != expected_size) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_PROTOCOL;
    }
    return vd_auth_exact_read(listener, socket, payload, payload_size,
                              deadline_ms);
}

static int vd_auth_send_frame(VdAttachAuthListener *listener,
                              int socket,
                              const uint8_t *payload,
                              size_t payload_size,
                              uint64_t deadline_ms) {
    uint8_t prefix[VD_AUTH_FRAME_PREFIX_SIZE];
    int result;
    if (payload == NULL || payload_size == 0u ||
        payload_size > VD_ATTACH_AUTH_FRAME_MAX_BYTES) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    vd_auth_put_u32(prefix, (uint32_t)payload_size);
    result = vd_auth_exact_write(listener, socket, prefix, sizeof(prefix),
                                 deadline_ms);
    if (result == VD_ATTACH_AUTH_LISTENER_OK) {
        result = vd_auth_exact_write(listener, socket, payload,
                                     payload_size, deadline_ms);
    }
    return result;
}

static int vd_auth_header(uint8_t *payload,
                          size_t payload_size,
                          uint8_t record_type,
                          uint16_t body_size) {
    if (payload_size != (size_t)body_size + VD_AUTH_HEADER_SIZE) {
        return 0;
    }
    payload[0] = 'V';
    payload[1] = 'D';
    payload[2] = 'A';
    payload[3] = '2';
    payload[4] = VD_ATTACH_AUTH_WIRE_VERSION;
    payload[5] = record_type;
    vd_auth_put_u16(payload + 6u, body_size);
    return 1;
}

static int vd_auth_header_matches(const uint8_t *payload,
                                  size_t payload_size,
                                  uint8_t record_type) {
    return payload_size >= VD_AUTH_HEADER_SIZE && payload[0] == 'V' &&
           payload[1] == 'D' && payload[2] == 'A' &&
           payload[3] == '2' &&
           payload[4] == VD_ATTACH_AUTH_WIRE_VERSION &&
           payload[5] == record_type &&
           vd_auth_get_u16(payload + 6u) ==
               payload_size - VD_AUTH_HEADER_SIZE;
}

static int vd_auth_find_failure(VdAttachAuthListener *listener,
                                uint32_t peer_ipv4,
                                uint64_t host_key_id,
                                int create) {
    uint32_t index;
    int empty = -1;
    for (index = 0u; index < VD_ATTACH_AUTH_LISTENER_MAX_FAILURES;
         ++index) {
        VdAttachAuthFailureSlot *slot = &listener->failures[index];
        if (slot->active && slot->peer_ipv4 == peer_ipv4 &&
            slot->host_key_id == host_key_id) {
            return (int)index;
        }
        if (!slot->active && empty < 0) {
            empty = (int)index;
        }
    }
    if (!create || empty < 0) {
        return -1;
    }
    memset(&listener->failures[empty], 0,
           sizeof(listener->failures[empty]));
    listener->failures[empty].active = 1;
    listener->failures[empty].peer_ipv4 = peer_ipv4;
    listener->failures[empty].host_key_id = host_key_id;
    return empty;
}

static uint32_t vd_auth_record_failure(VdAttachAuthListener *listener,
                                       uint32_t peer_ipv4,
                                       uint64_t host_key_id,
                                       uint64_t now_ms) {
    int index = vd_auth_find_failure(listener, peer_ipv4, host_key_id, 1);
    uint32_t shift;
    uint32_t delay;
    VdAttachAuthFailureSlot *slot;
    if (index < 0) {
        listener->failure_table_exhausted = 1;
        return VD_ATTACH_AUTH_LISTENER_MAX_BACKOFF_MS;
    }
    slot = &listener->failures[index];
    if (slot->failures != UINT32_MAX) {
        ++slot->failures;
    }
    shift = slot->failures > 6u ? 5u : slot->failures - 1u;
    delay = VD_ATTACH_AUTH_LISTENER_MIN_BACKOFF_MS << shift;
    if (delay > VD_ATTACH_AUTH_LISTENER_MAX_BACKOFF_MS) {
        delay = VD_ATTACH_AUTH_LISTENER_MAX_BACKOFF_MS;
    }
    slot->blocked_until_ms =
        UINT64_MAX - now_ms < delay ? UINT64_MAX : now_ms + delay;
    return delay;
}

static int vd_auth_rate_limited(VdAttachAuthListener *listener,
                                uint32_t peer_ipv4,
                                uint64_t host_key_id,
                                uint64_t now_ms) {
    int index;
    if (listener->failure_table_exhausted) {
        return 1;
    }
    index = vd_auth_find_failure(listener, peer_ipv4, host_key_id, 0);
    return index >= 0 &&
           now_ms < listener->failures[index].blocked_until_ms;
}

static void vd_auth_clear_failure(VdAttachAuthListener *listener,
                                  uint32_t peer_ipv4,
                                  uint64_t host_key_id) {
    int index = vd_auth_find_failure(listener, peer_ipv4, host_key_id, 0);
    if (index >= 0) {
        memset(&listener->failures[index], 0,
               sizeof(listener->failures[index]));
    }
}

static int vd_auth_replay_seen(const VdAttachAuthListener *listener,
                               const uint8_t nonce[32]) {
    uint32_t index;
    for (index = 0u; index < listener->replay_count; ++index) {
        if (vd_attach_auth_constant_time_equal(
                listener->replay_nonces[index], nonce, 32u)) {
            return 1;
        }
    }
    return 0;
}

static int vd_auth_make_transcript(const char *domain,
                                   const uint8_t *body,
                                   size_t body_size,
                                   uint8_t *output,
                                   size_t capacity,
                                   size_t *output_size) {
    size_t domain_size;
    if (domain == NULL || body == NULL || output == NULL ||
        output_size == NULL) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    domain_size = strlen(domain);
    if (domain_size > capacity || body_size > capacity - domain_size) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_RESOURCE;
    }
    memcpy(output, domain, domain_size);
    memcpy(output + domain_size, body, body_size);
    *output_size = domain_size + body_size;
    return VD_ATTACH_AUTH_LISTENER_OK;
}

static int vd_auth_make_challenge(
    VdAttachAuthListener *listener,
    const uint8_t hello[VD_ATTACH_AUTH_HELLO_BYTES],
    uint64_t transport_binding,
    uint64_t now_ms,
    uint64_t deadline_ms,
    VdAttachAuthPublicKey *local,
    uint64_t *session_id,
    uint8_t challenge[VD_ATTACH_AUTH_CHALLENGE_BYTES]) {
    uint8_t transcript[VD_ATTACH_AUTH_MAX_TRANSCRIPT_BYTES];
    size_t transcript_size = 0u;
    uint64_t expires_at_ms;
    int result;

    result = vd_attach_auth_load_local(listener->config.key_storage, local);
    if (result != VD_ATTACH_AUTH_OK) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_STORAGE;
    }
    if (listener->next_session_id == 0u) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_RESOURCE;
    }
    *session_id = listener->next_session_id++;
    expires_at_ms = deadline_ms;
    memset(challenge, 0, VD_ATTACH_AUTH_CHALLENGE_BYTES);
    (void)vd_auth_header(challenge, VD_ATTACH_AUTH_CHALLENGE_BYTES,
                         VD_AUTH_TYPE_CHALLENGE,
                         VD_ATTACH_AUTH_CHALLENGE_BYTES -
                             VD_AUTH_HEADER_SIZE);
    memcpy(challenge + 8u, hello + 8u, 16u);
    vd_auth_put_u64(challenge + 24u, local->key_id);
    vd_auth_put_u64(challenge + 32u, local->generation);
    vd_auth_put_u64(challenge + 40u, listener->service_generation);
    vd_auth_put_u64(challenge + 48u, *session_id);
    vd_auth_put_u64(challenge + 56u, transport_binding);
    vd_auth_put_u64(challenge + 64u, now_ms);
    vd_auth_put_u64(challenge + 72u, expires_at_ms);
    memcpy(challenge + 80u, hello + 24u, 64u);
    result = listener->config.socket_ops.entropy(
        listener->config.socket_ops.context, challenge + 144u, 32u);
    if (result < 0 || !vd_auth_nonzero(challenge + 144u, 32u)) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ENTROPY;
    }
    result = vd_auth_make_transcript(
        VD_ATTACH_AUTH_SERVER_CHALLENGE_DOMAIN, challenge + 8u, 168u,
        transcript, sizeof(transcript), &transcript_size);
    if (result == VD_ATTACH_AUTH_LISTENER_OK) {
        result = vd_attach_auth_sign_local(
            listener->config.key_storage, transcript, transcript_size,
            challenge + 176u, deadline_ms);
    }
    crypto_wipe(transcript, sizeof(transcript));
    return result == VD_ATTACH_AUTH_OK
               ? VD_ATTACH_AUTH_LISTENER_OK
               : VD_ATTACH_AUTH_LISTENER_ERROR_STORAGE;
}

static int vd_auth_proof_canonical(const uint8_t *proof) {
    uint64_t server_time = vd_auth_get_u64(proof + 64u);
    uint64_t challenge_expiry = vd_auth_get_u64(proof + 72u);
    uint64_t proof_expiry = vd_auth_get_u64(proof + 80u);
    return vd_auth_header_matches(proof, VD_ATTACH_AUTH_PROOF_BYTES,
                                  VD_AUTH_TYPE_PROOF) &&
           server_time < challenge_expiry &&
           server_time < proof_expiry &&
           proof_expiry <= challenge_expiry &&
           vd_auth_nonzero(proof + 88u, 32u) &&
           vd_auth_nonzero(proof + 120u, 32u) &&
           vd_auth_nonzero(proof + 152u, 32u) &&
           vd_auth_nonzero(proof + 184u, 64u);
}

static int vd_auth_proof_matches(const uint8_t *challenge,
                                 const uint8_t *proof,
                                 uint64_t now_ms) {
    return vd_attach_auth_constant_time_equal(
               challenge + 8u, proof + 8u, 72u) &&
           vd_attach_auth_constant_time_equal(
               challenge + 80u, proof + 88u, 96u) &&
           now_ms < vd_auth_get_u64(challenge + 72u) &&
           now_ms < vd_auth_get_u64(proof + 80u);
}

static int vd_auth_verify_proof(
    VdAttachAuthListener *listener,
    const uint8_t proof[VD_ATTACH_AUTH_PROOF_BYTES],
    uint64_t deadline_ms) {
    VdAttachAuthPublicKey peer;
    uint8_t transcript[VD_ATTACH_AUTH_MAX_TRANSCRIPT_BYTES];
    size_t transcript_size = 0u;
    int result = vd_attach_auth_lookup_peer(
        listener->config.key_storage, vd_auth_get_u64(proof + 8u),
        vd_auth_get_u64(proof + 16u), &peer);
    if (result != VD_ATTACH_AUTH_OK) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_PEER;
    }
    result = vd_auth_make_transcript(
        VD_ATTACH_AUTH_CLIENT_PROOF_DOMAIN, proof + 8u, 176u,
        transcript, sizeof(transcript), &transcript_size);
    if (result == VD_ATTACH_AUTH_LISTENER_OK &&
        listener->config.socket_ops.now_ms(
            listener->config.socket_ops.context) < deadline_ms) {
        result = vd_attach_auth_verify_ed25519(
            &peer, transcript, transcript_size, proof + 184u);
    } else if (result == VD_ATTACH_AUTH_LISTENER_OK) {
        result = VD_ATTACH_AUTH_ERROR_SIGNATURE;
    }
    crypto_wipe(&peer, sizeof(peer));
    crypto_wipe(transcript, sizeof(transcript));
    return result == VD_ATTACH_AUTH_OK
               ? VD_ATTACH_AUTH_LISTENER_OK
               : VD_ATTACH_AUTH_LISTENER_ERROR_PEER;
}

static int vd_auth_make_result(
    VdAttachAuthListener *listener,
    const uint8_t proof[VD_ATTACH_AUTH_PROOF_BYTES],
    uint32_t status,
    uint32_t retry_after_ms,
    uint64_t deadline_ms,
    uint8_t result_record[VD_ATTACH_AUTH_RESULT_BYTES]) {
    uint8_t transcript[VD_ATTACH_AUTH_MAX_TRANSCRIPT_BYTES];
    size_t transcript_size = 0u;
    int result;

    memset(result_record, 0, VD_ATTACH_AUTH_RESULT_BYTES);
    (void)vd_auth_header(result_record, VD_ATTACH_AUTH_RESULT_BYTES,
                         VD_AUTH_TYPE_RESULT,
                         VD_ATTACH_AUTH_RESULT_BYTES -
                             VD_AUTH_HEADER_SIZE);
    memcpy(result_record + 8u, proof + 8u, 80u);
    vd_auth_put_u32(result_record + 88u, status);
    vd_auth_put_u32(result_record + 92u, retry_after_ms);
    memcpy(result_record + 96u, proof + 88u, 96u);
    if (status == VD_AUTH_STATUS_OK) {
        result = listener->config.socket_ops.entropy(
            listener->config.socket_ops.context, result_record + 192u,
            32u);
        if (result < 0 ||
            !vd_auth_nonzero(result_record + 192u, 32u)) {
            return VD_ATTACH_AUTH_LISTENER_ERROR_ENTROPY;
        }
    }
    result = vd_auth_make_transcript(
        VD_ATTACH_AUTH_SESSION_RESULT_DOMAIN, result_record + 8u, 216u,
        transcript, sizeof(transcript), &transcript_size);
    if (result == VD_ATTACH_AUTH_LISTENER_OK) {
        result = vd_attach_auth_sign_local(
            listener->config.key_storage, transcript, transcript_size,
            result_record + 224u, deadline_ms);
    }
    crypto_wipe(transcript, sizeof(transcript));
    return result == VD_ATTACH_AUTH_OK
               ? VD_ATTACH_AUTH_LISTENER_OK
               : VD_ATTACH_AUTH_LISTENER_ERROR_STORAGE;
}

static int vd_auth_serve(VdAttachAuthListener *listener,
                         int socket,
                         uint32_t peer_ipv4,
                         uint64_t transport_binding) {
    uint8_t hello[VD_ATTACH_AUTH_HELLO_BYTES];
    uint8_t challenge[VD_ATTACH_AUTH_CHALLENGE_BYTES];
    uint8_t proof[VD_ATTACH_AUTH_PROOF_BYTES];
    uint8_t result_record[VD_ATTACH_AUTH_RESULT_BYTES];
    VdAttachAuthPublicKey peer;
    VdAttachAuthPublicKey local;
    uint64_t start_ms;
    uint64_t deadline_ms;
    uint64_t host_key_id;
    uint64_t host_generation;
    uint64_t session_id = 0u;
    uint32_t retry_after_ms;
    int result;

    memset(&peer, 0, sizeof(peer));
    memset(&local, 0, sizeof(local));
    start_ms = listener->config.socket_ops.now_ms(
        listener->config.socket_ops.context);
    if (UINT64_MAX - start_ms <
        listener->config.handshake_deadline_ms) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_STATE;
    }
    deadline_ms =
        start_ms + listener->config.handshake_deadline_ms;
    result = vd_auth_receive_frame(
        listener, socket, hello, sizeof(hello),
        VD_ATTACH_AUTH_HELLO_BYTES, deadline_ms);
    if (result != VD_ATTACH_AUTH_LISTENER_OK) {
        goto cleanup;
    }
    if (!vd_auth_header_matches(hello, sizeof(hello),
                                VD_AUTH_TYPE_HELLO) ||
        !vd_auth_nonzero(hello + 24u, 32u) ||
        !vd_auth_nonzero(hello + 56u, 32u)) {
        result = VD_ATTACH_AUTH_LISTENER_ERROR_PROTOCOL;
        goto cleanup;
    }
    host_key_id = vd_auth_get_u64(hello + 8u);
    host_generation = vd_auth_get_u64(hello + 16u);
    if (host_key_id == 0u || host_generation == 0u) {
        result = VD_ATTACH_AUTH_LISTENER_ERROR_PROTOCOL;
        goto cleanup;
    }
    if (vd_auth_rate_limited(listener, peer_ipv4, host_key_id,
                             start_ms)) {
        vd_auth_increment(&listener->stats.rate_limited);
        result = VD_ATTACH_AUTH_LISTENER_ERROR_RATE_LIMIT;
        goto cleanup;
    }
    if (vd_auth_replay_seen(listener, hello + 24u)) {
        (void)vd_auth_record_failure(listener, peer_ipv4, host_key_id,
                                     start_ms);
        vd_auth_increment(&listener->stats.replay_rejected);
        result = VD_ATTACH_AUTH_LISTENER_ERROR_REPLAY;
        goto cleanup;
    }
    if (listener->replay_count >=
        VD_ATTACH_AUTH_LISTENER_MAX_REPLAYS) {
        result = VD_ATTACH_AUTH_LISTENER_ERROR_REPLAY;
        goto cleanup;
    }
    result = vd_attach_auth_lookup_peer(
        listener->config.key_storage, host_key_id, host_generation,
        &peer);
    if (result != VD_ATTACH_AUTH_OK) {
        (void)vd_auth_record_failure(listener, peer_ipv4, host_key_id,
                                     start_ms);
        result = VD_ATTACH_AUTH_LISTENER_ERROR_PEER;
        goto cleanup;
    }
    result = vd_auth_make_challenge(
        listener, hello, transport_binding, start_ms, deadline_ms,
        &local, &session_id, challenge);
    if (result != VD_ATTACH_AUTH_LISTENER_OK) {
        goto cleanup;
    }
    memcpy(listener->replay_nonces[listener->replay_count],
           hello + 24u, 32u);
    ++listener->replay_count;
    result = vd_auth_send_frame(listener, socket, challenge,
                                sizeof(challenge), deadline_ms);
    if (result != VD_ATTACH_AUTH_LISTENER_OK) {
        goto cleanup;
    }
    result = vd_auth_receive_frame(
        listener, socket, proof, sizeof(proof),
        VD_ATTACH_AUTH_PROOF_BYTES, deadline_ms);
    if (result != VD_ATTACH_AUTH_LISTENER_OK) {
        goto cleanup;
    }
    if (!vd_auth_proof_canonical(proof) ||
        !vd_auth_proof_matches(
            challenge, proof,
            listener->config.socket_ops.now_ms(
                listener->config.socket_ops.context))) {
        (void)vd_auth_record_failure(
            listener, peer_ipv4, host_key_id,
            listener->config.socket_ops.now_ms(
                listener->config.socket_ops.context));
        result = VD_ATTACH_AUTH_LISTENER_ERROR_PROTOCOL;
        goto cleanup;
    }
    result = vd_auth_verify_proof(listener, proof, deadline_ms);
    if (result != VD_ATTACH_AUTH_LISTENER_OK) {
        retry_after_ms = vd_auth_record_failure(
            listener, peer_ipv4, host_key_id,
            listener->config.socket_ops.now_ms(
                listener->config.socket_ops.context));
        if (vd_auth_make_result(
                listener, proof, VD_AUTH_STATUS_DENIED, retry_after_ms,
                deadline_ms, result_record) ==
            VD_ATTACH_AUTH_LISTENER_OK) {
            (void)vd_auth_send_frame(
                listener, socket, result_record, sizeof(result_record),
                deadline_ms);
        }
        goto cleanup;
    }
    result = vd_auth_make_result(listener, proof, VD_AUTH_STATUS_OK, 0u,
                                 deadline_ms, result_record);
    if (result == VD_ATTACH_AUTH_LISTENER_OK) {
        result = vd_auth_send_frame(listener, socket, result_record,
                                    sizeof(result_record), deadline_ms);
    }
    if (result == VD_ATTACH_AUTH_LISTENER_OK) {
        vd_auth_clear_failure(listener, peer_ipv4, host_key_id);
        vd_auth_increment(&listener->stats.authenticated);
    }

cleanup:
    if (result == VD_ATTACH_AUTH_LISTENER_ERROR_PROTOCOL) {
        vd_auth_increment(&listener->stats.malformed);
    } else if (result == VD_ATTACH_AUTH_LISTENER_ERROR_IO ||
               result == VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT) {
        vd_auth_increment(&listener->stats.io_failures);
    } else if (result == VD_ATTACH_AUTH_LISTENER_ERROR_PEER) {
        vd_auth_increment(&listener->stats.denied);
    }
    crypto_wipe(hello, sizeof(hello));
    crypto_wipe(challenge, sizeof(challenge));
    crypto_wipe(proof, sizeof(proof));
    crypto_wipe(result_record, sizeof(result_record));
    crypto_wipe(&peer, sizeof(peer));
    crypto_wipe(&local, sizeof(local));
    (void)session_id;
    return result;
}

void vd_attach_auth_listener_config_init(
    VdAttachAuthListenerConfig *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->port = VD_ATTACH_AUTH_LISTENER_DEFAULT_PORT;
    config->handshake_deadline_ms = 3000u;
}

int vd_attach_auth_listener_init(
    VdAttachAuthListener *listener,
    const VdAttachAuthListenerConfig *config) {
    if (listener == NULL || config == NULL ||
        !vd_auth_ops_valid(&config->socket_ops) ||
        config->key_storage == NULL ||
        config->reserve_service_generation == NULL ||
        vd_attach_auth_storage_validate(config->key_storage) !=
            VD_ATTACH_AUTH_OK ||
        !vd_auth_private_ipv4(config->bind_ipv4) ||
        !vd_auth_mask_contiguous(config->peer_netmask) ||
        (config->bind_ipv4 & config->peer_netmask) !=
            config->peer_network ||
        config->port < VD_ATTACH_AUTH_LISTENER_MIN_PORT ||
        config->port > VD_ATTACH_AUTH_LISTENER_MAX_PORT ||
        config->handshake_deadline_ms <
            VD_ATTACH_AUTH_LISTENER_MIN_DEADLINE_MS ||
        config->handshake_deadline_ms >
            VD_ATTACH_AUTH_LISTENER_MAX_DEADLINE_MS) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    memset(listener, 0, sizeof(*listener));
    listener->config = *config;
    listener->listen_socket = VD_AUTH_SOCKET_INVALID;
    listener->active_socket = VD_AUTH_SOCKET_INVALID;
    listener->state = VD_ATTACH_AUTH_LISTENER_STOPPED;
    return VD_ATTACH_AUTH_LISTENER_OK;
}

int vd_attach_auth_listener_start(VdAttachAuthListener *listener) {
    uint64_t generation = 0u;
    int listen_socket = VD_AUTH_SOCKET_INVALID;
    int result;
    int expected;
    if (listener == NULL) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    if (__atomic_load_n(&listener->listen_socket, __ATOMIC_ACQUIRE) !=
            VD_AUTH_SOCKET_INVALID ||
        __atomic_load_n(&listener->active_socket, __ATOMIC_ACQUIRE) !=
            VD_AUTH_SOCKET_INVALID) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_RESOURCE;
    }
    expected = VD_ATTACH_AUTH_LISTENER_STOPPED;
    if (!__atomic_compare_exchange_n(
            &listener->state, &expected,
            VD_ATTACH_AUTH_LISTENER_STARTING, 0, __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE)) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_STATE;
    }
    result = listener->config.reserve_service_generation(
        listener->config.generation_context, &generation);
    if (result < 0 || generation == 0u) {
        __atomic_store_n(&listener->state,
                         VD_ATTACH_AUTH_LISTENER_STOPPED,
                         __ATOMIC_RELEASE);
        return VD_ATTACH_AUTH_LISTENER_ERROR_STORAGE;
    }
    result = listener->config.socket_ops.listen_open(
        listener->config.socket_ops.context,
        listener->config.bind_ipv4, listener->config.port,
        &listen_socket);
    if (result < 0 || listen_socket < 0) {
        if (listen_socket >= 0) {
            (void)listener->config.socket_ops.close(
                listener->config.socket_ops.context, listen_socket);
        }
        __atomic_store_n(&listener->state,
                         VD_ATTACH_AUTH_LISTENER_STOPPED,
                         __ATOMIC_RELEASE);
        return VD_ATTACH_AUTH_LISTENER_ERROR_IO;
    }
    listener->service_generation = generation;
    listener->next_session_id = 1u;
    listener->connection_generation = 0u;
    listener->replay_count = 0u;
    memset(listener->replay_nonces, 0, sizeof(listener->replay_nonces));
    memset(listener->failures, 0, sizeof(listener->failures));
    listener->failure_table_exhausted = 0;
    __atomic_store_n(&listener->listen_socket, listen_socket,
                     __ATOMIC_RELEASE);
    expected = VD_ATTACH_AUTH_LISTENER_STARTING;
    if (!__atomic_compare_exchange_n(
            &listener->state, &expected,
            VD_ATTACH_AUTH_LISTENER_RUNNING, 0, __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE)) {
        (void)vd_auth_close_registered(listener,
                                       &listener->listen_socket);
        __atomic_store_n(&listener->state,
                         VD_ATTACH_AUTH_LISTENER_STOPPED,
                         __ATOMIC_RELEASE);
        return VD_ATTACH_AUTH_LISTENER_ERROR_SHUTDOWN;
    }
    return VD_ATTACH_AUTH_LISTENER_OK;
}

int vd_attach_auth_listener_run(VdAttachAuthListener *listener) {
    int final_result = VD_ATTACH_AUTH_LISTENER_OK;
    if (listener == NULL ||
        __atomic_load_n(&listener->state, __ATOMIC_ACQUIRE) !=
            VD_ATTACH_AUTH_LISTENER_RUNNING) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_STATE;
    }
    for (;;) {
        uint64_t now_ms;
        uint64_t accept_deadline;
        uint64_t transport_binding;
        uint32_t peer_ipv4 = 0u;
        int listen_socket;
        int client_socket = VD_AUTH_SOCKET_INVALID;
        int result;
        if (__atomic_load_n(&listener->state, __ATOMIC_ACQUIRE) !=
            VD_ATTACH_AUTH_LISTENER_RUNNING) {
            break;
        }
        listen_socket = __atomic_load_n(&listener->listen_socket,
                                        __ATOMIC_ACQUIRE);
        if (listen_socket < 0) {
            break;
        }
        now_ms = listener->config.socket_ops.now_ms(
            listener->config.socket_ops.context);
        accept_deadline =
            UINT64_MAX - now_ms < VD_ATTACH_AUTH_LISTENER_ACCEPT_SLICE_MS
                ? UINT64_MAX
                : now_ms + VD_ATTACH_AUTH_LISTENER_ACCEPT_SLICE_MS;
        result = listener->config.socket_ops.accept(
            listener->config.socket_ops.context, listen_socket,
            &client_socket, &peer_ipv4, accept_deadline);
        if (result == VD_ATTACH_AUTH_LISTENER_IO_TIMEOUT) {
            continue;
        }
        if (result < 0 || client_socket < 0) {
            if (__atomic_load_n(&listener->state, __ATOMIC_ACQUIRE) ==
                VD_ATTACH_AUTH_LISTENER_STOPPING) {
                break;
            }
            final_result = VD_ATTACH_AUTH_LISTENER_ERROR_IO;
            break;
        }
        vd_auth_increment(&listener->stats.accepted);
        if (!vd_auth_peer_allowed(listener, peer_ipv4)) {
            vd_auth_increment(&listener->stats.peer_rejected);
            (void)listener->config.socket_ops.shutdown(
                listener->config.socket_ops.context, client_socket);
            if (listener->config.socket_ops.close(
                    listener->config.socket_ops.context,
                    client_socket) < 0) {
                vd_auth_increment(&listener->stats.close_failures);
            }
            continue;
        }
        __atomic_store_n(&listener->active_socket, client_socket,
                         __ATOMIC_RELEASE);
        if (listener->connection_generation == UINT32_MAX) {
            final_result = VD_ATTACH_AUTH_LISTENER_ERROR_RESOURCE;
            (void)vd_auth_close_registered(listener,
                                           &listener->active_socket);
            break;
        }
        ++listener->connection_generation;
        transport_binding =
            ((uint64_t)peer_ipv4 << 32) |
            listener->connection_generation;
        (void)vd_auth_serve(listener, client_socket, peer_ipv4,
                            transport_binding);
        (void)vd_auth_close_registered(listener,
                                       &listener->active_socket);
    }
    {
        int active_close = vd_auth_close_registered(
            listener, &listener->active_socket);
        int listen_close = vd_auth_close_registered(
            listener, &listener->listen_socket);
        if (active_close < 0 || listen_close < 0) {
            final_result = VD_ATTACH_AUTH_LISTENER_ERROR_IO;
        }
    }
    crypto_wipe(listener->replay_nonces,
                sizeof(listener->replay_nonces));
    listener->replay_count = 0u;
    __atomic_store_n(&listener->state, VD_ATTACH_AUTH_LISTENER_STOPPED,
                     __ATOMIC_RELEASE);
    return final_result;
}

int vd_attach_auth_listener_shutdown(VdAttachAuthListener *listener) {
    int state;
    int result = VD_ATTACH_AUTH_LISTENER_OK;
    if (listener == NULL) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    state = __atomic_load_n(&listener->state, __ATOMIC_ACQUIRE);
    for (;;) {
        if (state == VD_ATTACH_AUTH_LISTENER_STOPPED) {
            break;
        }
        if (state == VD_ATTACH_AUTH_LISTENER_STOPPING) {
            break;
        }
        if (__atomic_compare_exchange_n(
                &listener->state, &state,
                VD_ATTACH_AUTH_LISTENER_STOPPING, 0,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            vd_auth_increment(&listener->stats.shutdowns);
            break;
        }
    }
    if (vd_auth_close_registered(listener, &listener->active_socket) <
        0) {
        result = VD_ATTACH_AUTH_LISTENER_ERROR_IO;
    }
    if (vd_auth_close_registered(listener, &listener->listen_socket) <
        0) {
        result = VD_ATTACH_AUTH_LISTENER_ERROR_IO;
    }
    return result;
}

int vd_attach_auth_listener_get_stats(
    const VdAttachAuthListener *listener,
    VdAttachAuthListenerStats *stats) {
    if (listener == NULL || stats == NULL) {
        return VD_ATTACH_AUTH_LISTENER_ERROR_ARGUMENT;
    }
    *stats = listener->stats;
    return VD_ATTACH_AUTH_LISTENER_OK;
}

VdAttachAuthListenerState vd_attach_auth_listener_state(
    const VdAttachAuthListener *listener) {
    if (listener == NULL) {
        return VD_ATTACH_AUTH_LISTENER_STOPPED;
    }
    return (VdAttachAuthListenerState)__atomic_load_n(
        &listener->state, __ATOMIC_ACQUIRE);
}

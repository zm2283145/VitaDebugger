#include "vitadebug_attach_auth.h"

#include <monocypher.h>
#include <monocypher-ed25519.h>

#include <string.h>

static int vd_attach_auth_bytes_nonzero(const uint8_t *bytes,
                                        size_t size) {
    size_t index;
    uint8_t combined = 0u;
    if (bytes == NULL) {
        return 0;
    }
    for (index = 0u; index < size; ++index) {
        combined |= bytes[index];
    }
    return combined != 0u;
}

static int vd_attach_auth_key_valid(const VdAttachAuthPublicKey *key,
                                    int require_active) {
    return key != NULL && key->key_id != 0u &&
           key->generation != 0u &&
           (key->status == VD_ATTACH_AUTH_KEY_ACTIVE ||
            (!require_active &&
             key->status == VD_ATTACH_AUTH_KEY_REVOKED)) &&
           vd_attach_auth_bytes_nonzero(key->public_key,
                                        sizeof(key->public_key));
}

int vd_attach_auth_storage_validate(
    const VdAttachAuthKeyStorage *storage) {
    if (storage == NULL || storage->load_local_public == NULL ||
        storage->lookup_peer == NULL || storage->sign_local == NULL ||
        storage->provision_local == NULL ||
        storage->rotate_local == NULL || storage->allow_peer == NULL ||
        storage->revoke_peer == NULL) {
        return VD_ATTACH_AUTH_ERROR_UNAVAILABLE;
    }
    return VD_ATTACH_AUTH_OK;
}

int vd_attach_auth_load_local(
    const VdAttachAuthKeyStorage *storage,
    VdAttachAuthPublicKey *key) {
    int result;
    if (key != NULL) {
        memset(key, 0, sizeof(*key));
    }
    if (key == NULL ||
        vd_attach_auth_storage_validate(storage) != VD_ATTACH_AUTH_OK) {
        return VD_ATTACH_AUTH_ERROR_UNAVAILABLE;
    }
    result = storage->load_local_public(storage->context, key);
    if (result != VD_ATTACH_AUTH_OK ||
        !vd_attach_auth_key_valid(key, 1)) {
        memset(key, 0, sizeof(*key));
        return result == VD_ATTACH_AUTH_ERROR_REVOKED
                   ? VD_ATTACH_AUTH_ERROR_REVOKED
                   : VD_ATTACH_AUTH_ERROR_STORAGE;
    }
    return VD_ATTACH_AUTH_OK;
}

int vd_attach_auth_lookup_peer(
    const VdAttachAuthKeyStorage *storage,
    uint64_t key_id,
    uint64_t generation,
    VdAttachAuthPublicKey *key) {
    int result;
    if (key != NULL) {
        memset(key, 0, sizeof(*key));
    }
    if (key == NULL || key_id == 0u || generation == 0u ||
        vd_attach_auth_storage_validate(storage) != VD_ATTACH_AUTH_OK) {
        return VD_ATTACH_AUTH_ERROR_UNAVAILABLE;
    }
    result = storage->lookup_peer(storage->context, key_id, generation,
                                  key);
    if (result != VD_ATTACH_AUTH_OK) {
        memset(key, 0, sizeof(*key));
        return result;
    }
    if (!vd_attach_auth_key_valid(key, 0) ||
        key->key_id != key_id || key->generation != generation) {
        memset(key, 0, sizeof(*key));
        return VD_ATTACH_AUTH_ERROR_STORAGE;
    }
    if (key->status == VD_ATTACH_AUTH_KEY_REVOKED) {
        memset(key, 0, sizeof(*key));
        return VD_ATTACH_AUTH_ERROR_REVOKED;
    }
    return VD_ATTACH_AUTH_OK;
}

int vd_attach_auth_sign_local(
    const VdAttachAuthKeyStorage *storage,
    const uint8_t *transcript,
    size_t transcript_size,
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES],
    uint64_t deadline_ms) {
    VdAttachAuthPublicKey local;
    int result;

    if (signature != NULL) {
        memset(signature, 0, VD_ATTACH_AUTH_SIGNATURE_BYTES);
    }
    if (transcript == NULL || transcript_size == 0u ||
        transcript_size > VD_ATTACH_AUTH_MAX_TRANSCRIPT_BYTES ||
        signature == NULL || deadline_ms == 0u) {
        return VD_ATTACH_AUTH_ERROR_ARGUMENT;
    }
    result = vd_attach_auth_load_local(storage, &local);
    if (result != VD_ATTACH_AUTH_OK) {
        return result;
    }
    result = storage->sign_local(
        storage->context, local.key_id, local.generation, transcript,
        transcript_size, signature, deadline_ms);
    crypto_wipe(&local, sizeof(local));
    if (result != VD_ATTACH_AUTH_OK ||
        !vd_attach_auth_bytes_nonzero(signature,
                                      VD_ATTACH_AUTH_SIGNATURE_BYTES)) {
        crypto_wipe(signature, VD_ATTACH_AUTH_SIGNATURE_BYTES);
        return result == VD_ATTACH_AUTH_OK
                   ? VD_ATTACH_AUTH_ERROR_SIGNATURE
                   : result;
    }
    return VD_ATTACH_AUTH_OK;
}

int vd_attach_auth_verify_ed25519(
    const VdAttachAuthPublicKey *peer,
    const uint8_t *transcript,
    size_t transcript_size,
    const uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES]) {
    if (!vd_attach_auth_key_valid(peer, 1) || transcript == NULL ||
        transcript_size == 0u ||
        transcript_size > VD_ATTACH_AUTH_MAX_TRANSCRIPT_BYTES ||
        signature == NULL) {
        return VD_ATTACH_AUTH_ERROR_ARGUMENT;
    }
    return crypto_ed25519_check(signature, peer->public_key, transcript,
                                transcript_size) == 0
               ? VD_ATTACH_AUTH_OK
               : VD_ATTACH_AUTH_ERROR_SIGNATURE;
}

int vd_attach_auth_constant_time_equal(const uint8_t *left,
                                       const uint8_t *right,
                                       size_t size) {
    size_t index;
    uint8_t difference = 0u;
    if (left == NULL || right == NULL || size == 0u) {
        return 0;
    }
    for (index = 0u; index < size; ++index) {
        difference |= (uint8_t)(left[index] ^ right[index]);
    }
    return difference == 0u;
}

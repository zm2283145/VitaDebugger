#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_ATTACH_AUTH_WIRE_VERSION 2u
#define VD_ATTACH_AUTH_FRAME_MAX_BYTES 1024u
#define VD_ATTACH_AUTH_HEADER_BYTES 8u
#define VD_ATTACH_AUTH_HELLO_BYTES 88u
#define VD_ATTACH_AUTH_CHALLENGE_BYTES 240u
#define VD_ATTACH_AUTH_PROOF_BYTES 248u
#define VD_ATTACH_AUTH_RESULT_BYTES 288u
#define VD_ATTACH_AUTH_KEY_BYTES 32u
#define VD_ATTACH_AUTH_SIGNATURE_BYTES 64u
#define VD_ATTACH_AUTH_KEY_ID_BYTES 8u
#define VD_ATTACH_AUTH_MAX_TRANSCRIPT_BYTES 512u

#define VD_ATTACH_AUTH_SERVER_CHALLENGE_DOMAIN \
    "VITADEBUG-ATTACH/SERVER-CHALLENGE/v2"
#define VD_ATTACH_AUTH_CLIENT_PROOF_DOMAIN \
    "VITADEBUG-ATTACH/CLIENT-PROOF/v2"
#define VD_ATTACH_AUTH_SESSION_RESULT_DOMAIN \
    "VITADEBUG-ATTACH/SESSION-RESULT/v2"

enum {
    VD_ATTACH_AUTH_OK = 0,
    VD_ATTACH_AUTH_ERROR_ARGUMENT = -1,
    VD_ATTACH_AUTH_ERROR_UNAVAILABLE = -2,
    VD_ATTACH_AUTH_ERROR_NOT_ALLOWED = -3,
    VD_ATTACH_AUTH_ERROR_REVOKED = -4,
    VD_ATTACH_AUTH_ERROR_STORAGE = -5,
    VD_ATTACH_AUTH_ERROR_SIGNATURE = -6,
};

typedef enum VdAttachAuthKeyStatus {
    VD_ATTACH_AUTH_KEY_ACTIVE = 1,
    VD_ATTACH_AUTH_KEY_REVOKED = 2,
} VdAttachAuthKeyStatus;

typedef struct VdAttachAuthPublicKey {
    uint64_t key_id;
    uint64_t generation;
    uint32_t status;
    uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES];
} VdAttachAuthPublicKey;

/*
 * The implementation owns device-private-key storage. Runtime signing
 * identifies the active local key by ID and generation and returns only the
 * signature. Provision/rotation are explicit sensitive administration calls;
 * their private-key input must never be exposed by the network listener.
 *
 * All callbacks must be backed by one atomic persistent revision. Rotation
 * must make the replacement active before retiring the old private key.
 * Revocation must survive reboot before reporting success. Lookup returns
 * ACTIVE, REVOKED, or NOT_ALLOWED without silently selecting another key.
 */
typedef struct VdAttachAuthKeyStorage {
    void *context;
    int (*load_local_public)(void *context,
                             VdAttachAuthPublicKey *key);
    int (*lookup_peer)(void *context,
                       uint64_t key_id,
                       uint64_t generation,
                       VdAttachAuthPublicKey *key);
    int (*sign_local)(void *context,
                      uint64_t key_id,
                      uint64_t generation,
                      const uint8_t *transcript,
                      size_t transcript_size,
                      uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES],
                      uint64_t deadline_ms);
    int (*provision_local)(void *context,
                           uint64_t key_id,
                           uint64_t generation,
                           const uint8_t private_key[VD_ATTACH_AUTH_KEY_BYTES],
                           const uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES]);
    int (*rotate_local)(void *context,
                        uint64_t old_key_id,
                        uint64_t old_generation,
                        uint64_t new_key_id,
                        uint64_t new_generation,
                        const uint8_t private_key[VD_ATTACH_AUTH_KEY_BYTES],
                        const uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES]);
    int (*allow_peer)(void *context,
                      const VdAttachAuthPublicKey *key);
    int (*revoke_peer)(void *context,
                       uint64_t key_id,
                       uint64_t generation);
} VdAttachAuthKeyStorage;

int vd_attach_auth_storage_validate(
    const VdAttachAuthKeyStorage *storage);

int vd_attach_auth_load_local(
    const VdAttachAuthKeyStorage *storage,
    VdAttachAuthPublicKey *key);

int vd_attach_auth_lookup_peer(
    const VdAttachAuthKeyStorage *storage,
    uint64_t key_id,
    uint64_t generation,
    VdAttachAuthPublicKey *key);

int vd_attach_auth_sign_local(
    const VdAttachAuthKeyStorage *storage,
    const uint8_t *transcript,
    size_t transcript_size,
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES],
    uint64_t deadline_ms);

int vd_attach_auth_verify_ed25519(
    const VdAttachAuthPublicKey *peer,
    const uint8_t *transcript,
    size_t transcript_size,
    const uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES]);

/* Constant-time byte comparison for fixed authentication values. */
int vd_attach_auth_constant_time_equal(const uint8_t *left,
                                       const uint8_t *right,
                                       size_t size);

/* Exact default Vita boundary until reviewed persistent storage exists. */
void vd_attach_auth_vita_unavailable_storage(
    VdAttachAuthKeyStorage *storage);

#ifdef __cplusplus
}
#endif

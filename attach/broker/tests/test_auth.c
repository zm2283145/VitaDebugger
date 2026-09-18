#include "vitadebug_attach_auth.h"

#include <monocypher-ed25519.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct FakeKeyStorage {
    uint8_t secret_key[64];
    VdAttachAuthPublicKey local;
    VdAttachAuthPublicKey peer;
} FakeKeyStorage;

static int fake_load_local(void *opaque, VdAttachAuthPublicKey *key) {
    *key = ((FakeKeyStorage *)opaque)->local;
    return VD_ATTACH_AUTH_OK;
}

static int fake_lookup_peer(void *opaque, uint64_t key_id,
                            uint64_t generation,
                            VdAttachAuthPublicKey *key) {
    FakeKeyStorage *storage = (FakeKeyStorage *)opaque;
    if (storage->peer.key_id != key_id ||
        storage->peer.generation != generation) {
        return VD_ATTACH_AUTH_ERROR_NOT_ALLOWED;
    }
    *key = storage->peer;
    return VD_ATTACH_AUTH_OK;
}

static int fake_sign(void *opaque, uint64_t key_id, uint64_t generation,
                     const uint8_t *transcript, size_t transcript_size,
                     uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES],
                     uint64_t deadline_ms) {
    FakeKeyStorage *storage = (FakeKeyStorage *)opaque;
    if (key_id != storage->local.key_id ||
        generation != storage->local.generation || deadline_ms == 0u) {
        return VD_ATTACH_AUTH_ERROR_STORAGE;
    }
    crypto_ed25519_sign(signature, storage->secret_key, transcript,
                        transcript_size);
    return VD_ATTACH_AUTH_OK;
}

static int fake_provision(void *opaque, uint64_t key_id,
                          uint64_t generation,
                          const uint8_t private_key[32],
                          const uint8_t public_key[32]) {
    (void)opaque;
    (void)key_id;
    (void)generation;
    (void)private_key;
    (void)public_key;
    return VD_ATTACH_AUTH_OK;
}

static int fake_rotate(void *opaque, uint64_t old_key_id,
                       uint64_t old_generation, uint64_t new_key_id,
                       uint64_t new_generation,
                       const uint8_t private_key[32],
                       const uint8_t public_key[32]) {
    (void)opaque;
    (void)old_key_id;
    (void)old_generation;
    (void)new_key_id;
    (void)new_generation;
    (void)private_key;
    (void)public_key;
    return VD_ATTACH_AUTH_OK;
}

static int fake_allow(void *opaque, const VdAttachAuthPublicKey *key) {
    (void)opaque;
    (void)key;
    return VD_ATTACH_AUTH_OK;
}

static int fake_revoke(void *opaque, uint64_t key_id,
                       uint64_t generation) {
    (void)opaque;
    (void)key_id;
    (void)generation;
    return VD_ATTACH_AUTH_OK;
}

static VdAttachAuthKeyStorage fake_storage(FakeKeyStorage *context) {
    VdAttachAuthKeyStorage storage;
    memset(&storage, 0, sizeof(storage));
    storage.context = context;
    storage.load_local_public = fake_load_local;
    storage.lookup_peer = fake_lookup_peer;
    storage.sign_local = fake_sign;
    storage.provision_local = fake_provision;
    storage.rotate_local = fake_rotate;
    storage.allow_peer = fake_allow;
    storage.revoke_peer = fake_revoke;
    return storage;
}

static void test_unavailable_storage_fails_closed(void) {
    VdAttachAuthKeyStorage storage;
    VdAttachAuthPublicKey key;
    vd_attach_auth_vita_unavailable_storage(&storage);
    memset(&key, 0xa5, sizeof(key));
    assert(vd_attach_auth_storage_validate(&storage) ==
           VD_ATTACH_AUTH_ERROR_UNAVAILABLE);
    assert(vd_attach_auth_load_local(&storage, &key) ==
           VD_ATTACH_AUTH_ERROR_UNAVAILABLE);
    assert(key.key_id == 0u);
}

static void test_audited_ed25519_and_storage_boundary(void) {
    static const uint8_t transcript[] =
        "VITADEBUG-ATTACH/TEST-TRANSCRIPT/v2";
    FakeKeyStorage context;
    VdAttachAuthKeyStorage storage;
    VdAttachAuthPublicKey peer;
    uint8_t seed[32];
    uint8_t signature[64];
    size_t index;

    memset(&context, 0, sizeof(context));
    for (index = 0u; index < sizeof(seed); ++index) {
        seed[index] = (uint8_t)(index + 1u);
    }
    crypto_ed25519_key_pair(context.secret_key,
                            context.local.public_key, seed);
    context.local.key_id = UINT64_C(0x0102030405060708);
    context.local.generation = 1u;
    context.local.status = VD_ATTACH_AUTH_KEY_ACTIVE;
    context.peer = context.local;
    context.peer.key_id = UINT64_C(0x1112131415161718);
    storage = fake_storage(&context);

    assert(vd_attach_auth_storage_validate(&storage) ==
           VD_ATTACH_AUTH_OK);
    assert(vd_attach_auth_lookup_peer(
               &storage, context.peer.key_id, context.peer.generation,
               &peer) == VD_ATTACH_AUTH_OK);
    assert(vd_attach_auth_sign_local(
               &storage, transcript, sizeof(transcript) - 1u, signature,
               1000u) == VD_ATTACH_AUTH_OK);
    assert(vd_attach_auth_verify_ed25519(
               &peer, transcript, sizeof(transcript) - 1u, signature) ==
           VD_ATTACH_AUTH_OK);
    signature[0] ^= 1u;
    assert(vd_attach_auth_verify_ed25519(
               &peer, transcript, sizeof(transcript) - 1u, signature) ==
           VD_ATTACH_AUTH_ERROR_SIGNATURE);

    context.peer.status = VD_ATTACH_AUTH_KEY_REVOKED;
    assert(vd_attach_auth_lookup_peer(
               &storage, context.peer.key_id, context.peer.generation,
               &peer) == VD_ATTACH_AUTH_ERROR_REVOKED);
}

static void test_constant_time_comparison_contract(void) {
    uint8_t left[32];
    uint8_t right[32];
    memset(left, 0x5a, sizeof(left));
    memset(right, 0x5a, sizeof(right));
    assert(vd_attach_auth_constant_time_equal(left, right,
                                               sizeof(left)));
    right[0] ^= 1u;
    assert(!vd_attach_auth_constant_time_equal(left, right,
                                                sizeof(left)));
    right[0] ^= 1u;
    right[31] ^= 1u;
    assert(!vd_attach_auth_constant_time_equal(left, right,
                                                sizeof(left)));
}

static void test_wire_contract_constants(void) {
    assert(VD_ATTACH_AUTH_WIRE_VERSION == 2u);
    assert(VD_ATTACH_AUTH_FRAME_MAX_BYTES == 1024u);
    assert(VD_ATTACH_AUTH_HEADER_BYTES == 8u);
    assert(VD_ATTACH_AUTH_HELLO_BYTES == 88u);
    assert(VD_ATTACH_AUTH_CHALLENGE_BYTES == 240u);
    assert(VD_ATTACH_AUTH_PROOF_BYTES == 248u);
    assert(VD_ATTACH_AUTH_RESULT_BYTES == 288u);
    assert(sizeof(VD_ATTACH_AUTH_SERVER_CHALLENGE_DOMAIN) - 1u == 36u);
    assert(sizeof(VD_ATTACH_AUTH_CLIENT_PROOF_DOMAIN) - 1u == 32u);
    assert(sizeof(VD_ATTACH_AUTH_SESSION_RESULT_DOMAIN) - 1u == 34u);
}

int main(void) {
    test_unavailable_storage_fails_closed();
    test_audited_ed25519_and_storage_boundary();
    test_constant_time_comparison_contract();
    test_wire_contract_constants();
    puts("attach authentication tests passed");
    return 0;
}

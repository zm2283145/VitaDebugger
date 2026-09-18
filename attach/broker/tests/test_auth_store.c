#include "vitadebug_attach_auth_store.h"

#include <monocypher.h>
#include <monocypher-ed25519.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FAKE_BYTES (VD_ATTACH_AUTH_STORE_MAX_BYTES + 32u)
#define FAKE_HANDLES 6u
#define FAKE_KEYS 8u

typedef struct FakeObject {
    uint8_t data[FAKE_BYTES];
    size_t size;
    int present;
} FakeObject;

typedef struct FakeHandle {
    FakeObject *object;
    size_t offset;
    int active;
    int staged;
    int writable;
    int synced;
} FakeHandle;

typedef struct FakePersistence {
    FakeObject current;
    FakeObject staged;
    FakeHandle handles[FAKE_HANDLES];
    size_t read_chunk;
    size_t write_chunk;
    unsigned int active_handles;
    unsigned int sequence;
    unsigned int sync_sequence;
    unsigned int commit_sequence;
    unsigned int floor_sequence;
    int interrupt_commit;
    int fail_read;
    int fail_close;
} FakePersistence;

typedef struct FakeMonotonic {
    FakePersistence *ordering;
    uint64_t floor;
    int fail_load;
    int fail_advance;
} FakeMonotonic;

typedef struct FakeKey {
    uint64_t key_id;
    uint64_t generation;
    uint8_t secret_key[64];
    uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES];
    int present;
} FakeKey;

typedef struct FakePrivateKeys {
    FakeKey keys[FAKE_KEYS];
    uint8_t last_seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint64_t last_deadline;
    unsigned int destroy_count;
} FakePrivateKeys;

typedef struct FakeEnvironment {
    FakePersistence persistence;
    FakeMonotonic monotonic;
    FakePrivateKeys private_keys;
    int persistence_domain;
    int monotonic_domain;
    int private_key_domain;
} FakeEnvironment;

static FakeHandle *fake_handle(FakePersistence *persistence) {
    size_t index;
    for (index = 0u; index < FAKE_HANDLES; ++index) {
        if (!persistence->handles[index].active) {
            memset(&persistence->handles[index], 0,
                   sizeof(persistence->handles[index]));
            persistence->handles[index].active = 1;
            ++persistence->active_handles;
            return &persistence->handles[index];
        }
    }
    return NULL;
}

static int fake_open_object(FakePersistence *persistence,
                            FakeObject *object, int staged,
                            void **handle) {
    FakeHandle *opened;
    *handle = NULL;
    if (!object->present) {
        return VD_ATTACH_AUTH_STORE_BACKEND_NOT_FOUND;
    }
    opened = fake_handle(persistence);
    if (opened == NULL) {
        return -1;
    }
    opened->object = object;
    opened->staged = staged;
    *handle = opened;
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_open_current(void *context, void **handle) {
    FakePersistence *persistence = (FakePersistence *)context;
    return fake_open_object(persistence, &persistence->current, 0,
                            handle);
}

static int fake_open_staged(void *context, void **handle) {
    FakePersistence *persistence = (FakePersistence *)context;
    return fake_open_object(persistence, &persistence->staged, 1,
                            handle);
}

static int fake_create_staged(void *context, void **handle) {
    FakePersistence *persistence = (FakePersistence *)context;
    FakeHandle *opened;
    *handle = NULL;
    if (persistence->staged.present) {
        return -1;
    }
    opened = fake_handle(persistence);
    if (opened == NULL) {
        return -1;
    }
    memset(&persistence->staged, 0,
           sizeof(persistence->staged));
    persistence->staged.present = 1;
    opened->object = &persistence->staged;
    opened->staged = 1;
    opened->writable = 1;
    *handle = opened;
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_read(void *context, void *handle, uint8_t *data,
                     size_t capacity, size_t *read_size) {
    FakePersistence *persistence = (FakePersistence *)context;
    FakeHandle *opened = (FakeHandle *)handle;
    size_t remaining;
    size_t amount;
    *read_size = 0u;
    if (persistence->fail_read || !opened->active ||
        opened->writable ||
        opened->offset > opened->object->size) {
        return -1;
    }
    remaining = opened->object->size - opened->offset;
    amount = capacity < remaining ? capacity : remaining;
    if (persistence->read_chunk != 0u &&
        amount > persistence->read_chunk) {
        amount = persistence->read_chunk;
    }
    memcpy(data, opened->object->data + opened->offset, amount);
    opened->offset += amount;
    *read_size = amount;
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_write(void *context, void *handle,
                      const uint8_t *data, size_t size,
                      size_t *write_size) {
    FakePersistence *persistence = (FakePersistence *)context;
    FakeHandle *opened = (FakeHandle *)handle;
    size_t amount = size;
    *write_size = 0u;
    if (!opened->active || !opened->writable ||
        !opened->staged) {
        return -1;
    }
    if (persistence->write_chunk != 0u &&
        amount > persistence->write_chunk) {
        amount = persistence->write_chunk;
    }
    if (amount > FAKE_BYTES - opened->offset) {
        return -1;
    }
    memcpy(opened->object->data + opened->offset, data, amount);
    opened->offset += amount;
    if (opened->offset > opened->object->size) {
        opened->object->size = opened->offset;
    }
    *write_size = amount;
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_sync(void *context, void *handle) {
    FakePersistence *persistence = (FakePersistence *)context;
    FakeHandle *opened = (FakeHandle *)handle;
    if (!opened->active) {
        return -1;
    }
    opened->synced = 1;
    persistence->sync_sequence = ++persistence->sequence;
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_close(void *context, void *handle) {
    FakePersistence *persistence = (FakePersistence *)context;
    FakeHandle *opened = (FakeHandle *)handle;
    if (!opened->active) {
        return -1;
    }
    opened->active = 0;
    --persistence->active_handles;
    if (persistence->fail_close) {
        persistence->fail_close = 0;
        return -1;
    }
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_commit(void *context, void *handle) {
    FakePersistence *persistence = (FakePersistence *)context;
    FakeHandle *opened = (FakeHandle *)handle;
    persistence->commit_sequence = ++persistence->sequence;
    if (!opened->active || !opened->staged || !opened->synced ||
        opened->object != &persistence->staged) {
        return -1;
    }
    if (persistence->interrupt_commit) {
        persistence->interrupt_commit = 0;
        return VD_ATTACH_AUTH_STORE_BACKEND_INTERRUPTED;
    }
    persistence->current = persistence->staged;
    memset(&persistence->staged, 0,
           sizeof(persistence->staged));
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_discard(void *context, void *handle) {
    FakePersistence *persistence = (FakePersistence *)context;
    FakeHandle *opened = (FakeHandle *)handle;
    if (!opened->active || !opened->staged ||
        opened->object != &persistence->staged) {
        return -1;
    }
    memset(&persistence->staged, 0,
           sizeof(persistence->staged));
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_load_floor(void *context, uint64_t *revision) {
    FakeMonotonic *monotonic = (FakeMonotonic *)context;
    if (monotonic->fail_load) {
        return -1;
    }
    *revision = monotonic->floor;
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_advance_floor(void *context, uint64_t revision) {
    FakeMonotonic *monotonic = (FakeMonotonic *)context;
    if (monotonic->fail_advance || revision < monotonic->floor) {
        return -1;
    }
    monotonic->floor = revision;
    monotonic->ordering->floor_sequence =
        ++monotonic->ordering->sequence;
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static FakeKey *fake_find_key(FakePrivateKeys *keys,
                              uint64_t key_id,
                              uint64_t generation) {
    size_t index;
    for (index = 0u; index < FAKE_KEYS; ++index) {
        if (keys->keys[index].present &&
            keys->keys[index].key_id == key_id &&
            keys->keys[index].generation == generation) {
            return &keys->keys[index];
        }
    }
    return NULL;
}

static int fake_generate(void *context, uint64_t key_id,
                         uint64_t generation,
                         uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES]) {
    FakePrivateKeys *keys = (FakePrivateKeys *)context;
    FakeKey *key = NULL;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    size_t index;
    if (fake_find_key(keys, key_id, generation) != NULL) {
        return -1;
    }
    for (index = 0u; index < FAKE_KEYS; ++index) {
        if (!keys->keys[index].present) {
            key = &keys->keys[index];
            break;
        }
    }
    if (key == NULL) {
        return -1;
    }
    for (index = 0u; index < sizeof(seed); ++index) {
        seed[index] = (uint8_t)(key_id + generation + index + 1u);
    }
    memcpy(keys->last_seed, seed, sizeof(seed));
    memset(key, 0, sizeof(*key));
    key->key_id = key_id;
    key->generation = generation;
    crypto_ed25519_key_pair(key->secret_key, key->public_key, seed);
    memcpy(public_key, key->public_key, sizeof(key->public_key));
    key->present = 1;
    crypto_wipe(seed, sizeof(seed));
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_load_public(
    void *context, uint64_t key_id, uint64_t generation,
    uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES]) {
    FakeKey *key = fake_find_key((FakePrivateKeys *)context, key_id,
                                 generation);
    if (key == NULL) {
        return VD_ATTACH_AUTH_STORE_BACKEND_NOT_FOUND;
    }
    memcpy(public_key, key->public_key, sizeof(key->public_key));
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_sign(
    void *context, uint64_t key_id, uint64_t generation,
    const uint8_t *message, size_t message_size,
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES],
    uint64_t deadline_ms) {
    FakePrivateKeys *keys = (FakePrivateKeys *)context;
    FakeKey *key = fake_find_key(keys, key_id, generation);
    if (key == NULL || deadline_ms == 0u) {
        return -1;
    }
    keys->last_deadline = deadline_ms;
    crypto_ed25519_sign(signature, key->secret_key, message,
                        message_size);
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static int fake_destroy(void *context, uint64_t key_id,
                        uint64_t generation) {
    FakePrivateKeys *keys = (FakePrivateKeys *)context;
    FakeKey *key = fake_find_key(keys, key_id, generation);
    ++keys->destroy_count;
    if (key != NULL) {
        crypto_wipe(key, sizeof(*key));
    }
    return VD_ATTACH_AUTH_STORE_BACKEND_OK;
}

static void fake_environment_init(FakeEnvironment *environment) {
    memset(environment, 0, sizeof(*environment));
    environment->persistence.read_chunk = 7u;
    environment->persistence.write_chunk = 11u;
    environment->monotonic.ordering =
        &environment->persistence;
}

static void fake_ops(
    FakeEnvironment *environment,
    VdAttachAuthStorePersistenceOps *persistence,
    VdAttachAuthStoreMonotonicOps *monotonic,
    VdAttachAuthStorePrivateKeyOps *private_keys) {
    memset(persistence, 0, sizeof(*persistence));
    persistence->context = &environment->persistence;
    persistence->trust_domain =
        &environment->persistence_domain;
    persistence->capabilities =
        VD_ATTACH_AUTH_PERSISTENCE_HANDLE_BOUND |
        VD_ATTACH_AUTH_PERSISTENCE_DURABLE_COMMIT |
        VD_ATTACH_AUTH_PERSISTENCE_RECOVERABLE_STAGE;
    persistence->open_current = fake_open_current;
    persistence->open_staged = fake_open_staged;
    persistence->create_staged = fake_create_staged;
    persistence->read = fake_read;
    persistence->write = fake_write;
    persistence->sync = fake_sync;
    persistence->close = fake_close;
    persistence->commit_staged = fake_commit;
    persistence->discard_staged = fake_discard;

    memset(monotonic, 0, sizeof(*monotonic));
    monotonic->context = &environment->monotonic;
    monotonic->trust_domain = &environment->monotonic_domain;
    monotonic->capabilities =
        VD_ATTACH_AUTH_MONOTONIC_INDEPENDENT_TRUST_DOMAIN |
        VD_ATTACH_AUTH_MONOTONIC_DURABLE;
    monotonic->load_floor = fake_load_floor;
    monotonic->advance_floor = fake_advance_floor;

    memset(private_keys, 0, sizeof(*private_keys));
    private_keys->context = &environment->private_keys;
    private_keys->trust_domain = &environment->private_key_domain;
    private_keys->capabilities =
        VD_ATTACH_AUTH_PRIVATE_KEY_NON_EXPORTABLE |
        VD_ATTACH_AUTH_PRIVATE_KEY_ISOLATED;
    private_keys->generate = fake_generate;
    private_keys->load_public = fake_load_public;
    private_keys->sign = fake_sign;
    private_keys->destroy = fake_destroy;
}

static int fake_store_init(FakeEnvironment *environment,
                           VdAttachAuthStore *store) {
    VdAttachAuthStorePersistenceOps persistence;
    VdAttachAuthStoreMonotonicOps monotonic;
    VdAttachAuthStorePrivateKeyOps private_keys;
    fake_ops(environment, &persistence, &monotonic, &private_keys);
    return vd_attach_auth_store_init(store, &persistence, &monotonic,
                                     &private_keys);
}

static int all_zero(const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index;
    for (index = 0u; index < size; ++index) {
        if (bytes[index] != 0u) {
            return 0;
        }
    }
    return 1;
}

static int contains_bytes(const uint8_t *data, size_t size,
                          const uint8_t *needle,
                          size_t needle_size) {
    size_t offset;
    if (needle_size > size) {
        return 0;
    }
    for (offset = 0u; offset <= size - needle_size; ++offset) {
        if (memcmp(data + offset, needle, needle_size) == 0) {
            return 1;
        }
    }
    return 0;
}

static VdAttachAuthPublicKey make_peer(uint64_t key_id,
                                       uint64_t generation) {
    VdAttachAuthPublicKey peer;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t secret[64];
    size_t index;
    memset(&peer, 0, sizeof(peer));
    for (index = 0u; index < sizeof(seed); ++index) {
        seed[index] = (uint8_t)(key_id + generation + index + 90u);
    }
    crypto_ed25519_key_pair(secret, peer.public_key, seed);
    peer.key_id = key_id;
    peer.generation = generation;
    peer.status = VD_ATTACH_AUTH_KEY_ACTIVE;
    crypto_wipe(secret, sizeof(secret));
    crypto_wipe(seed, sizeof(seed));
    return peer;
}

static void provisioned(FakeEnvironment *environment,
                        VdAttachAuthStore *store) {
    fake_environment_init(environment);
    assert(fake_store_init(environment, store) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_provision(
               store, UINT64_C(0x101), 1u) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(environment->persistence.active_handles == 0u);
    assert(environment->persistence.sync_sequence <
           environment->persistence.commit_sequence);
    assert(environment->persistence.commit_sequence <
           environment->persistence.floor_sequence);
}

static void set_current(FakeEnvironment *environment,
                        const uint8_t *data, size_t size) {
    memset(&environment->persistence.current, 0,
           sizeof(environment->persistence.current));
    memcpy(environment->persistence.current.data, data, size);
    environment->persistence.current.size = size;
    environment->persistence.current.present = 1;
    memset(&environment->persistence.staged, 0,
           sizeof(environment->persistence.staged));
}

static void store_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void refresh_digest(uint8_t *data, size_t size) {
    crypto_blake2b(data + size - 32u, 32u, data, size - 32u);
}

static void test_capability_rejection_and_full_wipe(void) {
    FakeEnvironment environment;
    VdAttachAuthStore store;
    VdAttachAuthStorePersistenceOps persistence;
    VdAttachAuthStoreMonotonicOps monotonic;
    VdAttachAuthStorePrivateKeyOps private_keys;

    fake_environment_init(&environment);
    fake_ops(&environment, &persistence, &monotonic, &private_keys);

    memset(&store, 0xa5, sizeof(store));
    private_keys.capabilities = VD_ATTACH_AUTH_PRIVATE_KEY_ISOLATED;
    assert(vd_attach_auth_store_init(
               &store, &persistence, &monotonic, &private_keys) ==
           VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY);
    assert(all_zero(&store, sizeof(store)));

    fake_ops(&environment, &persistence, &monotonic, &private_keys);
    memset(&store, 0xa5, sizeof(store));
    private_keys.generate = NULL;
    assert(vd_attach_auth_store_init(
               &store, &persistence, &monotonic, &private_keys) ==
           VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY);
    assert(all_zero(&store, sizeof(store)));

    fake_ops(&environment, &persistence, &monotonic, &private_keys);
    memset(&store, 0xa5, sizeof(store));
    private_keys.trust_domain = persistence.trust_domain;
    assert(vd_attach_auth_store_init(
               &store, &persistence, &monotonic, &private_keys) ==
           VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY);
    assert(all_zero(&store, sizeof(store)));

    fake_ops(&environment, &persistence, &monotonic, &private_keys);
    memset(&store, 0xa5, sizeof(store));
    private_keys.capabilities |=
        VD_ATTACH_AUTH_PRIVATE_KEY_RAW_SEED_IMPORT;
    assert(vd_attach_auth_store_init(
               &store, &persistence, &monotonic, &private_keys) ==
           VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY);
    assert(all_zero(&store, sizeof(store)));

    fake_ops(&environment, &persistence, &monotonic, &private_keys);
    memset(&store, 0xa5, sizeof(store));
    persistence.capabilities |=
        VD_ATTACH_AUTH_PERSISTENCE_PATH_PROOF_ONLY;
    assert(vd_attach_auth_store_init(
               &store, &persistence, &monotonic, &private_keys) ==
           VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY);
    assert(all_zero(&store, sizeof(store)));

    fake_ops(&environment, &persistence, &monotonic, &private_keys);
    memset(&store, 0xa5, sizeof(store));
    monotonic.trust_domain = persistence.trust_domain;
    assert(vd_attach_auth_store_init(
               &store, &persistence, &monotonic, &private_keys) ==
           VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY);
    assert(all_zero(&store, sizeof(store)));

    fake_ops(&environment, &persistence, &monotonic, &private_keys);
    memset(&store, 0xa5, sizeof(store));
    monotonic.capabilities |=
        VD_ATTACH_AUTH_MONOTONIC_METADATA_NAMESPACE;
    assert(vd_attach_auth_store_init(
               &store, &persistence, &monotonic, &private_keys) ==
           VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY);
    assert(all_zero(&store, sizeof(store)));

    fake_ops(&environment, &persistence, &monotonic, &private_keys);
    memset(&store, 0xa5, sizeof(store));
    monotonic.context = persistence.context;
    assert(vd_attach_auth_store_init(
               &store, &persistence, &monotonic, &private_keys) ==
           VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY);
    assert(all_zero(&store, sizeof(store)));

    fake_ops(&environment, &persistence, &monotonic, &private_keys);
    memset(&store, 0xa5, sizeof(store));
    monotonic.capabilities = VD_ATTACH_AUTH_MONOTONIC_DURABLE;
    assert(vd_attach_auth_store_init(
               &store, &persistence, &monotonic, &private_keys) ==
           VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY);
    assert(all_zero(&store, sizeof(store)));

    fake_ops(&environment, &persistence, &monotonic, &private_keys);
    memset(&store, 0xa5, sizeof(store));
    persistence.capabilities =
        VD_ATTACH_AUTH_PERSISTENCE_DURABLE_COMMIT;
    persistence.commit_staged = NULL;
    assert(vd_attach_auth_store_init(
               &store, &persistence, &monotonic, &private_keys) ==
           VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY);
    assert(all_zero(&store, sizeof(store)));
}

static void test_public_only_metadata_and_signing(void) {
    static const uint8_t message[] = "opaque-key-signature";
    FakeEnvironment environment;
    VdAttachAuthStore store;
    VdAttachAuthKeyStorage storage;
    VdAttachAuthPublicKey local;
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES];

    provisioned(&environment, &store);
    assert(!contains_bytes(environment.persistence.current.data,
                           environment.persistence.current.size,
                           environment.private_keys.last_seed,
                           sizeof(environment.private_keys.last_seed)));
    assert(!contains_bytes(store.opaque, sizeof(store.opaque),
                           environment.private_keys.last_seed,
                           sizeof(environment.private_keys.last_seed)));
    assert(vd_attach_auth_store_load_local_public(&store, &local) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_sign_local(
               &store, message, sizeof(message) - 1u, signature,
               UINT64_C(9000)) == VD_ATTACH_AUTH_STORE_OK);
    assert(environment.private_keys.last_deadline == UINT64_C(9000));
    assert(crypto_ed25519_check(
               signature, local.public_key, message,
               sizeof(message) - 1u) == 0);
    assert(vd_attach_auth_store_bind(&store, &storage) ==
           VD_ATTACH_AUTH_STORE_OK);
    memset(signature, 0, sizeof(signature));
    assert(vd_attach_auth_sign_local(
               &storage, message, sizeof(message) - 1u, signature,
               UINT64_C(9001)) == VD_ATTACH_AUTH_OK);
    assert(environment.private_keys.last_deadline == UINT64_C(9001));
}

static void test_non_destructive_deinit(void) {
    FakeEnvironment environment;
    VdAttachAuthStore store;
    VdAttachAuthStore reopened;
    VdAttachAuthKeyStorage storage;
    VdAttachAuthPublicKey local;

    provisioned(&environment, &store);
    memset(&storage, 0xa5, sizeof(storage));
    assert(vd_attach_auth_store_bind(&store, &storage) ==
           VD_ATTACH_AUTH_STORE_OK);
    vd_attach_auth_store_deinit(&store);
    assert(all_zero(&store, sizeof(store)));
    assert(environment.persistence.current.present);
    assert(fake_find_key(&environment.private_keys,
                         UINT64_C(0x101), 1u) != NULL);
    assert(vd_attach_auth_load_local(&storage, &local) ==
           VD_ATTACH_AUTH_ERROR_STORAGE);
    memset(&storage, 0xa5, sizeof(storage));
    assert(vd_attach_auth_store_bind(&store, &storage) ==
           VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT);
    assert(all_zero(&storage, sizeof(storage)));
    assert(fake_store_init(&environment, &reopened) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_load_local_public(
               &reopened, &local) == VD_ATTACH_AUTH_STORE_OK);
}

static void test_corrupt_truncated_oversized_unknown_and_cleanup(void) {
    FakeEnvironment environment;
    VdAttachAuthStore store;
    VdAttachAuthStore recovered;
    uint8_t valid[FAKE_BYTES];
    size_t valid_size;

    provisioned(&environment, &store);
    valid_size = environment.persistence.current.size;
    memcpy(valid, environment.persistence.current.data, valid_size);

    environment.persistence.current.data[valid_size - 1u] ^= 1u;
    memset(&recovered, 0xa5, sizeof(recovered));
    assert(fake_store_init(&environment, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_MALFORMED);
    assert(all_zero(&recovered, sizeof(recovered)));
    assert(environment.persistence.active_handles == 0u);

    set_current(&environment, valid, valid_size - 1u);
    memset(&recovered, 0xa5, sizeof(recovered));
    assert(fake_store_init(&environment, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_MALFORMED);
    assert(all_zero(&recovered, sizeof(recovered)));

    memset(environment.persistence.current.data, 0x5a,
           VD_ATTACH_AUTH_STORE_MAX_BYTES + 1u);
    environment.persistence.current.size =
        VD_ATTACH_AUTH_STORE_MAX_BYTES + 1u;
    memset(&recovered, 0xa5, sizeof(recovered));
    assert(fake_store_init(&environment, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_MALFORMED);
    assert(all_zero(&recovered, sizeof(recovered)));

    set_current(&environment, valid, valid_size);
    store_u32(environment.persistence.current.data + 40u, 99u);
    refresh_digest(environment.persistence.current.data,
                   environment.persistence.current.size);
    memset(&recovered, 0xa5, sizeof(recovered));
    assert(fake_store_init(&environment, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_MALFORMED);
    assert(all_zero(&recovered, sizeof(recovered)));

    set_current(&environment, valid, valid_size);
    environment.persistence.fail_read = 1;
    memset(&recovered, 0xa5, sizeof(recovered));
    assert(fake_store_init(&environment, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_IO);
    assert(all_zero(&recovered, sizeof(recovered)));
    assert(environment.persistence.active_handles == 0u);
    environment.persistence.fail_read = 0;

    set_current(&environment, valid, valid_size);
    environment.persistence.fail_close = 1;
    memset(&recovered, 0xa5, sizeof(recovered));
    assert(fake_store_init(&environment, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_IO);
    assert(all_zero(&recovered, sizeof(recovered)));
    assert(environment.persistence.active_handles == 0u);
}

static void test_interrupted_rotation_recovery(void) {
    FakeEnvironment environment;
    VdAttachAuthStore store;
    VdAttachAuthStore recovered;
    VdAttachAuthPublicKey local;

    provisioned(&environment, &store);
    environment.persistence.interrupt_commit = 1;
    assert(vd_attach_auth_store_rotate(
               &store, UINT64_C(0x101), 1u,
               UINT64_C(0x202), 2u) ==
           VD_ATTACH_AUTH_STORE_ERROR_IO);
    assert(environment.persistence.current.present);
    assert(environment.persistence.staged.present);
    assert(environment.persistence.active_handles == 0u);
    assert(vd_attach_auth_store_load_local_public(&store, &local) ==
           VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE);

    assert(fake_store_init(&environment, &recovered) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(!environment.persistence.staged.present);
    assert(vd_attach_auth_store_load_local_public(
               &recovered, &local) == VD_ATTACH_AUTH_STORE_OK);
    assert(local.key_id == UINT64_C(0x202));
    assert(local.generation == 2u);
    assert(fake_find_key(&environment.private_keys,
                         UINT64_C(0x101), 1u) == NULL);
    assert(environment.persistence.active_handles == 0u);
}

static void test_rotation_and_recover_failure_wipe(void) {
    FakeEnvironment environment;
    VdAttachAuthStore store;
    VdAttachAuthStoreMetadata metadata;

    provisioned(&environment, &store);
    assert(vd_attach_auth_store_rotate(
               &store, UINT64_C(0x101), 1u,
               UINT64_C(0x252), 2u) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(fake_find_key(&environment.private_keys,
                         UINT64_C(0x101), 1u) == NULL);
    assert(fake_find_key(&environment.private_keys,
                         UINT64_C(0x252), 2u) != NULL);
    assert(vd_attach_auth_store_get_metadata(&store, &metadata) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(metadata.revision == 2u);
    assert(metadata.service_generation == 2u);

    environment.monotonic.fail_load = 1;
    assert(vd_attach_auth_store_recover(&store) ==
           VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE);
    assert(all_zero(&store, sizeof(store)));
    assert(environment.persistence.active_handles == 0u);
}

static void test_stale_revision_and_generation(void) {
    FakeEnvironment environment;
    VdAttachAuthStore store;
    VdAttachAuthStore recovered;
    uint8_t stale[FAKE_BYTES];
    size_t stale_size;
    uint64_t generation;

    provisioned(&environment, &store);
    stale_size = environment.persistence.current.size;
    memcpy(stale, environment.persistence.current.data, stale_size);
    assert(vd_attach_auth_store_reserve_service_generation(
               &store, &generation) == VD_ATTACH_AUTH_STORE_OK);
    assert(generation == 2u);
    assert(environment.monotonic.floor == 2u);
    set_current(&environment, stale, stale_size);
    memset(&recovered, 0xa5, sizeof(recovered));
    assert(fake_store_init(&environment, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_STALE);
    assert(all_zero(&recovered, sizeof(recovered)));
}

static void test_revoked_peers_and_restart_generation(void) {
    FakeEnvironment environment;
    VdAttachAuthStore store;
    VdAttachAuthStore restarted;
    VdAttachAuthPublicKey peer;
    VdAttachAuthPublicKey found;
    uint64_t first;
    uint64_t second;

    provisioned(&environment, &store);
    peer = make_peer(UINT64_C(0x303), 7u);
    assert(vd_attach_auth_store_allow_peer(&store, &peer) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_revoke_peer(
               &store, peer.key_id, peer.generation) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_lookup_peer(
               &store, peer.key_id, peer.generation, &found) ==
           VD_ATTACH_AUTH_ERROR_REVOKED);
    assert(vd_attach_auth_store_allow_peer(&store, &peer) ==
           VD_ATTACH_AUTH_ERROR_REVOKED);
    assert(vd_attach_auth_store_lookup_peer(
               &store, peer.key_id, peer.generation + 1u, &found) ==
           VD_ATTACH_AUTH_ERROR_NOT_ALLOWED);

    assert(vd_attach_auth_store_reserve_service_generation(
               &store, &first) == VD_ATTACH_AUTH_STORE_OK);
    assert(fake_store_init(&environment, &restarted) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_lookup_peer(
               &restarted, peer.key_id, peer.generation, &found) ==
           VD_ATTACH_AUTH_ERROR_REVOKED);
    assert(vd_attach_auth_store_reserve_service_generation(
               &restarted, &second) == VD_ATTACH_AUTH_STORE_OK);
    assert(second == first + 1u);
}

static void test_private_public_mismatch_rejected(void) {
    FakeEnvironment environment;
    VdAttachAuthStore store;
    VdAttachAuthStore recovered;
    FakeKey *key;

    provisioned(&environment, &store);
    key = fake_find_key(&environment.private_keys,
                        UINT64_C(0x101), 1u);
    assert(key != NULL);
    key->public_key[0] ^= 1u;
    memset(&recovered, 0xa5, sizeof(recovered));
    assert(fake_store_init(&environment, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY);
    assert(all_zero(&recovered, sizeof(recovered)));
}

static void test_uninstall_restore_and_deinit(void) {
    FakeEnvironment environment;
    VdAttachAuthStore store;
    VdAttachAuthStore restarted;
    VdAttachAuthStore stale_store;
    VdAttachAuthStoreMetadata metadata;
    uint8_t old_state[FAKE_BYTES];
    uint8_t tombstone[FAKE_BYTES];
    size_t old_size;
    size_t tombstone_size;

    provisioned(&environment, &store);
    old_size = environment.persistence.current.size;
    memcpy(old_state, environment.persistence.current.data, old_size);
    assert(vd_attach_auth_store_uninstall(&store) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(fake_find_key(&environment.private_keys,
                         UINT64_C(0x101), 1u) == NULL);
    tombstone_size = environment.persistence.current.size;
    memcpy(tombstone, environment.persistence.current.data,
           tombstone_size);

    vd_attach_auth_store_deinit(&store);
    assert(all_zero(&store, sizeof(store)));
    vd_attach_auth_store_deinit(&store);
    assert(all_zero(&store, sizeof(store)));
    assert(environment.persistence.current.present);

    assert(fake_store_init(&environment, &restarted) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_get_metadata(
               &restarted, &metadata) == VD_ATTACH_AUTH_STORE_OK);
    assert(!metadata.provisioned);

    set_current(&environment, old_state, old_size);
    memset(&stale_store, 0xa5, sizeof(stale_store));
    assert(fake_store_init(&environment, &stale_store) ==
           VD_ATTACH_AUTH_STORE_ERROR_STALE);
    assert(all_zero(&stale_store, sizeof(stale_store)));

    set_current(&environment, tombstone, tombstone_size);
    assert(fake_store_init(&environment, &restarted) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_provision(
               &restarted, UINT64_C(0x404), 1u) ==
           VD_ATTACH_AUTH_STORE_OK);
}

static void test_vita_sentinel(void) {
    VdAttachAuthStore store;
    memset(&store, 0xa5, sizeof(store));
    assert(vd_attach_auth_store_vita_init(&store) ==
           VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE);
    assert(all_zero(&store, sizeof(store)));
}

int main(void) {
    test_capability_rejection_and_full_wipe();
    test_public_only_metadata_and_signing();
    test_non_destructive_deinit();
    test_corrupt_truncated_oversized_unknown_and_cleanup();
    test_interrupted_rotation_recovery();
    test_rotation_and_recover_failure_wipe();
    test_stale_revision_and_generation();
    test_revoked_peers_and_restart_generation();
    test_private_public_mismatch_rejected();
    test_uninstall_restore_and_deinit();
    test_vita_sentinel();
    puts("attach authentication store tests passed");
    return 0;
}

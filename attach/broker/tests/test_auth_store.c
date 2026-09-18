#include "vitadebug_attach_auth_store.h"

#include <monocypher.h>
#include <monocypher-ed25519.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FAKE_FILE_CAPACITY (VD_ATTACH_AUTH_STORE_MAX_BYTES + 32u)
#define FAKE_HANDLE_COUNT 4u

typedef struct FakeFile {
    uint8_t data[FAKE_FILE_CAPACITY];
    size_t size;
    int present;
    int regular;
    int no_link;
} FakeFile;

typedef struct FakeHandle {
    FakeFile *file;
    size_t offset;
    int active;
    int writable;
} FakeHandle;

typedef struct FakeFs {
    FakeFile primary;
    FakeFile temporary;
    FakeHandle handles[FAKE_HANDLE_COUNT];
    uint64_t floor;
    size_t read_chunk;
    size_t write_chunk;
    unsigned int active_handles;
    unsigned int sequence;
    unsigned int sync_sequence;
    unsigned int replace_sequence;
    unsigned int parent_sync_sequence;
    unsigned int floor_sequence;
    int fail_read;
    int fail_close;
    int fail_replace;
    int fail_floor;
} FakeFs;

static FakeFile *fake_file(FakeFs *fs, const char *path) {
    if (strcmp(path, VD_ATTACH_AUTH_STORE_VITA_PATH) == 0) {
        return &fs->primary;
    }
    if (strcmp(path, VD_ATTACH_AUTH_STORE_VITA_TEMP_PATH) == 0) {
        return &fs->temporary;
    }
    return NULL;
}

static FakeHandle *fake_new_handle(FakeFs *fs) {
    size_t index;
    for (index = 0u; index < FAKE_HANDLE_COUNT; ++index) {
        if (!fs->handles[index].active) {
            memset(&fs->handles[index], 0,
                   sizeof(fs->handles[index]));
            fs->handles[index].active = 1;
            ++fs->active_handles;
            return &fs->handles[index];
        }
    }
    return NULL;
}

static int fake_open_read(void *context, const char *path,
                          void **handle) {
    FakeFs *fs = (FakeFs *)context;
    FakeFile *file = fake_file(fs, path);
    FakeHandle *opened;
    *handle = NULL;
    if (file == NULL || !file->present) {
        return VD_ATTACH_AUTH_STORE_IO_NOT_FOUND;
    }
    if (!file->regular || !file->no_link) {
        return -1;
    }
    opened = fake_new_handle(fs);
    if (opened == NULL) {
        return -1;
    }
    opened->file = file;
    *handle = opened;
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int fake_create_new(void *context, const char *path,
                           void **handle) {
    FakeFs *fs = (FakeFs *)context;
    FakeFile *file = fake_file(fs, path);
    FakeHandle *opened;
    *handle = NULL;
    if (file == NULL || file->present) {
        return -1;
    }
    opened = fake_new_handle(fs);
    if (opened == NULL) {
        return -1;
    }
    memset(file, 0, sizeof(*file));
    file->present = 1;
    file->regular = 1;
    file->no_link = 1;
    opened->file = file;
    opened->writable = 1;
    *handle = opened;
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int fake_read(void *context, void *handle, uint8_t *data,
                     size_t capacity, size_t *read_size) {
    FakeFs *fs = (FakeFs *)context;
    FakeHandle *opened = (FakeHandle *)handle;
    size_t remaining;
    size_t amount;
    *read_size = 0u;
    if (fs->fail_read || !opened->active || opened->writable) {
        return -1;
    }
    remaining = opened->file->size - opened->offset;
    amount = capacity < remaining ? capacity : remaining;
    if (fs->read_chunk != 0u && amount > fs->read_chunk) {
        amount = fs->read_chunk;
    }
    memcpy(data, opened->file->data + opened->offset, amount);
    opened->offset += amount;
    *read_size = amount;
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int fake_write(void *context, void *handle,
                      const uint8_t *data, size_t size,
                      size_t *write_size) {
    FakeFs *fs = (FakeFs *)context;
    FakeHandle *opened = (FakeHandle *)handle;
    size_t amount = size;
    *write_size = 0u;
    if (!opened->active || !opened->writable) {
        return -1;
    }
    if (fs->write_chunk != 0u && amount > fs->write_chunk) {
        amount = fs->write_chunk;
    }
    if (amount > FAKE_FILE_CAPACITY - opened->offset) {
        return -1;
    }
    memcpy(opened->file->data + opened->offset, data, amount);
    opened->offset += amount;
    if (opened->offset > opened->file->size) {
        opened->file->size = opened->offset;
    }
    *write_size = amount;
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int fake_sync(void *context, void *handle) {
    FakeFs *fs = (FakeFs *)context;
    FakeHandle *opened = (FakeHandle *)handle;
    if (!opened->active) {
        return -1;
    }
    fs->sync_sequence = ++fs->sequence;
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int fake_close(void *context, void *handle) {
    FakeFs *fs = (FakeFs *)context;
    FakeHandle *opened = (FakeHandle *)handle;
    int result = VD_ATTACH_AUTH_STORE_IO_OK;
    if (!opened->active) {
        return -1;
    }
    opened->active = 0;
    --fs->active_handles;
    if (fs->fail_close) {
        fs->fail_close = 0;
        result = -1;
    }
    return result;
}

static int fake_replace(void *context, const char *from,
                        const char *to) {
    FakeFs *fs = (FakeFs *)context;
    FakeFile *source = fake_file(fs, from);
    FakeFile *destination = fake_file(fs, to);
    fs->replace_sequence = ++fs->sequence;
    if (fs->fail_replace) {
        return -1;
    }
    if (source == NULL || destination == NULL || !source->present ||
        !source->regular || !source->no_link) {
        return -1;
    }
    *destination = *source;
    memset(source, 0, sizeof(*source));
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int fake_remove(void *context, const char *path) {
    FakeFs *fs = (FakeFs *)context;
    FakeFile *file = fake_file(fs, path);
    if (file == NULL || !file->present) {
        return VD_ATTACH_AUTH_STORE_IO_NOT_FOUND;
    }
    if (!file->regular || !file->no_link) {
        return -1;
    }
    memset(file, 0, sizeof(*file));
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int fake_sync_parent(void *context, const char *path) {
    FakeFs *fs = (FakeFs *)context;
    (void)path;
    fs->parent_sync_sequence = ++fs->sequence;
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int fake_load_floor(void *context, uint64_t *revision) {
    FakeFs *fs = (FakeFs *)context;
    *revision = fs->floor;
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static int fake_advance_floor(void *context, uint64_t revision) {
    FakeFs *fs = (FakeFs *)context;
    fs->floor_sequence = ++fs->sequence;
    if (fs->fail_floor || revision < fs->floor) {
        return -1;
    }
    fs->floor = revision;
    return VD_ATTACH_AUTH_STORE_IO_OK;
}

static void fake_reset(FakeFs *fs) {
    memset(fs, 0, sizeof(*fs));
    fs->read_chunk = 7u;
    fs->write_chunk = 11u;
}

static int fake_init_store(FakeFs *fs, VdAttachAuthStore *store) {
    VdAttachAuthStoreFileOps files;
    VdAttachAuthStoreRollbackOps rollback;
    memset(&files, 0, sizeof(files));
    files.context = fs;
    files.open_read_regular_no_follow = fake_open_read;
    files.create_new_regular_no_follow = fake_create_new;
    files.read = fake_read;
    files.write = fake_write;
    files.sync = fake_sync;
    files.close = fake_close;
    files.replace_atomic = fake_replace;
    files.remove_regular_no_follow = fake_remove;
    files.sync_parent = fake_sync_parent;
    memset(&rollback, 0, sizeof(rollback));
    rollback.context = fs;
    rollback.load_floor = fake_load_floor;
    rollback.advance_floor = fake_advance_floor;
    return vd_attach_auth_store_init(store, &files, &rollback);
}

static void fill_seed(uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES],
                      uint8_t first) {
    size_t index;
    for (index = 0u; index < VD_ATTACH_AUTH_KEY_BYTES; ++index) {
        seed[index] = (uint8_t)(first + (uint8_t)index);
    }
}

static VdAttachAuthPublicKey make_peer(uint64_t key_id,
                                       uint64_t generation,
                                       uint8_t seed_first) {
    VdAttachAuthPublicKey peer;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t secret_key[64];
    memset(&peer, 0, sizeof(peer));
    fill_seed(seed, seed_first);
    crypto_ed25519_key_pair(secret_key, peer.public_key, seed);
    peer.key_id = key_id;
    peer.generation = generation;
    peer.status = VD_ATTACH_AUTH_KEY_ACTIVE;
    crypto_wipe(seed, sizeof(seed));
    crypto_wipe(secret_key, sizeof(secret_key));
    return peer;
}

static void make_provisioned(FakeFs *fs, VdAttachAuthStore *store,
                             uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES]) {
    fake_reset(fs);
    fill_seed(seed, 1u);
    assert(fake_init_store(fs, store) == VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_provision(
               store, UINT64_C(0x101), 1u, seed) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(fs->active_handles == 0u);
    assert(fs->sync_sequence < fs->replace_sequence);
    assert(fs->replace_sequence < fs->parent_sync_sequence);
    assert(fs->parent_sync_sequence < fs->floor_sequence);
}

static void set_primary(FakeFs *fs, const uint8_t *data, size_t size) {
    memset(&fs->primary, 0, sizeof(fs->primary));
    memcpy(fs->primary.data, data, size);
    fs->primary.size = size;
    fs->primary.present = 1;
    fs->primary.regular = 1;
    fs->primary.no_link = 1;
    memset(&fs->temporary, 0, sizeof(fs->temporary));
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

static int contains_bytes(const uint8_t *haystack, size_t haystack_size,
                          const uint8_t *needle, size_t needle_size) {
    size_t offset;
    if (needle_size > haystack_size) {
        return 0;
    }
    for (offset = 0u; offset <= haystack_size - needle_size;
         ++offset) {
        if (memcmp(haystack + offset, needle, needle_size) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_strict_bounded_parsing_and_cleanup(void) {
    FakeFs fs;
    VdAttachAuthStore store;
    VdAttachAuthStore recovered;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t valid[FAKE_FILE_CAPACITY];
    size_t valid_size;

    make_provisioned(&fs, &store, seed);
    valid_size = fs.primary.size;
    memcpy(valid, fs.primary.data, valid_size);

    fs.primary.data[valid_size - 1u] ^= 1u;
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_MALFORMED);
    assert(fs.active_handles == 0u);

    set_primary(&fs, valid, valid_size - 1u);
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_MALFORMED);
    assert(fs.active_handles == 0u);

    memset(fs.primary.data, 0x5a,
           VD_ATTACH_AUTH_STORE_MAX_BYTES + 1u);
    fs.primary.size = VD_ATTACH_AUTH_STORE_MAX_BYTES + 1u;
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_MALFORMED);
    assert(fs.active_handles == 0u);

    set_primary(&fs, valid, valid_size);
    store_u32(fs.primary.data + 40u, 99u);
    refresh_digest(fs.primary.data, fs.primary.size);
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_MALFORMED);
    assert(fs.active_handles == 0u);

    set_primary(&fs, valid, valid_size);
    fs.primary.regular = 0;
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_IO);
    assert(fs.active_handles == 0u);

    set_primary(&fs, valid, valid_size);
    fs.primary.no_link = 0;
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_IO);
    assert(fs.active_handles == 0u);

    set_primary(&fs, valid, valid_size);
    fs.fail_read = 1;
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_IO);
    assert(fs.active_handles == 0u);
    fs.fail_read = 0;

    set_primary(&fs, valid, valid_size);
    fs.fail_close = 1;
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_IO);
    assert(fs.active_handles == 0u);
}

static void test_interrupted_rotation_recovery(void) {
    FakeFs fs;
    VdAttachAuthStore store;
    VdAttachAuthStore recovered;
    VdAttachAuthPublicKey local;
    uint8_t old_seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t new_seed[VD_ATTACH_AUTH_KEY_BYTES];

    make_provisioned(&fs, &store, old_seed);
    fill_seed(new_seed, 65u);
    fs.fail_replace = 1;
    assert(vd_attach_auth_store_rotate(
               &store, UINT64_C(0x101), 1u,
               UINT64_C(0x202), 2u, new_seed) ==
           VD_ATTACH_AUTH_STORE_ERROR_IO);
    assert(fs.primary.present && fs.temporary.present);
    assert(fs.active_handles == 0u);

    fs.fail_replace = 0;
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(!fs.temporary.present);
    assert(fs.floor == 2u);
    assert(vd_attach_auth_store_load_local_public(
               &recovered, &local) == VD_ATTACH_AUTH_STORE_OK);
    assert(local.key_id == UINT64_C(0x202));
    assert(local.generation == 2u);
    assert(fs.active_handles == 0u);
}

static void test_stale_revision_rejected(void) {
    FakeFs fs;
    VdAttachAuthStore store;
    VdAttachAuthStore recovered;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t new_seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t stale[FAKE_FILE_CAPACITY];
    size_t stale_size;

    make_provisioned(&fs, &store, seed);
    stale_size = fs.primary.size;
    memcpy(stale, fs.primary.data, stale_size);
    fill_seed(new_seed, 90u);
    assert(vd_attach_auth_store_rotate(
               &store, UINT64_C(0x101), 1u,
               UINT64_C(0x303), 2u, new_seed) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(fs.floor == 2u);
    set_primary(&fs, stale, stale_size);
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_STALE);
    assert(fs.active_handles == 0u);
}

static void test_exact_allowlist_and_revocation(void) {
    FakeFs fs;
    VdAttachAuthStore store;
    VdAttachAuthStore recovered;
    VdAttachAuthPublicKey peer;
    VdAttachAuthPublicKey next_generation;
    VdAttachAuthPublicKey found;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];

    make_provisioned(&fs, &store, seed);
    peer = make_peer(UINT64_C(0x404), 7u, 100u);
    next_generation = make_peer(UINT64_C(0x404), 8u, 110u);
    assert(vd_attach_auth_store_allow_peer(&store, &peer) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_lookup_peer(
               &store, peer.key_id, peer.generation, &found) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(memcmp(found.public_key, peer.public_key,
                  sizeof(peer.public_key)) == 0);
    assert(vd_attach_auth_store_lookup_peer(
               &store, peer.key_id, peer.generation + 1u, &found) ==
           VD_ATTACH_AUTH_ERROR_NOT_ALLOWED);
    assert(vd_attach_auth_store_revoke_peer(
               &store, peer.key_id, peer.generation) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_lookup_peer(
               &store, peer.key_id, peer.generation, &found) ==
           VD_ATTACH_AUTH_ERROR_REVOKED);
    assert(vd_attach_auth_store_allow_peer(&store, &peer) ==
           VD_ATTACH_AUTH_ERROR_REVOKED);
    assert(vd_attach_auth_store_allow_peer(
               &store, &next_generation) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_revoke_peer(
               &store, peer.key_id, 999u) ==
           VD_ATTACH_AUTH_ERROR_NOT_ALLOWED);
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_lookup_peer(
               &recovered, peer.key_id, peer.generation, &found) ==
           VD_ATTACH_AUTH_ERROR_REVOKED);
    assert(vd_attach_auth_store_lookup_peer(
               &recovered, next_generation.key_id,
               next_generation.generation, &found) ==
           VD_ATTACH_AUTH_STORE_OK);
}

static void test_rotation_and_signing(void) {
    static const uint8_t message[] = "store-signature";
    FakeFs fs;
    VdAttachAuthStore store;
    VdAttachAuthStoreMetadata before;
    VdAttachAuthStoreMetadata after;
    VdAttachAuthPublicKey local;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t next_seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES];

    make_provisioned(&fs, &store, seed);
    assert(vd_attach_auth_store_get_metadata(&store, &before) ==
           VD_ATTACH_AUTH_STORE_OK);
    fill_seed(next_seed, 150u);
    assert(vd_attach_auth_store_rotate(
               &store, UINT64_C(0x101), 1u,
               UINT64_C(0x505), 3u, next_seed) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_get_metadata(&store, &after) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(after.revision == before.revision + 1u);
    assert(after.service_generation ==
           before.service_generation + 1u);
    assert(vd_attach_auth_store_load_local_public(&store, &local) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(local.key_id == UINT64_C(0x505));
    assert(vd_attach_auth_store_sign_local(
               &store, message, sizeof(message) - 1u, signature) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(crypto_ed25519_check(
               signature, local.public_key, message,
               sizeof(message) - 1u) == 0);
    assert(vd_attach_auth_store_rotate(
               &store, UINT64_C(0x101), 1u,
               UINT64_C(0x606), 4u, seed) ==
           VD_ATTACH_AUTH_STORE_ERROR_CONFLICT);
}

static void test_uninstall_restore_and_zeroization(void) {
    FakeFs fs;
    VdAttachAuthStore store;
    VdAttachAuthStore recovered;
    VdAttachAuthStoreMetadata metadata;
    VdAttachAuthPublicKey local;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t restored[FAKE_FILE_CAPACITY];
    uint8_t tombstone[FAKE_FILE_CAPACITY];
    size_t restored_size;
    size_t tombstone_size;
    uint64_t tombstone_floor;

    make_provisioned(&fs, &store, seed);
    restored_size = fs.primary.size;
    memcpy(restored, fs.primary.data, restored_size);
    assert(contains_bytes(store.opaque, sizeof(store.opaque), seed,
                          sizeof(seed)));
    assert(vd_attach_auth_store_uninstall(&store) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_load_local_public(&store, &local) ==
           VD_ATTACH_AUTH_STORE_ERROR_NOT_PROVISIONED);
    assert(!contains_bytes(store.opaque, sizeof(store.opaque), seed,
                           sizeof(seed)));
    assert(!contains_bytes(fs.primary.data, fs.primary.size, seed,
                           sizeof(seed)));
    tombstone_size = fs.primary.size;
    memcpy(tombstone, fs.primary.data, tombstone_size);
    tombstone_floor = fs.floor;

    set_primary(&fs, restored, restored_size);
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_ERROR_STALE);

    set_primary(&fs, tombstone, tombstone_size);
    assert(fake_init_store(&fs, &recovered) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_get_metadata(&recovered, &metadata) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(!metadata.provisioned);
    fill_seed(seed, 200u);
    assert(vd_attach_auth_store_provision(
               &recovered, UINT64_C(0x707), 1u, seed) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(fs.floor == tombstone_floor + 1u);
    assert(vd_attach_auth_store_get_metadata(&recovered, &metadata) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(metadata.provisioned);
    assert(metadata.service_generation >= 3u);
}

static void test_floor_failure_forces_recovery(void) {
    FakeFs fs;
    VdAttachAuthStore store;
    VdAttachAuthPublicKey local;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];

    fake_reset(&fs);
    fill_seed(seed, 33u);
    assert(fake_init_store(&fs, &store) == VD_ATTACH_AUTH_STORE_OK);
    fs.fail_floor = 1;
    assert(vd_attach_auth_store_provision(
               &store, UINT64_C(0x808), 1u, seed) ==
           VD_ATTACH_AUTH_STORE_ERROR_IO);
    assert(vd_attach_auth_store_load_local_public(&store, &local) ==
           VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE);
    fs.fail_floor = 0;
    assert(vd_attach_auth_store_recover(&store) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_load_local_public(&store, &local) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(fs.floor == 1u);
}

static void test_monotonic_restart_generation(void) {
    FakeFs fs;
    VdAttachAuthStore store;
    VdAttachAuthStore restarted;
    VdAttachAuthStore rolled_back;
    VdAttachAuthStoreMetadata metadata;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t stale_generation[FAKE_FILE_CAPACITY];
    size_t stale_generation_size;
    uint64_t first_generation = 0u;
    uint64_t second_generation = 0u;

    make_provisioned(&fs, &store, seed);
    stale_generation_size = fs.primary.size;
    memcpy(stale_generation, fs.primary.data,
           stale_generation_size);

    assert(vd_attach_auth_store_reserve_service_generation(
               &store, &first_generation) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(first_generation == 2u);
    assert(fake_init_store(&fs, &restarted) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(vd_attach_auth_store_get_metadata(
               &restarted, &metadata) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(metadata.service_generation == first_generation);
    assert(vd_attach_auth_store_reserve_service_generation(
               &restarted, &second_generation) ==
           VD_ATTACH_AUTH_STORE_OK);
    assert(second_generation == first_generation + 1u);
    assert(fs.floor == 3u);

    set_primary(&fs, stale_generation, stale_generation_size);
    assert(fake_init_store(&fs, &rolled_back) ==
           VD_ATTACH_AUTH_STORE_ERROR_STALE);
    assert(fs.active_handles == 0u);
}

static void test_vita_adapter_fails_closed_on_host(void) {
    VdAttachAuthStore store;
    VdAttachAuthStoreVitaAssurance assurance;
    memset(&assurance, 0, sizeof(assurance));
    assert(vd_attach_auth_store_vita_init(&store, &assurance) ==
           VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE);
}

int main(void) {
    test_strict_bounded_parsing_and_cleanup();
    test_interrupted_rotation_recovery();
    test_stale_revision_rejected();
    test_exact_allowlist_and_revocation();
    test_rotation_and_signing();
    test_uninstall_restore_and_zeroization();
    test_floor_failure_forces_recovery();
    test_monotonic_restart_generation();
    test_vita_adapter_fails_closed_on_host();
    puts("attach authentication store tests passed");
    return 0;
}

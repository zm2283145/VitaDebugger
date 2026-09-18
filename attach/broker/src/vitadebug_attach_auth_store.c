#include "vitadebug_attach_auth_store.h"

#include <monocypher.h>

#include <limits.h>
#include <string.h>

#define VD_STORE_HEADER_BYTES 40u
#define VD_STORE_TLV_HEADER_BYTES 8u
#define VD_STORE_KEY_BYTES 56u
#define VD_STORE_DIGEST_BYTES 32u
#define VD_STORE_FIELD_LOCAL 1u
#define VD_STORE_FIELD_RETIRED 2u
#define VD_STORE_FIELD_PEER 3u
#define VD_STORE_FLAG_TOMBSTONE 1u
#define VD_STORE_RUNTIME_MAGIC UINT64_C(0x5644415352543032)

static const uint8_t vd_store_magic[8] = {
    'V', 'D', 'A', 'S', 'K', 'S', '0', '2'
};

typedef struct VdStoreState {
    uint64_t revision;
    uint64_t service_generation;
    VdAttachAuthPublicKey local;
    VdAttachAuthPublicKey retired;
    VdAttachAuthPublicKey peers[VD_ATTACH_AUTH_STORE_MAX_PEERS];
    size_t peer_count;
    int provisioned;
    int has_retired;
} VdStoreState;

typedef struct VdStoreImpl {
    VdAttachAuthStorePersistenceOps persistence;
    VdAttachAuthStoreMonotonicOps monotonic;
    VdAttachAuthStorePrivateKeyOps private_keys;
    VdStoreState state;
    uint64_t runtime_magic;
    int available;
} VdStoreImpl;

typedef struct VdLoadedState {
    VdStoreState state;
    void *handle;
    int present;
    int result;
} VdLoadedState;

typedef char vd_store_context_size_check[
    sizeof(VdStoreImpl) <= VD_ATTACH_AUTH_STORE_CONTEXT_BYTES ? 1 : -1];

static VdStoreImpl *vd_impl(VdAttachAuthStore *store) {
    return (VdStoreImpl *)(void *)store->opaque;
}

static const VdStoreImpl *vd_const_impl(
    const VdAttachAuthStore *store) {
    return (const VdStoreImpl *)(const void *)store->opaque;
}

static uint32_t vd_load_u32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t vd_load_u64(const uint8_t *data) {
    return (uint64_t)vd_load_u32(data) |
           ((uint64_t)vd_load_u32(data + 4u) << 32);
}

static void vd_store_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void vd_store_u64(uint8_t *data, uint64_t value) {
    vd_store_u32(data, (uint32_t)value);
    vd_store_u32(data + 4u, (uint32_t)(value >> 32));
}

static int vd_bytes_nonzero(const uint8_t *data, size_t size) {
    size_t index;
    uint8_t combined = 0u;
    for (index = 0u; index < size; ++index) {
        combined |= data[index];
    }
    return combined != 0u;
}

static int vd_bytes_equal(const uint8_t *left, const uint8_t *right,
                          size_t size) {
    size_t index;
    uint8_t difference = 0u;
    for (index = 0u; index < size; ++index) {
        difference |= (uint8_t)(left[index] ^ right[index]);
    }
    return difference == 0u;
}

static int vd_key_valid(const VdAttachAuthPublicKey *key,
                        int allow_revoked) {
    return key != NULL && key->key_id != 0u &&
           key->generation != 0u &&
           (key->status == VD_ATTACH_AUTH_KEY_ACTIVE ||
            (allow_revoked &&
             key->status == VD_ATTACH_AUTH_KEY_REVOKED)) &&
           vd_bytes_nonzero(key->public_key,
                            sizeof(key->public_key));
}

static int vd_state_valid(const VdStoreState *state) {
    size_t left;
    size_t right;
    if (state->revision == 0u ||
        state->service_generation == 0u ||
        state->service_generation > state->revision ||
        state->peer_count > VD_ATTACH_AUTH_STORE_MAX_PEERS) {
        return 0;
    }
    if (state->provisioned) {
        if (!vd_key_valid(&state->local, 0) ||
            state->local.status != VD_ATTACH_AUTH_KEY_ACTIVE) {
            return 0;
        }
    } else if (state->peer_count != 0u) {
        return 0;
    }
    if (state->has_retired &&
        (!vd_key_valid(&state->retired, 0) ||
         state->retired.status != VD_ATTACH_AUTH_KEY_ACTIVE)) {
        return 0;
    }
    for (left = 0u; left < state->peer_count; ++left) {
        if (!vd_key_valid(&state->peers[left], 1)) {
            return 0;
        }
        for (right = left + 1u; right < state->peer_count; ++right) {
            if (state->peers[left].key_id ==
                    state->peers[right].key_id &&
                state->peers[left].generation ==
                    state->peers[right].generation) {
                return 0;
            }
        }
    }
    return 1;
}

static size_t vd_write_key(uint8_t *output,
                           const VdAttachAuthPublicKey *key) {
    vd_store_u64(output, key->key_id);
    vd_store_u64(output + 8u, key->generation);
    vd_store_u32(output + 16u, key->status);
    vd_store_u32(output + 20u, 0u);
    memcpy(output + 24u, key->public_key, sizeof(key->public_key));
    return VD_STORE_KEY_BYTES;
}

static int vd_read_key(const uint8_t *input, size_t size,
                       VdAttachAuthPublicKey *key) {
    if (size != VD_STORE_KEY_BYTES ||
        vd_load_u32(input + 20u) != 0u) {
        return 0;
    }
    memset(key, 0, sizeof(*key));
    key->key_id = vd_load_u64(input);
    key->generation = vd_load_u64(input + 8u);
    key->status = vd_load_u32(input + 16u);
    memcpy(key->public_key, input + 24u, sizeof(key->public_key));
    return 1;
}

static void vd_write_field_header(uint8_t *output, uint32_t type) {
    vd_store_u32(output, type);
    vd_store_u32(output + 4u, VD_STORE_KEY_BYTES);
}

static int vd_serialize(const VdStoreState *state, uint8_t *output,
                        size_t capacity, size_t *output_size) {
    uint8_t digest[VD_STORE_DIGEST_BYTES];
    uint32_t field_count;
    size_t total;
    size_t offset;
    size_t index;

    if (!vd_state_valid(state)) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    field_count = (uint32_t)(state->peer_count +
                             (state->provisioned ? 1u : 0u) +
                             (state->has_retired ? 1u : 0u));
    total = VD_STORE_HEADER_BYTES + VD_STORE_DIGEST_BYTES +
            (size_t)field_count *
                (VD_STORE_TLV_HEADER_BYTES + VD_STORE_KEY_BYTES);
    if (total > capacity || total > VD_ATTACH_AUTH_STORE_MAX_BYTES ||
        total > UINT32_MAX) {
        return VD_ATTACH_AUTH_STORE_ERROR_LIMIT;
    }
    memset(output, 0, total);
    memcpy(output, vd_store_magic, sizeof(vd_store_magic));
    vd_store_u32(output + 8u, VD_ATTACH_AUTH_STORE_SCHEMA);
    vd_store_u32(output + 12u, (uint32_t)total);
    vd_store_u64(output + 16u, state->revision);
    vd_store_u64(output + 24u, state->service_generation);
    vd_store_u32(output + 32u,
                 state->provisioned ? 0u
                                    : VD_STORE_FLAG_TOMBSTONE);
    vd_store_u32(output + 36u, field_count);
    offset = VD_STORE_HEADER_BYTES;
    if (state->provisioned) {
        vd_write_field_header(output + offset, VD_STORE_FIELD_LOCAL);
        offset += VD_STORE_TLV_HEADER_BYTES;
        offset += vd_write_key(output + offset, &state->local);
    }
    if (state->has_retired) {
        vd_write_field_header(output + offset, VD_STORE_FIELD_RETIRED);
        offset += VD_STORE_TLV_HEADER_BYTES;
        offset += vd_write_key(output + offset, &state->retired);
    }
    for (index = 0u; index < state->peer_count; ++index) {
        vd_write_field_header(output + offset, VD_STORE_FIELD_PEER);
        offset += VD_STORE_TLV_HEADER_BYTES;
        offset += vd_write_key(output + offset, &state->peers[index]);
    }
    crypto_blake2b(digest, sizeof(digest), output, offset);
    memcpy(output + offset, digest, sizeof(digest));
    offset += sizeof(digest);
    crypto_wipe(digest, sizeof(digest));
    *output_size = offset;
    return VD_ATTACH_AUTH_STORE_OK;
}

static int vd_parse(const uint8_t *input, size_t size,
                    VdStoreState *state) {
    uint8_t digest[VD_STORE_DIGEST_BYTES];
    uint32_t flags;
    uint32_t field_count;
    uint32_t type;
    uint32_t field_size;
    size_t digest_offset;
    size_t offset;
    size_t index;
    int local_seen = 0;
    int retired_seen = 0;
    int peer_seen = 0;
    int valid = 0;

    memset(state, 0, sizeof(*state));
    if (size < VD_STORE_HEADER_BYTES + VD_STORE_DIGEST_BYTES ||
        size > VD_ATTACH_AUTH_STORE_MAX_BYTES ||
        !vd_bytes_equal(input, vd_store_magic,
                        sizeof(vd_store_magic)) ||
        vd_load_u32(input + 8u) != VD_ATTACH_AUTH_STORE_SCHEMA ||
        vd_load_u32(input + 12u) != size) {
        return VD_ATTACH_AUTH_STORE_ERROR_MALFORMED;
    }
    digest_offset = size - VD_STORE_DIGEST_BYTES;
    crypto_blake2b(digest, sizeof(digest), input, digest_offset);
    if (!vd_bytes_equal(digest, input + digest_offset,
                        sizeof(digest))) {
        goto cleanup;
    }
    state->revision = vd_load_u64(input + 16u);
    state->service_generation = vd_load_u64(input + 24u);
    flags = vd_load_u32(input + 32u);
    field_count = vd_load_u32(input + 36u);
    if ((flags & ~VD_STORE_FLAG_TOMBSTONE) != 0u ||
        field_count > VD_ATTACH_AUTH_STORE_MAX_PEERS + 2u) {
        goto cleanup;
    }
    state->provisioned =
        (flags & VD_STORE_FLAG_TOMBSTONE) == 0u;
    offset = VD_STORE_HEADER_BYTES;
    for (index = 0u; index < field_count; ++index) {
        if (offset > digest_offset ||
            digest_offset - offset < VD_STORE_TLV_HEADER_BYTES) {
            goto cleanup;
        }
        type = vd_load_u32(input + offset);
        field_size = vd_load_u32(input + offset + 4u);
        offset += VD_STORE_TLV_HEADER_BYTES;
        if (field_size != VD_STORE_KEY_BYTES ||
            (size_t)field_size > digest_offset - offset) {
            goto cleanup;
        }
        if (type == VD_STORE_FIELD_LOCAL && !local_seen &&
            !retired_seen && !peer_seen && state->provisioned) {
            local_seen = vd_read_key(input + offset, field_size,
                                     &state->local);
            if (!local_seen) {
                goto cleanup;
            }
        } else if (type == VD_STORE_FIELD_RETIRED &&
                   !retired_seen && !peer_seen &&
                   (!state->provisioned || local_seen)) {
            retired_seen = vd_read_key(input + offset, field_size,
                                       &state->retired);
            state->has_retired = retired_seen;
            if (!retired_seen) {
                goto cleanup;
            }
        } else if (type == VD_STORE_FIELD_PEER &&
                   state->provisioned && local_seen &&
                   state->peer_count <
                       VD_ATTACH_AUTH_STORE_MAX_PEERS) {
            peer_seen = 1;
            if (!vd_read_key(input + offset, field_size,
                             &state->peers[state->peer_count])) {
                goto cleanup;
            }
            ++state->peer_count;
        } else {
            goto cleanup;
        }
        offset += field_size;
    }
    if (offset != digest_offset ||
        (state->provisioned && !local_seen) ||
        !vd_state_valid(state)) {
        goto cleanup;
    }
    valid = 1;

cleanup:
    crypto_wipe(digest, sizeof(digest));
    if (!valid) {
        crypto_wipe(state, sizeof(*state));
        return VD_ATTACH_AUTH_STORE_ERROR_MALFORMED;
    }
    return VD_ATTACH_AUTH_STORE_OK;
}

static int vd_persistence_valid(
    const VdAttachAuthStorePersistenceOps *ops) {
    const uint32_t required =
        VD_ATTACH_AUTH_PERSISTENCE_HANDLE_BOUND |
        VD_ATTACH_AUTH_PERSISTENCE_DURABLE_COMMIT |
        VD_ATTACH_AUTH_PERSISTENCE_RECOVERABLE_STAGE;
    return ops != NULL && ops->trust_domain != NULL &&
           (ops->capabilities & required) == required &&
           (ops->capabilities &
            VD_ATTACH_AUTH_PERSISTENCE_PATH_PROOF_ONLY) == 0u &&
           ops->open_current != NULL && ops->open_staged != NULL &&
           ops->create_staged != NULL && ops->read != NULL &&
           ops->write != NULL && ops->sync != NULL &&
           ops->close != NULL && ops->commit_staged != NULL &&
           ops->discard_staged != NULL;
}

static int vd_monotonic_valid(
    const VdAttachAuthStoreMonotonicOps *ops) {
    const uint32_t required =
        VD_ATTACH_AUTH_MONOTONIC_INDEPENDENT_TRUST_DOMAIN |
        VD_ATTACH_AUTH_MONOTONIC_DURABLE;
    return ops != NULL && ops->trust_domain != NULL &&
           (ops->capabilities & required) == required &&
           (ops->capabilities &
            VD_ATTACH_AUTH_MONOTONIC_METADATA_NAMESPACE) == 0u &&
           ops->load_floor != NULL && ops->advance_floor != NULL;
}

static int vd_private_keys_valid(
    const VdAttachAuthStorePrivateKeyOps *ops) {
    const uint32_t required =
        VD_ATTACH_AUTH_PRIVATE_KEY_NON_EXPORTABLE |
        VD_ATTACH_AUTH_PRIVATE_KEY_ISOLATED;
    return ops != NULL && ops->trust_domain != NULL &&
           (ops->capabilities & required) == required &&
           (ops->capabilities &
            VD_ATTACH_AUTH_PRIVATE_KEY_RAW_SEED_IMPORT) == 0u &&
           ops->generate != NULL && ops->load_public != NULL &&
           ops->sign != NULL && ops->destroy != NULL;
}

static int vd_ready(const VdStoreImpl *impl) {
    return impl != NULL &&
           impl->runtime_magic == VD_STORE_RUNTIME_MAGIC &&
           impl->available;
}

static int vd_read_loaded(
    VdStoreImpl *impl,
    int (*open_slot)(void *context, void **handle),
    VdLoadedState *loaded) {
    uint8_t buffer[VD_ATTACH_AUTH_STORE_MAX_BYTES + 1u];
    size_t offset = 0u;
    size_t amount = 0u;
    int result;

    memset(loaded, 0, sizeof(*loaded));
    result = open_slot(impl->persistence.context, &loaded->handle);
    if (result == VD_ATTACH_AUTH_STORE_BACKEND_NOT_FOUND) {
        loaded->result = VD_ATTACH_AUTH_STORE_OK;
        return loaded->result;
    }
    loaded->present = 1;
    if (result != VD_ATTACH_AUTH_STORE_BACKEND_OK ||
        loaded->handle == NULL) {
        loaded->result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        return loaded->result;
    }
    do {
        amount = 0u;
        result = impl->persistence.read(
            impl->persistence.context, loaded->handle,
            buffer + offset, sizeof(buffer) - offset, &amount);
        if (result != VD_ATTACH_AUTH_STORE_BACKEND_OK ||
            amount > sizeof(buffer) - offset) {
            loaded->result = VD_ATTACH_AUTH_STORE_ERROR_IO;
            break;
        }
        offset += amount;
        if (offset == sizeof(buffer)) {
            loaded->result = VD_ATTACH_AUTH_STORE_ERROR_MALFORMED;
            break;
        }
    } while (amount != 0u);
    if (loaded->result == VD_ATTACH_AUTH_STORE_OK) {
        loaded->result = vd_parse(buffer, offset, &loaded->state);
    }
    crypto_wipe(buffer, sizeof(buffer));
    return loaded->result;
}

static int vd_close_loaded(VdStoreImpl *impl,
                           VdLoadedState *loaded) {
    int result = VD_ATTACH_AUTH_STORE_OK;
    if (loaded->handle != NULL) {
        if (impl->persistence.close(
                impl->persistence.context, loaded->handle) !=
            VD_ATTACH_AUTH_STORE_BACKEND_OK) {
            result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        }
        loaded->handle = NULL;
    }
    return result;
}

static int vd_validate_local_key(VdStoreImpl *impl,
                                 const VdStoreState *state) {
    uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES];
    int result;
    if (!state->provisioned) {
        return VD_ATTACH_AUTH_STORE_OK;
    }
    memset(public_key, 0, sizeof(public_key));
    result = impl->private_keys.load_public(
        impl->private_keys.context, state->local.key_id,
        state->local.generation, public_key);
    if (result != VD_ATTACH_AUTH_STORE_BACKEND_OK ||
        !vd_bytes_equal(public_key, state->local.public_key,
                        sizeof(public_key))) {
        crypto_wipe(public_key, sizeof(public_key));
        return VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY;
    }
    crypto_wipe(public_key, sizeof(public_key));
    return VD_ATTACH_AUTH_STORE_OK;
}

static int vd_destroy_retired(VdStoreImpl *impl,
                              const VdStoreState *state) {
    if (!state->has_retired) {
        return VD_ATTACH_AUTH_STORE_OK;
    }
    return impl->private_keys.destroy(
               impl->private_keys.context, state->retired.key_id,
               state->retired.generation) ==
                   VD_ATTACH_AUTH_STORE_BACKEND_OK
               ? VD_ATTACH_AUTH_STORE_OK
               : VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY;
}

static int vd_recover_fail(VdAttachAuthStore *store,
                           VdStoreImpl *impl,
                           VdLoadedState *current,
                           VdLoadedState *staged,
                           int result) {
    if (current != NULL) {
        (void)vd_close_loaded(impl, current);
        crypto_wipe(current, sizeof(*current));
    }
    if (staged != NULL) {
        (void)vd_close_loaded(impl, staged);
        crypto_wipe(staged, sizeof(*staged));
    }
    crypto_wipe(store, sizeof(*store));
    return result;
}

int vd_attach_auth_store_init(
    VdAttachAuthStore *store,
    const VdAttachAuthStorePersistenceOps *persistence,
    const VdAttachAuthStoreMonotonicOps *monotonic,
    const VdAttachAuthStorePrivateKeyOps *private_keys) {
    VdStoreImpl *impl;
    int result;
    if (store == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    crypto_wipe(store, sizeof(*store));
    if (!vd_persistence_valid(persistence) ||
        !vd_monotonic_valid(monotonic) ||
        !vd_private_keys_valid(private_keys) ||
        persistence->trust_domain == monotonic->trust_domain ||
        persistence->trust_domain == private_keys->trust_domain ||
        monotonic->trust_domain == private_keys->trust_domain ||
        persistence->context == monotonic->context ||
        persistence->context == private_keys->context ||
        monotonic->context == private_keys->context) {
        return VD_ATTACH_AUTH_STORE_ERROR_CAPABILITY;
    }
    impl = vd_impl(store);
    impl->persistence = *persistence;
    impl->monotonic = *monotonic;
    impl->private_keys = *private_keys;
    impl->runtime_magic = VD_STORE_RUNTIME_MAGIC;
    result = vd_attach_auth_store_recover(store);
    if (result != VD_ATTACH_AUTH_STORE_OK) {
        crypto_wipe(store, sizeof(*store));
    }
    return result;
}

int vd_attach_auth_store_recover(VdAttachAuthStore *store) {
    VdStoreImpl *impl;
    VdLoadedState current;
    VdLoadedState staged;
    VdLoadedState *selected = NULL;
    uint64_t floor = 0u;
    int select_staged = 0;
    int result;

    if (store == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_impl(store);
    if (impl->runtime_magic != VD_STORE_RUNTIME_MAGIC ||
        impl->monotonic.load_floor(
            impl->monotonic.context, &floor) !=
            VD_ATTACH_AUTH_STORE_BACKEND_OK) {
        crypto_wipe(store, sizeof(*store));
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    impl->available = 0;
    crypto_wipe(&impl->state, sizeof(impl->state));
    (void)vd_read_loaded(impl, impl->persistence.open_current,
                         &current);
    (void)vd_read_loaded(impl, impl->persistence.open_staged,
                         &staged);
    if (current.result == VD_ATTACH_AUTH_STORE_ERROR_IO ||
        staged.result == VD_ATTACH_AUTH_STORE_ERROR_IO) {
        return vd_recover_fail(store, impl, &current, &staged,
                               VD_ATTACH_AUTH_STORE_ERROR_IO);
    }
    if (current.present &&
        current.result == VD_ATTACH_AUTH_STORE_OK) {
        selected = &current;
    }
    if (staged.present && staged.result == VD_ATTACH_AUTH_STORE_OK &&
        (selected == NULL ||
         staged.state.revision > selected->state.revision)) {
        selected = &staged;
        select_staged = 1;
    }
    if (selected == NULL) {
        if (current.present || staged.present) {
            return vd_recover_fail(
                store, impl, &current, &staged,
                VD_ATTACH_AUTH_STORE_ERROR_MALFORMED);
        }
        crypto_wipe(&impl->state, sizeof(impl->state));
        impl->state.revision = floor;
        impl->state.service_generation = floor;
        impl->available = 1;
        crypto_wipe(&current, sizeof(current));
        crypto_wipe(&staged, sizeof(staged));
        return VD_ATTACH_AUTH_STORE_OK;
    }
    if (selected->state.revision < floor) {
        return vd_recover_fail(store, impl, &current, &staged,
                               VD_ATTACH_AUTH_STORE_ERROR_STALE);
    }
    if (current.present && staged.present &&
        current.result == VD_ATTACH_AUTH_STORE_OK &&
        staged.result == VD_ATTACH_AUTH_STORE_OK &&
        current.state.revision == staged.state.revision &&
        memcmp(&current.state, &staged.state,
               sizeof(current.state)) != 0) {
        return vd_recover_fail(
            store, impl, &current, &staged,
            VD_ATTACH_AUTH_STORE_ERROR_MALFORMED);
    }
    result = vd_validate_local_key(impl, &selected->state);
    if (result != VD_ATTACH_AUTH_STORE_OK) {
        return vd_recover_fail(store, impl, &current, &staged,
                               result);
    }
    if (select_staged) {
        if (impl->persistence.sync(
                impl->persistence.context, staged.handle) !=
                VD_ATTACH_AUTH_STORE_BACKEND_OK ||
            impl->persistence.commit_staged(
                impl->persistence.context, staged.handle) !=
                VD_ATTACH_AUTH_STORE_BACKEND_OK) {
            return vd_recover_fail(
                store, impl, &current, &staged,
                VD_ATTACH_AUTH_STORE_ERROR_IO);
        }
    } else if (staged.present) {
        if (impl->persistence.discard_staged(
                impl->persistence.context, staged.handle) !=
            VD_ATTACH_AUTH_STORE_BACKEND_OK) {
            return vd_recover_fail(
                store, impl, &current, &staged,
                VD_ATTACH_AUTH_STORE_ERROR_IO);
        }
    }
    if (vd_close_loaded(impl, &current) !=
            VD_ATTACH_AUTH_STORE_OK ||
        vd_close_loaded(impl, &staged) !=
            VD_ATTACH_AUTH_STORE_OK) {
        return vd_recover_fail(store, impl, &current, &staged,
                               VD_ATTACH_AUTH_STORE_ERROR_IO);
    }
    if (selected->state.revision > floor &&
        impl->monotonic.advance_floor(
            impl->monotonic.context,
            selected->state.revision) !=
            VD_ATTACH_AUTH_STORE_BACKEND_OK) {
        return vd_recover_fail(store, impl, &current, &staged,
                               VD_ATTACH_AUTH_STORE_ERROR_IO);
    }
    if (vd_destroy_retired(impl, &selected->state) !=
        VD_ATTACH_AUTH_STORE_OK) {
        return vd_recover_fail(
            store, impl, &current, &staged,
            VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY);
    }
    impl->state = selected->state;
    impl->available = 1;
    crypto_wipe(&current, sizeof(current));
    crypto_wipe(&staged, sizeof(staged));
    return VD_ATTACH_AUTH_STORE_OK;
}

void vd_attach_auth_store_deinit(VdAttachAuthStore *store) {
    if (store != NULL) {
        crypto_wipe(store, sizeof(*store));
    }
}

static int vd_discard_existing_stage(VdStoreImpl *impl) {
    VdLoadedState staged;
    int result;
    (void)vd_read_loaded(impl, impl->persistence.open_staged,
                         &staged);
    if (staged.result == VD_ATTACH_AUTH_STORE_ERROR_IO) {
        (void)vd_close_loaded(impl, &staged);
        crypto_wipe(&staged, sizeof(staged));
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    if (staged.present) {
        result = impl->persistence.discard_staged(
            impl->persistence.context, staged.handle);
        if (result != VD_ATTACH_AUTH_STORE_BACKEND_OK) {
            (void)vd_close_loaded(impl, &staged);
            crypto_wipe(&staged, sizeof(staged));
            return VD_ATTACH_AUTH_STORE_ERROR_IO;
        }
    }
    result = vd_close_loaded(impl, &staged);
    crypto_wipe(&staged, sizeof(staged));
    return result;
}

static int vd_commit(VdStoreImpl *impl,
                     const VdStoreState *candidate,
                     int *outcome_uncertain) {
    uint8_t buffer[VD_ATTACH_AUTH_STORE_MAX_BYTES];
    void *handle = NULL;
    size_t size = 0u;
    size_t offset = 0u;
    size_t amount;
    int result;
    int backend_result;
    int committed = 0;

    *outcome_uncertain = 0;
    result = vd_serialize(candidate, buffer, sizeof(buffer), &size);
    if (result != VD_ATTACH_AUTH_STORE_OK) {
        crypto_wipe(buffer, sizeof(buffer));
        return result;
    }
    result = vd_discard_existing_stage(impl);
    if (result != VD_ATTACH_AUTH_STORE_OK) {
        goto cleanup;
    }
    backend_result = impl->persistence.create_staged(
        impl->persistence.context, &handle);
    if (backend_result != VD_ATTACH_AUTH_STORE_BACKEND_OK ||
        handle == NULL) {
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    while (offset < size) {
        amount = 0u;
        backend_result = impl->persistence.write(
            impl->persistence.context, handle, buffer + offset,
            size - offset, &amount);
        if (backend_result != VD_ATTACH_AUTH_STORE_BACKEND_OK ||
            amount == 0u || amount > size - offset) {
            result = VD_ATTACH_AUTH_STORE_ERROR_IO;
            goto cleanup;
        }
        offset += amount;
    }
    if (impl->persistence.sync(
            impl->persistence.context, handle) !=
        VD_ATTACH_AUTH_STORE_BACKEND_OK) {
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    backend_result = impl->persistence.commit_staged(
        impl->persistence.context, handle);
    if (backend_result ==
        VD_ATTACH_AUTH_STORE_BACKEND_INTERRUPTED) {
        *outcome_uncertain = 1;
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    if (backend_result != VD_ATTACH_AUTH_STORE_BACKEND_OK) {
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    committed = 1;
    if (impl->persistence.close(
            impl->persistence.context, handle) !=
        VD_ATTACH_AUTH_STORE_BACKEND_OK) {
        handle = NULL;
        *outcome_uncertain = 1;
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    handle = NULL;
    if (impl->monotonic.advance_floor(
            impl->monotonic.context, candidate->revision) !=
        VD_ATTACH_AUTH_STORE_BACKEND_OK) {
        *outcome_uncertain = 1;
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    crypto_wipe(&impl->state, sizeof(impl->state));
    impl->state = *candidate;
    impl->available = 1;
    result = VD_ATTACH_AUTH_STORE_OK;

cleanup:
    if (handle != NULL) {
        if (!committed && !*outcome_uncertain) {
            (void)impl->persistence.discard_staged(
                impl->persistence.context, handle);
        }
        (void)impl->persistence.close(
            impl->persistence.context, handle);
    }
    if (*outcome_uncertain) {
        crypto_wipe(&impl->state, sizeof(impl->state));
        impl->available = 0;
    }
    crypto_wipe(buffer, sizeof(buffer));
    return result;
}

static int vd_next_revision(const VdStoreState *state,
                            uint64_t *revision) {
    if (state->revision == UINT64_MAX) {
        return VD_ATTACH_AUTH_STORE_ERROR_LIMIT;
    }
    *revision = state->revision + 1u;
    return VD_ATTACH_AUTH_STORE_OK;
}

int vd_attach_auth_store_get_metadata(
    const VdAttachAuthStore *store,
    VdAttachAuthStoreMetadata *metadata) {
    const VdStoreImpl *impl;
    if (metadata != NULL) {
        memset(metadata, 0, sizeof(*metadata));
    }
    if (store == NULL || metadata == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_const_impl(store);
    if (!vd_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    metadata->schema = VD_ATTACH_AUTH_STORE_SCHEMA;
    metadata->revision = impl->state.revision;
    metadata->service_generation =
        impl->state.service_generation;
    metadata->peer_count = impl->state.peer_count;
    metadata->provisioned = impl->state.provisioned;
    return VD_ATTACH_AUTH_STORE_OK;
}

int vd_attach_auth_store_reserve_service_generation(
    VdAttachAuthStore *store,
    uint64_t *service_generation) {
    VdStoreImpl *impl;
    VdStoreState candidate;
    int uncertain;
    int result;
    if (service_generation != NULL) {
        *service_generation = 0u;
    }
    if (store == NULL || service_generation == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_impl(store);
    if (!vd_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (!impl->state.provisioned) {
        return VD_ATTACH_AUTH_STORE_ERROR_NOT_PROVISIONED;
    }
    if (impl->state.service_generation == UINT64_MAX) {
        return VD_ATTACH_AUTH_STORE_ERROR_LIMIT;
    }
    candidate = impl->state;
    result = vd_next_revision(&impl->state, &candidate.revision);
    if (result == VD_ATTACH_AUTH_STORE_OK) {
        ++candidate.service_generation;
        result = vd_commit(impl, &candidate, &uncertain);
    }
    if (result == VD_ATTACH_AUTH_STORE_OK) {
        *service_generation = candidate.service_generation;
    }
    crypto_wipe(&candidate, sizeof(candidate));
    return result;
}

int vd_attach_auth_store_load_local_public(
    const VdAttachAuthStore *store,
    VdAttachAuthPublicKey *key) {
    const VdStoreImpl *impl;
    if (key != NULL) {
        memset(key, 0, sizeof(*key));
    }
    if (store == NULL || key == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_const_impl(store);
    if (!vd_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (!impl->state.provisioned) {
        return VD_ATTACH_AUTH_STORE_ERROR_NOT_PROVISIONED;
    }
    *key = impl->state.local;
    return VD_ATTACH_AUTH_STORE_OK;
}

int vd_attach_auth_store_lookup_peer(
    const VdAttachAuthStore *store,
    uint64_t key_id,
    uint64_t generation,
    VdAttachAuthPublicKey *key) {
    const VdStoreImpl *impl;
    size_t index;
    if (key != NULL) {
        memset(key, 0, sizeof(*key));
    }
    if (store == NULL || key == NULL || key_id == 0u ||
        generation == 0u) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_const_impl(store);
    if (!vd_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    for (index = 0u; index < impl->state.peer_count; ++index) {
        if (impl->state.peers[index].key_id == key_id &&
            impl->state.peers[index].generation == generation) {
            if (impl->state.peers[index].status ==
                VD_ATTACH_AUTH_KEY_REVOKED) {
                return VD_ATTACH_AUTH_ERROR_REVOKED;
            }
            *key = impl->state.peers[index];
            return VD_ATTACH_AUTH_STORE_OK;
        }
    }
    return VD_ATTACH_AUTH_ERROR_NOT_ALLOWED;
}

int vd_attach_auth_store_sign_local(
    VdAttachAuthStore *store,
    const uint8_t *message,
    size_t message_size,
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES],
    uint64_t deadline_ms) {
    VdStoreImpl *impl;
    int result;
    if (signature != NULL) {
        crypto_wipe(signature, VD_ATTACH_AUTH_SIGNATURE_BYTES);
    }
    if (store == NULL || message == NULL || message_size == 0u ||
        message_size > VD_ATTACH_AUTH_MAX_TRANSCRIPT_BYTES ||
        signature == NULL || deadline_ms == 0u) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_impl(store);
    if (!vd_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (!impl->state.provisioned) {
        return VD_ATTACH_AUTH_STORE_ERROR_NOT_PROVISIONED;
    }
    result = impl->private_keys.sign(
        impl->private_keys.context, impl->state.local.key_id,
        impl->state.local.generation, message, message_size,
        signature, deadline_ms);
    if (result != VD_ATTACH_AUTH_STORE_BACKEND_OK ||
        !vd_bytes_nonzero(signature,
                          VD_ATTACH_AUTH_SIGNATURE_BYTES)) {
        crypto_wipe(signature, VD_ATTACH_AUTH_SIGNATURE_BYTES);
        return VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY;
    }
    return VD_ATTACH_AUTH_STORE_OK;
}

int vd_attach_auth_store_provision(
    VdAttachAuthStore *store,
    uint64_t key_id,
    uint64_t generation) {
    VdStoreImpl *impl;
    VdStoreState candidate;
    uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES];
    int uncertain = 0;
    int result;
    if (store == NULL || key_id == 0u || generation == 0u) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_impl(store);
    if (!vd_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (impl->state.provisioned) {
        return VD_ATTACH_AUTH_STORE_ERROR_CONFLICT;
    }
    memset(public_key, 0, sizeof(public_key));
    if (impl->private_keys.generate(
            impl->private_keys.context, key_id, generation,
            public_key) != VD_ATTACH_AUTH_STORE_BACKEND_OK ||
        !vd_bytes_nonzero(public_key, sizeof(public_key))) {
        crypto_wipe(public_key, sizeof(public_key));
        return VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY;
    }
    memset(&candidate, 0, sizeof(candidate));
    result = vd_next_revision(&impl->state, &candidate.revision);
    if (result == VD_ATTACH_AUTH_STORE_OK &&
        impl->state.service_generation != UINT64_MAX) {
        candidate.service_generation =
            impl->state.service_generation + 1u;
        candidate.provisioned = 1;
        candidate.local.key_id = key_id;
        candidate.local.generation = generation;
        candidate.local.status = VD_ATTACH_AUTH_KEY_ACTIVE;
        memcpy(candidate.local.public_key, public_key,
               sizeof(public_key));
        result = vd_commit(impl, &candidate, &uncertain);
    } else {
        result = VD_ATTACH_AUTH_STORE_ERROR_LIMIT;
    }
    if (result != VD_ATTACH_AUTH_STORE_OK && !uncertain) {
        (void)impl->private_keys.destroy(
            impl->private_keys.context, key_id, generation);
    }
    crypto_wipe(public_key, sizeof(public_key));
    crypto_wipe(&candidate, sizeof(candidate));
    return result;
}

static void vd_mark_key_cleanup_failure(VdStoreImpl *impl) {
    crypto_wipe(&impl->state, sizeof(impl->state));
    impl->available = 0;
}

int vd_attach_auth_store_rotate(
    VdAttachAuthStore *store,
    uint64_t old_key_id,
    uint64_t old_generation,
    uint64_t new_key_id,
    uint64_t new_generation) {
    VdStoreImpl *impl;
    VdStoreState candidate;
    uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES];
    int uncertain = 0;
    int result;
    if (store == NULL || old_key_id == 0u ||
        old_generation == 0u || new_key_id == 0u ||
        new_generation == 0u ||
        new_generation <= old_generation) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_impl(store);
    if (!vd_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (!impl->state.provisioned) {
        return VD_ATTACH_AUTH_STORE_ERROR_NOT_PROVISIONED;
    }
    if (impl->state.local.key_id != old_key_id ||
        impl->state.local.generation != old_generation) {
        return VD_ATTACH_AUTH_STORE_ERROR_CONFLICT;
    }
    memset(public_key, 0, sizeof(public_key));
    if (impl->private_keys.generate(
            impl->private_keys.context, new_key_id,
            new_generation, public_key) !=
            VD_ATTACH_AUTH_STORE_BACKEND_OK ||
        !vd_bytes_nonzero(public_key, sizeof(public_key))) {
        crypto_wipe(public_key, sizeof(public_key));
        return VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY;
    }
    candidate = impl->state;
    result = vd_next_revision(&impl->state, &candidate.revision);
    if (result == VD_ATTACH_AUTH_STORE_OK &&
        candidate.service_generation != UINT64_MAX) {
        ++candidate.service_generation;
        candidate.retired = candidate.local;
        candidate.has_retired = 1;
        memset(&candidate.local, 0, sizeof(candidate.local));
        candidate.local.key_id = new_key_id;
        candidate.local.generation = new_generation;
        candidate.local.status = VD_ATTACH_AUTH_KEY_ACTIVE;
        memcpy(candidate.local.public_key, public_key,
               sizeof(public_key));
        result = vd_commit(impl, &candidate, &uncertain);
    } else {
        result = VD_ATTACH_AUTH_STORE_ERROR_LIMIT;
    }
    if (result != VD_ATTACH_AUTH_STORE_OK && !uncertain) {
        (void)impl->private_keys.destroy(
            impl->private_keys.context, new_key_id,
            new_generation);
    } else if (result == VD_ATTACH_AUTH_STORE_OK &&
               vd_destroy_retired(impl, &candidate) !=
                   VD_ATTACH_AUTH_STORE_OK) {
        vd_mark_key_cleanup_failure(impl);
        result = VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY;
    }
    crypto_wipe(public_key, sizeof(public_key));
    crypto_wipe(&candidate, sizeof(candidate));
    return result;
}

int vd_attach_auth_store_allow_peer(
    VdAttachAuthStore *store,
    const VdAttachAuthPublicKey *peer) {
    VdStoreImpl *impl;
    VdStoreState candidate;
    size_t index;
    int uncertain;
    int result;
    if (store == NULL || !vd_key_valid(peer, 0) ||
        peer->status != VD_ATTACH_AUTH_KEY_ACTIVE) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_impl(store);
    if (!vd_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (!impl->state.provisioned) {
        return VD_ATTACH_AUTH_STORE_ERROR_NOT_PROVISIONED;
    }
    for (index = 0u; index < impl->state.peer_count; ++index) {
        if (impl->state.peers[index].key_id == peer->key_id &&
            impl->state.peers[index].generation ==
                peer->generation) {
            if (impl->state.peers[index].status ==
                VD_ATTACH_AUTH_KEY_REVOKED) {
                return VD_ATTACH_AUTH_ERROR_REVOKED;
            }
            return vd_bytes_equal(
                       impl->state.peers[index].public_key,
                       peer->public_key, sizeof(peer->public_key))
                       ? VD_ATTACH_AUTH_STORE_OK
                       : VD_ATTACH_AUTH_STORE_ERROR_CONFLICT;
        }
    }
    if (impl->state.peer_count ==
        VD_ATTACH_AUTH_STORE_MAX_PEERS) {
        return VD_ATTACH_AUTH_STORE_ERROR_LIMIT;
    }
    candidate = impl->state;
    result = vd_next_revision(&impl->state, &candidate.revision);
    if (result == VD_ATTACH_AUTH_STORE_OK) {
        candidate.peers[candidate.peer_count++] = *peer;
        result = vd_commit(impl, &candidate, &uncertain);
    }
    crypto_wipe(&candidate, sizeof(candidate));
    return result;
}

int vd_attach_auth_store_revoke_peer(
    VdAttachAuthStore *store,
    uint64_t key_id,
    uint64_t generation) {
    VdStoreImpl *impl;
    VdStoreState candidate;
    size_t index;
    int uncertain;
    int result;
    if (store == NULL || key_id == 0u || generation == 0u) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_impl(store);
    if (!vd_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    for (index = 0u; index < impl->state.peer_count; ++index) {
        if (impl->state.peers[index].key_id == key_id &&
            impl->state.peers[index].generation == generation) {
            if (impl->state.peers[index].status ==
                VD_ATTACH_AUTH_KEY_REVOKED) {
                return VD_ATTACH_AUTH_STORE_OK;
            }
            candidate = impl->state;
            result = vd_next_revision(
                &impl->state, &candidate.revision);
            if (result == VD_ATTACH_AUTH_STORE_OK) {
                candidate.peers[index].status =
                    VD_ATTACH_AUTH_KEY_REVOKED;
                result = vd_commit(impl, &candidate,
                                   &uncertain);
            }
            crypto_wipe(&candidate, sizeof(candidate));
            return result;
        }
    }
    return VD_ATTACH_AUTH_ERROR_NOT_ALLOWED;
}

int vd_attach_auth_store_uninstall(VdAttachAuthStore *store) {
    VdStoreImpl *impl;
    VdStoreState candidate;
    int uncertain;
    int result;
    if (store == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_impl(store);
    if (!vd_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (!impl->state.provisioned) {
        if (vd_destroy_retired(impl, &impl->state) !=
            VD_ATTACH_AUTH_STORE_OK) {
            return VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY;
        }
        return VD_ATTACH_AUTH_STORE_OK;
    }
    memset(&candidate, 0, sizeof(candidate));
    result = vd_next_revision(&impl->state, &candidate.revision);
    if (result != VD_ATTACH_AUTH_STORE_OK ||
        impl->state.service_generation == UINT64_MAX) {
        return VD_ATTACH_AUTH_STORE_ERROR_LIMIT;
    }
    candidate.service_generation =
        impl->state.service_generation + 1u;
    candidate.retired = impl->state.local;
    candidate.has_retired = 1;
    result = vd_commit(impl, &candidate, &uncertain);
    if (result == VD_ATTACH_AUTH_STORE_OK &&
        vd_destroy_retired(impl, &candidate) !=
            VD_ATTACH_AUTH_STORE_OK) {
        vd_mark_key_cleanup_failure(impl);
        result = VD_ATTACH_AUTH_STORE_ERROR_PRIVATE_KEY;
    }
    crypto_wipe(&candidate, sizeof(candidate));
    return result;
}

static int vd_bound_load(void *context, VdAttachAuthPublicKey *key) {
    int result = vd_attach_auth_store_load_local_public(
        (const VdAttachAuthStore *)context, key);
    return result == VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_OK
               : VD_ATTACH_AUTH_ERROR_STORAGE;
}

static int vd_bound_lookup(void *context, uint64_t key_id,
                           uint64_t generation,
                           VdAttachAuthPublicKey *key) {
    int result = vd_attach_auth_store_lookup_peer(
        (const VdAttachAuthStore *)context, key_id, generation, key);
    if (result == VD_ATTACH_AUTH_STORE_OK ||
        result == VD_ATTACH_AUTH_ERROR_REVOKED ||
        result == VD_ATTACH_AUTH_ERROR_NOT_ALLOWED) {
        return result;
    }
    return VD_ATTACH_AUTH_ERROR_STORAGE;
}

static int vd_bound_sign(
    void *context, uint64_t key_id, uint64_t generation,
    const uint8_t *transcript, size_t transcript_size,
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES],
    uint64_t deadline_ms) {
    VdAttachAuthPublicKey local;
    int result = vd_attach_auth_store_load_local_public(
        (const VdAttachAuthStore *)context, &local);
    if (result != VD_ATTACH_AUTH_STORE_OK ||
        local.key_id != key_id || local.generation != generation) {
        crypto_wipe(&local, sizeof(local));
        return VD_ATTACH_AUTH_ERROR_STORAGE;
    }
    crypto_wipe(&local, sizeof(local));
    result = vd_attach_auth_store_sign_local(
        (VdAttachAuthStore *)context, transcript, transcript_size,
        signature, deadline_ms);
    return result == VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_OK
               : VD_ATTACH_AUTH_ERROR_STORAGE;
}

int vd_attach_auth_store_bind(VdAttachAuthStore *store,
                              VdAttachAuthKeyStorage *storage) {
    if (store == NULL || storage == NULL ||
        !vd_ready(vd_impl(store))) {
        if (storage != NULL) {
            memset(storage, 0, sizeof(*storage));
        }
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    memset(storage, 0, sizeof(*storage));
    storage->context = store;
    storage->load_local_public = vd_bound_load;
    storage->lookup_peer = vd_bound_lookup;
    storage->sign_local = vd_bound_sign;
    return VD_ATTACH_AUTH_STORE_OK;
}

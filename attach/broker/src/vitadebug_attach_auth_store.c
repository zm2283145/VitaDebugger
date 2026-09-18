#include "vitadebug_attach_auth_store.h"

#include <monocypher.h>
#include <monocypher-ed25519.h>

#include <limits.h>
#include <string.h>

#define VD_STORE_HEADER_BYTES 40u
#define VD_STORE_TLV_HEADER_BYTES 8u
#define VD_STORE_LOCAL_BYTES 88u
#define VD_STORE_PEER_BYTES 56u
#define VD_STORE_DIGEST_BYTES 32u
#define VD_STORE_FIELD_LOCAL 1u
#define VD_STORE_FIELD_PEER 2u
#define VD_STORE_FLAG_TOMBSTONE 1u

static const uint8_t vd_store_magic[8] = {
    'V', 'D', 'A', 'S', 'K', 'S', '0', '1'
};

typedef struct VdStoreState {
    uint64_t revision;
    uint64_t service_generation;
    VdAttachAuthPublicKey local;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    VdAttachAuthPublicKey peers[VD_ATTACH_AUTH_STORE_MAX_PEERS];
    size_t peer_count;
    int provisioned;
} VdStoreState;

typedef struct VdStoreImpl {
    VdAttachAuthStoreFileOps files;
    VdAttachAuthStoreRollbackOps rollback;
    VdStoreState state;
    int initialized;
    int available;
} VdStoreImpl;

typedef char vd_store_context_size_check[
    sizeof(VdStoreImpl) <= VD_ATTACH_AUTH_STORE_CONTEXT_BYTES ? 1 : -1];

static int vd_next_revision(const VdStoreState *state,
                            uint64_t *revision);

static VdStoreImpl *vd_store_impl(VdAttachAuthStore *store) {
    return (VdStoreImpl *)(void *)store->opaque;
}

static const VdStoreImpl *vd_store_const_impl(
    const VdAttachAuthStore *store) {
    return (const VdStoreImpl *)(const void *)store->opaque;
}

static uint32_t vd_load_u32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint64_t vd_load_u64(const uint8_t *data) {
    return (uint64_t)vd_load_u32(data) |
           ((uint64_t)vd_load_u32(data + 4) << 32);
}

static void vd_store_u32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void vd_store_u64(uint8_t *data, uint64_t value) {
    vd_store_u32(data, (uint32_t)value);
    vd_store_u32(data + 4, (uint32_t)(value >> 32));
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

static int vd_public_key_valid(const VdAttachAuthPublicKey *key,
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
    uint8_t derived_public[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t secret_key[64];
    size_t left;
    size_t right;
    int valid = 1;

    if (state->revision == 0u ||
        state->service_generation == 0u ||
        state->peer_count > VD_ATTACH_AUTH_STORE_MAX_PEERS) {
        return 0;
    }
    if (!state->provisioned) {
        return state->peer_count == 0u;
    }
    if (!vd_public_key_valid(&state->local, 0) ||
        state->local.status != VD_ATTACH_AUTH_KEY_ACTIVE ||
        !vd_bytes_nonzero(state->seed, sizeof(state->seed))) {
        return 0;
    }
    memcpy(seed, state->seed, sizeof(seed));
    crypto_ed25519_key_pair(secret_key, derived_public, seed);
    if (!vd_bytes_equal(derived_public, state->local.public_key,
                        sizeof(derived_public))) {
        valid = 0;
    }
    for (left = 0u; valid && left < state->peer_count; ++left) {
        if (!vd_public_key_valid(&state->peers[left], 1)) {
            valid = 0;
            break;
        }
        for (right = left + 1u; right < state->peer_count; ++right) {
            if (state->peers[left].key_id ==
                    state->peers[right].key_id &&
                state->peers[left].generation ==
                    state->peers[right].generation) {
                valid = 0;
                break;
            }
        }
    }
    crypto_wipe(secret_key, sizeof(secret_key));
    crypto_wipe(derived_public, sizeof(derived_public));
    crypto_wipe(seed, sizeof(seed));
    return valid;
}

static size_t vd_serialize_key(uint8_t *output,
                               const VdAttachAuthPublicKey *key) {
    vd_store_u64(output, key->key_id);
    vd_store_u64(output + 8u, key->generation);
    vd_store_u32(output + 16u, key->status);
    vd_store_u32(output + 20u, 0u);
    memcpy(output + 24u, key->public_key, sizeof(key->public_key));
    return VD_STORE_PEER_BYTES;
}

static int vd_parse_key(const uint8_t *input, size_t size,
                        VdAttachAuthPublicKey *key) {
    if (size != VD_STORE_PEER_BYTES ||
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

static int vd_serialize_state(const VdStoreState *state,
                              uint8_t *output, size_t capacity,
                              size_t *output_size) {
    uint8_t digest[VD_STORE_DIGEST_BYTES];
    uint32_t field_count;
    size_t offset;
    size_t index;
    size_t total;

    if (!vd_state_valid(state)) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    field_count = state->provisioned
                      ? (uint32_t)(state->peer_count + 1u)
                      : 0u;
    total = VD_STORE_HEADER_BYTES + VD_STORE_DIGEST_BYTES;
    if (state->provisioned) {
        total += VD_STORE_TLV_HEADER_BYTES + VD_STORE_LOCAL_BYTES;
        total += state->peer_count *
                 (VD_STORE_TLV_HEADER_BYTES + VD_STORE_PEER_BYTES);
    }
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
        vd_store_u32(output + offset, VD_STORE_FIELD_LOCAL);
        vd_store_u32(output + offset + 4u, VD_STORE_LOCAL_BYTES);
        offset += VD_STORE_TLV_HEADER_BYTES;
        (void)vd_serialize_key(output + offset, &state->local);
        memcpy(output + offset + VD_STORE_PEER_BYTES, state->seed,
               sizeof(state->seed));
        offset += VD_STORE_LOCAL_BYTES;
        for (index = 0u; index < state->peer_count; ++index) {
            vd_store_u32(output + offset, VD_STORE_FIELD_PEER);
            vd_store_u32(output + offset + 4u, VD_STORE_PEER_BYTES);
            offset += VD_STORE_TLV_HEADER_BYTES;
            offset += vd_serialize_key(output + offset,
                                       &state->peers[index]);
        }
    }
    crypto_blake2b(digest, sizeof(digest), output, offset);
    memcpy(output + offset, digest, sizeof(digest));
    offset += sizeof(digest);
    crypto_wipe(digest, sizeof(digest));
    *output_size = offset;
    return VD_ATTACH_AUTH_STORE_OK;
}

static int vd_parse_state(const uint8_t *input, size_t size,
                          VdStoreState *state) {
    uint8_t digest[VD_STORE_DIGEST_BYTES];
    uint32_t flags;
    uint32_t field_count;
    uint32_t field;
    uint32_t field_size;
    size_t digest_offset;
    size_t offset;
    size_t index;
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
        ((flags & VD_STORE_FLAG_TOMBSTONE) != 0u &&
         field_count != 0u) ||
        ((flags & VD_STORE_FLAG_TOMBSTONE) == 0u &&
         (field_count == 0u ||
          field_count > VD_ATTACH_AUTH_STORE_MAX_PEERS + 1u))) {
        goto cleanup;
    }
    offset = VD_STORE_HEADER_BYTES;
    state->provisioned =
        (flags & VD_STORE_FLAG_TOMBSTONE) == 0u;
    for (index = 0u; index < field_count; ++index) {
        if (offset > digest_offset ||
            digest_offset - offset < VD_STORE_TLV_HEADER_BYTES) {
            goto cleanup;
        }
        field = vd_load_u32(input + offset);
        field_size = vd_load_u32(input + offset + 4u);
        offset += VD_STORE_TLV_HEADER_BYTES;
        if ((size_t)field_size > digest_offset - offset) {
            goto cleanup;
        }
        if (index == 0u && field == VD_STORE_FIELD_LOCAL &&
            field_size == VD_STORE_LOCAL_BYTES) {
            if (!vd_parse_key(input + offset, VD_STORE_PEER_BYTES,
                              &state->local)) {
                goto cleanup;
            }
            memcpy(state->seed,
                   input + offset + VD_STORE_PEER_BYTES,
                   sizeof(state->seed));
        } else if (index > 0u && field == VD_STORE_FIELD_PEER &&
                   field_size == VD_STORE_PEER_BYTES &&
                   state->peer_count <
                       VD_ATTACH_AUTH_STORE_MAX_PEERS) {
            if (!vd_parse_key(input + offset, field_size,
                              &state->peers[state->peer_count])) {
                goto cleanup;
            }
            ++state->peer_count;
        } else {
            goto cleanup;
        }
        offset += field_size;
    }
    if (offset != digest_offset || !vd_state_valid(state)) {
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

static int vd_file_ops_valid(const VdAttachAuthStoreFileOps *ops) {
    return ops != NULL &&
           ops->open_read_regular_no_follow != NULL &&
           ops->create_new_regular_no_follow != NULL &&
           ops->read != NULL && ops->write != NULL &&
           ops->sync != NULL && ops->close != NULL &&
           ops->replace_atomic != NULL &&
           ops->remove_regular_no_follow != NULL &&
           ops->sync_parent != NULL;
}

static int vd_rollback_ops_valid(
    const VdAttachAuthStoreRollbackOps *ops) {
    return ops != NULL && ops->load_floor != NULL &&
           ops->advance_floor != NULL;
}

static int vd_read_state(VdStoreImpl *impl, const char *path,
                         VdStoreState *state, int *present) {
    uint8_t buffer[VD_ATTACH_AUTH_STORE_MAX_BYTES + 1u];
    void *handle = NULL;
    size_t offset = 0u;
    size_t amount = 0u;
    int result;
    int close_result;

    *present = 0;
    memset(state, 0, sizeof(*state));
    result = impl->files.open_read_regular_no_follow(
        impl->files.context, path, &handle);
    if (result == VD_ATTACH_AUTH_STORE_IO_NOT_FOUND) {
        return VD_ATTACH_AUTH_STORE_OK;
    }
    *present = 1;
    if (result != VD_ATTACH_AUTH_STORE_IO_OK || handle == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    do {
        amount = 0u;
        result = impl->files.read(
            impl->files.context, handle, buffer + offset,
            sizeof(buffer) - offset, &amount);
        if (result != VD_ATTACH_AUTH_STORE_IO_OK ||
            amount > sizeof(buffer) - offset) {
            result = VD_ATTACH_AUTH_STORE_ERROR_IO;
            break;
        }
        offset += amount;
        if (offset == sizeof(buffer)) {
            result = VD_ATTACH_AUTH_STORE_ERROR_MALFORMED;
            break;
        }
    } while (amount != 0u);
    close_result = impl->files.close(impl->files.context, handle);
    if (result == VD_ATTACH_AUTH_STORE_IO_OK &&
        close_result != VD_ATTACH_AUTH_STORE_IO_OK) {
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    if (result == VD_ATTACH_AUTH_STORE_IO_OK) {
        result = vd_parse_state(buffer, offset, state);
    }
    crypto_wipe(buffer, sizeof(buffer));
    if (result != VD_ATTACH_AUTH_STORE_OK) {
        crypto_wipe(state, sizeof(*state));
    }
    return result;
}

static int vd_remove_temp(VdStoreImpl *impl) {
    int result = impl->files.remove_regular_no_follow(
        impl->files.context, VD_ATTACH_AUTH_STORE_VITA_TEMP_PATH);
    if (result != VD_ATTACH_AUTH_STORE_IO_OK &&
        result != VD_ATTACH_AUTH_STORE_IO_NOT_FOUND) {
        return VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    return VD_ATTACH_AUTH_STORE_OK;
}

static int vd_commit(VdStoreImpl *impl,
                     const VdStoreState *candidate) {
    uint8_t buffer[VD_ATTACH_AUTH_STORE_MAX_BYTES];
    void *handle = NULL;
    size_t size = 0u;
    size_t offset = 0u;
    size_t amount;
    int result;
    int close_result;
    int replaced = 0;

    result = vd_serialize_state(candidate, buffer, sizeof(buffer), &size);
    if (result != VD_ATTACH_AUTH_STORE_OK) {
        crypto_wipe(buffer, sizeof(buffer));
        return result;
    }
    result = vd_remove_temp(impl);
    if (result != VD_ATTACH_AUTH_STORE_OK) {
        goto cleanup;
    }
    result = impl->files.create_new_regular_no_follow(
        impl->files.context, VD_ATTACH_AUTH_STORE_VITA_TEMP_PATH,
        &handle);
    if (result != VD_ATTACH_AUTH_STORE_IO_OK || handle == NULL) {
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    while (offset < size) {
        amount = 0u;
        result = impl->files.write(
            impl->files.context, handle, buffer + offset,
            size - offset, &amount);
        if (result != VD_ATTACH_AUTH_STORE_IO_OK || amount == 0u ||
            amount > size - offset) {
            result = VD_ATTACH_AUTH_STORE_ERROR_IO;
            break;
        }
        offset += amount;
    }
    if (result == VD_ATTACH_AUTH_STORE_IO_OK &&
        impl->files.sync(impl->files.context, handle) !=
            VD_ATTACH_AUTH_STORE_IO_OK) {
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    close_result = impl->files.close(impl->files.context, handle);
    handle = NULL;
    if (result == VD_ATTACH_AUTH_STORE_IO_OK &&
        close_result != VD_ATTACH_AUTH_STORE_IO_OK) {
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
    }
    if (result != VD_ATTACH_AUTH_STORE_IO_OK) {
        (void)vd_remove_temp(impl);
        goto cleanup;
    }
    if (impl->files.replace_atomic(
            impl->files.context,
            VD_ATTACH_AUTH_STORE_VITA_TEMP_PATH,
            VD_ATTACH_AUTH_STORE_VITA_PATH) !=
        VD_ATTACH_AUTH_STORE_IO_OK) {
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    replaced = 1;
    if (impl->files.sync_parent(
            impl->files.context,
            VD_ATTACH_AUTH_STORE_VITA_PATH) !=
        VD_ATTACH_AUTH_STORE_IO_OK) {
        crypto_wipe(&impl->state, sizeof(impl->state));
        impl->available = 0;
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    if (impl->rollback.advance_floor(
            impl->rollback.context, candidate->revision) !=
        VD_ATTACH_AUTH_STORE_IO_OK) {
        crypto_wipe(&impl->state, sizeof(impl->state));
        impl->available = 0;
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    crypto_wipe(&impl->state, sizeof(impl->state));
    impl->state = *candidate;
    impl->available = 1;
    result = VD_ATTACH_AUTH_STORE_OK;

cleanup:
    if (handle != NULL) {
        (void)impl->files.close(impl->files.context, handle);
    }
    if (replaced && result != VD_ATTACH_AUTH_STORE_OK) {
        crypto_wipe(&impl->state, sizeof(impl->state));
        impl->available = 0;
    }
    crypto_wipe(buffer, sizeof(buffer));
    return result;
}

static int vd_store_ready(const VdStoreImpl *impl) {
    return impl != NULL && impl->initialized && impl->available;
}

int vd_attach_auth_store_init(
    VdAttachAuthStore *store,
    const VdAttachAuthStoreFileOps *file_ops,
    const VdAttachAuthStoreRollbackOps *rollback_ops) {
    VdStoreImpl *impl;
    if (store == NULL || !vd_file_ops_valid(file_ops) ||
        !vd_rollback_ops_valid(rollback_ops)) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    crypto_wipe(store, sizeof(*store));
    impl = vd_store_impl(store);
    impl->files = *file_ops;
    impl->rollback = *rollback_ops;
    impl->initialized = 1;
    return vd_attach_auth_store_recover(store);
}

int vd_attach_auth_store_recover(VdAttachAuthStore *store) {
    VdStoreImpl *impl;
    VdStoreState primary;
    VdStoreState temporary;
    VdStoreState *selected = NULL;
    uint64_t floor = 0u;
    int primary_present = 0;
    int temporary_present = 0;
    int primary_result;
    int temporary_result;
    int selected_temporary = 0;
    int result = VD_ATTACH_AUTH_STORE_OK;

    if (store == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_store_impl(store);
    if (!impl->initialized ||
        impl->rollback.load_floor(impl->rollback.context, &floor) !=
            VD_ATTACH_AUTH_STORE_IO_OK) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    impl->available = 0;
    crypto_wipe(&impl->state, sizeof(impl->state));
    primary_result = vd_read_state(
        impl, VD_ATTACH_AUTH_STORE_VITA_PATH, &primary,
        &primary_present);
    temporary_result = vd_read_state(
        impl, VD_ATTACH_AUTH_STORE_VITA_TEMP_PATH, &temporary,
        &temporary_present);

    if (primary_result == VD_ATTACH_AUTH_STORE_OK &&
        primary_present) {
        selected = &primary;
    }
    if (temporary_result == VD_ATTACH_AUTH_STORE_OK &&
        temporary_present &&
        (selected == NULL ||
         temporary.revision > selected->revision)) {
        selected = &temporary;
        selected_temporary = 1;
    }
    if (selected == NULL) {
        if ((primary_present &&
             primary_result != VD_ATTACH_AUTH_STORE_OK) ||
            (temporary_present &&
             temporary_result != VD_ATTACH_AUTH_STORE_OK)) {
            result =
                primary_result == VD_ATTACH_AUTH_STORE_ERROR_MALFORMED ||
                        temporary_result ==
                            VD_ATTACH_AUTH_STORE_ERROR_MALFORMED
                    ? VD_ATTACH_AUTH_STORE_ERROR_MALFORMED
                    : VD_ATTACH_AUTH_STORE_ERROR_IO;
            goto cleanup;
        }
        crypto_wipe(&impl->state, sizeof(impl->state));
        impl->state.revision = floor;
        impl->state.service_generation = floor;
        impl->available = 1;
        result = VD_ATTACH_AUTH_STORE_OK;
        goto cleanup;
    }
    if (selected->revision < floor) {
        result = VD_ATTACH_AUTH_STORE_ERROR_STALE;
        goto cleanup;
    }
    if (primary_result == VD_ATTACH_AUTH_STORE_OK &&
        temporary_result == VD_ATTACH_AUTH_STORE_OK &&
        primary_present && temporary_present &&
        primary.revision == temporary.revision &&
        memcmp(&primary, &temporary, sizeof(primary)) != 0) {
        result = VD_ATTACH_AUTH_STORE_ERROR_MALFORMED;
        goto cleanup;
    }
    if (selected_temporary) {
        if (impl->files.replace_atomic(
                impl->files.context,
                VD_ATTACH_AUTH_STORE_VITA_TEMP_PATH,
                VD_ATTACH_AUTH_STORE_VITA_PATH) !=
                VD_ATTACH_AUTH_STORE_IO_OK ||
            impl->files.sync_parent(
                impl->files.context,
                VD_ATTACH_AUTH_STORE_VITA_PATH) !=
                VD_ATTACH_AUTH_STORE_IO_OK) {
            result = VD_ATTACH_AUTH_STORE_ERROR_IO;
            goto cleanup;
        }
    } else if (temporary_present) {
        if (vd_remove_temp(impl) != VD_ATTACH_AUTH_STORE_OK ||
            impl->files.sync_parent(
                impl->files.context,
                VD_ATTACH_AUTH_STORE_VITA_PATH) !=
                VD_ATTACH_AUTH_STORE_IO_OK) {
            result = VD_ATTACH_AUTH_STORE_ERROR_IO;
            goto cleanup;
        }
    }
    if (selected->revision > floor &&
        impl->rollback.advance_floor(
            impl->rollback.context, selected->revision) !=
            VD_ATTACH_AUTH_STORE_IO_OK) {
        result = VD_ATTACH_AUTH_STORE_ERROR_IO;
        goto cleanup;
    }
    crypto_wipe(&impl->state, sizeof(impl->state));
    impl->state = *selected;
    impl->available = 1;

cleanup:
    crypto_wipe(&primary, sizeof(primary));
    crypto_wipe(&temporary, sizeof(temporary));
    return result;
}

int vd_attach_auth_store_get_metadata(
    const VdAttachAuthStore *store,
    VdAttachAuthStoreMetadata *metadata) {
    const VdStoreImpl *impl;
    if (store == NULL || metadata == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    memset(metadata, 0, sizeof(*metadata));
    impl = vd_store_const_impl(store);
    if (!vd_store_ready(impl)) {
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
    int result;

    if (service_generation != NULL) {
        *service_generation = 0u;
    }
    if (store == NULL || service_generation == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_store_impl(store);
    if (!vd_store_ready(impl)) {
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
        result = vd_commit(impl, &candidate);
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
    impl = vd_store_const_impl(store);
    if (!vd_store_ready(impl)) {
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
    impl = vd_store_const_impl(store);
    if (!vd_store_ready(impl)) {
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
    uint8_t signature[VD_ATTACH_AUTH_SIGNATURE_BYTES]) {
    VdStoreImpl *impl;
    uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t secret_key[64];
    int result = VD_ATTACH_AUTH_STORE_OK;

    if (signature != NULL) {
        crypto_wipe(signature, VD_ATTACH_AUTH_SIGNATURE_BYTES);
    }
    if (store == NULL || message == NULL || message_size == 0u ||
        message_size > VD_ATTACH_AUTH_MAX_TRANSCRIPT_BYTES ||
        signature == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_store_impl(store);
    if (!vd_store_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (!impl->state.provisioned) {
        return VD_ATTACH_AUTH_STORE_ERROR_NOT_PROVISIONED;
    }
    memcpy(seed, impl->state.seed, sizeof(seed));
    crypto_ed25519_key_pair(secret_key, public_key, seed);
    if (!vd_bytes_equal(public_key, impl->state.local.public_key,
                        sizeof(public_key))) {
        result = VD_ATTACH_AUTH_STORE_ERROR_MALFORMED;
    } else {
        crypto_ed25519_sign(signature, secret_key, message,
                            message_size);
    }
    crypto_wipe(seed, sizeof(seed));
    crypto_wipe(secret_key, sizeof(secret_key));
    crypto_wipe(public_key, sizeof(public_key));
    if (result != VD_ATTACH_AUTH_STORE_OK) {
        crypto_wipe(signature, VD_ATTACH_AUTH_SIGNATURE_BYTES);
    }
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

int vd_attach_auth_store_provision(
    VdAttachAuthStore *store,
    uint64_t key_id,
    uint64_t generation,
    const uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES]) {
    VdStoreImpl *impl;
    VdStoreState candidate;
    uint8_t seed_copy[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t secret_key[64];
    int result;

    if (store == NULL || key_id == 0u || generation == 0u ||
        seed == NULL || !vd_bytes_nonzero(seed, VD_ATTACH_AUTH_KEY_BYTES)) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_store_impl(store);
    if (!vd_store_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (impl->state.provisioned) {
        return VD_ATTACH_AUTH_STORE_ERROR_CONFLICT;
    }
    memset(&candidate, 0, sizeof(candidate));
    result = vd_next_revision(&impl->state, &candidate.revision);
    if (result != VD_ATTACH_AUTH_STORE_OK ||
        impl->state.service_generation == UINT64_MAX) {
        crypto_wipe(&candidate, sizeof(candidate));
        return VD_ATTACH_AUTH_STORE_ERROR_LIMIT;
    }
    candidate.service_generation =
        impl->state.service_generation + 1u;
    candidate.provisioned = 1;
    candidate.local.key_id = key_id;
    candidate.local.generation = generation;
    candidate.local.status = VD_ATTACH_AUTH_KEY_ACTIVE;
    memcpy(candidate.seed, seed, sizeof(candidate.seed));
    memcpy(seed_copy, candidate.seed, sizeof(seed_copy));
    crypto_ed25519_key_pair(secret_key, candidate.local.public_key,
                            seed_copy);
    crypto_wipe(seed_copy, sizeof(seed_copy));
    crypto_wipe(secret_key, sizeof(secret_key));
    result = vd_commit(impl, &candidate);
    crypto_wipe(&candidate, sizeof(candidate));
    return result;
}

int vd_attach_auth_store_rotate(
    VdAttachAuthStore *store,
    uint64_t old_key_id,
    uint64_t old_generation,
    uint64_t new_key_id,
    uint64_t new_generation,
    const uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES]) {
    VdStoreImpl *impl;
    VdStoreState candidate;
    uint8_t seed_copy[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t secret_key[64];
    int result;

    if (store == NULL || old_key_id == 0u ||
        old_generation == 0u || new_key_id == 0u ||
        new_generation == 0u || seed == NULL ||
        !vd_bytes_nonzero(seed, VD_ATTACH_AUTH_KEY_BYTES)) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_store_impl(store);
    if (!vd_store_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (!impl->state.provisioned) {
        return VD_ATTACH_AUTH_STORE_ERROR_NOT_PROVISIONED;
    }
    if (impl->state.local.key_id != old_key_id ||
        impl->state.local.generation != old_generation) {
        return VD_ATTACH_AUTH_STORE_ERROR_CONFLICT;
    }
    if (new_generation <= old_generation ||
        (new_key_id == old_key_id &&
         new_generation == old_generation)) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    candidate = impl->state;
    result = vd_next_revision(&impl->state, &candidate.revision);
    if (result != VD_ATTACH_AUTH_STORE_OK ||
        candidate.service_generation == UINT64_MAX) {
        crypto_wipe(&candidate, sizeof(candidate));
        return VD_ATTACH_AUTH_STORE_ERROR_LIMIT;
    }
    ++candidate.service_generation;
    crypto_wipe(&candidate.local, sizeof(candidate.local));
    crypto_wipe(candidate.seed, sizeof(candidate.seed));
    candidate.local.key_id = new_key_id;
    candidate.local.generation = new_generation;
    candidate.local.status = VD_ATTACH_AUTH_KEY_ACTIVE;
    memcpy(candidate.seed, seed, sizeof(candidate.seed));
    memcpy(seed_copy, candidate.seed, sizeof(seed_copy));
    crypto_ed25519_key_pair(secret_key, candidate.local.public_key,
                            seed_copy);
    crypto_wipe(seed_copy, sizeof(seed_copy));
    crypto_wipe(secret_key, sizeof(secret_key));
    result = vd_commit(impl, &candidate);
    crypto_wipe(&candidate, sizeof(candidate));
    return result;
}

int vd_attach_auth_store_allow_peer(
    VdAttachAuthStore *store,
    const VdAttachAuthPublicKey *peer) {
    VdStoreImpl *impl;
    VdStoreState candidate;
    size_t index;
    int result;

    if (store == NULL || !vd_public_key_valid(peer, 0) ||
        peer->status != VD_ATTACH_AUTH_KEY_ACTIVE) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_store_impl(store);
    if (!vd_store_ready(impl)) {
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
        result = vd_commit(impl, &candidate);
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
    int result;

    if (store == NULL || key_id == 0u || generation == 0u) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_store_impl(store);
    if (!vd_store_ready(impl)) {
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
                result = vd_commit(impl, &candidate);
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
    int result;
    if (store == NULL) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    impl = vd_store_impl(store);
    if (!vd_store_ready(impl)) {
        return VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE;
    }
    if (!impl->state.provisioned) {
        return VD_ATTACH_AUTH_STORE_OK;
    }
    memset(&candidate, 0, sizeof(candidate));
    result = vd_next_revision(&impl->state, &candidate.revision);
    if (result != VD_ATTACH_AUTH_STORE_OK ||
        impl->state.service_generation == UINT64_MAX) {
        crypto_wipe(&candidate, sizeof(candidate));
        return VD_ATTACH_AUTH_STORE_ERROR_LIMIT;
    }
    candidate.service_generation =
        impl->state.service_generation + 1u;
    result = vd_commit(impl, &candidate);
    crypto_wipe(&candidate, sizeof(candidate));
    return result;
}

static int vd_bound_load(void *context, VdAttachAuthPublicKey *key) {
    int result = vd_attach_auth_store_load_local_public(
        (const VdAttachAuthStore *)context, key);
    return result == VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_OK
               : VD_ATTACH_AUTH_ERROR_UNAVAILABLE;
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
    int result;
    if (deadline_ms == 0u) {
        return VD_ATTACH_AUTH_ERROR_ARGUMENT;
    }
    result = vd_attach_auth_store_load_local_public(
        (const VdAttachAuthStore *)context, &local);
    if (result != VD_ATTACH_AUTH_STORE_OK ||
        local.key_id != key_id || local.generation != generation) {
        crypto_wipe(&local, sizeof(local));
        return VD_ATTACH_AUTH_ERROR_STORAGE;
    }
    crypto_wipe(&local, sizeof(local));
    result = vd_attach_auth_store_sign_local(
        (VdAttachAuthStore *)context, transcript, transcript_size,
        signature);
    return result == VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_OK
               : VD_ATTACH_AUTH_ERROR_STORAGE;
}

static int vd_expected_public_matches(
    const uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES],
    const uint8_t expected[VD_ATTACH_AUTH_KEY_BYTES]) {
    uint8_t secret_key[64];
    uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES];
    uint8_t seed_copy[VD_ATTACH_AUTH_KEY_BYTES];
    int matches;
    if (seed == NULL || expected == NULL) {
        return 0;
    }
    memcpy(seed_copy, seed, sizeof(seed_copy));
    crypto_ed25519_key_pair(secret_key, public_key, seed_copy);
    matches = vd_bytes_equal(public_key, expected, sizeof(public_key));
    crypto_wipe(secret_key, sizeof(secret_key));
    crypto_wipe(public_key, sizeof(public_key));
    crypto_wipe(seed_copy, sizeof(seed_copy));
    return matches;
}

static int vd_bound_provision(
    void *context, uint64_t key_id, uint64_t generation,
    const uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES],
    const uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES]) {
    if (!vd_expected_public_matches(seed, public_key)) {
        return VD_ATTACH_AUTH_ERROR_ARGUMENT;
    }
    return vd_attach_auth_store_provision(
               (VdAttachAuthStore *)context, key_id, generation, seed) ==
                   VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_OK
               : VD_ATTACH_AUTH_ERROR_STORAGE;
}

static int vd_bound_rotate(
    void *context, uint64_t old_key_id, uint64_t old_generation,
    uint64_t new_key_id, uint64_t new_generation,
    const uint8_t seed[VD_ATTACH_AUTH_KEY_BYTES],
    const uint8_t public_key[VD_ATTACH_AUTH_KEY_BYTES]) {
    if (!vd_expected_public_matches(seed, public_key)) {
        return VD_ATTACH_AUTH_ERROR_ARGUMENT;
    }
    return vd_attach_auth_store_rotate(
               (VdAttachAuthStore *)context, old_key_id,
               old_generation, new_key_id, new_generation, seed) ==
                   VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_OK
               : VD_ATTACH_AUTH_ERROR_STORAGE;
}

static int vd_bound_allow(void *context,
                          const VdAttachAuthPublicKey *key) {
    int result = vd_attach_auth_store_allow_peer(
        (VdAttachAuthStore *)context, key);
    return result == VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_OK
               : result == VD_ATTACH_AUTH_ERROR_REVOKED
                     ? VD_ATTACH_AUTH_ERROR_REVOKED
                     : VD_ATTACH_AUTH_ERROR_STORAGE;
}

static int vd_bound_revoke(void *context, uint64_t key_id,
                           uint64_t generation) {
    int result = vd_attach_auth_store_revoke_peer(
        (VdAttachAuthStore *)context, key_id, generation);
    return result == VD_ATTACH_AUTH_STORE_OK
               ? VD_ATTACH_AUTH_OK
               : result == VD_ATTACH_AUTH_ERROR_NOT_ALLOWED
                     ? VD_ATTACH_AUTH_ERROR_NOT_ALLOWED
                     : VD_ATTACH_AUTH_ERROR_STORAGE;
}

int vd_attach_auth_store_bind(VdAttachAuthStore *store,
                              VdAttachAuthKeyStorage *storage) {
    if (store == NULL || storage == NULL ||
        !vd_store_ready(vd_store_impl(store))) {
        return VD_ATTACH_AUTH_STORE_ERROR_ARGUMENT;
    }
    memset(storage, 0, sizeof(*storage));
    storage->context = store;
    storage->load_local_public = vd_bound_load;
    storage->lookup_peer = vd_bound_lookup;
    storage->sign_local = vd_bound_sign;
    storage->provision_local = vd_bound_provision;
    storage->rotate_local = vd_bound_rotate;
    storage->allow_peer = vd_bound_allow;
    storage->revoke_peer = vd_bound_revoke;
    return VD_ATTACH_AUTH_STORE_OK;
}

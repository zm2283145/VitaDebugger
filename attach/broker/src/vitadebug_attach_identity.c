#include "vitadebug_attach_identity.h"

#include <limits.h>
#include <string.h>

static int vd_identity_title_valid(
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES]) {
    size_t i;
    if (title_id == NULL || title_id[9] != '\0') {
        return 0;
    }
    for (i = 0u; i < 9u; ++i) {
        const unsigned char value = (unsigned char)title_id[i];
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9'))) {
            return 0;
        }
    }
    return 1;
}

static int vd_identity_snapshot_valid(
    const VdAttachTargetIdentity *identity, const char *title_id,
    uint32_t pid) {
    return identity != NULL &&
           memcmp(identity->title_id, title_id,
                  VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) == 0 &&
           identity->pid == pid && pid > 0u && pid <= (uint32_t)INT32_MAX &&
           identity->main_modid > 0u &&
           identity->main_modid <= (uint32_t)INT32_MAX &&
           identity->main_fingerprint != 0u &&
           identity->target_generation != 0u;
}

static int vd_identity_snapshot_matches(
    const VdAttachTargetIdentity *left,
    const VdAttachTargetIdentity *right) {
    return memcmp(left->title_id, right->title_id,
                  VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) == 0 &&
           left->pid == right->pid &&
           left->main_modid == right->main_modid &&
           left->main_fingerprint == right->main_fingerprint &&
           left->target_generation == right->target_generation;
}

static int vd_identity_allowed(const VdAttachIdentityProvider *provider,
                               const char *title_id) {
    size_t i;
    for (i = 0u; i < provider->allowed_title_count; ++i) {
        if (memcmp(provider->allowed_title_ids[i], title_id,
                   VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) == 0) {
            return 1;
        }
    }
    return 0;
}

static int vd_identity_time_open(const VdAttachIdentityProvider *provider,
                                 uint64_t deadline_ms) {
    return provider->config.now_ms(provider->config.callback_context) <
           deadline_ms;
}

int vd_attach_identity_init(VdAttachIdentityProvider *provider,
                            const VdAttachIdentityConfig *config) {
    size_t i;
    size_t j;
    if (provider == NULL || config == NULL || config->now_ms == NULL ||
        config->resolve_title == NULL || config->reverse_title == NULL ||
        config->snapshot_trusted == NULL ||
        config->allowed_title_ids == NULL ||
        config->allowed_title_count == 0u ||
        config->allowed_title_count > VD_ATTACH_IDENTITY_MAX_TITLES) {
        return -1;
    }
    if (provider->initialized) {
        return -2;
    }
    for (i = 0u; i < config->allowed_title_count; ++i) {
        if (!vd_identity_title_valid(config->allowed_title_ids[i])) {
            return -1;
        }
        for (j = 0u; j < i; ++j) {
            if (memcmp(config->allowed_title_ids[i],
                       config->allowed_title_ids[j],
                       VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) == 0) {
                return -1;
            }
        }
    }
    memset(provider, 0, sizeof(*provider));
    provider->config = *config;
    provider->config.allowed_title_ids = NULL;
    provider->config.allowed_title_count = 0u;
    provider->allowed_title_count = config->allowed_title_count;
    for (i = 0u; i < config->allowed_title_count; ++i) {
        memcpy(provider->allowed_title_ids[i], config->allowed_title_ids[i],
               VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES);
    }
    provider->initialized = 1;
    return 0;
}

VdAttachInventoryResult vd_attach_identity_discover_exact(
    void *context,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity, uint64_t deadline_ms) {
    VdAttachIdentityProvider *provider =
        (VdAttachIdentityProvider *)context;
    VdAttachTargetIdentity first;
    VdAttachTargetIdentity second;
    char reverse_title[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES];
    uint32_t first_pid = 0u;
    uint32_t second_pid = 0u;
    VdAttachInventoryResult result;

    if (identity != NULL) {
        memset(identity, 0, sizeof(*identity));
    }
    if (provider == NULL || identity == NULL || !provider->initialized ||
        !vd_identity_title_valid(title_id) ||
        !vd_identity_allowed(provider, title_id)) {
        return VD_ATTACH_INVENTORY_DENIED;
    }
    if (!vd_identity_time_open(provider, deadline_ms)) {
        return VD_ATTACH_INVENTORY_UNAVAILABLE;
    }
    result = provider->config.resolve_title(provider->config.callback_context,
                                            title_id, &first_pid,
                                            deadline_ms);
    if (result != VD_ATTACH_INVENTORY_FOUND) {
        return result;
    }
    if (first_pid == 0u || first_pid > (uint32_t)INT32_MAX) {
        return VD_ATTACH_INVENTORY_ERROR;
    }
    memset(reverse_title, 0, sizeof(reverse_title));
    result = provider->config.reverse_title(provider->config.callback_context,
                                            first_pid, reverse_title,
                                            deadline_ms);
    if (result != VD_ATTACH_INVENTORY_FOUND) {
        return result;
    }
    if (!vd_identity_title_valid(reverse_title) ||
        memcmp(reverse_title, title_id, sizeof(reverse_title)) != 0) {
        return VD_ATTACH_INVENTORY_CHANGED;
    }
    memset(&first, 0, sizeof(first));
    result = provider->config.snapshot_trusted(
        provider->config.callback_context, first_pid, title_id, &first,
        deadline_ms);
    if (result != VD_ATTACH_INVENTORY_FOUND) {
        return result;
    }
    if (!vd_identity_snapshot_valid(&first, title_id, first_pid)) {
        return VD_ATTACH_INVENTORY_ERROR;
    }
    if (!vd_identity_time_open(provider, deadline_ms)) {
        return VD_ATTACH_INVENTORY_UNAVAILABLE;
    }

    result = provider->config.resolve_title(provider->config.callback_context,
                                            title_id, &second_pid,
                                            deadline_ms);
    if (result != VD_ATTACH_INVENTORY_FOUND || second_pid != first_pid) {
        return result == VD_ATTACH_INVENTORY_FOUND
                   ? VD_ATTACH_INVENTORY_CHANGED
                   : result;
    }
    memset(reverse_title, 0, sizeof(reverse_title));
    result = provider->config.reverse_title(provider->config.callback_context,
                                            second_pid, reverse_title,
                                            deadline_ms);
    if (result != VD_ATTACH_INVENTORY_FOUND) {
        return result;
    }
    if (!vd_identity_title_valid(reverse_title) ||
        memcmp(reverse_title, title_id, sizeof(reverse_title)) != 0) {
        return VD_ATTACH_INVENTORY_CHANGED;
    }
    memset(&second, 0, sizeof(second));
    result = provider->config.snapshot_trusted(
        provider->config.callback_context, second_pid, title_id, &second,
        deadline_ms);
    if (result != VD_ATTACH_INVENTORY_FOUND) {
        return result;
    }
    if (!vd_identity_snapshot_valid(&second, title_id, second_pid)) {
        return VD_ATTACH_INVENTORY_ERROR;
    }
    if (!vd_identity_snapshot_matches(&first, &second)) {
        return VD_ATTACH_INVENTORY_CHANGED;
    }
    if (!vd_identity_time_open(provider, deadline_ms)) {
        return VD_ATTACH_INVENTORY_UNAVAILABLE;
    }
    *identity = second;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(reverse_title, 0, sizeof(reverse_title));
    return VD_ATTACH_INVENTORY_FOUND;
}

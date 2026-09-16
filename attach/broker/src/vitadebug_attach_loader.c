#include "vitadebug_attach_loader.h"
#include "vitadebug_attach_control_wire.h"

#include <limits.h>
#include <string.h>

static uint64_t vd_loader_add_ms(uint64_t value, uint32_t delta) {
    if (UINT64_MAX - value < (uint64_t)delta) {
        return UINT64_MAX;
    }
    return value + (uint64_t)delta;
}

static uint64_t vd_loader_min_u64(uint64_t left, uint64_t right) {
    return left < right ? left : right;
}

static int vd_loader_bytes_nonzero(const uint8_t *bytes, size_t size) {
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

static int vd_loader_title_valid(
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

static int vd_loader_path_has_suffix(const char *path, size_t length,
                                     const char *suffix) {
    size_t suffix_length = strlen(suffix);
    return length >= suffix_length &&
           memcmp(path + length - suffix_length, suffix,
                  suffix_length) == 0;
}

static int vd_loader_path_valid(
    const char path[VD_ATTACH_LOADER_MAX_PATH_BYTES]) {
    size_t length = 0u;
    size_t i;
    size_t component_start;
    if (path == NULL) {
        return 0;
    }
    while (length < VD_ATTACH_LOADER_MAX_PATH_BYTES &&
           path[length] != '\0') {
        ++length;
    }
    if (length < 5u || length >= VD_ATTACH_LOADER_MAX_PATH_BYTES ||
        !((memcmp(path, "ux0:", 4u) == 0) ||
          (memcmp(path, "ur0:", 4u) == 0)) ||
        !vd_loader_path_has_suffix(path, length, ".suprx")) {
        return 0;
    }
    for (i = 0u; i < length; ++i) {
        const unsigned char value = (unsigned char)path[i];
        if (value < 0x21u || value > 0x7eu || value == '\\' ||
            (i >= 4u && value == ':') ||
            (value == '/' && i + 1u < length && path[i + 1u] == '/') ||
            (i >= 4u &&
             !((value >= 'A' && value <= 'Z') ||
               (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9') || value == '_' ||
               value == '-' || value == '.' || value == '/'))) {
            return 0;
        }
    }
    component_start = 4u;
    for (i = 4u; i <= length; ++i) {
        if (i == length || path[i] == '/') {
            const size_t component_size = i - component_start;
            if (component_size == 0u ||
                (component_size == 1u && path[component_start] == '.') ||
                (component_size == 2u && path[component_start] == '.' &&
                 path[component_start + 1u] == '.')) {
                return 0;
            }
            component_start = i + 1u;
        }
    }
    return 1;
}

static int vd_loader_identity_valid(const VdAttachTargetIdentity *identity) {
    return identity != NULL && vd_loader_title_valid(identity->title_id) &&
           identity->pid > 0u && identity->pid <= (uint32_t)INT32_MAX &&
           identity->main_modid > 0u &&
           identity->main_modid <= (uint32_t)INT32_MAX &&
           identity->main_fingerprint != 0u &&
           identity->target_generation != 0u;
}

static int vd_loader_identity_matches(
    const VdAttachTargetIdentity *left,
    const VdAttachTargetIdentity *right) {
    return left != NULL && right != NULL &&
           memcmp(left->title_id, right->title_id,
                  VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) == 0 &&
           left->pid == right->pid &&
           left->main_modid == right->main_modid &&
           left->main_fingerprint == right->main_fingerprint &&
           left->target_generation == right->target_generation;
}

static int vd_loader_title_allowed(
    const VdAttachFixedLoader *loader,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES]) {
    size_t i;
    for (i = 0u; i < loader->allowed_title_count; ++i) {
        if (memcmp(loader->allowed_title_ids[i], title_id,
                   VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) == 0) {
            return 1;
        }
    }
    return 0;
}

static int vd_loader_time_open(const VdAttachFixedLoader *loader,
                               uint64_t deadline_ms,
                               uint64_t first_expiry,
                               uint64_t second_expiry) {
    const uint64_t now_ms =
        loader->config.now_ms(loader->config.callback_context);
    return now_ms < deadline_ms && now_ms < first_expiry &&
           now_ms < second_expiry;
}

static int vd_loader_nonce_seen(
    const VdAttachFixedLoader *loader,
    const uint8_t nonce[VD_ATTACH_CONTROL_NONCE_BYTES]) {
    uint32_t i;
    for (i = 0u; i < loader->operation_nonce_count; ++i) {
        if (memcmp(loader->operation_nonces[i], nonce,
                   VD_ATTACH_CONTROL_NONCE_BYTES) == 0) {
            return 1;
        }
    }
    return 0;
}

static int vd_loader_claim_nonce(
    VdAttachFixedLoader *loader,
    const uint8_t nonce[VD_ATTACH_CONTROL_NONCE_BYTES]) {
    if (vd_loader_nonce_seen(loader, nonce)) {
        return VD_ATTACH_LOADER_ERROR_REPLAY;
    }
    if (loader->operation_nonce_count >=
        VD_ATTACH_LOADER_MAX_REPLAY_NONCES) {
        return VD_ATTACH_LOADER_ERROR_LIMIT;
    }
    memcpy(loader->operation_nonces[loader->operation_nonce_count], nonce,
           VD_ATTACH_CONTROL_NONCE_BYTES);
    ++loader->operation_nonce_count;
    return VD_ATTACH_LOADER_OK;
}

static int vd_loader_lease_id_seen(const VdAttachFixedLoader *loader,
                                   uint64_t lease_id) {
    uint32_t i;
    for (i = 0u; i < loader->lease_id_count; ++i) {
        if (loader->lease_ids[i] == lease_id) {
            return 1;
        }
    }
    return 0;
}

static int vd_loader_new_lease_id(VdAttachFixedLoader *loader,
                                  uint64_t *lease_id) {
    unsigned int attempt;
    if (loader->lease_id_count >= VD_ATTACH_LOADER_MAX_LEASE_IDS) {
        return VD_ATTACH_LOADER_ERROR_LIMIT;
    }
    for (attempt = 0u; attempt < 4u; ++attempt) {
        uint8_t bytes[8];
        uint64_t value = 0u;
        size_t i;
        if (loader->config.entropy(loader->config.callback_context, bytes,
                                   sizeof(bytes)) != 0) {
            memset(bytes, 0, sizeof(bytes));
            return VD_ATTACH_LOADER_ERROR_ENTROPY;
        }
        for (i = 0u; i < sizeof(bytes); ++i) {
            value = (value << 8) | bytes[i];
        }
        memset(bytes, 0, sizeof(bytes));
        if (value != 0u && !vd_loader_lease_id_seen(loader, value)) {
            loader->lease_ids[loader->lease_id_count++] = value;
            *lease_id = value;
            return VD_ATTACH_LOADER_OK;
        }
    }
    return VD_ATTACH_LOADER_ERROR_ENTROPY;
}

static void vd_loader_clear_journal(VdAttachFixedLoader *loader) {
    memset(&loader->journal, 0, sizeof(loader->journal));
    loader->journal.state = VD_ATTACH_LOADER_JOURNAL_IDLE;
}

static int vd_loader_authorization_equal(
    const VdAttachControlAuthorization *left,
    const VdAttachControlAuthorization *right) {
    uint8_t left_bytes[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    uint8_t right_bytes[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    size_t left_size = 0u;
    size_t right_size = 0u;
    int equal = 0;
    if (vd_attach_control_encode_operation_authorization(
            left, left_bytes, sizeof(left_bytes), &left_size) ==
            VD_ATTACH_CONTROL_OK &&
        vd_attach_control_encode_operation_authorization(
            right, right_bytes, sizeof(right_bytes), &right_size) ==
            VD_ATTACH_CONTROL_OK &&
        left_size == right_size &&
        memcmp(left_bytes, right_bytes, left_size) == 0 &&
        memcmp(left->signature, right->signature,
               VD_ATTACH_CONTROL_SIGNATURE_BYTES) == 0) {
        equal = 1;
    }
    memset(left_bytes, 0, sizeof(left_bytes));
    memset(right_bytes, 0, sizeof(right_bytes));
    return equal;
}

static int vd_loader_verify_authorization(
    VdAttachFixedLoader *loader,
    const VdAttachControlAuthorization *authorization,
    uint64_t deadline_ms) {
    uint8_t signed_bytes[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    size_t signed_size = 0u;
    uint64_t verification_deadline_ms;
    int result = VD_ATTACH_LOADER_ERROR_AUTH;
    if (authorization == NULL || loader->service_generation == 0u ||
        authorization->service_generation != loader->service_generation ||
        authorization->fixed_module_slot !=
            loader->config.debugger_module.fixed_module_slot ||
        !vd_loader_title_allowed(loader, authorization->target.title_id) ||
        !vd_loader_identity_valid(&authorization->target) ||
        !vd_loader_bytes_nonzero(authorization->signature,
                                 sizeof(authorization->signature)) ||
        vd_attach_control_encode_operation_authorization(
            authorization, signed_bytes, sizeof(signed_bytes),
            &signed_size) != VD_ATTACH_CONTROL_OK) {
        memset(signed_bytes, 0, sizeof(signed_bytes));
        return VD_ATTACH_LOADER_ERROR_AUTH;
    }
    if (!vd_loader_time_open(loader, deadline_ms,
                             authorization->expires_at_ms,
                             authorization->session_expires_at_ms)) {
        memset(signed_bytes, 0, sizeof(signed_bytes));
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    verification_deadline_ms = vd_loader_min_u64(
        deadline_ms, authorization->expires_at_ms);
    verification_deadline_ms = vd_loader_min_u64(
        verification_deadline_ms,
        authorization->session_expires_at_ms);
    if (loader->config.verify_authorization(
            loader->config.callback_context, authorization, signed_bytes,
            signed_size, verification_deadline_ms) == 1) {
        result = vd_loader_time_open(
                     loader, deadline_ms, authorization->expires_at_ms,
                     authorization->session_expires_at_ms)
                     ? VD_ATTACH_LOADER_OK
                     : VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    memset(signed_bytes, 0, sizeof(signed_bytes));
    return result;
}

static int vd_loader_resolve_exact(
    VdAttachFixedLoader *loader, const VdAttachTargetIdentity *expected,
    uint64_t deadline_ms) {
    VdAttachTargetIdentity current;
    VdAttachInventoryResult result;
    if (loader->config.now_ms(loader->config.callback_context) >=
        deadline_ms) {
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    memset(&current, 0, sizeof(current));
    result = loader->config.resolve_target(
        loader->config.callback_context, expected->title_id, &current,
        deadline_ms);
    if (loader->config.now_ms(loader->config.callback_context) >=
        deadline_ms) {
        memset(&current, 0, sizeof(current));
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    if (result != VD_ATTACH_INVENTORY_FOUND ||
        !vd_loader_identity_valid(&current) ||
        !vd_loader_identity_matches(&current, expected)) {
        memset(&current, 0, sizeof(current));
        return VD_ATTACH_LOADER_ERROR_TARGET;
    }
    memset(&current, 0, sizeof(current));
    return VD_ATTACH_LOADER_OK;
}

static int vd_loader_action_matches(
    const VdAttachFixedLoader *loader,
    const VdAttachControlModuleAction *action) {
    const VdAttachLoaderJournal *journal = &loader->journal;
    return action != NULL &&
           journal->state != VD_ATTACH_LOADER_JOURNAL_IDLE &&
           action->version == VD_ATTACH_CONTROL_VERSION &&
           action->fixed_module_slot ==
               loader->config.debugger_module.fixed_module_slot &&
           action->service_generation == loader->service_generation &&
           action->service_generation == journal->service_generation &&
           action->lease_id == journal->grant.lease_id &&
           action->lease_expires_at_ms ==
               journal->grant.lease_expires_at_ms &&
           action->owner_host_key_id == journal->owner_host_key_id &&
           action->injected_module_uid ==
               journal->grant.injected_module_uid &&
           vd_loader_identity_matches(&action->target, &journal->target);
}

static int vd_loader_cleanup_authorized(
    VdAttachFixedLoader *loader,
    const VdAttachControlModuleAction *action,
    uint64_t deadline_ms) {
    const uint32_t capability = action->journal_capability;
    if (capability == VD_ATTACH_CONTROL_JOURNAL_HOST_CLEANUP) {
        const VdAttachControlAuthorization *authorization =
            &action->signed_authorization;
        int result;
        if (!action->has_signed_authorization ||
            (authorization->operation !=
                 VD_ATTACH_CONTROL_OPERATION_DETACH &&
             authorization->operation !=
                 VD_ATTACH_CONTROL_OPERATION_RECOVER) ||
            authorization->service_generation !=
                loader->service_generation ||
            authorization->fixed_module_slot !=
                loader->config.debugger_module.fixed_module_slot ||
            authorization->host_key_id !=
                loader->journal.owner_host_key_id ||
            authorization->lease_id != loader->journal.grant.lease_id ||
            authorization->lease_expires_at_ms !=
                loader->journal.grant.lease_expires_at_ms ||
            authorization->injected_module_uid !=
                loader->journal.grant.injected_module_uid ||
            !vd_loader_identity_matches(&authorization->target,
                                        &loader->journal.target)) {
            return VD_ATTACH_LOADER_ERROR_AUTH;
        }
        result = vd_loader_verify_authorization(loader, authorization,
                                                deadline_ms);
        if (result != VD_ATTACH_LOADER_OK) {
            return result;
        }
        if (loader->journal.cleanup_authorization_active &&
            vd_loader_authorization_equal(
                &loader->journal.cleanup_authorization,
                authorization)) {
            return VD_ATTACH_LOADER_OK;
        }
        result = vd_loader_claim_nonce(loader,
                                       authorization->request_nonce);
        if (result != VD_ATTACH_LOADER_OK) {
            return result;
        }
        loader->journal.cleanup_authorization = *authorization;
        loader->journal.cleanup_authorization_active = 1;
        return VD_ATTACH_LOADER_OK;
    }
    if (action->has_signed_authorization ||
        (capability != VD_ATTACH_CONTROL_JOURNAL_ROLLBACK &&
         capability != VD_ATTACH_CONTROL_JOURNAL_LEASE_EXPIRED &&
         capability != VD_ATTACH_CONTROL_JOURNAL_DISCONNECT &&
         capability != VD_ATTACH_CONTROL_JOURNAL_SHUTDOWN &&
         capability != VD_ATTACH_CONTROL_JOURNAL_RECOVERY_RETRY)) {
        return VD_ATTACH_LOADER_ERROR_AUTH;
    }
    if (capability == VD_ATTACH_CONTROL_JOURNAL_LEASE_EXPIRED &&
        loader->config.now_ms(loader->config.callback_context) <
            loader->journal.grant.lease_expires_at_ms) {
        return VD_ATTACH_LOADER_ERROR_AUTH;
    }
    return loader->config.now_ms(loader->config.callback_context) <
                   deadline_ms
               ? VD_ATTACH_LOADER_OK
               : VD_ATTACH_LOADER_ERROR_EXPIRED;
}

static int vd_loader_cleanup_time_open(
    const VdAttachFixedLoader *loader,
    const VdAttachControlModuleAction *action,
    uint64_t deadline_ms) {
    if (action->journal_capability ==
        VD_ATTACH_CONTROL_JOURNAL_HOST_CLEANUP) {
        return vd_loader_time_open(
            loader, deadline_ms,
            action->signed_authorization.expires_at_ms,
            action->signed_authorization.session_expires_at_ms);
    }
    return loader->config.now_ms(loader->config.callback_context) <
           deadline_ms;
}

static uint64_t vd_loader_cleanup_deadline(
    const VdAttachControlModuleAction *action,
    uint64_t deadline_ms) {
    if (action->journal_capability ==
        VD_ATTACH_CONTROL_JOURNAL_HOST_CLEANUP) {
        deadline_ms = vd_loader_min_u64(
            deadline_ms, action->signed_authorization.expires_at_ms);
        deadline_ms = vd_loader_min_u64(
            deadline_ms,
            action->signed_authorization.session_expires_at_ms);
    }
    return deadline_ms;
}

int vd_attach_fixed_loader_init(VdAttachFixedLoader *loader,
                                const VdAttachFixedLoaderConfig *config) {
    size_t i;
    size_t j;
    if (loader == NULL || config == NULL || config->now_ms == NULL ||
        config->entropy == NULL ||
        config->verify_authorization == NULL ||
        config->resolve_target == NULL || config->verify_module == NULL ||
        config->load_module == NULL || config->start_module == NULL ||
        config->stop_module == NULL || config->unload_module == NULL ||
        config->probe_module == NULL ||
        config->allowed_title_ids == NULL ||
        config->allowed_title_count == 0u ||
        config->allowed_title_count > VD_ATTACH_LOADER_MAX_TITLES ||
        config->debugger_module.fixed_module_slot !=
            VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT ||
        !vd_loader_path_valid(config->debugger_module.canonical_path) ||
        !vd_loader_bytes_nonzero(config->debugger_module.sha256,
                                 sizeof(config->debugger_module.sha256)) ||
        config->min_lease_ms < VD_ATTACH_CONTROL_MIN_LEASE_MS ||
        config->max_lease_ms > VD_ATTACH_CONTROL_MAX_LEASE_MS ||
        config->min_lease_ms > config->max_lease_ms) {
        return VD_ATTACH_LOADER_ERROR_ARGUMENT;
    }
    if (loader->initialized) {
        return VD_ATTACH_LOADER_ERROR_STATE;
    }
    for (i = 0u; i < config->allowed_title_count; ++i) {
        if (!vd_loader_title_valid(config->allowed_title_ids[i])) {
            return VD_ATTACH_LOADER_ERROR_ARGUMENT;
        }
        for (j = 0u; j < i; ++j) {
            if (memcmp(config->allowed_title_ids[i],
                       config->allowed_title_ids[j],
                       VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) == 0) {
                return VD_ATTACH_LOADER_ERROR_ARGUMENT;
            }
        }
    }
    memset(loader, 0, sizeof(*loader));
    loader->config = *config;
    loader->config.allowed_title_ids = NULL;
    loader->config.allowed_title_count = 0u;
    loader->allowed_title_count = config->allowed_title_count;
    for (i = 0u; i < config->allowed_title_count; ++i) {
        memcpy(loader->allowed_title_ids[i],
               config->allowed_title_ids[i],
               VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES);
    }
    loader->journal.state = VD_ATTACH_LOADER_JOURNAL_IDLE;
    loader->initialized = 1;
    return VD_ATTACH_LOADER_OK;
}

int vd_attach_fixed_loader_bind_service_generation(
    VdAttachFixedLoader *loader, uint64_t service_generation) {
    if (loader == NULL || !loader->initialized ||
        service_generation == 0u) {
        return VD_ATTACH_LOADER_ERROR_ARGUMENT;
    }
    if (loader->service_generation != 0u) {
        return VD_ATTACH_LOADER_ERROR_STATE;
    }
    loader->service_generation = service_generation;
    return VD_ATTACH_LOADER_OK;
}

int vd_attach_fixed_loader_load(
    void *context, const VdAttachControlFixedLoadRequest *request,
    VdAttachControlLeaseGrant *grant, uint64_t deadline_ms) {
    VdAttachFixedLoader *loader = (VdAttachFixedLoader *)context;
    const VdAttachControlAuthorization *authorization;
    uint64_t load_started_at_ms;
    uint64_t lease_id;
    uint64_t lease_expiry;
    uint64_t load_deadline_ms;
    uint32_t module_uid = 0u;
    int platform_result;
    int result;

    if (grant != NULL) {
        memset(grant, 0, sizeof(*grant));
    }
    if (loader == NULL || request == NULL || grant == NULL ||
        !loader->initialized || loader->service_generation == 0u) {
        return VD_ATTACH_LOADER_ERROR_ARGUMENT;
    }
    if (loader->journal.state != VD_ATTACH_LOADER_JOURNAL_IDLE) {
        return VD_ATTACH_LOADER_ERROR_STATE;
    }
    authorization = &request->authorization;
    if (authorization->operation != VD_ATTACH_CONTROL_OPERATION_ATTACH ||
        authorization->requested_lease_ms < loader->config.min_lease_ms ||
        authorization->requested_lease_ms > loader->config.max_lease_ms) {
        return VD_ATTACH_LOADER_ERROR_AUTH;
    }
    result = vd_loader_verify_authorization(loader, authorization,
                                            deadline_ms);
    if (result != VD_ATTACH_LOADER_OK) {
        return result;
    }
    result = vd_loader_claim_nonce(loader, authorization->request_nonce);
    if (result != VD_ATTACH_LOADER_OK) {
        return result;
    }
    result = vd_loader_resolve_exact(loader, &authorization->target,
                                     deadline_ms);
    if (result != VD_ATTACH_LOADER_OK) {
        return result;
    }
    if (loader->config.verify_module(
            loader->config.callback_context,
            &loader->config.debugger_module, deadline_ms) != 1) {
        return VD_ATTACH_LOADER_ERROR_MODULE;
    }
    load_started_at_ms =
        loader->config.now_ms(loader->config.callback_context);
    if (!vd_loader_time_open(loader, deadline_ms,
                             authorization->expires_at_ms,
                             authorization->session_expires_at_ms)) {
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    result = vd_loader_new_lease_id(loader, &lease_id);
    if (result != VD_ATTACH_LOADER_OK) {
        return result;
    }
    lease_expiry = vd_loader_min_u64(
        vd_loader_add_ms(load_started_at_ms,
                         authorization->requested_lease_ms),
        authorization->session_expires_at_ms);
    if (lease_expiry <= load_started_at_ms) {
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    if (!vd_loader_time_open(loader, deadline_ms,
                             authorization->expires_at_ms,
                             lease_expiry)) {
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    load_deadline_ms = vd_loader_min_u64(deadline_ms,
                                         authorization->expires_at_ms);
    load_deadline_ms = vd_loader_min_u64(
        load_deadline_ms, authorization->session_expires_at_ms);
    load_deadline_ms = vd_loader_min_u64(load_deadline_ms,
                                         lease_expiry);
    /* Close the catalog/entropy-to-load target reuse window. */
    result = vd_loader_resolve_exact(loader, &authorization->target,
                                     load_deadline_ms);
    if (result != VD_ATTACH_LOADER_OK) {
        return result;
    }
    if (!vd_loader_time_open(loader, deadline_ms,
                             authorization->expires_at_ms,
                             lease_expiry)) {
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }

    vd_loader_clear_journal(loader);
    loader->journal.state = VD_ATTACH_LOADER_JOURNAL_LOADING;
    loader->journal.service_generation = loader->service_generation;
    loader->journal.owner_host_key_id = authorization->host_key_id;
    loader->journal.target = authorization->target;
    loader->journal.grant.lease_id = lease_id;
    loader->journal.grant.lease_expires_at_ms = lease_expiry;
    loader->journal.attach_authorization = *authorization;

    platform_result = loader->config.load_module(
        loader->config.callback_context,
        &loader->config.debugger_module, &authorization->target,
        &module_uid, load_deadline_ms);
    if (module_uid == 0u) {
        vd_loader_clear_journal(loader);
        return VD_ATTACH_LOADER_ERROR_PLATFORM;
    }
    loader->journal.grant.injected_module_uid = module_uid;
    loader->journal.module_loaded = module_uid != 0u;
    loader->journal.state = VD_ATTACH_LOADER_JOURNAL_LOADED;
    *grant = loader->journal.grant;
    if (module_uid > (uint32_t)INT32_MAX) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return VD_ATTACH_LOADER_ERROR_PLATFORM;
    }
    if (platform_result != 0) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return VD_ATTACH_LOADER_ERROR_PLATFORM;
    }
    if (!vd_loader_time_open(loader, deadline_ms,
                             authorization->expires_at_ms,
                             lease_expiry)) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    return VD_ATTACH_LOADER_OK;
}

int vd_attach_fixed_loader_start(
    void *context, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms) {
    VdAttachFixedLoader *loader = (VdAttachFixedLoader *)context;
    uint64_t action_deadline_ms;
    int result;
    int platform_result;
    if (loader == NULL || action == NULL || !loader->initialized ||
        !vd_loader_action_matches(loader, action) ||
        action->journal_capability != VD_ATTACH_CONTROL_JOURNAL_START ||
        !action->has_signed_authorization ||
        loader->journal.state != VD_ATTACH_LOADER_JOURNAL_LOADED ||
        !loader->journal.module_loaded ||
        !vd_loader_authorization_equal(
            &loader->journal.attach_authorization,
            &action->signed_authorization)) {
        return VD_ATTACH_LOADER_ERROR_STATE;
    }
    result = vd_loader_verify_authorization(
        loader, &action->signed_authorization, deadline_ms);
    if (result != VD_ATTACH_LOADER_OK) {
        return result;
    }
    if (action->signed_authorization.operation !=
        VD_ATTACH_CONTROL_OPERATION_ATTACH) {
        return VD_ATTACH_LOADER_ERROR_AUTH;
    }
    action_deadline_ms = vd_loader_min_u64(
        deadline_ms, action->signed_authorization.expires_at_ms);
    action_deadline_ms = vd_loader_min_u64(
        action_deadline_ms,
        action->signed_authorization.session_expires_at_ms);
    action_deadline_ms = vd_loader_min_u64(
        action_deadline_ms,
        loader->journal.grant.lease_expires_at_ms);
    result = vd_loader_resolve_exact(loader, &loader->journal.target,
                                     action_deadline_ms);
    if (result != VD_ATTACH_LOADER_OK) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return result;
    }
    if (!vd_loader_time_open(loader, deadline_ms,
                             action->signed_authorization.expires_at_ms,
                             loader->journal.grant.lease_expires_at_ms)) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    loader->journal.start_may_have_run = 1;
    loader->journal.state = VD_ATTACH_LOADER_JOURNAL_START_MAYBE;
    platform_result = loader->config.start_module(
        loader->config.callback_context,
        &loader->config.debugger_module, &loader->journal.target,
        loader->journal.grant.injected_module_uid,
        action_deadline_ms);
    if (platform_result != 0) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return VD_ATTACH_LOADER_ERROR_PLATFORM;
    }
    loader->journal.module_started = 1;
    loader->journal.state = VD_ATTACH_LOADER_JOURNAL_STARTED;
    if (!vd_loader_time_open(loader, deadline_ms,
                             action->signed_authorization.expires_at_ms,
                             loader->journal.grant.lease_expires_at_ms)) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    return VD_ATTACH_LOADER_OK;
}

int vd_attach_fixed_loader_stop(
    void *context, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms) {
    VdAttachFixedLoader *loader = (VdAttachFixedLoader *)context;
    uint64_t action_deadline_ms;
    int result;
    if (loader == NULL || action == NULL || !loader->initialized ||
        !vd_loader_action_matches(loader, action) ||
        !loader->journal.module_loaded) {
        return VD_ATTACH_LOADER_ERROR_STATE;
    }
    result = vd_loader_cleanup_authorized(loader, action, deadline_ms);
    if (result != VD_ATTACH_LOADER_OK) {
        return result;
    }
    action_deadline_ms = vd_loader_cleanup_deadline(action, deadline_ms);
    result = vd_loader_resolve_exact(loader, &loader->journal.target,
                                     action_deadline_ms);
    if (result != VD_ATTACH_LOADER_OK) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return result;
    }
    if (!vd_loader_cleanup_time_open(loader, action, deadline_ms)) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    if (loader->journal.module_started ||
        loader->journal.start_may_have_run) {
        if (loader->config.stop_module(
                loader->config.callback_context,
                &loader->config.debugger_module, &loader->journal.target,
                loader->journal.grant.injected_module_uid,
                action_deadline_ms) != 0) {
            loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
            return VD_ATTACH_LOADER_ERROR_PLATFORM;
        }
    }
    loader->journal.module_started = 0;
    loader->journal.start_may_have_run = 0;
    loader->journal.state = VD_ATTACH_LOADER_JOURNAL_STOPPED;
    return loader->config.now_ms(loader->config.callback_context) <
                   deadline_ms
               ? VD_ATTACH_LOADER_OK
               : VD_ATTACH_LOADER_ERROR_EXPIRED;
}

int vd_attach_fixed_loader_unload(
    void *context, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms) {
    VdAttachFixedLoader *loader = (VdAttachFixedLoader *)context;
    uint64_t action_deadline_ms;
    int result;
    if (loader == NULL || action == NULL || !loader->initialized ||
        !vd_loader_action_matches(loader, action) ||
        !loader->journal.module_loaded ||
        loader->journal.module_started ||
        loader->journal.start_may_have_run) {
        return VD_ATTACH_LOADER_ERROR_STATE;
    }
    result = vd_loader_cleanup_authorized(loader, action, deadline_ms);
    if (result != VD_ATTACH_LOADER_OK) {
        return result;
    }
    action_deadline_ms = vd_loader_cleanup_deadline(action, deadline_ms);
    result = vd_loader_resolve_exact(loader, &loader->journal.target,
                                     action_deadline_ms);
    if (result != VD_ATTACH_LOADER_OK) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return result;
    }
    if (!vd_loader_cleanup_time_open(loader, action, deadline_ms)) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return VD_ATTACH_LOADER_ERROR_EXPIRED;
    }
    if (loader->config.unload_module(
            loader->config.callback_context,
            &loader->config.debugger_module, &loader->journal.target,
            loader->journal.grant.injected_module_uid,
            action_deadline_ms) != 0) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return VD_ATTACH_LOADER_ERROR_PLATFORM;
    }
    vd_loader_clear_journal(loader);
    return VD_ATTACH_LOADER_OK;
}

VdAttachControlModulePresence vd_attach_fixed_loader_probe(
    void *context, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms) {
    VdAttachFixedLoader *loader = (VdAttachFixedLoader *)context;
    VdAttachControlModulePresence presence;
    uint64_t action_deadline_ms;
    if (loader == NULL || action == NULL || !loader->initialized ||
        !vd_loader_action_matches(loader, action) ||
        vd_loader_cleanup_authorized(loader, action, deadline_ms) !=
            VD_ATTACH_LOADER_OK) {
        return VD_ATTACH_CONTROL_MODULE_UNKNOWN;
    }
    action_deadline_ms = vd_loader_cleanup_deadline(action, deadline_ms);
    if (!vd_loader_cleanup_time_open(loader, action, deadline_ms)) {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
        return VD_ATTACH_CONTROL_MODULE_UNKNOWN;
    }
    presence = loader->config.probe_module(
        loader->config.callback_context,
        &loader->config.debugger_module, &loader->journal.target,
        loader->journal.grant.injected_module_uid,
        action_deadline_ms);
    if (presence != VD_ATTACH_CONTROL_MODULE_UNKNOWN &&
        presence != VD_ATTACH_CONTROL_MODULE_PRESENT &&
        presence != VD_ATTACH_CONTROL_MODULE_GONE) {
        presence = VD_ATTACH_CONTROL_MODULE_UNKNOWN;
    }
    if (presence == VD_ATTACH_CONTROL_MODULE_GONE) {
        vd_loader_clear_journal(loader);
    } else {
        loader->journal.state = VD_ATTACH_LOADER_JOURNAL_RECOVERY;
    }
    return presence;
}

int vd_attach_fixed_loader_snapshot(const VdAttachFixedLoader *loader,
                                    VdAttachLoaderSnapshot *snapshot) {
    if (loader == NULL || snapshot == NULL || !loader->initialized) {
        return VD_ATTACH_LOADER_ERROR_ARGUMENT;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = loader->journal.state;
    snapshot->service_generation = loader->service_generation;
    snapshot->owner_host_key_id = loader->journal.owner_host_key_id;
    snapshot->target = loader->journal.target;
    snapshot->grant = loader->journal.grant;
    snapshot->module_loaded = loader->journal.module_loaded;
    snapshot->module_started = loader->journal.module_started;
    snapshot->start_may_have_run =
        loader->journal.start_may_have_run;
    snapshot->replay_nonce_count = loader->operation_nonce_count;
    return VD_ATTACH_LOADER_OK;
}

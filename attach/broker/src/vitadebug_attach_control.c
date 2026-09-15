#include "vitadebug_attach_control.h"
#include "vitadebug_attach_control_wire.h"

#include <limits.h>
#include <string.h>

static uint64_t vd_control_add_ms(uint64_t now_ms, uint32_t delta_ms) {
    if (UINT64_MAX - now_ms < (uint64_t)delta_ms) {
        return UINT64_MAX;
    }
    return now_ms + (uint64_t)delta_ms;
}

static uint64_t vd_control_min_u64(uint64_t left, uint64_t right) {
    return left < right ? left : right;
}

static uint64_t vd_control_bounded_deadline(VdAttachControl *control,
                                            uint64_t now_ms,
                                            uint64_t first_limit,
                                            uint64_t second_limit) {
    uint64_t deadline =
        vd_control_add_ms(now_ms, control->config.callback_timeout_ms);
    deadline = vd_control_min_u64(deadline, first_limit);
    return vd_control_min_u64(deadline, second_limit);
}

static int vd_control_before_deadline(VdAttachControl *control,
                                      uint64_t first_limit,
                                      uint64_t second_limit,
                                      uint64_t deadline_ms) {
    const uint64_t now_ms =
        control->config.now_ms(control->config.callback_context);
    return now_ms < first_limit && now_ms < second_limit &&
           now_ms < deadline_ms;
}

static int vd_control_bytes_nonzero(const uint8_t *bytes, size_t size) {
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

static int vd_control_title_valid(
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

static int vd_control_identity_valid(
    const VdAttachTargetIdentity *identity,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES]) {
    return identity != NULL && vd_control_title_valid(identity->title_id) &&
           memcmp(identity->title_id, title_id,
                  VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) == 0 &&
           identity->pid > 0u && identity->pid <= (uint32_t)INT32_MAX &&
           identity->main_modid > 0u &&
           identity->main_modid <= (uint32_t)INT32_MAX &&
           identity->main_fingerprint != 0u &&
           identity->target_generation != 0u;
}

static int vd_control_identity_matches(
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

static int vd_control_fill_random(VdAttachControl *control,
                                  uint8_t *output,
                                  size_t size) {
    unsigned int attempt;
    for (attempt = 0u; attempt < 4u; ++attempt) {
        if (control->config.entropy(control->config.callback_context,
                                    output, size) != 0) {
            memset(output, 0, size);
            return VD_ATTACH_CONTROL_ERROR_ENTROPY;
        }
        if (vd_control_bytes_nonzero(output, size)) {
            return VD_ATTACH_CONTROL_OK;
        }
    }
    memset(output, 0, size);
    return VD_ATTACH_CONTROL_ERROR_ENTROPY;
}

static int vd_control_make_u64(VdAttachControl *control, uint64_t *output) {
    uint8_t bytes[8];
    uint64_t value = 0u;
    size_t i;
    int result;

    if (output == NULL) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    result = vd_control_fill_random(control, bytes, sizeof(bytes));
    if (result != VD_ATTACH_CONTROL_OK) {
        return result;
    }
    for (i = 0u; i < sizeof(bytes); ++i) {
        value = (value << 8) | bytes[i];
    }
    memset(bytes, 0, sizeof(bytes));
    if (value == 0u) {
        return VD_ATTACH_CONTROL_ERROR_ENTROPY;
    }
    *output = value;
    return VD_ATTACH_CONTROL_OK;
}

static uint64_t vd_control_deadline(VdAttachControl *control,
                                    uint64_t now_ms) {
    return vd_control_add_ms(now_ms, control->config.callback_timeout_ms);
}

static VdAttachControlSession *vd_control_find_session(
    VdAttachControl *control, uint64_t session_id, uint64_t peer_id,
    int *error) {
    size_t i;
    for (i = 0u; i < VD_ATTACH_CONTROL_MAX_SESSIONS; ++i) {
        VdAttachControlSession *session = &control->sessions[i];
        if (session->active && session->session_id == session_id) {
            if (session->peer_id != peer_id) {
                *error = VD_ATTACH_CONTROL_ERROR_PEER;
                return NULL;
            }
            *error = VD_ATTACH_CONTROL_OK;
            return session;
        }
    }
    *error = VD_ATTACH_CONTROL_ERROR_STATE;
    return NULL;
}

static int vd_control_nonce_seen(
    const uint8_t nonces[VD_ATTACH_CONTROL_MAX_REPLAY_NONCES]
                        [VD_ATTACH_CONTROL_NONCE_BYTES],
    uint32_t count,
    const uint8_t nonce[VD_ATTACH_CONTROL_NONCE_BYTES]) {
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        if (memcmp(nonces[i], nonce, VD_ATTACH_CONTROL_NONCE_BYTES) == 0) {
            return 1;
        }
    }
    return 0;
}

static int vd_control_claim_nonce(
    uint8_t nonces[VD_ATTACH_CONTROL_MAX_REPLAY_NONCES]
                  [VD_ATTACH_CONTROL_NONCE_BYTES],
    uint32_t *count,
    const uint8_t nonce[VD_ATTACH_CONTROL_NONCE_BYTES]) {
    if (vd_control_nonce_seen(nonces, *count, nonce)) {
        return VD_ATTACH_CONTROL_ERROR_REPLAY;
    }
    if (*count >= VD_ATTACH_CONTROL_MAX_REPLAY_NONCES) {
        return VD_ATTACH_CONTROL_ERROR_ROLLOVER_REQUIRED;
    }
    memcpy(nonces[*count], nonce, VD_ATTACH_CONTROL_NONCE_BYTES);
    ++*count;
    return VD_ATTACH_CONTROL_OK;
}

static int vd_control_server_nonce_in_use(
    const VdAttachControl *control,
    const VdAttachControlSession *candidate) {
    size_t i;
    for (i = 0u; i < VD_ATTACH_CONTROL_MAX_SESSIONS; ++i) {
        const VdAttachControlSession *session = &control->sessions[i];
        if (session != candidate && session->active &&
            memcmp(session->server_nonce, candidate->server_nonce,
                   VD_ATTACH_CONTROL_NONCE_BYTES) == 0) {
            return 1;
        }
    }
    return 0;
}

static int vd_control_resolve(VdAttachControl *control,
                              const char *title_id,
                              VdAttachTargetIdentity *identity,
                              uint64_t deadline_ms) {
    VdAttachInventoryResult result;

    memset(identity, 0, sizeof(*identity));
    result = control->config.resolve_target(
        control->config.callback_context, title_id, identity, deadline_ms);
    if (result != VD_ATTACH_INVENTORY_FOUND) {
        memset(identity, 0, sizeof(*identity));
        return result == VD_ATTACH_INVENTORY_CHANGED
                   ? VD_ATTACH_CONTROL_ERROR_STALE_TARGET
                   : VD_ATTACH_CONTROL_ERROR_TARGET;
    }
    if (!vd_control_identity_valid(identity, title_id)) {
        memset(identity, 0, sizeof(*identity));
        return VD_ATTACH_CONTROL_ERROR_TARGET;
    }
    return VD_ATTACH_CONTROL_OK;
}

static void vd_control_make_action(const VdAttachControl *control,
                                   VdAttachControlModuleAction *action,
                                   VdAttachControlJournalCapability capability,
                                   const VdAttachControlAuthorization
                                       *signed_authorization) {
    memset(action, 0, sizeof(*action));
    action->version = VD_ATTACH_CONTROL_VERSION;
    action->fixed_module_slot = VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT;
    action->service_generation = control->service_generation;
    action->lease_id = control->lease.lease_id;
    action->lease_expires_at_ms = control->lease.expires_at_ms;
    action->owner_host_key_id = control->lease.owner_host_key_id;
    action->target = control->lease.target;
    action->injected_module_uid = control->lease.injected_module_uid;
    action->journal_capability = (uint32_t)capability;
    if (signed_authorization != NULL) {
        action->has_signed_authorization = 1;
        action->signed_authorization = *signed_authorization;
    }
}

static void vd_control_clear_lease(VdAttachControl *control) {
    memset(&control->lease, 0, sizeof(control->lease));
    control->lease.state = VD_ATTACH_CONTROL_LEASE_IDLE;
}

static int vd_control_grant_present(
    const VdAttachControlLeaseGrant *grant) {
    return grant->lease_id != 0u || grant->lease_expires_at_ms != 0u ||
           grant->injected_module_uid != 0u;
}

static int vd_control_grant_valid(
    const VdAttachControlLeaseGrant *grant, uint64_t load_started_at_ms,
    uint32_t requested_lease_ms, uint64_t session_expires_at_ms) {
    uint64_t maximum_expiry =
        vd_control_add_ms(load_started_at_ms, requested_lease_ms);
    maximum_expiry = vd_control_min_u64(maximum_expiry,
                                        session_expires_at_ms);
    return grant->lease_id != 0u &&
           grant->lease_expires_at_ms > load_started_at_ms &&
           grant->lease_expires_at_ms <= maximum_expiry &&
           grant->injected_module_uid > 0u &&
           grant->injected_module_uid <= (uint32_t)INT32_MAX;
}

static int vd_control_cleanup_deadline_open(VdAttachControl *control,
                                            uint64_t deadline_ms) {
    return control->config.now_ms(control->config.callback_context) <
           deadline_ms;
}

static int vd_control_reconcile_gone(VdAttachControl *control,
                                     uint64_t deadline_ms,
                                     VdAttachControlJournalCapability capability,
                                     const VdAttachControlAuthorization
                                         *signed_authorization) {
    VdAttachControlModuleAction action;
    VdAttachControlModulePresence presence;

    if (!vd_control_cleanup_deadline_open(control, deadline_ms)) {
        return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
    }
    vd_control_make_action(control, &action, capability,
                           signed_authorization);
    presence = control->config.probe_fixed(control->config.callback_context,
                                            &action, deadline_ms);
    if (presence == VD_ATTACH_CONTROL_MODULE_GONE) {
        control->lease.module_started = 0;
        control->lease.module_loaded = 0;
        if (!vd_control_cleanup_deadline_open(control, deadline_ms)) {
            return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
        }
        vd_control_clear_lease(control);
        return VD_ATTACH_CONTROL_OK;
    }
    if (!vd_control_cleanup_deadline_open(control, deadline_ms)) {
        return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
    }
    return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
}

/*
 * Reverse-order cleanup uses one absolute deadline and revalidates the entire
 * target snapshot before each lifecycle call.  If the PID has been reused or
 * any identity field changed, no callback is allowed to touch that PID.
 */
static int vd_control_cleanup(
    VdAttachControl *control, uint64_t deadline_ms,
    VdAttachControlJournalCapability capability,
    const VdAttachControlAuthorization *signed_authorization) {
    VdAttachTargetIdentity current;
    VdAttachControlModuleAction action;
    int action_result;
    int result;

    if (control->lease.state == VD_ATTACH_CONTROL_LEASE_IDLE) {
        return VD_ATTACH_CONTROL_OK;
    }
    control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;

    if (!control->lease.module_loaded) {
        vd_control_clear_lease(control);
        return VD_ATTACH_CONTROL_OK;
    }
    if (control->lease.lease_id == 0u ||
        control->lease.expires_at_ms == 0u ||
        control->lease.injected_module_uid == 0u ||
        control->lease.injected_module_uid > (uint32_t)INT32_MAX) {
        return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
    }
    if (!vd_control_cleanup_deadline_open(control, deadline_ms)) {
        return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
    }

    result = vd_control_resolve(control, control->lease.target.title_id,
                                &current, deadline_ms);
    if (!vd_control_cleanup_deadline_open(control, deadline_ms)) {
        return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
    }
    if (result != VD_ATTACH_CONTROL_OK ||
        !vd_control_identity_matches(&current, &control->lease.target)) {
        return vd_control_reconcile_gone(control, deadline_ms, capability,
                                         signed_authorization);
    }
    vd_control_make_action(control, &action, capability,
                           signed_authorization);

    if (control->lease.module_started) {
        action_result = control->config.stop_fixed(
            control->config.callback_context, &action, deadline_ms);
        if (action_result == 0) {
            control->lease.module_started = 0;
        }
        if (!vd_control_cleanup_deadline_open(control, deadline_ms)) {
            return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
        }
        if (action_result != 0) {
            return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
        }

        result = vd_control_resolve(control, control->lease.target.title_id,
                                    &current, deadline_ms);
        if (!vd_control_cleanup_deadline_open(control, deadline_ms)) {
            return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
        }
        if (result != VD_ATTACH_CONTROL_OK ||
            !vd_control_identity_matches(&current, &control->lease.target)) {
            return vd_control_reconcile_gone(
                control, deadline_ms, capability, signed_authorization);
        }
    }

    vd_control_make_action(control, &action, capability,
                           signed_authorization);
    if (!vd_control_cleanup_deadline_open(control, deadline_ms)) {
        return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
    }
    action_result = control->config.unload_fixed(
        control->config.callback_context, &action, deadline_ms);
    if (action_result == 0) {
        control->lease.module_loaded = 0;
    }
    if (!vd_control_cleanup_deadline_open(control, deadline_ms) ||
        action_result != 0) {
        return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
    }
    vd_control_clear_lease(control);
    return VD_ATTACH_CONTROL_OK;
}

static int vd_control_rollback_with_result(VdAttachControl *control,
                                           int completed_result) {
    const uint64_t now_ms =
        control->config.now_ms(control->config.callback_context);
    const int cleanup_result = vd_control_cleanup(
        control, vd_control_deadline(control, now_ms),
        VD_ATTACH_CONTROL_JOURNAL_ROLLBACK, NULL);
    return cleanup_result == VD_ATTACH_CONTROL_OK
               ? completed_result
               : VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
}

static int vd_control_get_authenticated(
    VdAttachControl *control, uint64_t session_id, uint64_t peer_id,
    VdAttachControlSession **session_out, uint64_t now_ms) {
    VdAttachControlSession *session;
    int error;

    session = vd_control_find_session(control, session_id, peer_id, &error);
    if (session == NULL) {
        return error;
    }
    if (!session->authenticated) {
        return VD_ATTACH_CONTROL_ERROR_AUTH;
    }
    if (now_ms >= session->auth_expires_at_ms) {
        session->authenticated = 0;
        session->host_key_id = 0u;
        memset(session->client_nonce, 0, sizeof(session->client_nonce));
        return VD_ATTACH_CONTROL_ERROR_EXPIRED;
    }
    *session_out = session;
    return VD_ATTACH_CONTROL_OK;
}

static int vd_control_proof_valid(
    const VdAttachControl *control,
    const VdAttachControlSession *session,
    const VdAttachControlOperationProof *proof,
    VdAttachControlOperation operation,
    uint64_t now_ms) {
    const int is_attach = operation == VD_ATTACH_CONTROL_OPERATION_ATTACH;
    if (proof == NULL || !vd_control_title_valid(proof->target_title_id) ||
        proof->expected_target_generation == 0u ||
        !vd_control_bytes_nonzero(proof->request_nonce,
                                  sizeof(proof->request_nonce)) ||
        !vd_control_bytes_nonzero(proof->signature,
                                  sizeof(proof->signature)) ||
        proof->expires_at_ms <= now_ms ||
        proof->expires_at_ms > session->auth_expires_at_ms ||
        proof->expires_at_ms >
            vd_control_add_ms(now_ms, control->config.auth_window_ms)) {
        return 0;
    }
    if (is_attach) {
        return proof->requested_lease_ms >= control->config.min_lease_ms &&
               proof->requested_lease_ms <= control->config.max_lease_ms;
    }
    return proof->requested_lease_ms == 0u;
}

static void vd_control_build_authorization(
    const VdAttachControl *control,
    const VdAttachControlSession *session,
    const VdAttachControlOperationProof *proof,
    const VdAttachTargetIdentity *target,
    VdAttachControlOperation operation,
    VdAttachControlAuthorization *authorization) {
    memset(authorization, 0, sizeof(*authorization));
    authorization->version = VD_ATTACH_CONTROL_VERSION;
    authorization->operation = (uint32_t)operation;
    authorization->fixed_module_slot =
        VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT;
    authorization->service_generation = control->service_generation;
    authorization->session_id = session->session_id;
    authorization->transport_binding = session->peer_id;
    authorization->host_key_id = session->host_key_id;
    authorization->expires_at_ms = proof->expires_at_ms;
    authorization->session_expires_at_ms = session->auth_expires_at_ms;
    authorization->requested_lease_ms = proof->requested_lease_ms;
    if (operation != VD_ATTACH_CONTROL_OPERATION_ATTACH) {
        authorization->lease_id = control->lease.lease_id;
        authorization->lease_expires_at_ms = control->lease.expires_at_ms;
        authorization->injected_module_uid =
            control->lease.injected_module_uid;
    }
    authorization->target = *target;
    memcpy(authorization->server_nonce, session->server_nonce,
           sizeof(authorization->server_nonce));
    memcpy(authorization->client_nonce, session->client_nonce,
           sizeof(authorization->client_nonce));
    memcpy(authorization->request_nonce, proof->request_nonce,
           sizeof(authorization->request_nonce));
    memcpy(authorization->signature, proof->signature,
           sizeof(authorization->signature));
}

static int vd_control_authorize_operation(
    VdAttachControl *control,
    VdAttachControlSession *session,
    const VdAttachControlOperationProof *proof,
    VdAttachControlOperation operation,
    const VdAttachTargetIdentity *target,
    VdAttachControlAuthorization *authorization,
    uint64_t now_ms,
    uint64_t deadline_ms) {
    uint8_t signed_bytes[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    size_t signed_size = 0u;
    int result;

    now_ms = control->config.now_ms(control->config.callback_context);
    if (!vd_control_proof_valid(control, session, proof, operation, now_ms)) {
        return proof->expires_at_ms <= now_ms ||
                       session->auth_expires_at_ms <= now_ms
                   ? VD_ATTACH_CONTROL_ERROR_EXPIRED
                   : VD_ATTACH_CONTROL_ERROR_AUTH;
    }
    if (vd_control_nonce_seen(session->operation_nonces,
                              session->operation_nonce_count,
                              proof->request_nonce)) {
        return VD_ATTACH_CONTROL_ERROR_REPLAY;
    }
    if (session->operation_nonce_count >=
        VD_ATTACH_CONTROL_MAX_REPLAY_NONCES) {
        return VD_ATTACH_CONTROL_ERROR_ROLLOVER_REQUIRED;
    }
    deadline_ms = vd_control_min_u64(
        deadline_ms,
        vd_control_bounded_deadline(control, now_ms, proof->expires_at_ms,
                                    session->auth_expires_at_ms));
    vd_control_build_authorization(control, session, proof, target, operation,
                                   authorization);
    result = vd_attach_control_encode_operation_authorization(
        authorization, signed_bytes, sizeof(signed_bytes), &signed_size);
    if (result != VD_ATTACH_CONTROL_OK) {
        memset(authorization, 0, sizeof(*authorization));
        memset(signed_bytes, 0, sizeof(signed_bytes));
        return VD_ATTACH_CONTROL_ERROR_STATE;
    }
    if (control->config.verify_operation(
            control->config.callback_context, authorization,
            signed_bytes, signed_size, deadline_ms) != 1) {
        memset(authorization, 0, sizeof(*authorization));
        memset(signed_bytes, 0, sizeof(signed_bytes));
        return VD_ATTACH_CONTROL_ERROR_AUTH;
    }
    if (!vd_control_before_deadline(control, proof->expires_at_ms,
                                    session->auth_expires_at_ms,
                                    deadline_ms)) {
        memset(authorization, 0, sizeof(*authorization));
        memset(signed_bytes, 0, sizeof(signed_bytes));
        return VD_ATTACH_CONTROL_ERROR_EXPIRED;
    }
    result = vd_control_claim_nonce(session->operation_nonces,
                                    &session->operation_nonce_count,
                                    proof->request_nonce);
    if (result != VD_ATTACH_CONTROL_OK) {
        memset(authorization, 0, sizeof(*authorization));
    }
    memset(signed_bytes, 0, sizeof(signed_bytes));
    return result;
}

static int vd_control_require_original_target(
    const VdAttachControl *control,
    const VdAttachControlOperationProof *proof,
    const VdAttachTargetIdentity *current) {
    return memcmp(proof->target_title_id, control->lease.target.title_id,
                  VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) == 0 &&
           proof->expected_target_generation ==
               control->lease.target.target_generation &&
           vd_control_identity_matches(current, &control->lease.target);
}

static void vd_control_copy_snapshot(
    const VdAttachControl *control,
    VdAttachControlLeaseSnapshot *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = control->lease.state;
    snapshot->lease_id = control->lease.lease_id;
    snapshot->expires_at_ms = control->lease.expires_at_ms;
    snapshot->owner_host_key_id = control->lease.owner_host_key_id;
    snapshot->target = control->lease.target;
    snapshot->injected_module_uid = control->lease.injected_module_uid;
    snapshot->module_loaded = control->lease.module_loaded;
    snapshot->module_started = control->lease.module_started;
}

int vd_attach_control_init(VdAttachControl *control,
                           const VdAttachControlConfig *config) {
    int result;
    if (control == NULL || config == NULL) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    if (control->initialized) {
        return VD_ATTACH_CONTROL_ERROR_STATE;
    }
    if (config->entropy == NULL ||
        config->now_ms == NULL || config->verify_peer == NULL ||
        config->verify_operation == NULL || config->resolve_target == NULL ||
        config->load_fixed == NULL || config->start_fixed == NULL ||
        config->stop_fixed == NULL || config->unload_fixed == NULL ||
        config->probe_fixed == NULL ||
        config->challenge_timeout_ms < VD_ATTACH_CONTROL_MIN_TIMEOUT_MS ||
        config->challenge_timeout_ms > VD_ATTACH_CONTROL_MAX_TIMEOUT_MS ||
        config->auth_window_ms < VD_ATTACH_CONTROL_MIN_TIMEOUT_MS ||
        config->auth_window_ms > VD_ATTACH_CONTROL_MAX_AUTH_WINDOW_MS ||
        config->min_lease_ms < VD_ATTACH_CONTROL_MIN_LEASE_MS ||
        config->max_lease_ms > VD_ATTACH_CONTROL_MAX_LEASE_MS ||
        config->min_lease_ms > config->max_lease_ms ||
        config->callback_timeout_ms < VD_ATTACH_CONTROL_MIN_TIMEOUT_MS ||
        config->callback_timeout_ms > VD_ATTACH_CONTROL_MAX_TIMEOUT_MS) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    memset(control, 0, sizeof(*control));
    control->config = *config;
    result = vd_control_make_u64(control, &control->service_generation);
    if (result != VD_ATTACH_CONTROL_OK) {
        memset(control, 0, sizeof(*control));
        return result;
    }
    control->initialized = 1;
    control->next_session_id = 1u;
    control->lease.state = VD_ATTACH_CONTROL_LEASE_IDLE;
    return VD_ATTACH_CONTROL_OK;
}

int vd_attach_control_open_session(VdAttachControl *control,
                                   uint64_t peer_id,
                                   VdAttachControlChallenge *challenge) {
    size_t i;
    unsigned int attempt;
    VdAttachControlSession *session = NULL;
    uint64_t now_ms;
    int result;

    if (control == NULL || challenge == NULL || peer_id == 0u ||
        !control->initialized) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    if (control->shutting_down) {
        return VD_ATTACH_CONTROL_ERROR_SHUTDOWN;
    }
    for (i = 0u; i < VD_ATTACH_CONTROL_MAX_SESSIONS; ++i) {
        if (!control->sessions[i].active) {
            session = &control->sessions[i];
            break;
        }
    }
    if (session == NULL) {
        return VD_ATTACH_CONTROL_ERROR_LIMIT;
    }
    if (control->next_session_id == 0u) {
        return VD_ATTACH_CONTROL_ERROR_LIMIT;
    }
    memset(session, 0, sizeof(*session));
    session->session_id = control->next_session_id++;
    for (attempt = 0u; attempt < 4u; ++attempt) {
        result = vd_control_fill_random(control, session->server_nonce,
                                        sizeof(session->server_nonce));
        if (result != VD_ATTACH_CONTROL_OK) {
            memset(session, 0, sizeof(*session));
            return result;
        }
        if (!vd_control_server_nonce_in_use(control, session)) {
            break;
        }
    }
    if (attempt == 4u) {
        memset(session, 0, sizeof(*session));
        return VD_ATTACH_CONTROL_ERROR_ENTROPY;
    }
    session->peer_id = peer_id;
    now_ms = control->config.now_ms(control->config.callback_context);
    session->opened_at_ms = now_ms;
    session->active = 1;
    memset(challenge, 0, sizeof(*challenge));
    challenge->service_generation = control->service_generation;
    challenge->session_id = session->session_id;
    challenge->transport_binding = peer_id;
    challenge->server_time_ms = now_ms;
    challenge->challenge_expires_at_ms = vd_control_add_ms(
        now_ms, control->config.challenge_timeout_ms);
    memcpy(challenge->server_nonce, session->server_nonce,
           sizeof(challenge->server_nonce));
    return VD_ATTACH_CONTROL_OK;
}

int vd_attach_control_authenticate(VdAttachControl *control,
                                   uint64_t session_id,
                                   uint64_t peer_id,
                                   const VdAttachControlPeerProof *proof) {
    VdAttachControlSession *session;
    VdAttachControlPeerTranscript transcript;
    uint8_t signed_bytes[VD_ATTACH_CONTROL_PEER_SIGNED_BYTES];
    size_t signed_size = 0u;
    uint64_t now_ms;
    uint64_t challenge_expires_at_ms;
    uint64_t deadline_ms;
    int error;

    if (control == NULL || proof == NULL || !control->initialized) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    if (control->shutting_down) {
        return VD_ATTACH_CONTROL_ERROR_SHUTDOWN;
    }
    session = vd_control_find_session(control, session_id, peer_id, &error);
    if (session == NULL) {
        return error;
    }
    if (session->authenticated ||
        session->auth_attempts >= VD_ATTACH_CONTROL_MAX_AUTH_ATTEMPTS) {
        return VD_ATTACH_CONTROL_ERROR_STATE;
    }
    now_ms = control->config.now_ms(control->config.callback_context);
    challenge_expires_at_ms = vd_control_add_ms(
        session->opened_at_ms, control->config.challenge_timeout_ms);
    if (now_ms >= challenge_expires_at_ms) {
        memset(session, 0, sizeof(*session));
        return VD_ATTACH_CONTROL_ERROR_EXPIRED;
    }
    ++session->auth_attempts;
    if (proof->host_key_id == 0u || proof->expires_at_ms <= now_ms ||
        proof->expires_at_ms >
            vd_control_add_ms(now_ms, control->config.auth_window_ms) ||
        !vd_control_bytes_nonzero(proof->client_nonce,
                                  sizeof(proof->client_nonce)) ||
        !vd_control_bytes_nonzero(proof->signature,
                                  sizeof(proof->signature))) {
        if (session->auth_attempts >= VD_ATTACH_CONTROL_MAX_AUTH_ATTEMPTS) {
            memset(session, 0, sizeof(*session));
        }
        return VD_ATTACH_CONTROL_ERROR_AUTH;
    }

    memset(&transcript, 0, sizeof(transcript));
    transcript.version = VD_ATTACH_CONTROL_VERSION;
    transcript.service_generation = control->service_generation;
    transcript.session_id = session->session_id;
    transcript.transport_binding = session->peer_id;
    transcript.host_key_id = proof->host_key_id;
    transcript.server_time_ms = session->opened_at_ms;
    transcript.challenge_expires_at_ms = challenge_expires_at_ms;
    transcript.expires_at_ms = proof->expires_at_ms;
    memcpy(transcript.server_nonce, session->server_nonce,
           sizeof(transcript.server_nonce));
    memcpy(transcript.client_nonce, proof->client_nonce,
           sizeof(transcript.client_nonce));
    memcpy(transcript.signature, proof->signature,
           sizeof(transcript.signature));
    deadline_ms = vd_control_bounded_deadline(
        control, now_ms, proof->expires_at_ms, challenge_expires_at_ms);
    if (vd_attach_control_encode_peer_transcript(
            &transcript, signed_bytes, sizeof(signed_bytes),
            &signed_size) != VD_ATTACH_CONTROL_OK) {
        memset(&transcript, 0, sizeof(transcript));
        memset(signed_bytes, 0, sizeof(signed_bytes));
        return VD_ATTACH_CONTROL_ERROR_STATE;
    }
    if (control->config.verify_peer(control->config.callback_context,
                                    &transcript, signed_bytes, signed_size,
                                    deadline_ms) != 1) {
        memset(&transcript, 0, sizeof(transcript));
        memset(signed_bytes, 0, sizeof(signed_bytes));
        if (session->auth_attempts >= VD_ATTACH_CONTROL_MAX_AUTH_ATTEMPTS) {
            memset(session, 0, sizeof(*session));
        }
        return VD_ATTACH_CONTROL_ERROR_AUTH;
    }
    if (!vd_control_before_deadline(control, proof->expires_at_ms,
                                    challenge_expires_at_ms,
                                    deadline_ms)) {
        memset(&transcript, 0, sizeof(transcript));
        memset(signed_bytes, 0, sizeof(signed_bytes));
        if (control->config.now_ms(control->config.callback_context) >=
            challenge_expires_at_ms) {
            memset(session, 0, sizeof(*session));
        }
        return VD_ATTACH_CONTROL_ERROR_EXPIRED;
    }
    session->host_key_id = proof->host_key_id;
    session->auth_expires_at_ms = proof->expires_at_ms;
    memcpy(session->client_nonce, proof->client_nonce,
           sizeof(session->client_nonce));
    session->authenticated = 1;
    memset(&transcript, 0, sizeof(transcript));
    memset(signed_bytes, 0, sizeof(signed_bytes));
    return VD_ATTACH_CONTROL_OK;
}

int vd_attach_control_begin_attach(
    VdAttachControl *control, uint64_t session_id, uint64_t peer_id,
    const VdAttachControlOperationProof *proof,
    VdAttachControlLeaseSnapshot *snapshot) {
    VdAttachControlSession *session;
    VdAttachTargetIdentity authorized_target;
    VdAttachTargetIdentity current;
    VdAttachControlAuthorization authorization;
    VdAttachControlFixedLoadRequest load_request;
    VdAttachControlLeaseGrant grant;
    VdAttachControlModuleAction action;
    uint64_t now_ms;
    uint64_t deadline_ms;
    uint64_t lease_deadline;
    uint64_t load_started_at_ms;
    int load_result;
    int result;

    if (control == NULL || proof == NULL || snapshot == NULL ||
        !control->initialized) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    if (control->shutting_down) {
        return VD_ATTACH_CONTROL_ERROR_SHUTDOWN;
    }
    now_ms = control->config.now_ms(control->config.callback_context);
    result = vd_control_get_authenticated(control, session_id, peer_id,
                                          &session, now_ms);
    if (result != VD_ATTACH_CONTROL_OK) {
        return result;
    }
    if (control->lease.state != VD_ATTACH_CONTROL_LEASE_IDLE) {
        return VD_ATTACH_CONTROL_ERROR_BUSY;
    }
    if (!vd_control_proof_valid(control, session, proof,
                                VD_ATTACH_CONTROL_OPERATION_ATTACH,
                                now_ms)) {
        return proof->expires_at_ms <= now_ms ||
                       session->auth_expires_at_ms <= now_ms
                   ? VD_ATTACH_CONTROL_ERROR_EXPIRED
                   : VD_ATTACH_CONTROL_ERROR_AUTH;
    }
    deadline_ms = vd_control_bounded_deadline(
        control, now_ms, proof->expires_at_ms,
        session->auth_expires_at_ms);
    result = vd_control_resolve(control, proof->target_title_id,
                                &authorized_target, deadline_ms);
    if (result != VD_ATTACH_CONTROL_OK) {
        return result;
    }
    if (!vd_control_before_deadline(control, proof->expires_at_ms,
                                    session->auth_expires_at_ms,
                                    deadline_ms)) {
        return VD_ATTACH_CONTROL_ERROR_EXPIRED;
    }
    if (authorized_target.target_generation !=
        proof->expected_target_generation) {
        return VD_ATTACH_CONTROL_ERROR_STALE_TARGET;
    }
    result = vd_control_authorize_operation(
        control, session, proof, VD_ATTACH_CONTROL_OPERATION_ATTACH,
        &authorized_target, &authorization, now_ms, deadline_ms);
    if (result != VD_ATTACH_CONTROL_OK) {
        return result;
    }

    /* Close the authorization-to-load PID reuse window. */
    result = vd_control_resolve(control, proof->target_title_id, &current,
                                deadline_ms);
    if (result != VD_ATTACH_CONTROL_OK ||
        !vd_control_identity_matches(&current, &authorized_target)) {
        memset(&authorization, 0, sizeof(authorization));
        return VD_ATTACH_CONTROL_ERROR_STALE_TARGET;
    }
    if (!vd_control_before_deadline(control, proof->expires_at_ms,
                                    session->auth_expires_at_ms,
                                    deadline_ms)) {
        memset(&authorization, 0, sizeof(authorization));
        return VD_ATTACH_CONTROL_ERROR_EXPIRED;
    }

    memset(&load_request, 0, sizeof(load_request));
    load_request.authorization = authorization;
    memset(&grant, 0, sizeof(grant));
    load_started_at_ms =
        control->config.now_ms(control->config.callback_context);
    if (!vd_control_before_deadline(control, proof->expires_at_ms,
                                    session->auth_expires_at_ms,
                                    deadline_ms)) {
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return VD_ATTACH_CONTROL_ERROR_EXPIRED;
    }

    load_result = control->config.load_fixed(
        control->config.callback_context, &load_request, &grant,
        deadline_ms);
    if (load_result != 0 && !vd_control_grant_present(&grant)) {
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return VD_ATTACH_CONTROL_ERROR_BACKEND;
    }

    vd_control_clear_lease(control);
    control->lease.state = VD_ATTACH_CONTROL_LEASE_STARTING;
    control->lease.lease_id = grant.lease_id;
    control->lease.expires_at_ms = grant.lease_expires_at_ms;
    control->lease.owner_session_id = session->session_id;
    control->lease.owner_host_key_id = session->host_key_id;
    control->lease.target = authorized_target;
    control->lease.injected_module_uid = grant.injected_module_uid;
    control->lease.module_loaded = 1;

    if (!vd_control_grant_valid(
            &grant, load_started_at_ms,
            authorization.requested_lease_ms,
            authorization.session_expires_at_ms)) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
    }
    lease_deadline = grant.lease_expires_at_ms;
    deadline_ms = vd_control_min_u64(deadline_ms, lease_deadline);
    if (load_result != 0) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        result = vd_control_rollback_with_result(
            control, VD_ATTACH_CONTROL_ERROR_BACKEND);
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return result;
    }
    if (!vd_control_before_deadline(control, proof->expires_at_ms,
                                    lease_deadline, deadline_ms)) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        result = vd_control_rollback_with_result(
            control, VD_ATTACH_CONTROL_ERROR_EXPIRED);
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return result;
    }

    /* Revalidate again before module start. */
    result = vd_control_resolve(control, proof->target_title_id, &current,
                                deadline_ms);
    if (result != VD_ATTACH_CONTROL_OK ||
        !vd_control_identity_matches(&current, &authorized_target)) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
    }
    if (!vd_control_before_deadline(control, proof->expires_at_ms,
                                    lease_deadline, deadline_ms)) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        result = vd_control_rollback_with_result(
            control, VD_ATTACH_CONTROL_ERROR_EXPIRED);
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return result;
    }

    vd_control_make_action(control, &action,
                           VD_ATTACH_CONTROL_JOURNAL_START,
                           &authorization);
    /* A failed start may have run partially, so rollback includes stop. */
    control->lease.module_started = 1;
    if (control->config.start_fixed(control->config.callback_context,
                                    &action, deadline_ms) != 0) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        result = vd_control_rollback_with_result(
            control, VD_ATTACH_CONTROL_ERROR_BACKEND);
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return result;
    }
    if (!vd_control_before_deadline(control, proof->expires_at_ms,
                                    lease_deadline, deadline_ms)) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        result = vd_control_rollback_with_result(
            control, VD_ATTACH_CONTROL_ERROR_EXPIRED);
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return result;
    }
    result = vd_control_resolve(control, proof->target_title_id, &current,
                                deadline_ms);
    if (result != VD_ATTACH_CONTROL_OK ||
        !vd_control_identity_matches(&current, &authorized_target)) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return VD_ATTACH_CONTROL_RECOVERY_REQUIRED;
    }
    if (!vd_control_before_deadline(control, proof->expires_at_ms,
                                    lease_deadline, deadline_ms)) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        result = vd_control_rollback_with_result(
            control, VD_ATTACH_CONTROL_ERROR_EXPIRED);
        memset(&authorization, 0, sizeof(authorization));
        memset(&load_request, 0, sizeof(load_request));
        memset(&grant, 0, sizeof(grant));
        return result;
    }
    control->lease.state = VD_ATTACH_CONTROL_LEASE_ACTIVE;
    vd_control_copy_snapshot(control, snapshot);
    memset(&authorization, 0, sizeof(authorization));
    memset(&load_request, 0, sizeof(load_request));
    memset(&grant, 0, sizeof(grant));
    return VD_ATTACH_CONTROL_OK;
}

static int vd_control_authorized_cleanup(
    VdAttachControl *control, uint64_t session_id, uint64_t peer_id,
    const VdAttachControlOperationProof *proof,
    VdAttachControlOperation operation) {
    VdAttachControlSession *session;
    VdAttachTargetIdentity current;
    VdAttachTargetIdentity revalidated;
    VdAttachControlAuthorization authorization;
    uint64_t now_ms;
    uint64_t deadline_ms;
    int result;

    if (control == NULL || proof == NULL || !control->initialized) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    if (control->shutting_down) {
        return VD_ATTACH_CONTROL_ERROR_SHUTDOWN;
    }
    now_ms = control->config.now_ms(control->config.callback_context);
    result = vd_control_get_authenticated(control, session_id, peer_id,
                                          &session, now_ms);
    if (result != VD_ATTACH_CONTROL_OK) {
        return result;
    }
    if ((operation == VD_ATTACH_CONTROL_OPERATION_DETACH &&
         control->lease.state != VD_ATTACH_CONTROL_LEASE_ACTIVE) ||
        (operation == VD_ATTACH_CONTROL_OPERATION_RECOVER &&
         control->lease.state != VD_ATTACH_CONTROL_LEASE_RECOVERY)) {
        return VD_ATTACH_CONTROL_ERROR_STATE;
    }
    if (control->lease.owner_host_key_id != session->host_key_id) {
        return VD_ATTACH_CONTROL_ERROR_AUTH;
    }
    if (operation == VD_ATTACH_CONTROL_OPERATION_DETACH &&
        control->lease.owner_session_id != session->session_id) {
        return VD_ATTACH_CONTROL_ERROR_AUTH;
    }
    if (!vd_control_proof_valid(control, session, proof, operation, now_ms)) {
        return proof->expires_at_ms <= now_ms ||
                       session->auth_expires_at_ms <= now_ms
                   ? VD_ATTACH_CONTROL_ERROR_EXPIRED
                   : VD_ATTACH_CONTROL_ERROR_AUTH;
    }
    deadline_ms = vd_control_bounded_deadline(
        control, now_ms, proof->expires_at_ms,
        session->auth_expires_at_ms);
    result = vd_control_resolve(control, proof->target_title_id, &current,
                                deadline_ms);
    if (result != VD_ATTACH_CONTROL_OK ||
        !vd_control_require_original_target(control, proof, &current)) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        return VD_ATTACH_CONTROL_ERROR_STALE_TARGET;
    }
    if (!vd_control_before_deadline(control, proof->expires_at_ms,
                                    session->auth_expires_at_ms,
                                    deadline_ms)) {
        return VD_ATTACH_CONTROL_ERROR_EXPIRED;
    }
    result = vd_control_authorize_operation(control, session, proof, operation,
                                            &current, &authorization, now_ms,
                                            deadline_ms);
    if (result != VD_ATTACH_CONTROL_OK) {
        return result;
    }

    /* Close the verifier-to-cleanup PID reuse window. */
    result = vd_control_resolve(control, proof->target_title_id, &revalidated,
                                deadline_ms);
    if (result != VD_ATTACH_CONTROL_OK ||
        !vd_control_identity_matches(&revalidated, &current)) {
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        memset(&authorization, 0, sizeof(authorization));
        return VD_ATTACH_CONTROL_ERROR_STALE_TARGET;
    }
    now_ms = control->config.now_ms(control->config.callback_context);
    result = vd_control_cleanup(
        control, vd_control_deadline(control, now_ms),
        VD_ATTACH_CONTROL_JOURNAL_HOST_CLEANUP, &authorization);
    memset(&authorization, 0, sizeof(authorization));
    return result;
}

int vd_attach_control_detach(VdAttachControl *control,
                             uint64_t session_id,
                             uint64_t peer_id,
                             const VdAttachControlOperationProof *proof) {
    return vd_control_authorized_cleanup(
        control, session_id, peer_id, proof,
        VD_ATTACH_CONTROL_OPERATION_DETACH);
}

int vd_attach_control_recover(VdAttachControl *control,
                              uint64_t session_id,
                              uint64_t peer_id,
                              const VdAttachControlOperationProof *proof) {
    return vd_control_authorized_cleanup(
        control, session_id, peer_id, proof,
        VD_ATTACH_CONTROL_OPERATION_RECOVER);
}

int vd_attach_control_service(VdAttachControl *control) {
    uint64_t now_ms;
    VdAttachControlJournalCapability capability;
    if (control == NULL || !control->initialized) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    if (control->lease.state == VD_ATTACH_CONTROL_LEASE_IDLE) {
        return VD_ATTACH_CONTROL_OK;
    }
    now_ms = control->config.now_ms(control->config.callback_context);
    if (control->lease.state == VD_ATTACH_CONTROL_LEASE_ACTIVE &&
        now_ms < control->lease.expires_at_ms) {
        return VD_ATTACH_CONTROL_OK;
    }
    capability = control->lease.state == VD_ATTACH_CONTROL_LEASE_ACTIVE
                     ? VD_ATTACH_CONTROL_JOURNAL_LEASE_EXPIRED
                     : VD_ATTACH_CONTROL_JOURNAL_RECOVERY_RETRY;
    control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
    return vd_control_cleanup(control, vd_control_deadline(control, now_ms),
                              capability, NULL);
}

int vd_attach_control_close_session(VdAttachControl *control,
                                    uint64_t session_id,
                                    uint64_t peer_id) {
    VdAttachControlSession *session;
    uint64_t now_ms;
    int error;
    int result = VD_ATTACH_CONTROL_OK;

    if (control == NULL || !control->initialized) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    session = vd_control_find_session(control, session_id, peer_id, &error);
    if (session == NULL) {
        return error;
    }
    if (control->lease.state != VD_ATTACH_CONTROL_LEASE_IDLE &&
        control->lease.owner_session_id == session->session_id) {
        now_ms = control->config.now_ms(control->config.callback_context);
        control->lease.state = VD_ATTACH_CONTROL_LEASE_RECOVERY;
        result = vd_control_cleanup(
            control, vd_control_deadline(control, now_ms),
            VD_ATTACH_CONTROL_JOURNAL_DISCONNECT, NULL);
        if (control->lease.state != VD_ATTACH_CONTROL_LEASE_IDLE) {
            control->lease.owner_session_id = 0u;
        }
    }
    memset(session, 0, sizeof(*session));
    return result;
}

int vd_attach_control_snapshot(const VdAttachControl *control,
                               VdAttachControlLeaseSnapshot *snapshot) {
    if (control == NULL || snapshot == NULL || !control->initialized) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    vd_control_copy_snapshot(control, snapshot);
    return VD_ATTACH_CONTROL_OK;
}

int vd_attach_control_shutdown(VdAttachControl *control) {
    uint64_t now_ms;
    int result;
    if (control == NULL || !control->initialized) {
        return VD_ATTACH_CONTROL_ERROR_ARGUMENT;
    }
    now_ms = control->config.now_ms(control->config.callback_context);
    if (control->shutting_down &&
        control->lease.state == VD_ATTACH_CONTROL_LEASE_IDLE) {
        return VD_ATTACH_CONTROL_OK;
    }
    result = vd_control_cleanup(
        control, vd_control_deadline(control, now_ms),
        VD_ATTACH_CONTROL_JOURNAL_SHUTDOWN, NULL);
    control->shutting_down = 1;
    memset(control->sessions, 0, sizeof(control->sessions));
    if (control->lease.state != VD_ATTACH_CONTROL_LEASE_IDLE) {
        control->lease.owner_session_id = 0u;
    }
    return result;
}

#include "vitadebug_attach_control.h"
#include "vitadebug_attach_control_wire.h"
#include "control_golden_vectors.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct FakeControlContext {
    VdAttachControl *control;
    uint64_t now_ms;
    uint8_t entropy_value;
    VdAttachInventoryResult inventory_result;
    VdAttachTargetIdentity identity;
    VdAttachTargetIdentity changed_identity;
    unsigned int change_identity_on_resolve;
    unsigned int resolve_calls;
    int verify_peer_result;
    int verify_operation_result;
    int load_result;
    int return_uid_on_load_failure;
    int start_result;
    int stop_result;
    int unload_result;
    VdAttachControlModulePresence probe_presence;
    uint32_t module_uid;
    uint64_t next_lease_id;
    uint64_t grant_expiry_override;
    uint64_t last_load_started_ms;
    int journal_active;
    VdAttachControlLeaseGrant journal_grant;
    VdAttachTargetIdentity journal_target;
    uint64_t journal_service_generation;
    uint64_t journal_owner_host_key_id;
    unsigned int privileged_attach_verifications;
    unsigned int privileged_cleanup_verifications;
    unsigned int privileged_cleanup_claims;
    uint8_t privileged_cleanup_nonces[VD_ATTACH_CONTROL_MAX_REPLAY_NONCES]
                                      [VD_ATTACH_CONTROL_NONCE_BYTES];
    uint32_t privileged_cleanup_nonce_count;
    uint8_t current_cleanup_nonce[VD_ATTACH_CONTROL_NONCE_BYTES];
    int has_current_cleanup_nonce;
    unsigned int journal_capability_calls[8];
    unsigned int peer_verify_calls;
    unsigned int operation_verify_calls;
    unsigned int load_calls;
    unsigned int start_calls;
    unsigned int stop_calls;
    unsigned int unload_calls;
    unsigned int probe_calls;
    uint32_t advance_peer_verify_ms;
    uint32_t advance_operation_verify_ms;
    uint32_t advance_resolve_ms;
    uint32_t advance_load_ms;
    uint32_t advance_start_ms;
    uint32_t advance_stop_ms;
    uint32_t advance_unload_ms;
    uint32_t advance_probe_ms;
    char actions[256];
    size_t action_count;
    VdAttachControlPeerTranscript last_peer;
    VdAttachControlAuthorization last_authorization;
    VdAttachControlFixedLoadRequest last_load;
    VdAttachControlModuleAction last_action;
    uint8_t last_peer_signed[VD_ATTACH_CONTROL_PEER_SIGNED_BYTES];
    size_t last_peer_signed_size;
    uint8_t last_operation_signed[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    size_t last_operation_signed_size;
    uint64_t first_deadline;
    uint64_t last_deadline;
    int deadline_changed;
} FakeControlContext;

/* Deterministic test authenticator only; production requires Ed25519. */
static void fake_sign(const uint8_t *input,
                      size_t input_size,
                      uint8_t signature[VD_ATTACH_CONTROL_SIGNATURE_BYTES]) {
    uint64_t state = UINT64_C(1469598103934665603);
    size_t i;
    for (i = 0u; i < input_size; ++i) {
        state ^= input[i];
        state *= UINT64_C(1099511628211);
        state ^= state >> 29;
    }
    for (i = 0u; i < VD_ATTACH_CONTROL_SIGNATURE_BYTES; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        signature[i] = (uint8_t)(state >> ((i & 7u) * 8u));
    }
}

static int fake_signature_matches(
    const uint8_t *input,
    size_t input_size,
    const uint8_t signature[VD_ATTACH_CONTROL_SIGNATURE_BYTES]) {
    uint8_t expected[VD_ATTACH_CONTROL_SIGNATURE_BYTES];
    int matches;
    fake_sign(input, input_size, expected);
    matches = memcmp(expected, signature, sizeof(expected)) == 0;
    memset(expected, 0, sizeof(expected));
    return matches;
}

static int fake_authorization_signature_matches(
    const VdAttachControlAuthorization *authorization) {
    uint8_t signed_bytes[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    size_t signed_size = 0u;
    int matches;
    assert(vd_attach_control_encode_operation_authorization(
               authorization, signed_bytes, sizeof(signed_bytes),
               &signed_size) == VD_ATTACH_CONTROL_OK);
    matches = fake_signature_matches(signed_bytes, signed_size,
                                     authorization->signature);
    memset(signed_bytes, 0, sizeof(signed_bytes));
    return matches;
}

static void fake_require_exact_journal_action(
    FakeControlContext *context,
    const VdAttachControlModuleAction *action,
    int is_start) {
    assert(context->journal_active);
    assert(action->version == VD_ATTACH_CONTROL_VERSION);
    assert(action->fixed_module_slot ==
           VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT);
    assert(action->service_generation ==
           context->journal_service_generation);
    assert(action->lease_id == context->journal_grant.lease_id);
    assert(action->lease_expires_at_ms ==
           context->journal_grant.lease_expires_at_ms);
    assert(action->owner_host_key_id ==
           context->journal_owner_host_key_id);
    assert(memcmp(action->target.title_id,
                  context->journal_target.title_id,
                  VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) == 0);
    assert(action->target.pid == context->journal_target.pid);
    assert(action->target.main_modid ==
           context->journal_target.main_modid);
    assert(action->target.main_fingerprint ==
           context->journal_target.main_fingerprint);
    assert(action->target.target_generation ==
           context->journal_target.target_generation);
    assert(action->injected_module_uid ==
           context->journal_grant.injected_module_uid);
    assert(action->journal_capability > 0u &&
           action->journal_capability < 8u);
    ++context->journal_capability_calls[action->journal_capability];
    if (is_start) {
        assert(action->journal_capability ==
               VD_ATTACH_CONTROL_JOURNAL_START);
        assert(action->has_signed_authorization);
        assert(action->signed_authorization.operation ==
               VD_ATTACH_CONTROL_OPERATION_ATTACH);
        assert(fake_authorization_signature_matches(
            &action->signed_authorization));
        return;
    }
    assert(action->journal_capability !=
           VD_ATTACH_CONTROL_JOURNAL_START);
    if (action->journal_capability ==
        VD_ATTACH_CONTROL_JOURNAL_HOST_CLEANUP) {
        uint32_t nonce_index;
        int nonce_seen = 0;
        assert(action->has_signed_authorization);
        assert(action->signed_authorization.operation ==
                   VD_ATTACH_CONTROL_OPERATION_DETACH ||
               action->signed_authorization.operation ==
                   VD_ATTACH_CONTROL_OPERATION_RECOVER);
        assert(action->signed_authorization.lease_id == action->lease_id);
        assert(action->signed_authorization.lease_expires_at_ms ==
               action->lease_expires_at_ms);
        assert(action->signed_authorization.injected_module_uid ==
               action->injected_module_uid);
        assert(fake_authorization_signature_matches(
            &action->signed_authorization));
        for (nonce_index = 0u;
             nonce_index < context->privileged_cleanup_nonce_count;
             ++nonce_index) {
            if (memcmp(context->privileged_cleanup_nonces[nonce_index],
                       action->signed_authorization.request_nonce,
                       VD_ATTACH_CONTROL_NONCE_BYTES) == 0) {
                nonce_seen = 1;
                break;
            }
        }
        if (!nonce_seen) {
            assert(context->privileged_cleanup_nonce_count <
                   VD_ATTACH_CONTROL_MAX_REPLAY_NONCES);
            memcpy(context->privileged_cleanup_nonces[
                       context->privileged_cleanup_nonce_count++],
                   action->signed_authorization.request_nonce,
                   VD_ATTACH_CONTROL_NONCE_BYTES);
            memcpy(context->current_cleanup_nonce,
                   action->signed_authorization.request_nonce,
                   VD_ATTACH_CONTROL_NONCE_BYTES);
            context->has_current_cleanup_nonce = 1;
            ++context->privileged_cleanup_claims;
        } else {
            /* Same nonce is only a continuation of this stop/unload unit. */
            assert(context->has_current_cleanup_nonce);
            assert(memcmp(context->current_cleanup_nonce,
                          action->signed_authorization.request_nonce,
                          VD_ATTACH_CONTROL_NONCE_BYTES) == 0);
        }
        ++context->privileged_cleanup_verifications;
    } else {
        assert(!action->has_signed_authorization);
        assert(action->journal_capability ==
                   VD_ATTACH_CONTROL_JOURNAL_ROLLBACK ||
               action->journal_capability ==
                   VD_ATTACH_CONTROL_JOURNAL_LEASE_EXPIRED ||
               action->journal_capability ==
                   VD_ATTACH_CONTROL_JOURNAL_DISCONNECT ||
               action->journal_capability ==
                   VD_ATTACH_CONTROL_JOURNAL_SHUTDOWN ||
               action->journal_capability ==
                   VD_ATTACH_CONTROL_JOURNAL_RECOVERY_RETRY);
        if (action->journal_capability ==
            VD_ATTACH_CONTROL_JOURNAL_LEASE_EXPIRED) {
            assert(context->now_ms >= action->lease_expires_at_ms);
        }
    }
}

static void record_deadline(FakeControlContext *context,
                            uint64_t deadline_ms) {
    context->last_deadline = deadline_ms;
    if (context->first_deadline == 0u) {
        context->first_deadline = deadline_ms;
    } else if (context->first_deadline != deadline_ms) {
        context->deadline_changed = 1;
    }
}

static void record_action(FakeControlContext *context, char action) {
    assert(context->action_count + 1u < sizeof(context->actions));
    context->actions[context->action_count++] = action;
    context->actions[context->action_count] = '\0';
}

static int fake_entropy(void *opaque, uint8_t *output, size_t size) {
    FakeControlContext *context = (FakeControlContext *)opaque;
    size_t i;
    ++context->entropy_value;
    if (context->entropy_value == 0u) {
        ++context->entropy_value;
    }
    for (i = 0u; i < size; ++i) {
        output[i] = (uint8_t)(context->entropy_value + (uint8_t)i);
    }
    return 0;
}

static uint64_t fake_now(void *opaque) {
    return ((FakeControlContext *)opaque)->now_ms;
}

static VdAttachInventoryResult fake_resolve(
    void *opaque,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity,
    uint64_t deadline_ms) {
    FakeControlContext *context = (FakeControlContext *)opaque;
    ++context->resolve_calls;
    record_deadline(context, deadline_ms);
    if (context->inventory_result != VD_ATTACH_INVENTORY_FOUND) {
        return context->inventory_result;
    }
    assert(memcmp(title_id, "UVDBDEMO1", 10u) == 0);
    if (context->change_identity_on_resolve == context->resolve_calls) {
        *identity = context->changed_identity;
    } else {
        *identity = context->identity;
    }
    context->now_ms += context->advance_resolve_ms;
    return VD_ATTACH_INVENTORY_FOUND;
}

static int fake_verify_peer(
    void *opaque, const VdAttachControlPeerTranscript *transcript,
    const uint8_t *signed_bytes, size_t signed_size,
    uint64_t deadline_ms) {
    FakeControlContext *context = (FakeControlContext *)opaque;
    uint8_t encoded[VD_ATTACH_CONTROL_PEER_SIGNED_BYTES];
    size_t encoded_size = 0u;
    ++context->peer_verify_calls;
    record_deadline(context, deadline_ms);
    context->last_peer = *transcript;
    assert(transcript->version == VD_ATTACH_CONTROL_VERSION);
    assert(transcript->service_generation != 0u);
    assert(transcript->session_id != 0u);
    assert(transcript->transport_binding != 0u);
    assert(transcript->host_key_id != 0u);
    assert(transcript->server_time_ms <
           transcript->challenge_expires_at_ms);
    assert(signed_size == sizeof(encoded));
    assert(vd_attach_control_encode_peer_transcript(
               transcript, encoded, sizeof(encoded), &encoded_size) ==
           VD_ATTACH_CONTROL_OK);
    assert(encoded_size == signed_size);
    assert(memcmp(encoded, signed_bytes, signed_size) == 0);
    memcpy(context->last_peer_signed, signed_bytes, signed_size);
    context->last_peer_signed_size = signed_size;
    context->now_ms += context->advance_peer_verify_ms;
    return fake_signature_matches(signed_bytes, signed_size,
                                  transcript->signature)
               ? context->verify_peer_result : 0;
}

static int fake_verify_operation(
    void *opaque, const VdAttachControlAuthorization *authorization,
    const uint8_t *signed_bytes, size_t signed_size,
    uint64_t deadline_ms) {
    FakeControlContext *context = (FakeControlContext *)opaque;
    uint8_t encoded[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    size_t encoded_size = 0u;
    ++context->operation_verify_calls;
    record_deadline(context, deadline_ms);
    context->last_authorization = *authorization;
    assert(authorization->version == VD_ATTACH_CONTROL_VERSION);
    assert(authorization->fixed_module_slot ==
           VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT);
    assert(authorization->service_generation != 0u);
    assert(authorization->target.target_generation != 0u);
    assert(signed_size == sizeof(encoded));
    assert(vd_attach_control_encode_operation_authorization(
               authorization, encoded, sizeof(encoded), &encoded_size) ==
           VD_ATTACH_CONTROL_OK);
    assert(encoded_size == signed_size);
    assert(memcmp(encoded, signed_bytes, signed_size) == 0);
    memcpy(context->last_operation_signed, signed_bytes, signed_size);
    context->last_operation_signed_size = signed_size;
    context->now_ms += context->advance_operation_verify_ms;
    return fake_signature_matches(signed_bytes, signed_size,
                                  authorization->signature)
               ? context->verify_operation_result : 0;
}

static int fake_load_fixed(
    void *opaque, const VdAttachControlFixedLoadRequest *request,
    VdAttachControlLeaseGrant *grant, uint64_t deadline_ms) {
    FakeControlContext *context = (FakeControlContext *)opaque;
    uint64_t expiry;
    ++context->load_calls;
    record_deadline(context, deadline_ms);
    record_action(context, 'L');
    context->last_load = *request;
    context->last_load_started_ms = context->now_ms;
    assert(request->authorization.operation ==
           VD_ATTACH_CONTROL_OPERATION_ATTACH);
    assert(request->authorization.fixed_module_slot ==
           VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT);
    assert(memcmp(request->authorization.target.title_id,
                  "UVDBDEMO1", 10u) == 0);
    assert(request->authorization.target.pid == context->identity.pid);
    assert(request->authorization.session_expires_at_ms >=
           request->authorization.expires_at_ms);
    assert(fake_authorization_signature_matches(
        &request->authorization));
    ++context->privileged_attach_verifications;
    if (context->load_result == 0 || context->return_uid_on_load_failure) {
        expiry = context->now_ms +
                 request->authorization.requested_lease_ms;
        if (expiry > request->authorization.session_expires_at_ms) {
            expiry = request->authorization.session_expires_at_ms;
        }
        if (context->grant_expiry_override != 0u) {
            expiry = context->grant_expiry_override;
        }
        grant->lease_id = context->next_lease_id++;
        grant->lease_expires_at_ms = expiry;
        grant->injected_module_uid = context->module_uid;
        context->journal_active = 1;
        context->journal_grant = *grant;
        context->journal_target = request->authorization.target;
        context->journal_service_generation =
            request->authorization.service_generation;
        context->journal_owner_host_key_id =
            request->authorization.host_key_id;
    }
    context->now_ms += context->advance_load_ms;
    return context->load_result;
}

static int fake_start_fixed(void *opaque,
                            const VdAttachControlModuleAction *action,
                            uint64_t deadline_ms) {
    FakeControlContext *context = (FakeControlContext *)opaque;
    ++context->start_calls;
    record_deadline(context, deadline_ms);
    record_action(context, 'S');
    context->last_action = *action;
    fake_require_exact_journal_action(context, action, 1);
    context->now_ms += context->advance_start_ms;
    return context->start_result;
}

static int fake_stop_fixed(void *opaque,
                           const VdAttachControlModuleAction *action,
                           uint64_t deadline_ms) {
    FakeControlContext *context = (FakeControlContext *)opaque;
    ++context->stop_calls;
    record_deadline(context, deadline_ms);
    record_action(context, 'T');
    context->last_action = *action;
    fake_require_exact_journal_action(context, action, 0);
    context->now_ms += context->advance_stop_ms;
    return context->stop_result;
}

static int fake_unload_fixed(void *opaque,
                             const VdAttachControlModuleAction *action,
                             uint64_t deadline_ms) {
    FakeControlContext *context = (FakeControlContext *)opaque;
    ++context->unload_calls;
    record_deadline(context, deadline_ms);
    record_action(context, 'U');
    context->last_action = *action;
    fake_require_exact_journal_action(context, action, 0);
    context->now_ms += context->advance_unload_ms;
    if (context->unload_result == 0) {
        context->journal_active = 0;
    }
    return context->unload_result;
}

static VdAttachControlModulePresence fake_probe_fixed(
    void *opaque, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms) {
    FakeControlContext *context = (FakeControlContext *)opaque;
    ++context->probe_calls;
    record_deadline(context, deadline_ms);
    context->last_action = *action;
    fake_require_exact_journal_action(context, action, 0);
    context->now_ms += context->advance_probe_ms;
    if (context->probe_presence == VD_ATTACH_CONTROL_MODULE_GONE) {
        context->journal_active = 0;
    }
    return context->probe_presence;
}

static void setup_context(FakeControlContext *context) {
    memset(context, 0, sizeof(*context));
    context->now_ms = 1000u;
    context->inventory_result = VD_ATTACH_INVENTORY_FOUND;
    context->verify_peer_result = 1;
    context->verify_operation_result = 1;
    context->probe_presence = VD_ATTACH_CONTROL_MODULE_PRESENT;
    context->module_uid = 0x40004000u;
    context->next_lease_id = UINT64_C(0x4142434445464700);
    memcpy(context->identity.title_id, "UVDBDEMO1", 10u);
    context->identity.pid = 0x10005u;
    context->identity.main_modid = 0x40001234u;
    context->identity.main_fingerprint = 0xaabbccddu;
    context->identity.target_generation =
        UINT64_C(0x0102030405060708);
    context->changed_identity = context->identity;
    context->changed_identity.pid = 0x10006u;
    ++context->changed_identity.target_generation;
}

static void make_config(FakeControlContext *context,
                        VdAttachControlConfig *config) {
    memset(config, 0, sizeof(*config));
    config->callback_context = context;
    config->entropy = fake_entropy;
    config->now_ms = fake_now;
    config->verify_peer = fake_verify_peer;
    config->verify_operation = fake_verify_operation;
    config->resolve_target = fake_resolve;
    config->load_fixed = fake_load_fixed;
    config->start_fixed = fake_start_fixed;
    config->stop_fixed = fake_stop_fixed;
    config->unload_fixed = fake_unload_fixed;
    config->probe_fixed = fake_probe_fixed;
    config->challenge_timeout_ms = 1000u;
    config->auth_window_ms = 10000u;
    config->min_lease_ms = 250u;
    config->max_lease_ms = 5000u;
    config->callback_timeout_ms = 1000u;
}

static void setup_control(VdAttachControl *control,
                          FakeControlContext *context) {
    VdAttachControlConfig config;
    memset(control, 0, sizeof(*control));
    make_config(context, &config);
    assert(vd_attach_control_init(control, &config) ==
           VD_ATTACH_CONTROL_OK);
    context->control = control;
}

static void fill_bytes(uint8_t *bytes, size_t size, uint8_t value) {
    memset(bytes, value, size);
}

static void bytes_to_hex(const uint8_t *bytes, size_t size, char *hex) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0u; i < size; ++i) {
        hex[i * 2u] = digits[bytes[i] >> 4];
        hex[i * 2u + 1u] = digits[bytes[i] & 0x0fu];
    }
    hex[size * 2u] = '\0';
}

static void test_canonical_wire_golden_vectors(void) {
    VdAttachControlPeerTranscript peer;
    VdAttachControlAuthorization operation;
    uint8_t peer_bytes[VD_ATTACH_CONTROL_PEER_SIGNED_BYTES];
    uint8_t peer_again[VD_ATTACH_CONTROL_PEER_SIGNED_BYTES];
    uint8_t operation_bytes[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    uint8_t operation_again[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    char peer_hex[VD_ATTACH_CONTROL_PEER_SIGNED_BYTES * 2u + 1u];
    char operation_hex[
        VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES * 2u + 1u];
    size_t size = 0u;
    size_t i;

    memset(&peer, 0, sizeof(peer));
    peer.version = VD_ATTACH_CONTROL_VERSION;
    peer.service_generation = UINT64_C(0x0102030405060708);
    peer.session_id = UINT64_C(0x1112131415161718);
    peer.transport_binding = UINT64_C(0x2122232425262728);
    peer.host_key_id = UINT64_C(0x3132333435363738);
    peer.server_time_ms = UINT64_C(0x0000018bcfe56800);
    peer.challenge_expires_at_ms = UINT64_C(0x0000018bcfe56be8);
    peer.expires_at_ms = UINT64_C(0x0000018bcfe569f4);
    for (i = 0u; i < VD_ATTACH_CONTROL_NONCE_BYTES; ++i) {
        peer.server_nonce[i] = (uint8_t)i;
        peer.client_nonce[i] = (uint8_t)(i + 0x20u);
    }
    assert(vd_attach_control_encode_peer_transcript(
               &peer, peer_bytes, sizeof(peer_bytes), &size) ==
           VD_ATTACH_CONTROL_OK);
    assert(size == sizeof(peer_bytes));
    bytes_to_hex(peer_bytes, size, peer_hex);
    assert(strcmp(peer_hex, VD_ATTACH_CONTROL_GOLDEN_PEER_HEX) == 0);
    memset(peer.signature, 0xa5, sizeof(peer.signature));
    assert(vd_attach_control_encode_peer_transcript(
               &peer, peer_again, sizeof(peer_again), &size) ==
           VD_ATTACH_CONTROL_OK);
    assert(memcmp(peer_bytes, peer_again, sizeof(peer_bytes)) == 0);
    size = 99u;
    assert(vd_attach_control_encode_peer_transcript(
               &peer, peer_again, sizeof(peer_again) - 1u, &size) ==
           VD_ATTACH_CONTROL_ERROR_ARGUMENT);
    assert(size == 0u);

    memset(&operation, 0, sizeof(operation));
    operation.version = VD_ATTACH_CONTROL_VERSION;
    operation.operation = VD_ATTACH_CONTROL_OPERATION_DETACH;
    operation.fixed_module_slot = VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT;
    operation.service_generation = UINT64_C(0x0102030405060708);
    operation.session_id = UINT64_C(0x1112131415161718);
    operation.transport_binding = UINT64_C(0x2122232425262728);
    operation.host_key_id = UINT64_C(0x3132333435363738);
    operation.expires_at_ms = UINT64_C(0x0000018bcfe569f4);
    operation.session_expires_at_ms = UINT64_C(0x0000018bcfe58000);
    operation.lease_id = UINT64_C(0x4142434445464748);
    operation.lease_expires_at_ms = UINT64_C(0x0000018bcfe57000);
    operation.injected_module_uid = 0x40000042u;
    memcpy(operation.target.title_id, "UVDBDEMO1", 10u);
    operation.target.pid = 0x00010005u;
    operation.target.main_modid = 0x40001234u;
    operation.target.main_fingerprint = 0xaabbccddu;
    operation.target.target_generation =
        UINT64_C(0x5152535455565758);
    for (i = 0u; i < VD_ATTACH_CONTROL_NONCE_BYTES; ++i) {
        operation.server_nonce[i] = (uint8_t)i;
        operation.client_nonce[i] = (uint8_t)(i + 0x20u);
        operation.request_nonce[i] = (uint8_t)(i + 0x40u);
    }
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_OK);
    assert(size == sizeof(operation_bytes));
    bytes_to_hex(operation_bytes, size, operation_hex);
    assert(strcmp(operation_hex,
                  VD_ATTACH_CONTROL_GOLDEN_DETACH_HEX) == 0);
    memset(operation.signature, 0x5a, sizeof(operation.signature));
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_again, sizeof(operation_again),
               &size) == VD_ATTACH_CONTROL_OK);
    assert(memcmp(operation_bytes, operation_again,
                  sizeof(operation_bytes)) == 0);

    operation.operation = VD_ATTACH_CONTROL_OPERATION_RECOVER;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_OK);
    bytes_to_hex(operation_bytes, size, operation_hex);
    assert(strcmp(operation_hex,
                  VD_ATTACH_CONTROL_GOLDEN_RECOVER_HEX) == 0);

    operation.operation = VD_ATTACH_CONTROL_OPERATION_ATTACH;
    operation.requested_lease_ms =
        VD_ATTACH_CONTROL_GOLDEN_LEASE_MIN_MS;
    operation.lease_id = 0u;
    operation.lease_expires_at_ms = 0u;
    operation.injected_module_uid = 0u;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_OK);
    bytes_to_hex(operation_bytes, size, operation_hex);
    assert(strcmp(operation_hex,
                  VD_ATTACH_CONTROL_GOLDEN_ATTACH_MIN_HEX) == 0);
    operation.requested_lease_ms =
        VD_ATTACH_CONTROL_GOLDEN_LEASE_MAX_MS;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_OK);
    bytes_to_hex(operation_bytes, size, operation_hex);
    assert(strcmp(operation_hex,
                  VD_ATTACH_CONTROL_GOLDEN_ATTACH_MAX_HEX) == 0);

    /* Positive SceUID boundaries are enforced by the canonical encoder. */
    operation.target.pid = VD_ATTACH_CONTROL_GOLDEN_SCEUID_MIN_VALID;
    operation.target.main_modid =
        VD_ATTACH_CONTROL_GOLDEN_SCEUID_MAX_VALID;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_OK);
    operation.target.pid = VD_ATTACH_CONTROL_GOLDEN_SCEUID_MAX_VALID;
    operation.target.main_modid =
        VD_ATTACH_CONTROL_GOLDEN_SCEUID_MIN_VALID;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_OK);
    operation.target.pid = VD_ATTACH_CONTROL_GOLDEN_SCEUID_INVALID_ZERO;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_ERROR_ARGUMENT);
    operation.target.pid = VD_ATTACH_CONTROL_GOLDEN_SCEUID_INVALID_HIGH;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_ERROR_ARGUMENT);
    operation.target.pid = 0x00010005u;
    operation.target.main_modid =
        VD_ATTACH_CONTROL_GOLDEN_SCEUID_INVALID_ZERO;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_ERROR_ARGUMENT);
    operation.target.main_modid =
        VD_ATTACH_CONTROL_GOLDEN_SCEUID_INVALID_HIGH;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_ERROR_ARGUMENT);
    operation.target.main_modid = 0x40001234u;
    operation.operation = VD_ATTACH_CONTROL_OPERATION_DETACH;
    operation.requested_lease_ms = 0u;
    operation.lease_id = UINT64_C(0x4142434445464748);
    operation.lease_expires_at_ms = UINT64_C(0x0000018bcfe57000);
    operation.injected_module_uid =
        VD_ATTACH_CONTROL_GOLDEN_SCEUID_MIN_VALID;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_OK);
    operation.injected_module_uid =
        VD_ATTACH_CONTROL_GOLDEN_SCEUID_MAX_VALID;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_OK);
    operation.injected_module_uid =
        VD_ATTACH_CONTROL_GOLDEN_SCEUID_INVALID_ZERO;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_ERROR_ARGUMENT);
    operation.injected_module_uid =
        VD_ATTACH_CONTROL_GOLDEN_SCEUID_INVALID_HIGH;
    assert(vd_attach_control_encode_operation_authorization(
               &operation, operation_bytes, sizeof(operation_bytes),
               &size) == VD_ATTACH_CONTROL_ERROR_ARGUMENT);
}

static void make_peer_proof(FakeControlContext *context,
                            const VdAttachControlChallenge *challenge,
                            VdAttachControlPeerProof *proof,
                            uint8_t nonce_value) {
    VdAttachControlPeerTranscript transcript;
    uint8_t signed_bytes[VD_ATTACH_CONTROL_PEER_SIGNED_BYTES];
    size_t signed_size = 0u;

    memset(proof, 0, sizeof(*proof));
    proof->host_key_id = UINT64_C(0x1122334455667788);
    proof->expires_at_ms = context->now_ms + 10000u;
    fill_bytes(proof->client_nonce, sizeof(proof->client_nonce), nonce_value);

    memset(&transcript, 0, sizeof(transcript));
    transcript.version = VD_ATTACH_CONTROL_VERSION;
    transcript.service_generation = challenge->service_generation;
    transcript.session_id = challenge->session_id;
    transcript.transport_binding = challenge->transport_binding;
    transcript.host_key_id = proof->host_key_id;
    transcript.server_time_ms = challenge->server_time_ms;
    transcript.challenge_expires_at_ms =
        challenge->challenge_expires_at_ms;
    transcript.expires_at_ms = proof->expires_at_ms;
    memcpy(transcript.server_nonce, challenge->server_nonce,
           sizeof(transcript.server_nonce));
    memcpy(transcript.client_nonce, proof->client_nonce,
           sizeof(transcript.client_nonce));
    assert(vd_attach_control_encode_peer_transcript(
               &transcript, signed_bytes, sizeof(signed_bytes),
               &signed_size) == VD_ATTACH_CONTROL_OK);
    fake_sign(signed_bytes, signed_size, proof->signature);
    memset(signed_bytes, 0, sizeof(signed_bytes));
    memset(&transcript, 0, sizeof(transcript));
}

static void open_authenticated(VdAttachControl *control,
                               FakeControlContext *context,
                               uint64_t peer_id,
                               uint8_t nonce_value,
                               VdAttachControlChallenge *challenge) {
    VdAttachControlPeerProof proof;
    assert(vd_attach_control_open_session(control, peer_id, challenge) ==
           VD_ATTACH_CONTROL_OK);
    make_peer_proof(context, challenge, &proof, nonce_value);
    assert(vd_attach_control_authenticate(control, challenge->session_id,
                                          peer_id, &proof) ==
           VD_ATTACH_CONTROL_OK);
    assert(context->last_peer.session_id == challenge->session_id);
    assert(challenge->transport_binding == peer_id);
    assert(challenge->server_time_ms == context->last_peer.server_time_ms);
    assert(challenge->challenge_expires_at_ms ==
           context->last_peer.challenge_expires_at_ms);
    assert(memcmp(context->last_peer.server_nonce, challenge->server_nonce,
                  sizeof(challenge->server_nonce)) == 0);
}

static void sign_operation_proof(
    FakeControlContext *context, VdAttachControlOperationProof *proof) {
    VdAttachControlAuthorization authorization;
    const VdAttachControl *control = context->control;
    VdAttachControlOperation operation;
    uint8_t signed_bytes[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    size_t signed_size = 0u;

    assert(control != NULL);
    if (context->last_peer.session_id == 0u ||
        proof->expires_at_ms > context->last_peer.expires_at_ms ||
        context->now_ms >= context->last_peer.expires_at_ms) {
        /* Used only to prove a call fails before operation verification. */
        fill_bytes(proof->signature, sizeof(proof->signature), 0x5au);
        return;
    }
    operation = proof->requested_lease_ms != 0u
                    ? VD_ATTACH_CONTROL_OPERATION_ATTACH
                    : control->lease.state ==
                              VD_ATTACH_CONTROL_LEASE_RECOVERY
                          ? VD_ATTACH_CONTROL_OPERATION_RECOVER
                          : VD_ATTACH_CONTROL_OPERATION_DETACH;
    memset(&authorization, 0, sizeof(authorization));
    authorization.version = VD_ATTACH_CONTROL_VERSION;
    authorization.operation = (uint32_t)operation;
    authorization.fixed_module_slot =
        VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT;
    authorization.service_generation = control->service_generation;
    authorization.session_id = context->last_peer.session_id;
    authorization.transport_binding =
        context->last_peer.transport_binding;
    authorization.host_key_id = context->last_peer.host_key_id;
    authorization.expires_at_ms = proof->expires_at_ms;
    authorization.session_expires_at_ms =
        context->last_peer.expires_at_ms;
    authorization.requested_lease_ms = proof->requested_lease_ms;
    if (operation == VD_ATTACH_CONTROL_OPERATION_ATTACH) {
        authorization.target = context->identity;
    } else {
        authorization.lease_id = control->lease.lease_id;
        authorization.lease_expires_at_ms = control->lease.expires_at_ms;
        authorization.injected_module_uid =
            control->lease.injected_module_uid;
        authorization.target = control->lease.target;
    }
    memcpy(authorization.server_nonce, context->last_peer.server_nonce,
           sizeof(authorization.server_nonce));
    memcpy(authorization.client_nonce, context->last_peer.client_nonce,
           sizeof(authorization.client_nonce));
    memcpy(authorization.request_nonce, proof->request_nonce,
           sizeof(authorization.request_nonce));
    assert(vd_attach_control_encode_operation_authorization(
               &authorization, signed_bytes, sizeof(signed_bytes),
               &signed_size) == VD_ATTACH_CONTROL_OK);
    fake_sign(signed_bytes, signed_size, proof->signature);
    memset(signed_bytes, 0, sizeof(signed_bytes));
    memset(&authorization, 0, sizeof(authorization));
}

static void make_operation_proof(FakeControlContext *context,
                                 VdAttachControlOperationProof *proof,
                                 uint32_t lease_ms,
                                 uint8_t nonce_value) {
    memset(proof, 0, sizeof(*proof));
    memcpy(proof->target_title_id, "UVDBDEMO1", 10u);
    proof->expected_target_generation =
        context->identity.target_generation;
    proof->requested_lease_ms = lease_ms;
    proof->expires_at_ms = context->now_ms + 5000u;
    fill_bytes(proof->request_nonce, sizeof(proof->request_nonce),
               nonce_value);
    sign_operation_proof(context, proof);
}

static void attach_success(VdAttachControl *control,
                           FakeControlContext *context,
                           const VdAttachControlChallenge *challenge,
                           uint64_t peer_id,
                           uint8_t nonce_value,
                           VdAttachControlLeaseSnapshot *snapshot) {
    VdAttachControlOperationProof proof;
    context->first_deadline = 0u;
    context->deadline_changed = 0;
    make_operation_proof(context, &proof, 1000u, nonce_value);
    assert(vd_attach_control_begin_attach(
               control, challenge->session_id, peer_id, &proof, snapshot) ==
           VD_ATTACH_CONTROL_OK);
    assert(snapshot->state == VD_ATTACH_CONTROL_LEASE_ACTIVE);
    assert(snapshot->module_loaded && snapshot->module_started);
    assert(snapshot->target.pid == context->identity.pid);
    assert(snapshot->target.target_generation ==
           context->identity.target_generation);
    assert(!context->deadline_changed);
}

static void test_init_requires_every_security_boundary(void) {
    FakeControlContext context;
    VdAttachControlConfig config;
    VdAttachControl control;

    setup_context(&context);
    memset(&control, 0, sizeof(control));
    make_config(&context, &config);
    config.verify_peer = NULL;
    assert(vd_attach_control_init(&control, &config) ==
           VD_ATTACH_CONTROL_ERROR_ARGUMENT);
    make_config(&context, &config);
    config.verify_operation = NULL;
    assert(vd_attach_control_init(&control, &config) ==
           VD_ATTACH_CONTROL_ERROR_ARGUMENT);
    make_config(&context, &config);
    config.resolve_target = NULL;
    assert(vd_attach_control_init(&control, &config) ==
           VD_ATTACH_CONTROL_ERROR_ARGUMENT);
    make_config(&context, &config);
    config.load_fixed = NULL;
    assert(vd_attach_control_init(&control, &config) ==
           VD_ATTACH_CONTROL_ERROR_ARGUMENT);
    make_config(&context, &config);
    config.probe_fixed = NULL;
    assert(vd_attach_control_init(&control, &config) ==
           VD_ATTACH_CONTROL_ERROR_ARGUMENT);
}

static void test_authentication_peer_binding_and_replay(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge first;
    VdAttachControlChallenge second;
    VdAttachControlPeerProof peer_proof;
    VdAttachControlOperationProof operation;
    VdAttachControlLeaseSnapshot snapshot;

    setup_context(&context);
    setup_control(&control, &context);
    assert(vd_attach_control_open_session(&control, 10u, &first) == 0);
    make_operation_proof(&context, &operation, 1000u, 0x21u);
    assert(vd_attach_control_begin_attach(
               &control, first.session_id, 10u, &operation, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_AUTH);
    assert(context.load_calls == 0u);

    make_peer_proof(&context, &first, &peer_proof, 0x11u);
    assert(vd_attach_control_authenticate(&control, first.session_id, 99u,
                                          &peer_proof) ==
           VD_ATTACH_CONTROL_ERROR_PEER);
    context.verify_peer_result = 0;
    assert(vd_attach_control_authenticate(&control, first.session_id, 10u,
                                          &peer_proof) ==
           VD_ATTACH_CONTROL_ERROR_AUTH);
    context.verify_peer_result = 1;
    assert(vd_attach_control_authenticate(&control, first.session_id, 10u,
                                          &peer_proof) == 0);

    assert(vd_attach_control_open_session(&control, 20u, &second) == 0);
    assert(vd_attach_control_authenticate(&control, second.session_id, 20u,
                                          &peer_proof) ==
           VD_ATTACH_CONTROL_ERROR_AUTH);
    assert(context.last_peer.session_id == second.session_id);
    assert(vd_attach_control_begin_attach(
               &control, first.session_id, 99u, &operation, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_PEER);
    assert(context.load_calls == 0u);
}

static void test_fixed_loader_and_authenticated_detach(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlLeaseSnapshot snapshot;
    VdAttachControlOperationProof proof;
    VdAttachControlConfig config;
    uint64_t lease_id;
    uint64_t service_generation;

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 101u, 0x12u, &challenge);
    attach_success(&control, &context, &challenge, 101u, 0x22u, &snapshot);
    lease_id = snapshot.lease_id;
    assert(lease_id == UINT64_C(0x4142434445464700));
    assert(snapshot.expires_at_ms <=
           context.last_load_started_ms + 1000u);
    assert(snapshot.expires_at_ms <=
           context.last_load.authorization.session_expires_at_ms);
    assert(context.privileged_attach_verifications == 1u);
    service_generation = control.service_generation;
    make_config(&context, &config);
    assert(vd_attach_control_init(&control, &config) ==
           VD_ATTACH_CONTROL_ERROR_STATE);
    assert(control.service_generation == service_generation);
    assert(control.lease.lease_id == lease_id);
    assert(strcmp(context.actions, "LS") == 0);
    assert(context.last_load.authorization.target.pid == 0x10005u);
    assert(context.last_load.authorization.target.main_modid ==
           0x40001234u);
    assert(context.last_load.authorization.target.main_fingerprint ==
           0xaabbccddu);
    assert(context.last_load.authorization.fixed_module_slot ==
           VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT);

    make_operation_proof(&context, &proof, 1000u, 0x23u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 101u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_BUSY);

    make_operation_proof(&context, &proof, 0u, 0x24u);
    context.first_deadline = 0u;
    context.deadline_changed = 0;
    assert(vd_attach_control_detach(&control, challenge.session_id, 101u,
                                    &proof) == VD_ATTACH_CONTROL_OK);
    assert(strcmp(context.actions, "LSTU") == 0);
    assert(context.last_authorization.operation ==
           VD_ATTACH_CONTROL_OPERATION_DETACH);
    assert(context.last_authorization.lease_id == lease_id);
    assert(context.last_authorization.injected_module_uid ==
           context.module_uid);
    assert(context.privileged_cleanup_verifications == 2u);
    assert(context.privileged_cleanup_claims == 1u);
    assert(context.journal_capability_calls[
               VD_ATTACH_CONTROL_JOURNAL_START] == 1u);
    assert(context.journal_capability_calls[
               VD_ATTACH_CONTROL_JOURNAL_HOST_CLEANUP] == 2u);
    assert(context.last_action.has_signed_authorization);
    assert(context.last_action.signed_authorization.operation ==
           VD_ATTACH_CONTROL_OPERATION_DETACH);
    assert(!context.deadline_changed);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);
}

static void test_privileged_lease_grant_enforces_signed_bounds(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlLeaseSnapshot snapshot;
    VdAttachControlOperationProof proof;

    /* An over-duration grant is retained only as a recovery obligation. */
    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 102u, 0x35u, &challenge);
    context.grant_expiry_override = context.now_ms + 1001u;
    make_operation_proof(&context, &proof, 1000u, 0x36u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 102u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.privileged_attach_verifications == 1u);
    assert(context.load_calls == 1u && context.start_calls == 0u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(snapshot.expires_at_ms == context.grant_expiry_override);
    assert(vd_attach_control_service(&control) == VD_ATTACH_CONTROL_OK);
    assert(context.journal_capability_calls[
               VD_ATTACH_CONTROL_JOURNAL_RECOVERY_RETRY] == 1u);

    /* The privileged allocator clamps a long duration to session expiry. */
    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 103u, 0x37u, &challenge);
    context.now_ms = context.last_peer.expires_at_ms - 1000u;
    make_operation_proof(&context, &proof, 5000u, 0x38u);
    proof.expires_at_ms = context.last_peer.expires_at_ms - 1u;
    sign_operation_proof(&context, &proof);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 103u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_OK);
    assert(snapshot.expires_at_ms == context.last_peer.expires_at_ms);
    assert(snapshot.expires_at_ms <
           context.last_load_started_ms + proof.requested_lease_ms);
    assert(vd_attach_control_close_session(
               &control, challenge.session_id, 103u) ==
           VD_ATTACH_CONTROL_OK);
    assert(context.journal_capability_calls[
               VD_ATTACH_CONTROL_JOURNAL_DISCONNECT] == 2u);

    /* A malformed backend lease identifier never reaches lifecycle calls. */
    setup_context(&context);
    context.next_lease_id = 0u;
    setup_control(&control, &context);
    open_authenticated(&control, &context, 104u, 0x39u, &challenge);
    make_operation_proof(&context, &proof, 1000u, 0x3au);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 104u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(snapshot.lease_id == 0u && snapshot.module_loaded);
    assert(vd_attach_control_service(&control) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.start_calls == 0u && context.stop_calls == 0u &&
           context.unload_calls == 0u && context.probe_calls == 0u);
}

static void test_operation_auth_and_generation_revalidation(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlOperationProof proof;
    VdAttachControlLeaseSnapshot snapshot;

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 1u, 0x13u, &challenge);

    make_operation_proof(&context, &proof, 1000u, 0x31u);
    ++proof.expected_target_generation;
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 1u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_STALE_TARGET);
    assert(context.operation_verify_calls == 0u && context.load_calls == 0u);

    make_operation_proof(&context, &proof, 1000u, 0x32u);
    context.verify_operation_result = 0;
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 1u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_AUTH);
    assert(context.load_calls == 0u);

    context.verify_operation_result = 1;
    context.change_identity_on_resolve = context.resolve_calls + 2u;
    make_operation_proof(&context, &proof, 1000u, 0x33u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 1u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_STALE_TARGET);
    assert(context.load_calls == 0u);
    context.change_identity_on_resolve = 0u;
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 1u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_REPLAY);
    assert(context.load_calls == 0u);
}

static void test_failed_start_rolls_back_in_reverse_order(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlOperationProof proof;
    VdAttachControlLeaseSnapshot snapshot;

    setup_context(&context);
    context.start_result = -1;
    setup_control(&control, &context);
    open_authenticated(&control, &context, 2u, 0x14u, &challenge);
    make_operation_proof(&context, &proof, 1000u, 0x41u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 2u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_BACKEND);
    assert(strcmp(context.actions, "LSTU") == 0);
    assert(context.stop_calls == 1u && context.unload_calls == 1u);
    assert(context.journal_capability_calls[
               VD_ATTACH_CONTROL_JOURNAL_START] == 1u);
    assert(context.journal_capability_calls[
               VD_ATTACH_CONTROL_JOURNAL_ROLLBACK] == 2u);
    assert(!context.last_action.has_signed_authorization);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);
}

static void test_partial_load_rolls_back_and_invalid_uid_poison_blocks(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlOperationProof proof;
    VdAttachControlLeaseSnapshot snapshot;

    setup_context(&context);
    context.load_result = -1;
    context.return_uid_on_load_failure = 1;
    setup_control(&control, &context);
    open_authenticated(&control, &context, 12u, 0x1cu, &challenge);
    make_operation_proof(&context, &proof, 1000u, 0x42u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 12u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_BACKEND);
    assert(strcmp(context.actions, "LU") == 0);
    assert(context.start_calls == 0u && context.stop_calls == 0u &&
           context.unload_calls == 1u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);

    setup_context(&context);
    context.module_uid = 0u;
    setup_control(&control, &context);
    open_authenticated(&control, &context, 13u, 0x1du, &challenge);
    make_operation_proof(&context, &proof, 1000u, 0x43u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 13u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(snapshot.module_loaded && snapshot.injected_module_uid == 0u);
    assert(vd_attach_control_service(&control) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.stop_calls == 0u && context.unload_calls == 0u);
    make_operation_proof(&context, &proof, 1000u, 0x44u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 13u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_BUSY);
}

static void test_failed_stop_is_recoverable_and_blocks_new_attach(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlLeaseSnapshot snapshot;
    VdAttachControlOperationProof proof;
    VdAttachControlConfig config;
    uint64_t service_generation;

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 3u, 0x15u, &challenge);
    attach_success(&control, &context, &challenge, 3u, 0x51u, &snapshot);

    context.stop_result = -1;
    make_operation_proof(&context, &proof, 0u, 0x52u);
    assert(vd_attach_control_detach(&control, challenge.session_id, 3u,
                                    &proof) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.stop_calls == 1u && context.unload_calls == 0u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(snapshot.module_started && snapshot.module_loaded);

    /* Re-init must not discard a durable recovery obligation. */
    service_generation = control.service_generation;
    make_config(&context, &config);
    assert(vd_attach_control_init(&control, &config) ==
           VD_ATTACH_CONTROL_ERROR_STATE);
    assert(control.service_generation == service_generation);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(snapshot.module_started && snapshot.module_loaded);

    make_operation_proof(&context, &proof, 1000u, 0x53u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 3u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_BUSY);

    context.stop_result = 0;
    make_operation_proof(&context, &proof, 0u, 0x54u);
    assert(vd_attach_control_recover(&control, challenge.session_id, 3u,
                                     &proof) == VD_ATTACH_CONTROL_OK);
    assert(context.stop_calls == 2u && context.unload_calls == 1u);
    assert(context.privileged_cleanup_verifications == 3u);
    assert(context.privileged_cleanup_claims == 2u);
    assert(context.last_action.journal_capability ==
           VD_ATTACH_CONTROL_JOURNAL_HOST_CLEANUP);
    assert(context.last_action.has_signed_authorization);
    assert(context.last_action.signed_authorization.operation ==
           VD_ATTACH_CONTROL_OPERATION_RECOVER);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);
}

static void test_expiry_preserves_unload_progress(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlLeaseSnapshot snapshot;

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 4u, 0x16u, &challenge);
    attach_success(&control, &context, &challenge, 4u, 0x61u, &snapshot);
    context.now_ms = snapshot.expires_at_ms;
    context.unload_result = -1;
    assert(vd_attach_control_service(&control) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.stop_calls == 1u && context.unload_calls == 1u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(snapshot.module_loaded && !snapshot.module_started);
    assert(context.journal_capability_calls[
               VD_ATTACH_CONTROL_JOURNAL_LEASE_EXPIRED] == 2u);
    assert(!context.last_action.has_signed_authorization);

    context.unload_result = 0;
    assert(vd_attach_control_service(&control) == VD_ATTACH_CONTROL_OK);
    assert(context.stop_calls == 1u && context.unload_calls == 2u);
    assert(context.journal_capability_calls[
               VD_ATTACH_CONTROL_JOURNAL_RECOVERY_RETRY] == 1u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);
}

static void test_cleanup_resamples_after_late_callbacks(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlLeaseSnapshot snapshot;

    /* A late successful stop is recorded, but unload cannot begin. */
    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 30u, 0x30u, &challenge);
    attach_success(&control, &context, &challenge, 30u, 0xa1u, &snapshot);
    context.now_ms = snapshot.expires_at_ms;
    context.advance_stop_ms = 1000u;
    assert(vd_attach_control_service(&control) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.stop_calls == 1u && context.unload_calls == 0u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(snapshot.module_loaded && !snapshot.module_started);
    context.advance_stop_ms = 0u;
    assert(vd_attach_control_service(&control) == VD_ATTACH_CONTROL_OK);
    assert(context.stop_calls == 1u && context.unload_calls == 1u);

    /* A late successful unload is durable; the next service only clears. */
    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 31u, 0x31u, &challenge);
    attach_success(&control, &context, &challenge, 31u, 0xa2u, &snapshot);
    context.now_ms = snapshot.expires_at_ms;
    context.advance_unload_ms = 1000u;
    assert(vd_attach_control_service(&control) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.stop_calls == 1u && context.unload_calls == 1u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(!snapshot.module_loaded && !snapshot.module_started);
    assert(vd_attach_control_service(&control) == VD_ATTACH_CONTROL_OK);
    assert(context.stop_calls == 1u && context.unload_calls == 1u);

    /* A late exact-GONE probe records terminal absence without mutation. */
    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 32u, 0x32u, &challenge);
    attach_success(&control, &context, &challenge, 32u, 0xa3u, &snapshot);
    context.now_ms = snapshot.expires_at_ms;
    context.identity = context.changed_identity;
    context.probe_presence = VD_ATTACH_CONTROL_MODULE_GONE;
    context.advance_probe_ms = 1000u;
    assert(vd_attach_control_service(&control) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.probe_calls == 1u);
    assert(context.stop_calls == 0u && context.unload_calls == 0u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(!snapshot.module_loaded && !snapshot.module_started);
    assert(vd_attach_control_service(&control) == VD_ATTACH_CONTROL_OK);
    assert(context.probe_calls == 1u);
    assert(context.stop_calls == 0u && context.unload_calls == 0u);
}

static void test_pid_reuse_never_reaches_cleanup_callbacks(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlLeaseSnapshot snapshot;
    VdAttachControlOperationProof proof;

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 5u, 0x17u, &challenge);
    attach_success(&control, &context, &challenge, 5u, 0x71u, &snapshot);
    context.now_ms = snapshot.expires_at_ms;
    context.identity = context.changed_identity;
    assert(vd_attach_control_service(&control) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.stop_calls == 0u && context.unload_calls == 0u);
    assert(context.probe_calls == 1u);

    make_operation_proof(&context, &proof, 0u, 0x72u);
    proof.expected_target_generation--;
    assert(vd_attach_control_recover(&control, challenge.session_id, 5u,
                                     &proof) ==
           VD_ATTACH_CONTROL_ERROR_STALE_TARGET);
    assert(context.stop_calls == 0u && context.unload_calls == 0u);

    /* A privileged read-only probe can terminally reconcile the old lease. */
    context.probe_presence = VD_ATTACH_CONTROL_MODULE_GONE;
    assert(vd_attach_control_service(&control) == VD_ATTACH_CONTROL_OK);
    assert(context.probe_calls == 2u);
    assert(context.stop_calls == 0u && context.unload_calls == 0u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);
}

static void test_callback_deadlines_and_expired_success_roll_back(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlPeerProof peer_proof;
    VdAttachControlOperationProof proof;
    VdAttachControlLeaseSnapshot snapshot;

    setup_context(&context);
    setup_control(&control, &context);
    assert(vd_attach_control_open_session(&control, 15u, &challenge) == 0);
    make_peer_proof(&context, &challenge, &peer_proof, 0x1fu);
    context.advance_peer_verify_ms = 1000u;
    assert(vd_attach_control_authenticate(&control, challenge.session_id, 15u,
                                          &peer_proof) ==
           VD_ATTACH_CONTROL_ERROR_EXPIRED);
    assert(context.last_deadline == challenge.challenge_expires_at_ms);

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 16u, 0x20u, &challenge);
    make_operation_proof(&context, &proof, 1000u, 0x74u);
    proof.expires_at_ms = context.now_ms + 50u;
    sign_operation_proof(&context, &proof);
    context.advance_operation_verify_ms = 50u;
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 16u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_EXPIRED);
    assert(context.last_deadline == proof.expires_at_ms);
    assert(context.load_calls == 0u);

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 20u, 0x24u, &challenge);
    make_operation_proof(&context, &proof, 1000u, 0x77u);
    context.advance_resolve_ms = 1000u;
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 20u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_EXPIRED);
    assert(context.operation_verify_calls == 0u && context.load_calls == 0u);

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 17u, 0x21u, &challenge);
    make_operation_proof(&context, &proof, 1000u, 0x75u);
    proof.expires_at_ms = context.now_ms + 50u;
    sign_operation_proof(&context, &proof);
    context.advance_load_ms = 50u;
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 17u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_EXPIRED);
    assert(strcmp(context.actions, "LU") == 0);
    assert(context.start_calls == 0u && context.unload_calls == 1u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 18u, 0x22u, &challenge);
    make_operation_proof(&context, &proof, 250u, 0x76u);
    context.advance_start_ms = 250u;
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 18u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_EXPIRED);
    assert(strcmp(context.actions, "LSTU") == 0);
    assert(context.stop_calls == 1u && context.unload_calls == 1u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);
}

static void test_replay_capacity_requires_authenticated_session_rollover(
    void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge first;
    VdAttachControlChallenge second;
    VdAttachControlLeaseSnapshot snapshot;
    VdAttachControlOperationProof proof;
    unsigned int cycle;

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 19u, 0x23u, &first);
    for (cycle = 0u; cycle < 32u; ++cycle) {
        make_operation_proof(&context, &proof, 1000u,
                             (uint8_t)(cycle * 2u + 1u));
        assert(vd_attach_control_begin_attach(
                   &control, first.session_id, 19u, &proof, &snapshot) ==
               VD_ATTACH_CONTROL_OK);
        make_operation_proof(&context, &proof, 0u,
                             (uint8_t)(cycle * 2u + 2u));
        assert(vd_attach_control_detach(&control, first.session_id, 19u,
                                        &proof) == VD_ATTACH_CONTROL_OK);
    }
    make_operation_proof(&context, &proof, 1000u, 0x7fu);
    assert(vd_attach_control_begin_attach(
               &control, first.session_id, 19u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_ROLLOVER_REQUIRED);
    assert(context.load_calls == 32u);
    assert(vd_attach_control_close_session(&control, first.session_id, 19u) ==
           VD_ATTACH_CONTROL_OK);

    /* Rollover is explicit: close, reconnect, and authenticate again. */
    open_authenticated(&control, &context, 19u, 0x23u, &second);
    assert(second.session_id != first.session_id);
    /* The byte-for-byte old-session proof remains invalid. */
    assert(vd_attach_control_begin_attach(
               &control, second.session_id, 19u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_AUTH);
    assert(context.load_calls == 32u);
    /* Only a transcript newly signed for the replacement session works. */
    sign_operation_proof(&context, &proof);
    assert(vd_attach_control_begin_attach(
               &control, second.session_id, 19u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_OK);
    assert(context.load_calls == 33u);
}

static void test_identity_change_after_load_prevents_start_and_unload(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlOperationProof proof;
    VdAttachControlLeaseSnapshot snapshot;

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 14u, 0x1eu, &challenge);
    /* Resolve before auth, after auth, then immediately before start. */
    context.change_identity_on_resolve = 3u;
    make_operation_proof(&context, &proof, 1000u, 0x73u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 14u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.load_calls == 1u && context.start_calls == 0u);
    assert(context.stop_calls == 0u && context.unload_calls == 0u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(snapshot.module_loaded && !snapshot.module_started);

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 21u, 0x25u, &challenge);
    /* The fourth resolve is the final identity check after start returns. */
    context.change_identity_on_resolve = 4u;
    make_operation_proof(&context, &proof, 1000u, 0x78u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 21u, &proof, &snapshot) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.load_calls == 1u && context.start_calls == 1u);
    assert(context.stop_calls == 0u && context.unload_calls == 0u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(snapshot.module_loaded && snapshot.module_started);
}

static void test_disconnect_cleanup_and_shutdown_recovery(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlChallenge extra;
    VdAttachControlLeaseSnapshot snapshot;

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 6u, 0x18u, &challenge);
    attach_success(&control, &context, &challenge, 6u, 0x81u, &snapshot);
    assert(vd_attach_control_close_session(&control, challenge.session_id,
                                           99u) ==
           VD_ATTACH_CONTROL_ERROR_PEER);
    assert(context.stop_calls == 0u);
    assert(vd_attach_control_close_session(&control, challenge.session_id,
                                           6u) == VD_ATTACH_CONTROL_OK);
    assert(context.stop_calls == 1u && context.unload_calls == 1u);
    assert(context.journal_capability_calls[
               VD_ATTACH_CONTROL_JOURNAL_DISCONNECT] == 2u);
    assert(!context.last_action.has_signed_authorization);

    open_authenticated(&control, &context, 7u, 0x19u, &challenge);
    attach_success(&control, &context, &challenge, 7u, 0x82u, &snapshot);
    context.stop_result = -1;
    assert(vd_attach_control_shutdown(&control) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);
    assert(vd_attach_control_open_session(&control, 8u, &extra) ==
           VD_ATTACH_CONTROL_ERROR_SHUTDOWN);
    context.stop_result = 0;
    assert(vd_attach_control_shutdown(&control) == VD_ATTACH_CONTROL_OK);
    assert(context.stop_calls == 3u && context.unload_calls == 2u);
    assert(context.journal_capability_calls[
               VD_ATTACH_CONTROL_JOURNAL_SHUTDOWN] == 3u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);
    assert(vd_attach_control_shutdown(&control) == VD_ATTACH_CONTROL_OK);
    assert(context.stop_calls == 3u && context.unload_calls == 2u);
}

static void test_disconnected_recovery_uses_new_same_host_session(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge owner;
    VdAttachControlChallenge replacement;
    VdAttachControlLeaseSnapshot snapshot;
    VdAttachControlOperationProof proof;

    setup_context(&context);
    setup_control(&control, &context);
    open_authenticated(&control, &context, 33u, 0x33u, &owner);
    attach_success(&control, &context, &owner, 33u, 0xb1u, &snapshot);
    context.stop_result = -1;
    assert(vd_attach_control_close_session(&control, owner.session_id, 33u) ==
           VD_ATTACH_CONTROL_RECOVERY_REQUIRED);
    assert(context.stop_calls == 1u && context.unload_calls == 0u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_RECOVERY);

    context.stop_result = 0;
    open_authenticated(&control, &context, 34u, 0x34u, &replacement);
    assert(replacement.session_id != owner.session_id);
    make_operation_proof(&context, &proof, 0u, 0xb2u);
    assert(vd_attach_control_recover(
               &control, replacement.session_id, 34u, &proof) ==
           VD_ATTACH_CONTROL_OK);
    assert(context.stop_calls == 2u && context.unload_calls == 1u);
    assert(vd_attach_control_snapshot(&control, &snapshot) == 0);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);
}

static void test_expired_challenge_and_session_fail_closed(void) {
    FakeControlContext context;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlPeerProof peer_proof;
    VdAttachControlOperationProof operation;
    VdAttachControlLeaseSnapshot snapshot;

    setup_context(&context);
    setup_control(&control, &context);
    assert(vd_attach_control_open_session(&control, 9u, &challenge) == 0);
    make_peer_proof(&context, &challenge, &peer_proof, 0x1au);
    context.now_ms += 1000u;
    assert(vd_attach_control_authenticate(&control, challenge.session_id, 9u,
                                          &peer_proof) ==
           VD_ATTACH_CONTROL_ERROR_EXPIRED);

    open_authenticated(&control, &context, 10u, 0x1bu, &challenge);
    context.now_ms += 10000u;
    make_operation_proof(&context, &operation, 1000u, 0x91u);
    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, 10u, &operation, &snapshot) ==
           VD_ATTACH_CONTROL_ERROR_EXPIRED);
    assert(context.load_calls == 0u);
}

int main(void) {
    test_canonical_wire_golden_vectors();
    test_init_requires_every_security_boundary();
    test_authentication_peer_binding_and_replay();
    test_fixed_loader_and_authenticated_detach();
    test_privileged_lease_grant_enforces_signed_bounds();
    test_operation_auth_and_generation_revalidation();
    test_failed_start_rolls_back_in_reverse_order();
    test_partial_load_rolls_back_and_invalid_uid_poison_blocks();
    test_failed_stop_is_recoverable_and_blocks_new_attach();
    test_expiry_preserves_unload_progress();
    test_cleanup_resamples_after_late_callbacks();
    test_pid_reuse_never_reaches_cleanup_callbacks();
    test_callback_deadlines_and_expired_success_roll_back();
    test_replay_capacity_requires_authenticated_session_rollover();
    test_identity_change_after_load_prevents_start_and_unload();
    test_disconnect_cleanup_and_shutdown_recovery();
    test_disconnected_recovery_uses_new_same_host_session();
    test_expired_challenge_and_session_fail_closed();
    puts("attach control host tests: PASS");
    return 0;
}

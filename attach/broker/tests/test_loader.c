#include "vitadebug_attach_control_wire.h"
#include "vitadebug_attach_loader.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_SERVICE_GENERATION UINT64_C(0x0102030405060708)
#define TEST_HOST_KEY UINT64_C(0x1020304050607080)

typedef struct LoaderFixture {
    uint64_t now_ms;
    VdAttachFixedLoader loader;
    uint8_t entropy_value;
    VdAttachTargetIdentity identity;
    VdAttachTargetIdentity changed_identity;
    unsigned int change_identity_on_resolve;
    unsigned int verify_authorization_calls;
    unsigned int resolve_calls;
    unsigned int verify_module_calls;
    unsigned int load_calls;
    unsigned int start_calls;
    unsigned int stop_calls;
    unsigned int unload_calls;
    unsigned int probe_calls;
    uint64_t load_deadline_ms;
    uint64_t start_deadline_ms;
    uint64_t stop_deadline_ms;
    uint64_t unload_deadline_ms;
    int verify_module_result;
    int load_result;
    int start_result;
    int stop_result;
    int unload_result;
    uint32_t load_uid;
    VdAttachControlModulePresence probe_result;
    char actions[32];
    size_t action_count;
} LoaderFixture;

static void fake_sign(const uint8_t *input, size_t input_size,
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
    const uint8_t *input, size_t input_size,
    const uint8_t signature[VD_ATTACH_CONTROL_SIGNATURE_BYTES]) {
    uint8_t expected[VD_ATTACH_CONTROL_SIGNATURE_BYTES];
    int result;
    fake_sign(input, input_size, expected);
    result = memcmp(expected, signature, sizeof(expected)) == 0;
    memset(expected, 0, sizeof(expected));
    return result;
}

static void sign_authorization(VdAttachControlAuthorization *authorization) {
    uint8_t signed_bytes[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    size_t signed_size = 0u;
    memset(authorization->signature, 0,
           sizeof(authorization->signature));
    assert(vd_attach_control_encode_operation_authorization(
               authorization, signed_bytes, sizeof(signed_bytes),
               &signed_size) == VD_ATTACH_CONTROL_OK);
    fake_sign(signed_bytes, signed_size, authorization->signature);
    memset(signed_bytes, 0, sizeof(signed_bytes));
}

static void record_action(LoaderFixture *fixture, char action) {
    assert(fixture->action_count + 1u < sizeof(fixture->actions));
    fixture->actions[fixture->action_count++] = action;
    fixture->actions[fixture->action_count] = '\0';
}

static uint64_t fake_now(void *opaque) {
    return ((LoaderFixture *)opaque)->now_ms;
}

static int fake_entropy(void *opaque, uint8_t *output, size_t size) {
    LoaderFixture *fixture = (LoaderFixture *)opaque;
    size_t i;
    ++fixture->entropy_value;
    if (fixture->entropy_value == 0u) {
        ++fixture->entropy_value;
    }
    for (i = 0u; i < size; ++i) {
        output[i] = (uint8_t)(fixture->entropy_value + (uint8_t)i);
    }
    return 0;
}

static int fake_verify_authorization(
    void *opaque, const VdAttachControlAuthorization *authorization,
    const uint8_t *signed_bytes, size_t signed_size,
    uint64_t deadline_ms) {
    LoaderFixture *fixture = (LoaderFixture *)opaque;
    (void)deadline_ms;
    ++fixture->verify_authorization_calls;
    return fake_signature_matches(signed_bytes, signed_size,
                                  authorization->signature);
}

static int fake_verify_peer(
    void *opaque, const VdAttachControlPeerTranscript *transcript,
    const uint8_t *signed_bytes, size_t signed_size,
    uint64_t deadline_ms) {
    (void)opaque;
    (void)deadline_ms;
    return fake_signature_matches(signed_bytes, signed_size,
                                  transcript->signature);
}

static VdAttachInventoryResult fake_resolve(
    void *opaque,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity, uint64_t deadline_ms) {
    LoaderFixture *fixture = (LoaderFixture *)opaque;
    (void)deadline_ms;
    ++fixture->resolve_calls;
    if (memcmp(title_id, fixture->identity.title_id,
               VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) != 0) {
        return VD_ATTACH_INVENTORY_DENIED;
    }
    *identity = fixture->change_identity_on_resolve ==
                        fixture->resolve_calls
                    ? fixture->changed_identity
                    : fixture->identity;
    return VD_ATTACH_INVENTORY_FOUND;
}

static void require_fixed_module(
    const VdAttachFixedModuleDescriptor *module) {
    uint8_t expected_digest[VD_ATTACH_LOADER_DIGEST_BYTES];
    memset(expected_digest, 0x5a, sizeof(expected_digest));
    assert(module->fixed_module_slot ==
           VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT);
    assert(strcmp(module->canonical_path,
                  "ux0:data/vitadebug/fixed-debugger.suprx") == 0);
    assert(memcmp(module->sha256, expected_digest,
                  sizeof(expected_digest)) == 0);
}

static int fake_verify_module(
    void *opaque, const VdAttachFixedModuleDescriptor *module,
    uint64_t deadline_ms) {
    LoaderFixture *fixture = (LoaderFixture *)opaque;
    (void)deadline_ms;
    ++fixture->verify_module_calls;
    require_fixed_module(module);
    return fixture->verify_module_result;
}

static int fake_load_module(
    void *opaque, const VdAttachFixedModuleDescriptor *module,
    const VdAttachTargetIdentity *target, uint32_t *module_uid,
    uint64_t deadline_ms) {
    LoaderFixture *fixture = (LoaderFixture *)opaque;
    (void)deadline_ms;
    ++fixture->load_calls;
    fixture->load_deadline_ms = deadline_ms;
    record_action(fixture, 'L');
    require_fixed_module(module);
    assert(target->target_generation ==
           fixture->identity.target_generation);
    if (fixture->load_result == 0 || fixture->load_uid != 0u) {
        *module_uid = fixture->load_uid;
    }
    return fixture->load_result;
}

static int fake_start_module(
    void *opaque, const VdAttachFixedModuleDescriptor *module,
    const VdAttachTargetIdentity *target, uint32_t module_uid,
    uint64_t deadline_ms) {
    LoaderFixture *fixture = (LoaderFixture *)opaque;
    (void)target;
    (void)deadline_ms;
    ++fixture->start_calls;
    fixture->start_deadline_ms = deadline_ms;
    record_action(fixture, 'S');
    require_fixed_module(module);
    assert(module_uid == fixture->load_uid);
    return fixture->start_result;
}

static int fake_stop_module(
    void *opaque, const VdAttachFixedModuleDescriptor *module,
    const VdAttachTargetIdentity *target, uint32_t module_uid,
    uint64_t deadline_ms) {
    LoaderFixture *fixture = (LoaderFixture *)opaque;
    (void)target;
    (void)deadline_ms;
    ++fixture->stop_calls;
    fixture->stop_deadline_ms = deadline_ms;
    record_action(fixture, 'T');
    require_fixed_module(module);
    assert(module_uid == fixture->load_uid);
    return fixture->stop_result;
}

static int fake_unload_module(
    void *opaque, const VdAttachFixedModuleDescriptor *module,
    const VdAttachTargetIdentity *target, uint32_t module_uid,
    uint64_t deadline_ms) {
    LoaderFixture *fixture = (LoaderFixture *)opaque;
    (void)target;
    (void)deadline_ms;
    ++fixture->unload_calls;
    fixture->unload_deadline_ms = deadline_ms;
    record_action(fixture, 'U');
    require_fixed_module(module);
    assert(module_uid == fixture->load_uid);
    return fixture->unload_result;
}

static VdAttachControlModulePresence fake_probe_module(
    void *opaque, const VdAttachFixedModuleDescriptor *module,
    const VdAttachTargetIdentity *target, uint32_t module_uid,
    uint64_t deadline_ms) {
    LoaderFixture *fixture = (LoaderFixture *)opaque;
    (void)target;
    (void)module_uid;
    (void)deadline_ms;
    ++fixture->probe_calls;
    record_action(fixture, 'P');
    require_fixed_module(module);
    return fixture->probe_result;
}

static void make_loader_config(LoaderFixture *fixture,
                               VdAttachFixedLoaderConfig *config) {
    static const char allowed_titles[][VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES] = {
        "UVDBDEMO1",
        "UVDBTEST2",
    };
    memset(config, 0, sizeof(*config));
    config->callback_context = fixture;
    config->now_ms = fake_now;
    config->entropy = fake_entropy;
    config->verify_authorization = fake_verify_authorization;
    config->resolve_target = fake_resolve;
    config->verify_module = fake_verify_module;
    config->load_module = fake_load_module;
    config->start_module = fake_start_module;
    config->stop_module = fake_stop_module;
    config->unload_module = fake_unload_module;
    config->probe_module = fake_probe_module;
    config->allowed_title_ids = allowed_titles;
    config->allowed_title_count = 2u;
    config->debugger_module.fixed_module_slot =
        VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT;
    memcpy(config->debugger_module.canonical_path,
           "ux0:data/vitadebug/fixed-debugger.suprx",
           sizeof("ux0:data/vitadebug/fixed-debugger.suprx"));
    memset(config->debugger_module.sha256, 0x5a,
           sizeof(config->debugger_module.sha256));
    config->min_lease_ms = 250u;
    config->max_lease_ms = 5000u;
}

static void setup_fixture(LoaderFixture *fixture) {
    VdAttachFixedLoaderConfig config;
    memset(fixture, 0, sizeof(*fixture));
    fixture->now_ms = 1000u;
    fixture->verify_module_result = 1;
    fixture->load_uid = 0x40002000u;
    fixture->probe_result = VD_ATTACH_CONTROL_MODULE_PRESENT;
    memcpy(fixture->identity.title_id, "UVDBDEMO1", 10u);
    fixture->identity.pid = 0x10005u;
    fixture->identity.main_modid = 0x40001234u;
    fixture->identity.main_fingerprint = 0xaabbccddu;
    fixture->identity.target_generation =
        UINT64_C(0x1112131415161718);
    fixture->changed_identity = fixture->identity;
    ++fixture->changed_identity.target_generation;
    make_loader_config(fixture, &config);
    assert(vd_attach_fixed_loader_init(&fixture->loader, &config) ==
           VD_ATTACH_LOADER_OK);
    assert(vd_attach_fixed_loader_bind_service_generation(
               &fixture->loader, TEST_SERVICE_GENERATION) ==
           VD_ATTACH_LOADER_OK);
}

static void make_attach_authorization(
    const LoaderFixture *fixture,
    VdAttachControlAuthorization *authorization,
    uint8_t nonce_value) {
    memset(authorization, 0, sizeof(*authorization));
    authorization->version = VD_ATTACH_CONTROL_VERSION;
    authorization->operation = VD_ATTACH_CONTROL_OPERATION_ATTACH;
    authorization->fixed_module_slot =
        VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT;
    authorization->service_generation = TEST_SERVICE_GENERATION;
    authorization->session_id = UINT64_C(0x2122232425262728);
    authorization->transport_binding =
        UINT64_C(0x3132333435363738);
    authorization->host_key_id = TEST_HOST_KEY;
    authorization->expires_at_ms = 5000u;
    authorization->session_expires_at_ms = 10000u;
    authorization->requested_lease_ms = 1000u;
    authorization->target = fixture->identity;
    memset(authorization->server_nonce, 0x41,
           sizeof(authorization->server_nonce));
    memset(authorization->client_nonce, 0x42,
           sizeof(authorization->client_nonce));
    memset(authorization->request_nonce, nonce_value,
           sizeof(authorization->request_nonce));
    sign_authorization(authorization);
}

static void make_cleanup_authorization(
    const LoaderFixture *fixture,
    const VdAttachControlLeaseGrant *grant,
    VdAttachControlAuthorization *authorization,
    uint8_t nonce_value) {
    make_attach_authorization(fixture, authorization, nonce_value);
    authorization->operation = VD_ATTACH_CONTROL_OPERATION_DETACH;
    authorization->requested_lease_ms = 0u;
    authorization->lease_id = grant->lease_id;
    authorization->lease_expires_at_ms = grant->lease_expires_at_ms;
    authorization->injected_module_uid = grant->injected_module_uid;
    sign_authorization(authorization);
}

static void make_action(
    const LoaderFixture *fixture,
    const VdAttachControlLeaseGrant *grant, uint32_t capability,
    const VdAttachControlAuthorization *authorization,
    VdAttachControlModuleAction *action) {
    memset(action, 0, sizeof(*action));
    action->version = VD_ATTACH_CONTROL_VERSION;
    action->fixed_module_slot = VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT;
    action->service_generation = TEST_SERVICE_GENERATION;
    action->lease_id = grant->lease_id;
    action->lease_expires_at_ms = grant->lease_expires_at_ms;
    action->owner_host_key_id = TEST_HOST_KEY;
    action->target = fixture->identity;
    action->injected_module_uid = grant->injected_module_uid;
    action->journal_capability = capability;
    if (authorization != NULL) {
        action->has_signed_authorization = 1;
        action->signed_authorization = *authorization;
    }
}

static void acquire_and_start(
    LoaderFixture *fixture, uint8_t nonce_value,
    VdAttachControlAuthorization *authorization,
    VdAttachControlLeaseGrant *grant) {
    VdAttachControlFixedLoadRequest request;
    VdAttachControlModuleAction action;
    make_attach_authorization(fixture, authorization, nonce_value);
    memset(&request, 0, sizeof(request));
    request.authorization = *authorization;
    assert(vd_attach_fixed_loader_load(
               &fixture->loader, &request, grant, 4000u) ==
           VD_ATTACH_LOADER_OK);
    make_action(fixture, grant, VD_ATTACH_CONTROL_JOURNAL_START,
                authorization, &action);
    assert(vd_attach_fixed_loader_start(
               &fixture->loader, &action, 4000u) ==
           VD_ATTACH_LOADER_OK);
}

static void test_fixed_allowlist_and_reverse_rollback(void) {
    LoaderFixture fixture;
    VdAttachControlAuthorization authorization;
    VdAttachControlLeaseGrant grant;
    VdAttachControlModuleAction action;
    VdAttachLoaderSnapshot snapshot;

    setup_fixture(&fixture);
    acquire_and_start(&fixture, 0x51u, &authorization, &grant);
    assert(grant.lease_id != 0u);
    assert(grant.lease_expires_at_ms == 2000u);
    assert(grant.injected_module_uid == fixture.load_uid);
    assert(strcmp(fixture.actions, "LS") == 0);
    assert(fixture.load_deadline_ms == grant.lease_expires_at_ms);
    assert(fixture.start_deadline_ms == grant.lease_expires_at_ms);

    make_action(&fixture, &grant, VD_ATTACH_CONTROL_JOURNAL_ROLLBACK,
                NULL, &action);
    {
        VdAttachControlModuleAction tampered = action;
        tampered.lease_id ^= 1u;
        assert(vd_attach_fixed_loader_stop(
                   &fixture.loader, &tampered, 4000u) ==
               VD_ATTACH_LOADER_ERROR_STATE);
        assert(fixture.stop_calls == 0u);
    }
    assert(vd_attach_fixed_loader_stop(&fixture.loader, &action, 4000u) ==
           VD_ATTACH_LOADER_OK);
    assert(vd_attach_fixed_loader_unload(&fixture.loader, &action, 4000u) ==
           VD_ATTACH_LOADER_OK);
    assert(strcmp(fixture.actions, "LSTU") == 0);
    assert(fixture.stop_deadline_ms == 4000u);
    assert(fixture.unload_deadline_ms == 4000u);
    assert(vd_attach_fixed_loader_snapshot(&fixture.loader, &snapshot) ==
           VD_ATTACH_LOADER_OK);
    assert(snapshot.state == VD_ATTACH_LOADER_JOURNAL_IDLE);
    assert(!snapshot.module_loaded && !snapshot.module_started);
    assert(snapshot.replay_nonce_count == 1u);
}

static void test_failed_start_is_stopped_then_unloaded(void) {
    LoaderFixture fixture;
    VdAttachControlAuthorization authorization;
    VdAttachControlFixedLoadRequest request;
    VdAttachControlLeaseGrant grant;
    VdAttachControlModuleAction action;
    VdAttachLoaderSnapshot snapshot;

    setup_fixture(&fixture);
    fixture.start_result = -1;
    make_attach_authorization(&fixture, &authorization, 0x61u);
    memset(&request, 0, sizeof(request));
    request.authorization = authorization;
    assert(vd_attach_fixed_loader_load(
               &fixture.loader, &request, &grant, 4000u) ==
           VD_ATTACH_LOADER_OK);
    make_action(&fixture, &grant, VD_ATTACH_CONTROL_JOURNAL_START,
                &authorization, &action);
    assert(vd_attach_fixed_loader_start(
               &fixture.loader, &action, 4000u) ==
           VD_ATTACH_LOADER_ERROR_PLATFORM);
    assert(vd_attach_fixed_loader_snapshot(&fixture.loader, &snapshot) ==
           VD_ATTACH_LOADER_OK);
    assert(snapshot.state == VD_ATTACH_LOADER_JOURNAL_RECOVERY);
    assert(snapshot.start_may_have_run);

    make_action(&fixture, &grant, VD_ATTACH_CONTROL_JOURNAL_ROLLBACK,
                NULL, &action);
    assert(vd_attach_fixed_loader_stop(&fixture.loader, &action, 4000u) ==
           VD_ATTACH_LOADER_OK);
    assert(vd_attach_fixed_loader_unload(&fixture.loader, &action, 4000u) ==
           VD_ATTACH_LOADER_OK);
    assert(strcmp(fixture.actions, "LSTU") == 0);
}

static void test_signed_cleanup_claimed_once_for_stop_unload(void) {
    LoaderFixture fixture;
    VdAttachControlAuthorization attach_authorization;
    VdAttachControlAuthorization cleanup_authorization;
    VdAttachControlLeaseGrant grant;
    VdAttachControlModuleAction action;
    VdAttachLoaderSnapshot snapshot;

    setup_fixture(&fixture);
    acquire_and_start(&fixture, 0x71u, &attach_authorization, &grant);
    make_cleanup_authorization(&fixture, &grant,
                               &cleanup_authorization, 0x72u);
    make_action(&fixture, &grant,
                VD_ATTACH_CONTROL_JOURNAL_HOST_CLEANUP,
                &cleanup_authorization, &action);
    assert(vd_attach_fixed_loader_stop(&fixture.loader, &action, 4000u) ==
           VD_ATTACH_LOADER_OK);
    assert(vd_attach_fixed_loader_unload(&fixture.loader, &action, 4000u) ==
           VD_ATTACH_LOADER_OK);
    assert(vd_attach_fixed_loader_snapshot(&fixture.loader, &snapshot) ==
           VD_ATTACH_LOADER_OK);
    assert(snapshot.state == VD_ATTACH_LOADER_JOURNAL_IDLE);
    assert(snapshot.replay_nonce_count == 2u);
    assert(fixture.verify_authorization_calls == 4u);
    assert(strcmp(fixture.actions, "LSTU") == 0);
}

static void test_unallowlisted_title_and_early_expiry_are_denied(void) {
    LoaderFixture fixture;
    VdAttachControlAuthorization authorization;
    VdAttachControlFixedLoadRequest request;
    VdAttachControlLeaseGrant grant;
    VdAttachControlModuleAction action;

    setup_fixture(&fixture);
    make_attach_authorization(&fixture, &authorization, 0x81u);
    memcpy(authorization.target.title_id, "NOTALLOW1", 10u);
    sign_authorization(&authorization);
    memset(&request, 0, sizeof(request));
    request.authorization = authorization;
    assert(vd_attach_fixed_loader_load(
               &fixture.loader, &request, &grant, 4000u) ==
           VD_ATTACH_LOADER_ERROR_AUTH);
    assert(fixture.verify_authorization_calls == 0u);
    assert(fixture.load_calls == 0u);

    acquire_and_start(&fixture, 0x82u, &authorization, &grant);
    make_action(&fixture, &grant,
                VD_ATTACH_CONTROL_JOURNAL_LEASE_EXPIRED,
                NULL, &action);
    assert(vd_attach_fixed_loader_stop(&fixture.loader, &action, 4000u) ==
           VD_ATTACH_LOADER_ERROR_AUTH);
    assert(fixture.stop_calls == 0u);
    fixture.now_ms = grant.lease_expires_at_ms;
    assert(vd_attach_fixed_loader_stop(
               &fixture.loader, &action, fixture.now_ms + 1000u) ==
           VD_ATTACH_LOADER_OK);
    assert(vd_attach_fixed_loader_unload(
               &fixture.loader, &action, fixture.now_ms + 1000u) ==
           VD_ATTACH_LOADER_OK);
}

static void test_privileged_operation_nonce_replay_is_rejected(void) {
    LoaderFixture fixture;
    VdAttachControlAuthorization authorization;
    VdAttachControlFixedLoadRequest request;
    VdAttachControlLeaseGrant grant;

    setup_fixture(&fixture);
    fixture.load_result = -1;
    fixture.load_uid = 0u;
    make_attach_authorization(&fixture, &authorization, 0x91u);
    memset(&request, 0, sizeof(request));
    request.authorization = authorization;
    assert(vd_attach_fixed_loader_load(
               &fixture.loader, &request, &grant, 4000u) ==
           VD_ATTACH_LOADER_ERROR_PLATFORM);
    assert(grant.lease_id == 0u && grant.injected_module_uid == 0u);
    assert(fixture.load_calls == 1u);
    assert(vd_attach_fixed_loader_load(
               &fixture.loader, &request, &grant, 4000u) ==
           VD_ATTACH_LOADER_ERROR_REPLAY);
    assert(fixture.load_calls == 1u);
}

static void test_target_change_before_platform_load_is_rejected(void) {
    LoaderFixture fixture;
    VdAttachControlAuthorization authorization;
    VdAttachControlFixedLoadRequest request;
    VdAttachControlLeaseGrant grant;
    VdAttachLoaderSnapshot snapshot;

    setup_fixture(&fixture);
    fixture.change_identity_on_resolve = 2u;
    make_attach_authorization(&fixture, &authorization, 0xa1u);
    memset(&request, 0, sizeof(request));
    request.authorization = authorization;
    assert(vd_attach_fixed_loader_load(
               &fixture.loader, &request, &grant, 4000u) ==
           VD_ATTACH_LOADER_ERROR_TARGET);
    assert(fixture.resolve_calls == 2u);
    assert(fixture.load_calls == 0u);
    assert(vd_attach_fixed_loader_snapshot(&fixture.loader, &snapshot) ==
           VD_ATTACH_LOADER_OK);
    assert(snapshot.state == VD_ATTACH_LOADER_JOURNAL_IDLE);
}

static void test_configuration_copies_fixed_catalog(void) {
    LoaderFixture fixture;
    VdAttachFixedLoaderConfig config;
    char mutable_titles[1][VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES] = {
        "UVDBDEMO1"
    };

    memset(&fixture, 0, sizeof(fixture));
    fixture.now_ms = 1000u;
    make_loader_config(&fixture, &config);
    config.allowed_title_ids = mutable_titles;
    config.allowed_title_count = 1u;
    assert(vd_attach_fixed_loader_init(&fixture.loader, &config) ==
           VD_ATTACH_LOADER_OK);
    memset(mutable_titles, 'X', sizeof(mutable_titles));
    memset(config.debugger_module.canonical_path, 'X',
           sizeof(config.debugger_module.canonical_path));
    assert(memcmp(fixture.loader.allowed_title_ids[0], "UVDBDEMO1",
                  10u) == 0);
    assert(strcmp(fixture.loader.config.debugger_module.canonical_path,
                  "ux0:data/vitadebug/fixed-debugger.suprx") == 0);

    memset(&fixture.loader, 0, sizeof(fixture.loader));
    make_loader_config(&fixture, &config);
    memcpy(config.debugger_module.canonical_path,
           "ux0:data/../escape.suprx",
           sizeof("ux0:data/../escape.suprx"));
    assert(vd_attach_fixed_loader_init(&fixture.loader, &config) ==
           VD_ATTACH_LOADER_ERROR_ARGUMENT);
}

static void test_control_uses_dedicated_loader_context(void) {
    LoaderFixture fixture;
    VdAttachFixedLoaderConfig loader_config;
    VdAttachControlConfig control_config;
    VdAttachControl control;
    VdAttachControlChallenge challenge;
    VdAttachControlPeerProof peer_proof;
    VdAttachControlPeerTranscript peer_transcript;
    VdAttachControlOperationProof operation_proof;
    VdAttachControlAuthorization authorization;
    VdAttachControlLeaseSnapshot snapshot;
    VdAttachLoaderSnapshot loader_snapshot;
    uint8_t signed_bytes[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    uint8_t peer_signed_bytes[VD_ATTACH_CONTROL_PEER_SIGNED_BYTES];
    size_t signed_size = 0u;
    uint64_t peer_id = UINT64_C(0x8877665544332211);

    memset(&fixture, 0, sizeof(fixture));
    fixture.now_ms = 1000u;
    fixture.verify_module_result = 1;
    fixture.load_uid = 0x40002000u;
    fixture.probe_result = VD_ATTACH_CONTROL_MODULE_PRESENT;
    memcpy(fixture.identity.title_id, "UVDBDEMO1", 10u);
    fixture.identity.pid = 0x10005u;
    fixture.identity.main_modid = 0x40001234u;
    fixture.identity.main_fingerprint = 0xaabbccddu;
    fixture.identity.target_generation =
        UINT64_C(0x1112131415161718);
    make_loader_config(&fixture, &loader_config);
    assert(vd_attach_fixed_loader_init(&fixture.loader, &loader_config) ==
           VD_ATTACH_LOADER_OK);

    memset(&control, 0, sizeof(control));
    memset(&control_config, 0, sizeof(control_config));
    control_config.callback_context = &fixture;
    control_config.loader_context = &fixture.loader;
    control_config.entropy = fake_entropy;
    control_config.now_ms = fake_now;
    control_config.verify_peer = fake_verify_peer;
    control_config.verify_operation = fake_verify_authorization;
    control_config.resolve_target = fake_resolve;
    control_config.load_fixed = vd_attach_fixed_loader_load;
    control_config.start_fixed = vd_attach_fixed_loader_start;
    control_config.stop_fixed = vd_attach_fixed_loader_stop;
    control_config.unload_fixed = vd_attach_fixed_loader_unload;
    control_config.probe_fixed = vd_attach_fixed_loader_probe;
    control_config.challenge_timeout_ms = 1000u;
    control_config.auth_window_ms = 10000u;
    control_config.min_lease_ms = 250u;
    control_config.max_lease_ms = 5000u;
    control_config.callback_timeout_ms = 1000u;
    assert(vd_attach_control_init(&control, &control_config) ==
           VD_ATTACH_CONTROL_OK);
    assert(vd_attach_fixed_loader_bind_service_generation(
               &fixture.loader, control.service_generation) ==
           VD_ATTACH_LOADER_OK);

    assert(vd_attach_control_open_session(&control, peer_id, &challenge) ==
           VD_ATTACH_CONTROL_OK);
    memset(&peer_proof, 0, sizeof(peer_proof));
    peer_proof.host_key_id = TEST_HOST_KEY;
    peer_proof.expires_at_ms = 8000u;
    memset(peer_proof.client_nonce, 0x42,
           sizeof(peer_proof.client_nonce));
    memset(&peer_transcript, 0, sizeof(peer_transcript));
    peer_transcript.version = VD_ATTACH_CONTROL_VERSION;
    peer_transcript.service_generation = challenge.service_generation;
    peer_transcript.session_id = challenge.session_id;
    peer_transcript.transport_binding = challenge.transport_binding;
    peer_transcript.host_key_id = peer_proof.host_key_id;
    peer_transcript.server_time_ms = challenge.server_time_ms;
    peer_transcript.challenge_expires_at_ms =
        challenge.challenge_expires_at_ms;
    peer_transcript.expires_at_ms = peer_proof.expires_at_ms;
    memcpy(peer_transcript.server_nonce, challenge.server_nonce,
           sizeof(peer_transcript.server_nonce));
    memcpy(peer_transcript.client_nonce, peer_proof.client_nonce,
           sizeof(peer_transcript.client_nonce));
    assert(vd_attach_control_encode_peer_transcript(
               &peer_transcript, peer_signed_bytes,
               sizeof(peer_signed_bytes), &signed_size) ==
           VD_ATTACH_CONTROL_OK);
    fake_sign(peer_signed_bytes, signed_size, peer_proof.signature);
    assert(vd_attach_control_authenticate(
               &control, challenge.session_id, peer_id, &peer_proof) ==
           VD_ATTACH_CONTROL_OK);

    memset(&operation_proof, 0, sizeof(operation_proof));
    memcpy(operation_proof.target_title_id, fixture.identity.title_id,
           VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES);
    operation_proof.expected_target_generation =
        fixture.identity.target_generation;
    operation_proof.requested_lease_ms = 1000u;
    operation_proof.expires_at_ms = 7000u;
    memset(operation_proof.request_nonce, 0x51,
           sizeof(operation_proof.request_nonce));
    memset(&authorization, 0, sizeof(authorization));
    authorization.version = VD_ATTACH_CONTROL_VERSION;
    authorization.operation = VD_ATTACH_CONTROL_OPERATION_ATTACH;
    authorization.fixed_module_slot =
        VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT;
    authorization.service_generation = control.service_generation;
    authorization.session_id = challenge.session_id;
    authorization.transport_binding = peer_id;
    authorization.host_key_id = TEST_HOST_KEY;
    authorization.expires_at_ms = operation_proof.expires_at_ms;
    authorization.session_expires_at_ms = peer_proof.expires_at_ms;
    authorization.requested_lease_ms =
        operation_proof.requested_lease_ms;
    authorization.target = fixture.identity;
    memcpy(authorization.server_nonce, challenge.server_nonce,
           sizeof(authorization.server_nonce));
    memcpy(authorization.client_nonce, peer_proof.client_nonce,
           sizeof(authorization.client_nonce));
    memcpy(authorization.request_nonce, operation_proof.request_nonce,
           sizeof(authorization.request_nonce));
    signed_size = 0u;
    assert(vd_attach_control_encode_operation_authorization(
               &authorization, signed_bytes, sizeof(signed_bytes),
               &signed_size) == VD_ATTACH_CONTROL_OK);
    fake_sign(signed_bytes, signed_size, operation_proof.signature);

    assert(vd_attach_control_begin_attach(
               &control, challenge.session_id, peer_id,
               &operation_proof, &snapshot) == VD_ATTACH_CONTROL_OK);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_ACTIVE);
    assert(strcmp(fixture.actions, "LS") == 0);
    assert(vd_attach_control_close_session(
               &control, challenge.session_id, peer_id) ==
           VD_ATTACH_CONTROL_OK);
    assert(strcmp(fixture.actions, "LSTU") == 0);
    /* The public snapshots agree that cleanup released the only lease. */
    assert(vd_attach_fixed_loader_snapshot(&fixture.loader,
                                           &loader_snapshot) ==
           VD_ATTACH_LOADER_OK);
    assert(loader_snapshot.state == VD_ATTACH_LOADER_JOURNAL_IDLE);
    assert(vd_attach_control_snapshot(&control, &snapshot) ==
           VD_ATTACH_CONTROL_OK);
    assert(snapshot.state == VD_ATTACH_CONTROL_LEASE_IDLE);
    memset(signed_bytes, 0, sizeof(signed_bytes));
    memset(peer_signed_bytes, 0, sizeof(peer_signed_bytes));
}

int main(void) {
    test_fixed_allowlist_and_reverse_rollback();
    test_failed_start_is_stopped_then_unloaded();
    test_signed_cleanup_claimed_once_for_stop_unload();
    test_unallowlisted_title_and_early_expiry_are_denied();
    test_privileged_operation_nonce_replay_is_rejected();
    test_target_change_before_platform_load_is_rejected();
    test_configuration_copies_fixed_catalog();
    test_control_uses_dedicated_loader_context();
    puts("attach fixed loader host tests: PASS");
    return 0;
}

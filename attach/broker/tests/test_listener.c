#include "vitadebug_attach_control.h"
#include "vitadebug_attach_control_wire.h"
#include "vitadebug_attach_listener.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct ListenerFixture {
    VdAttachControl control;
    VdAttachListener listener;
    uint64_t now_ms;
    uint8_t entropy_value;
    VdAttachTargetIdentity identity;
    unsigned int peer_verify_calls;
    unsigned int operation_verify_calls;
    unsigned int load_calls;
    unsigned int start_calls;
    unsigned int stop_calls;
    unsigned int unload_calls;
    int load_result;
} ListenerFixture;

typedef struct FakeTransport {
    uint8_t input[32];
    size_t input_size;
    size_t input_offset;
    uint8_t output[512];
    size_t output_size;
    size_t max_chunk;
    unsigned int close_calls;
} FakeTransport;

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

static int fake_entropy(void *opaque, uint8_t *output, size_t size) {
    ListenerFixture *fixture = (ListenerFixture *)opaque;
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

static uint64_t fake_now(void *opaque) {
    return ((ListenerFixture *)opaque)->now_ms;
}

static int fake_verify_peer(
    void *opaque, const VdAttachControlPeerTranscript *transcript,
    const uint8_t *signed_bytes, size_t signed_size,
    uint64_t deadline_ms) {
    ListenerFixture *fixture = (ListenerFixture *)opaque;
    (void)deadline_ms;
    ++fixture->peer_verify_calls;
    return fake_signature_matches(signed_bytes, signed_size,
                                  transcript->signature);
}

static int fake_verify_operation(
    void *opaque, const VdAttachControlAuthorization *authorization,
    const uint8_t *signed_bytes, size_t signed_size,
    uint64_t deadline_ms) {
    ListenerFixture *fixture = (ListenerFixture *)opaque;
    (void)deadline_ms;
    ++fixture->operation_verify_calls;
    return fake_signature_matches(signed_bytes, signed_size,
                                  authorization->signature);
}

static VdAttachInventoryResult fake_resolve(
    void *opaque,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity, uint64_t deadline_ms) {
    ListenerFixture *fixture = (ListenerFixture *)opaque;
    (void)deadline_ms;
    if (memcmp(title_id, fixture->identity.title_id,
               VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES) != 0) {
        return VD_ATTACH_INVENTORY_DENIED;
    }
    *identity = fixture->identity;
    return VD_ATTACH_INVENTORY_FOUND;
}

static int fake_load(void *opaque,
                     const VdAttachControlFixedLoadRequest *request,
                     VdAttachControlLeaseGrant *grant,
                     uint64_t deadline_ms) {
    ListenerFixture *fixture = (ListenerFixture *)opaque;
    (void)deadline_ms;
    ++fixture->load_calls;
    if (fixture->load_result != 0) {
        return fixture->load_result;
    }
    grant->lease_id = UINT64_C(0x1122334455667788);
    grant->lease_expires_at_ms =
        fixture->now_ms + request->authorization.requested_lease_ms;
    grant->injected_module_uid = 0x40001000u;
    return 0;
}

static int fake_start(void *opaque,
                      const VdAttachControlModuleAction *action,
                      uint64_t deadline_ms) {
    ListenerFixture *fixture = (ListenerFixture *)opaque;
    (void)action;
    (void)deadline_ms;
    ++fixture->start_calls;
    return 0;
}

static int fake_stop(void *opaque,
                     const VdAttachControlModuleAction *action,
                     uint64_t deadline_ms) {
    ListenerFixture *fixture = (ListenerFixture *)opaque;
    (void)action;
    (void)deadline_ms;
    ++fixture->stop_calls;
    return 0;
}

static int fake_unload(void *opaque,
                       const VdAttachControlModuleAction *action,
                       uint64_t deadline_ms) {
    ListenerFixture *fixture = (ListenerFixture *)opaque;
    (void)action;
    (void)deadline_ms;
    ++fixture->unload_calls;
    return 0;
}

static VdAttachControlModulePresence fake_probe(
    void *opaque, const VdAttachControlModuleAction *action,
    uint64_t deadline_ms) {
    (void)opaque;
    (void)action;
    (void)deadline_ms;
    return VD_ATTACH_CONTROL_MODULE_PRESENT;
}

static int fake_transport_read(void *opaque, void *output, size_t size,
                               uint64_t deadline_ms) {
    FakeTransport *transport = (FakeTransport *)opaque;
    size_t remaining = transport->input_size - transport->input_offset;
    size_t count = size < remaining ? size : remaining;
    (void)deadline_ms;
    if (transport->max_chunk != 0u && count > transport->max_chunk) {
        count = transport->max_chunk;
    }
    if (count == 0u) {
        return 0;
    }
    memcpy(output, transport->input + transport->input_offset, count);
    transport->input_offset += count;
    return (int)count;
}

static int fake_transport_write(void *opaque, const void *input,
                                size_t size, uint64_t deadline_ms) {
    FakeTransport *transport = (FakeTransport *)opaque;
    size_t count = size;
    (void)deadline_ms;
    if (transport->max_chunk != 0u && count > transport->max_chunk) {
        count = transport->max_chunk;
    }
    assert(transport->output_size + count <= sizeof(transport->output));
    memcpy(transport->output + transport->output_size, input, count);
    transport->output_size += count;
    return (int)count;
}

static void fake_transport_close(void *opaque) {
    ++((FakeTransport *)opaque)->close_calls;
}

static void setup_fixture(ListenerFixture *fixture) {
    VdAttachControlConfig control_config;
    VdAttachListenerConfig listener_config;
    memset(fixture, 0, sizeof(*fixture));
    fixture->now_ms = 1000u;
    memcpy(fixture->identity.title_id, "UVDBDEMO1", 10u);
    fixture->identity.pid = 0x10005u;
    fixture->identity.main_modid = 0x40001234u;
    fixture->identity.main_fingerprint = 0xaabbccddu;
    fixture->identity.target_generation =
        UINT64_C(0x0102030405060708);

    memset(&control_config, 0, sizeof(control_config));
    control_config.callback_context = fixture;
    control_config.entropy = fake_entropy;
    control_config.now_ms = fake_now;
    control_config.verify_peer = fake_verify_peer;
    control_config.verify_operation = fake_verify_operation;
    control_config.resolve_target = fake_resolve;
    control_config.load_fixed = fake_load;
    control_config.start_fixed = fake_start;
    control_config.stop_fixed = fake_stop;
    control_config.unload_fixed = fake_unload;
    control_config.probe_fixed = fake_probe;
    control_config.challenge_timeout_ms = 1000u;
    control_config.auth_window_ms = 10000u;
    control_config.min_lease_ms = 250u;
    control_config.max_lease_ms = 5000u;
    control_config.callback_timeout_ms = 1000u;
    assert(vd_attach_control_init(&fixture->control, &control_config) ==
           VD_ATTACH_CONTROL_OK);

    memset(&listener_config, 0, sizeof(listener_config));
    listener_config.control = &fixture->control;
    listener_config.clock_context = fixture;
    listener_config.now_ms = fake_now;
    listener_config.io_timeout_ms = 1000u;
    assert(vd_attach_listener_init(&fixture->listener, &listener_config) ==
           VD_ATTACH_LISTENER_OK);
}

static void make_auth_request(
    const VdAttachListenerChallengeFrame *challenge,
    VdAttachListenerRequest *request, uint8_t request_id_value,
    uint64_t host_key_id) {
    VdAttachControlPeerTranscript transcript;
    uint8_t signed_bytes[VD_ATTACH_CONTROL_PEER_SIGNED_BYTES];
    size_t signed_size = 0u;
    memset(request, 0, sizeof(*request));
    request->type = VD_ATTACH_LISTENER_REQUEST_AUTHENTICATE;
    request->session_id = challenge->session_id;
    request->peer_proof.host_key_id = host_key_id;
    request->peer_proof.expires_at_ms = 8000u;
    memset(request->peer_proof.client_nonce, 0x42,
           sizeof(request->peer_proof.client_nonce));
    memset(request->peer_proof.client_nonce, request_id_value,
           VD_ATTACH_LISTENER_REQUEST_ID_BYTES);
    memcpy(request->request_id, request->peer_proof.client_nonce,
           sizeof(request->request_id));

    memset(&transcript, 0, sizeof(transcript));
    transcript.version = VD_ATTACH_CONTROL_VERSION;
    transcript.service_generation = challenge->challenge.service_generation;
    transcript.session_id = challenge->session_id;
    transcript.transport_binding =
        challenge->challenge.transport_binding;
    transcript.host_key_id = request->peer_proof.host_key_id;
    transcript.server_time_ms = challenge->challenge.server_time_ms;
    transcript.challenge_expires_at_ms =
        challenge->challenge.challenge_expires_at_ms;
    transcript.expires_at_ms = request->peer_proof.expires_at_ms;
    memcpy(transcript.server_nonce, challenge->challenge.server_nonce,
           sizeof(transcript.server_nonce));
    memcpy(transcript.client_nonce, request->peer_proof.client_nonce,
           sizeof(transcript.client_nonce));
    assert(vd_attach_control_encode_peer_transcript(
               &transcript, signed_bytes, sizeof(signed_bytes),
               &signed_size) == VD_ATTACH_CONTROL_OK);
    fake_sign(signed_bytes, signed_size, request->peer_proof.signature);
    memset(&transcript, 0, sizeof(transcript));
    memset(signed_bytes, 0, sizeof(signed_bytes));
}

static void make_attach_request(
    const ListenerFixture *fixture,
    const VdAttachListenerChallengeFrame *challenge,
    const VdAttachListenerRequest *auth,
    VdAttachListenerRequest *request, uint8_t request_id_value,
    uint8_t operation_nonce_value) {
    VdAttachControlAuthorization authorization;
    uint8_t signed_bytes[VD_ATTACH_CONTROL_OPERATION_SIGNED_BYTES];
    size_t signed_size = 0u;
    memset(request, 0, sizeof(*request));
    request->type = VD_ATTACH_LISTENER_REQUEST_ATTACH;
    request->session_id = challenge->session_id;
    memcpy(request->operation_proof.target_title_id,
           fixture->identity.title_id,
           VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES);
    request->operation_proof.expected_target_generation =
        fixture->identity.target_generation;
    request->operation_proof.requested_lease_ms = 1000u;
    request->operation_proof.expires_at_ms = 7000u;
    memset(request->operation_proof.request_nonce,
           operation_nonce_value,
           sizeof(request->operation_proof.request_nonce));
    memset(request->operation_proof.request_nonce, request_id_value,
           VD_ATTACH_LISTENER_REQUEST_ID_BYTES);
    memcpy(request->request_id,
           request->operation_proof.request_nonce,
           sizeof(request->request_id));

    memset(&authorization, 0, sizeof(authorization));
    authorization.version = VD_ATTACH_CONTROL_VERSION;
    authorization.operation = VD_ATTACH_CONTROL_OPERATION_ATTACH;
    authorization.fixed_module_slot =
        VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT;
    authorization.service_generation =
        challenge->challenge.service_generation;
    authorization.session_id = challenge->session_id;
    authorization.transport_binding =
        challenge->challenge.transport_binding;
    authorization.host_key_id = auth->peer_proof.host_key_id;
    authorization.expires_at_ms =
        request->operation_proof.expires_at_ms;
    authorization.session_expires_at_ms =
        auth->peer_proof.expires_at_ms;
    authorization.requested_lease_ms =
        request->operation_proof.requested_lease_ms;
    authorization.target = fixture->identity;
    memcpy(authorization.server_nonce,
           challenge->challenge.server_nonce,
           sizeof(authorization.server_nonce));
    memcpy(authorization.client_nonce, auth->peer_proof.client_nonce,
           sizeof(authorization.client_nonce));
    memcpy(authorization.request_nonce,
           request->operation_proof.request_nonce,
           sizeof(authorization.request_nonce));
    assert(vd_attach_control_encode_operation_authorization(
               &authorization, signed_bytes, sizeof(signed_bytes),
               &signed_size) == VD_ATTACH_CONTROL_OK);
    fake_sign(signed_bytes, signed_size,
              request->operation_proof.signature);
    memset(&authorization, 0, sizeof(authorization));
    memset(signed_bytes, 0, sizeof(signed_bytes));
}

static void dispatch_ok(ListenerFixture *fixture,
                        const VdAttachListenerRequest *request,
                        uint64_t peer_id,
                        VdAttachListenerResponse *response) {
    uint8_t request_bytes[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    uint8_t response_bytes[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    size_t request_size = 0u;
    size_t response_size = 0u;
    assert(vd_attach_listener_encode_request(
               request, request_bytes, sizeof(request_bytes),
               &request_size) == VD_ATTACH_LISTENER_OK);
    assert(vd_attach_listener_handle_record(
               &fixture->listener, request->session_id,
               peer_id, request_bytes,
               request_size, response_bytes, sizeof(response_bytes),
               &response_size) == VD_ATTACH_LISTENER_OK);
    assert(vd_attach_listener_decode_response(
               response_bytes, response_size, response) ==
           VD_ATTACH_LISTENER_OK);
    assert(response->request_type == request->type);
    assert(memcmp(response->request_id, request->request_id,
                  sizeof(response->request_id)) == 0);
    memset(request_bytes, 0, sizeof(request_bytes));
    memset(response_bytes, 0, sizeof(response_bytes));
}

static void test_authenticated_framing_and_request_replay(void) {
    ListenerFixture fixture;
    VdAttachListenerChallengeFrame challenge;
    VdAttachListenerChallengeFrame decoded_challenge;
    VdAttachListenerChallengeFrame other_challenge;
    VdAttachListenerRequest auth;
    VdAttachListenerRequest attach;
    VdAttachListenerRequest other_auth;
    VdAttachListenerRequest other_attach;
    VdAttachListenerResponse response;
    uint8_t challenge_bytes[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    uint8_t request_bytes[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    uint8_t response_bytes[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    size_t challenge_size = 0u;
    size_t request_size = 0u;
    size_t response_size = 99u;
    unsigned int verify_calls;

    setup_fixture(&fixture);
    assert(vd_attach_listener_open_connection(
               &fixture.listener, UINT64_C(0x8877665544332211),
               &challenge) == VD_ATTACH_LISTENER_OK);
    assert(vd_attach_listener_encode_challenge(
               &challenge, challenge_bytes, sizeof(challenge_bytes),
               &challenge_size) == VD_ATTACH_LISTENER_OK);
    assert(challenge_size == VD_ATTACH_LISTENER_CHALLENGE_BYTES);
    assert(vd_attach_listener_decode_challenge(
               challenge_bytes, challenge_size, &decoded_challenge) ==
           VD_ATTACH_LISTENER_OK);
    assert(decoded_challenge.session_id == challenge.session_id);
    assert(decoded_challenge.challenge.transport_binding ==
           UINT64_C(0x8877665544332211));

    make_auth_request(&challenge, &auth, 0x11u,
                      UINT64_C(0x1020304050607080));
    dispatch_ok(&fixture, &auth, UINT64_C(0x8877665544332211),
                &response);
    assert(response.result_code == VD_ATTACH_LISTENER_RESULT_OK);
    assert(response.lease_state == VD_ATTACH_CONTROL_LEASE_IDLE);
    assert(fixture.peer_verify_calls == 1u);

    make_attach_request(&fixture, &challenge, &auth, &attach, 0x22u,
                        0x51u);
    dispatch_ok(&fixture, &attach, UINT64_C(0x8877665544332211),
                &response);
    assert(response.result_code == VD_ATTACH_LISTENER_RESULT_OK);
    assert(response.lease_state == VD_ATTACH_CONTROL_LEASE_ACTIVE);
    assert(response.lease_id == UINT64_C(0x1122334455667788));
    assert(response.injected_module_uid == 0x40001000u);
    assert(response.target_generation ==
           fixture.identity.target_generation);
    assert(fixture.load_calls == 1u && fixture.start_calls == 1u);

    /* A different authenticated host cannot read the owner's lease grant. */
    assert(vd_attach_listener_open_connection(
               &fixture.listener, UINT64_C(0x7766554433221100),
               &other_challenge) == VD_ATTACH_LISTENER_OK);
    make_auth_request(&other_challenge, &other_auth, 0x33u,
                      UINT64_C(0x1020304050607081));
    dispatch_ok(&fixture, &other_auth, UINT64_C(0x7766554433221100),
                &response);
    assert(response.result_code == VD_ATTACH_LISTENER_RESULT_OK);
    make_attach_request(&fixture, &other_challenge, &other_auth,
                        &other_attach, 0x34u, 0x54u);
    dispatch_ok(&fixture, &other_attach,
                UINT64_C(0x7766554433221100), &response);
    assert(response.result_code == VD_ATTACH_LISTENER_RESULT_BUSY);
    assert(response.lease_state == VD_ATTACH_CONTROL_LEASE_IDLE);
    assert(response.lease_id == 0u && response.injected_module_uid == 0u);
    assert(vd_attach_listener_close_connection(
               &fixture.listener, other_challenge.session_id,
               UINT64_C(0x7766554433221100)) ==
           VD_ATTACH_LISTENER_OK);

    /* Same wire request ID is rejected before authentication is re-entered. */
    assert(vd_attach_listener_encode_request(
               &auth, request_bytes, sizeof(request_bytes),
               &request_size) == VD_ATTACH_LISTENER_OK);
    verify_calls = fixture.peer_verify_calls;
    assert(vd_attach_listener_handle_record(
               &fixture.listener, challenge.session_id,
               UINT64_C(0x8877665544332211), request_bytes,
               request_size, response_bytes, sizeof(response_bytes),
               &response_size) == VD_ATTACH_LISTENER_ERROR_REPLAY);
    assert(response_size == 0u);
    assert(fixture.peer_verify_calls == verify_calls);

    assert(vd_attach_listener_close_connection(
               &fixture.listener, challenge.session_id,
               UINT64_C(0x8877665544332211)) ==
           VD_ATTACH_LISTENER_OK);
    assert(fixture.stop_calls == 1u && fixture.unload_calls == 1u);
}

static void test_signed_request_id_cannot_be_rewritten(void) {
    ListenerFixture fixture;
    VdAttachListenerChallengeFrame challenge;
    VdAttachListenerRequest auth;
    VdAttachListenerRequest attach;
    VdAttachListenerResponse response;
    uint8_t request_bytes[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    uint8_t response_bytes[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    size_t request_size = 0u;
    size_t response_size = 0u;

    setup_fixture(&fixture);
    fixture.load_result = -1;
    assert(vd_attach_listener_open_connection(
               &fixture.listener, UINT64_C(0x0101010102020202),
               &challenge) == VD_ATTACH_LISTENER_OK);
    make_auth_request(&challenge, &auth, 0x31u,
                      UINT64_C(0x1020304050607080));
    dispatch_ok(&fixture, &auth, UINT64_C(0x0101010102020202),
                &response);
    assert(response.result_code == VD_ATTACH_LISTENER_RESULT_OK);

    make_attach_request(&fixture, &challenge, &auth, &attach, 0x32u,
                        0x71u);
    dispatch_ok(&fixture, &attach, UINT64_C(0x0101010102020202),
                &response);
    assert(response.result_code == VD_ATTACH_LISTENER_RESULT_BACKEND);
    assert(response.lease_state == VD_ATTACH_CONTROL_LEASE_IDLE);
    assert(fixture.operation_verify_calls == 1u);

    /* The framing ID is derived from the signed nonce and cannot be rewritten. */
    assert(vd_attach_listener_encode_request(
               &attach, request_bytes, sizeof(request_bytes),
               &request_size) == VD_ATTACH_LISTENER_OK);
    request_bytes[16] ^= 1u;
    assert(vd_attach_listener_handle_record(
               &fixture.listener, challenge.session_id,
               UINT64_C(0x0101010102020202), request_bytes,
               request_size, response_bytes, sizeof(response_bytes),
               &response_size) == VD_ATTACH_LISTENER_ERROR_PROTOCOL);
    assert(response_size == 0u);
    assert(fixture.operation_verify_calls == 1u);
}

static void test_noncanonical_record_terminates_connection(void) {
    ListenerFixture fixture;
    VdAttachListenerChallengeFrame challenge;
    VdAttachListenerRequest auth;
    uint8_t request_bytes[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    uint8_t response_bytes[VD_ATTACH_LISTENER_MAX_FRAME_SIZE];
    size_t request_size = 0u;
    size_t response_size = 7u;

    setup_fixture(&fixture);
    assert(vd_attach_listener_open_connection(
               &fixture.listener, UINT64_C(0x9999999999999999),
               &challenge) == VD_ATTACH_LISTENER_OK);
    make_auth_request(&challenge, &auth, 0x61u,
                      UINT64_C(0x1020304050607080));
    assert(vd_attach_listener_encode_request(
               &auth, request_bytes, sizeof(request_bytes),
               &request_size) == VD_ATTACH_LISTENER_OK);
    request_bytes[11] ^= 1u; /* Corrupt the fixed protocol version. */
    assert(vd_attach_listener_handle_record(
               &fixture.listener, challenge.session_id,
               UINT64_C(0x9999999999999999), request_bytes,
               request_size, response_bytes, sizeof(response_bytes),
               &response_size) == VD_ATTACH_LISTENER_ERROR_PROTOCOL);
    assert(response_size == 0u);
    assert(fixture.peer_verify_calls == 0u);

    assert(fixture.listener.connections[0].request_id_count == 0u);
    assert(fixture.listener.connections[0].terminal);
    /* A direct caller cannot ignore the fatal parser result and continue. */
    request_bytes[11] ^= 1u;
    assert(vd_attach_listener_handle_record(
               &fixture.listener, challenge.session_id,
               UINT64_C(0x9999999999999999), request_bytes,
               request_size, response_bytes, sizeof(response_bytes),
               &response_size) == VD_ATTACH_LISTENER_ERROR_STATE);
    assert(fixture.peer_verify_calls == 0u);
}

static void test_serve_rejects_oversized_frame_without_draining(void) {
    ListenerFixture fixture;
    FakeTransport fake_transport;
    VdAttachTransport transport;
    VdAttachListenerChallengeFrame challenge;
    uint32_t advertised = VD_ATTACH_LISTENER_MAX_FRAME_SIZE + 1u;

    setup_fixture(&fixture);
    memset(&fake_transport, 0, sizeof(fake_transport));
    fake_transport.input[0] = (uint8_t)(advertised >> 24);
    fake_transport.input[1] = (uint8_t)(advertised >> 16);
    fake_transport.input[2] = (uint8_t)(advertised >> 8);
    fake_transport.input[3] = (uint8_t)advertised;
    fake_transport.input_size = 4u;
    fake_transport.max_chunk = 3u;
    memset(&transport, 0, sizeof(transport));
    transport.context = &fake_transport;
    transport.read = fake_transport_read;
    transport.write = fake_transport_write;
    transport.close = fake_transport_close;
    transport.peer_id = UINT64_C(0x123456789abcdef0);

    assert(vd_attach_listener_serve(&fixture.listener, &transport) ==
           VD_ATTACH_LISTENER_ERROR_FRAME);
    assert(fake_transport.input_offset == 4u);
    assert(fake_transport.close_calls == 1u);
    assert(fake_transport.output_size ==
           4u + VD_ATTACH_LISTENER_CHALLENGE_BYTES);
    assert(fake_transport.output[0] == 0u &&
           fake_transport.output[1] == 0u &&
           fake_transport.output[2] == 0u &&
           fake_transport.output[3] ==
               VD_ATTACH_LISTENER_CHALLENGE_BYTES);
    assert(vd_attach_listener_decode_challenge(
               fake_transport.output + 4u,
               VD_ATTACH_LISTENER_CHALLENGE_BYTES, &challenge) ==
           VD_ATTACH_LISTENER_OK);
    assert(challenge.challenge.transport_binding == transport.peer_id);
}

int main(void) {
    test_authenticated_framing_and_request_replay();
    test_signed_request_id_cannot_be_rewritten();
    test_noncanonical_record_terminates_connection();
    test_serve_rejects_oversized_frame_without_draining();
    puts("attach listener host tests: PASS");
    return 0;
}

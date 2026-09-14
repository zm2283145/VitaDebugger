#include "vitadebug_attach_broker.h"
#include "vitadebug_attach_vita.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct FakeContext {
    uint64_t now_ms;
    uint32_t entropy_counter;
    VdAttachInventoryResult inventory_result;
    VdAttachTargetIdentity identity;
    unsigned int inventory_calls;
    uint64_t last_inventory_deadline;
} FakeContext;

typedef struct FakeTransport {
    uint8_t input[8192];
    size_t input_size;
    size_t input_offset;
    uint8_t output[8192];
    size_t output_size;
    size_t max_chunk;
    uint64_t deadline;
    int deadline_changed;
    int close_count;
} FakeTransport;

static uint64_t fake_now(void *opaque) {
    FakeContext *context = (FakeContext *)opaque;
    return context->now_ms;
}

static int fake_entropy(void *opaque, uint8_t *output, size_t size) {
    FakeContext *context = (FakeContext *)opaque;
    size_t i;
    ++context->entropy_counter;
    for (i = 0u; i < size; ++i) {
        output[i] = (uint8_t)(((context->entropy_counter + (uint32_t)i) %
                              254u) + 1u);
    }
    return 0;
}

static VdAttachInventoryResult fake_discover(
    void *opaque,
    const char title_id[VD_ATTACH_BROKER_MAX_TITLE_ID_BYTES],
    VdAttachTargetIdentity *identity,
    uint64_t deadline_ms) {
    FakeContext *context = (FakeContext *)opaque;
    ++context->inventory_calls;
    context->last_inventory_deadline = deadline_ms;
    if (context->inventory_result == VD_ATTACH_INVENTORY_FOUND) {
        if (strcmp(title_id, context->identity.title_id) != 0) {
            return VD_ATTACH_INVENTORY_NOT_FOUND;
        }
        *identity = context->identity;
    }
    return context->inventory_result;
}

static void setup_context(FakeContext *context) {
    memset(context, 0, sizeof(*context));
    context->now_ms = 1000u;
    context->inventory_result = VD_ATTACH_INVENTORY_FOUND;
    memcpy(context->identity.title_id, "UVDBDEMO1", 10u);
    context->identity.pid = 0x10005u;
    context->identity.main_modid = 0x40001234u;
    context->identity.main_fingerprint = 0xaabbccddu;
    context->identity.target_generation = UINT64_C(0x0102030405060708);
}

static void setup_broker(VdAttachBroker *broker, FakeContext *context) {
    VdAttachBrokerConfig config;
    memset(&config, 0, sizeof(config));
    config.callback_context = context;
    config.discover_exact = fake_discover;
    config.entropy = fake_entropy;
    config.now_ms = fake_now;
    config.attach_caps = VD_ATTACH_READ_ONLY_CAPABILITIES;
    config.kernel_abi = VD_ATTACH_CURRENT_KERNEL_ABI;
    config.kernel_caps = 0x0000000fu;
    config.ticket_lease_ms = 5000u;
    config.io_timeout_ms = 3000u;
    config.service_ready = 1;
    assert(vd_attach_broker_init(broker, &config) == VD_ATTACH_BROKER_OK);
}

static void request_id(char output[33], uint64_t value) {
    assert(snprintf(output, 33u, "%032" PRIx64, value) == 32);
}

static size_t make_hello(char *output, size_t capacity, uint64_t id) {
    char request[33];
    int count;
    request_id(request, id);
    count = snprintf(
        output, capacity,
        VD_ATTACH_HELLO_HEADER "\n"
        "request_id=%s\n"
        "client_nonce="
        "1111111111111111111111111111111111111111111111111111111111111111\n"
        "mode=observe\n"
        "expected_kernel_abi=%08x\n"
        "required_caps=%08x\n",
        request, VD_ATTACH_CURRENT_KERNEL_ABI,
        VD_ATTACH_READ_ONLY_CAPABILITIES);
    assert(count > 0 && (size_t)count < capacity);
    return (size_t)count;
}

static size_t make_discover(char *output,
                            size_t capacity,
                            const VdAttachBroker *broker,
                            const VdAttachBrokerSession *session,
                            uint64_t id,
                            const char *title_id) {
    char request[33];
    int count;
    request_id(request, id);
    count = snprintf(
        output, capacity,
        VD_ATTACH_DISCOVER_HEADER "\n"
        "request_id=%s\n"
        "server_nonce=%s\n"
        "service_generation=%016" PRIx64 "\n"
        "target_title_id=%s\n"
        "mode=observe\n",
        request, session->server_nonce, broker->service_generation, title_id);
    assert(count > 0 && (size_t)count < capacity);
    return (size_t)count;
}

static size_t make_release(char *output,
                           size_t capacity,
                           const VdAttachBroker *broker,
                           const VdAttachBrokerSession *session,
                           uint64_t id,
                           const char *ticket) {
    char request[33];
    int count;
    request_id(request, id);
    count = snprintf(
        output, capacity,
        VD_ATTACH_RELEASE_HEADER "\n"
        "request_id=%s\n"
        "server_nonce=%s\n"
        "service_generation=%016" PRIx64 "\n"
        "target_ticket=%s\n",
        request, session->server_nonce, broker->service_generation, ticket);
    assert(count > 0 && (size_t)count < capacity);
    return (size_t)count;
}

static VdAttachBrokerSession *session_for(VdAttachBroker *broker,
                                          uint64_t session_id) {
    size_t i;
    for (i = 0u; i < VD_ATTACH_BROKER_MAX_SESSIONS; ++i) {
        if (broker->sessions[i].active &&
            broker->sessions[i].session_id == session_id) {
            return &broker->sessions[i];
        }
    }
    assert(!"session not found");
    return NULL;
}

static void assert_contains(const uint8_t *response,
                            size_t response_size,
                            const char *needle) {
    char copy[VD_ATTACH_MAX_FRAME_SIZE + 1u];
    assert(response_size <= VD_ATTACH_MAX_FRAME_SIZE);
    memcpy(copy, response, response_size);
    copy[response_size] = '\0';
    assert(strstr(copy, needle) != NULL);
}

static void extract_field(const uint8_t *response,
                          size_t response_size,
                          const char *name,
                          char *output,
                          size_t capacity) {
    char copy[VD_ATTACH_MAX_FRAME_SIZE + 1u];
    char pattern[64];
    char *start;
    char *end;
    size_t length;
    assert(response_size <= VD_ATTACH_MAX_FRAME_SIZE);
    memcpy(copy, response, response_size);
    copy[response_size] = '\0';
    assert(snprintf(pattern, sizeof(pattern), "\n%s=", name) > 0);
    start = strstr(copy, pattern);
    assert(start != NULL);
    start += strlen(pattern);
    end = strchr(start, '\n');
    assert(end != NULL);
    length = (size_t)(end - start);
    assert(length + 1u <= capacity);
    memcpy(output, start, length);
    output[length] = '\0';
}

static void do_hello(VdAttachBroker *broker,
                     uint64_t session_id,
                     uint64_t peer_id,
                     uint64_t request_number) {
    char request[512];
    uint8_t response[VD_ATTACH_MAX_FRAME_SIZE];
    size_t request_size = make_hello(request, sizeof(request), request_number);
    size_t response_size = 0u;
    assert(vd_attach_broker_handle_record(
               broker, session_id, peer_id, (const uint8_t *)request,
               request_size, response, sizeof(response), &response_size) ==
           VD_ATTACH_BROKER_OK);
    assert_contains(response, response_size,
                    "\nstate=ready\nattach_caps=0000000f\n");
    assert_contains(response, response_size,
                    "\ncontrol_policy=disabled\nmessage=%00\n");
}

static void test_discover_release_and_replay(void) {
    FakeContext context;
    VdAttachBroker broker;
    VdAttachBrokerSession *session;
    uint64_t session_id;
    const uint64_t peer_id = 0xabcdu;
    char request[512];
    uint8_t response[VD_ATTACH_MAX_FRAME_SIZE];
    size_t request_size;
    size_t response_size;
    char ticket[65];

    setup_context(&context);
    setup_broker(&broker, &context);
    assert(vd_attach_broker_open_session(&broker, peer_id, &session_id) == 0);
    session = session_for(&broker, session_id);
    do_hello(&broker, session_id, peer_id, 1u);

    request_size = make_discover(request, sizeof(request), &broker, session,
                                 2u, "UVDBDEMO1");
    assert(vd_attach_broker_handle_record(
               &broker, session_id, peer_id, (const uint8_t *)request,
               request_size, response, sizeof(response), &response_size) == 0);
    assert_contains(response, response_size, "\nstate=found\n");
    assert_contains(response, response_size,
                    "\npid=00010005\nmain_modid=40001234\n"
                    "main_fingerprint=aabbccdd\n");
    assert(context.last_inventory_deadline == 4000u);
    extract_field(response, response_size, "target_ticket", ticket,
                  sizeof(ticket));
    assert(strlen(ticket) == 64u);

    request_size = make_release(request, sizeof(request), &broker, session, 3u,
                                ticket);
    assert(vd_attach_broker_handle_record(
               &broker, session_id, peer_id, (const uint8_t *)request,
               request_size, response, sizeof(response), &response_size) == 0);
    assert_contains(response, response_size, "\nstate=released\n");
    assert(context.inventory_calls == 2u);
    assert(context.last_inventory_deadline == 4000u);
    assert(vd_attach_broker_handle_record(
               &broker, session_id, peer_id, (const uint8_t *)request,
               request_size, response, sizeof(response), &response_size) ==
           VD_ATTACH_BROKER_ERROR_REPLAY);
}

static void test_ticket_expiration_is_missing_and_allows_rediscovery(void) {
    FakeContext context;
    VdAttachBroker broker;
    VdAttachBrokerSession *session;
    uint64_t session_id;
    char request[512];
    uint8_t response[VD_ATTACH_MAX_FRAME_SIZE];
    size_t request_size;
    size_t response_size;
    char ticket[65];

    setup_context(&context);
    setup_broker(&broker, &context);
    assert(vd_attach_broker_open_session(&broker, 1u, &session_id) == 0);
    session = session_for(&broker, session_id);
    do_hello(&broker, session_id, 1u, 10u);
    request_size = make_discover(request, sizeof(request), &broker, session,
                                 11u, "UVDBDEMO1");
    assert(vd_attach_broker_handle_record(
               &broker, session_id, 1u, (const uint8_t *)request, request_size,
               response, sizeof(response), &response_size) == 0);
    extract_field(response, response_size, "target_ticket", ticket,
                  sizeof(ticket));
    context.now_ms += 5000u;
    request_size = make_release(request, sizeof(request), &broker, session,
                                12u, ticket);
    assert(vd_attach_broker_handle_record(
               &broker, session_id, 1u, (const uint8_t *)request, request_size,
               response, sizeof(response), &response_size) == 0);
    assert_contains(response, response_size, "\nstate=missing\n");
    assert(context.inventory_calls == 1u);
    request_size = make_discover(request, sizeof(request), &broker, session,
                                 13u, "UVDBDEMO1");
    assert(vd_attach_broker_handle_record(
               &broker, session_id, 1u, (const uint8_t *)request, request_size,
               response, sizeof(response), &response_size) == 0);
    assert_contains(response, response_size, "\nstate=found\n");
}

static void test_wrong_peer_and_session_cannot_consume_ticket(void) {
    FakeContext context;
    VdAttachBroker broker;
    VdAttachBrokerSession *session_one;
    VdAttachBrokerSession *session_two;
    uint64_t session_id_one;
    uint64_t session_id_two;
    char request[512];
    uint8_t response[VD_ATTACH_MAX_FRAME_SIZE];
    size_t request_size;
    size_t response_size;
    char ticket[65];

    setup_context(&context);
    setup_broker(&broker, &context);
    assert(vd_attach_broker_open_session(&broker, 101u, &session_id_one) == 0);
    assert(vd_attach_broker_open_session(&broker, 202u, &session_id_two) == 0);
    session_one = session_for(&broker, session_id_one);
    session_two = session_for(&broker, session_id_two);
    do_hello(&broker, session_id_one, 101u, 20u);
    do_hello(&broker, session_id_two, 202u, 20u);

    request_size = make_discover(request, sizeof(request), &broker, session_one,
                                 21u, "UVDBDEMO1");
    assert(vd_attach_broker_handle_record(
               &broker, session_id_one, 999u, (const uint8_t *)request,
               request_size, response, sizeof(response), &response_size) ==
           VD_ATTACH_BROKER_ERROR_PEER);
    assert(vd_attach_broker_handle_record(
               &broker, session_id_one, 101u, (const uint8_t *)request,
               request_size, response, sizeof(response), &response_size) == 0);
    extract_field(response, response_size, "target_ticket", ticket,
                  sizeof(ticket));

    request_size = make_release(request, sizeof(request), &broker, session_two,
                                21u, ticket);
    assert(vd_attach_broker_handle_record(
               &broker, session_id_two, 202u, (const uint8_t *)request,
               request_size, response, sizeof(response), &response_size) == 0);
    assert_contains(response, response_size, "\nstate=changed\n");
    assert(session_one->ticket.active);

    request_size = make_release(request, sizeof(request), &broker, session_one,
                                22u, ticket);
    assert(vd_attach_broker_handle_record(
               &broker, session_id_one, 101u, (const uint8_t *)request,
               request_size, response, sizeof(response), &response_size) == 0);
    assert_contains(response, response_size, "\nstate=released\n");
}

static void test_changed_release_is_consumed_once(void) {
    FakeContext context;
    VdAttachBroker broker;
    VdAttachBrokerSession *session;
    uint64_t session_id;
    char request[512];
    uint8_t response[VD_ATTACH_MAX_FRAME_SIZE];
    size_t request_size;
    size_t response_size;
    char ticket[65];

    setup_context(&context);
    setup_broker(&broker, &context);
    assert(vd_attach_broker_open_session(&broker, 1u, &session_id) == 0);
    session = session_for(&broker, session_id);
    do_hello(&broker, session_id, 1u, 30u);
    request_size = make_discover(request, sizeof(request), &broker, session,
                                 31u, "UVDBDEMO1");
    assert(vd_attach_broker_handle_record(
               &broker, session_id, 1u, (const uint8_t *)request, request_size,
               response, sizeof(response), &response_size) == 0);
    extract_field(response, response_size, "target_ticket", ticket,
                  sizeof(ticket));
    context.identity.main_fingerprint ^= 1u;
    request_size = make_release(request, sizeof(request), &broker, session,
                                32u, ticket);
    assert(vd_attach_broker_handle_record(
               &broker, session_id, 1u, (const uint8_t *)request, request_size,
               response, sizeof(response), &response_size) == 0);
    assert_contains(response, response_size, "\nstate=changed\n");
    request_size = make_release(request, sizeof(request), &broker, session,
                                33u, ticket);
    assert(vd_attach_broker_handle_record(
               &broker, session_id, 1u, (const uint8_t *)request, request_size,
               response, sizeof(response), &response_size) == 0);
    assert_contains(response, response_size, "\nstate=missing\n");
}

static int fake_transport_read(void *opaque,
                               void *output,
                               size_t size,
                               uint64_t deadline) {
    FakeTransport *transport = (FakeTransport *)opaque;
    size_t remaining = transport->input_size - transport->input_offset;
    size_t count = size;
    if (transport->deadline == 0u) {
        transport->deadline = deadline;
    } else if (transport->deadline != deadline && remaining != 0u) {
        transport->deadline_changed = 1;
    }
    if (remaining == 0u) {
        return 0;
    }
    if (count > remaining) {
        count = remaining;
    }
    if (transport->max_chunk != 0u && count > transport->max_chunk) {
        count = transport->max_chunk;
    }
    memcpy(output, transport->input + transport->input_offset, count);
    transport->input_offset += count;
    return (int)count;
}

static int fake_transport_write(void *opaque,
                                const void *data,
                                size_t size,
                                uint64_t deadline) {
    FakeTransport *transport = (FakeTransport *)opaque;
    size_t count = size;
    if (transport->deadline != deadline) {
        transport->deadline_changed = 1;
    }
    if (transport->max_chunk != 0u && count > transport->max_chunk) {
        count = transport->max_chunk;
    }
    assert(transport->output_size + count <= sizeof(transport->output));
    memcpy(transport->output + transport->output_size, data, count);
    transport->output_size += count;
    return (int)count;
}

static void fake_transport_close(void *opaque) {
    FakeTransport *transport = (FakeTransport *)opaque;
    ++transport->close_count;
}

static void test_transport_bounds_and_absolute_deadline(void) {
    FakeContext context;
    VdAttachBroker broker;
    VdAttachTransport interface;
    FakeTransport transport;
    char hello[512];
    size_t hello_size;

    setup_context(&context);
    setup_broker(&broker, &context);
    memset(&transport, 0, sizeof(transport));
    memset(&interface, 0, sizeof(interface));
    interface.context = &transport;
    interface.read = fake_transport_read;
    interface.write = fake_transport_write;
    interface.close = fake_transport_close;
    interface.peer_id = 55u;

    transport.input[0] = 0u;
    transport.input[1] = 0u;
    transport.input[2] = 0x10u;
    transport.input[3] = 0x01u;
    transport.input[4] = 0xaau;
    transport.input_size = 5u;
    assert(vd_attach_broker_serve(&broker, &interface) ==
           VD_ATTACH_BROKER_ERROR_FRAME);
    assert(transport.input_offset == 4u);
    assert(transport.output_size == 0u);
    assert(transport.close_count == 1);

    memset(&transport, 0, sizeof(transport));
    hello_size = make_hello(hello, sizeof(hello), 40u);
    transport.input[0] = (uint8_t)(hello_size >> 24);
    transport.input[1] = (uint8_t)(hello_size >> 16);
    transport.input[2] = (uint8_t)(hello_size >> 8);
    transport.input[3] = (uint8_t)hello_size;
    memcpy(transport.input + 4u, hello, hello_size);
    transport.input_size = hello_size + 4u;
    transport.max_chunk = 2u;
    assert(vd_attach_broker_serve(&broker, &interface) ==
           VD_ATTACH_BROKER_CLOSED);
    assert(transport.output_size > 4u);
    assert(transport.output[0] == 0u && transport.output[1] == 0u);
    assert(!transport.deadline_changed);
    assert(transport.close_count == 1);
}

static void test_fail_closed_vita_adapter_and_shutdown(void) {
    FakeContext context;
    VdAttachBroker broker;
    VdAttachBrokerConfig config;
    VdAttachBrokerSession *session;
    uint64_t session_id;
    char request[512];
    uint8_t response[VD_ATTACH_MAX_FRAME_SIZE];
    size_t request_size;
    size_t response_size;

    setup_context(&context);
    memset(&config, 0, sizeof(config));
    config.callback_context = &context;
    config.discover_exact = vd_attach_vita_inventory_unavailable;
    config.entropy = fake_entropy;
    config.now_ms = fake_now;
    config.attach_caps = VD_ATTACH_READ_ONLY_CAPABILITIES;
    config.kernel_abi = VD_ATTACH_CURRENT_KERNEL_ABI;
    config.ticket_lease_ms = 5000u;
    config.io_timeout_ms = 3000u;
    config.service_ready = 1;
    assert(vd_attach_broker_init(&broker, &config) == 0);
    assert(vd_attach_broker_open_session(&broker, 1u, &session_id) == 0);
    session = session_for(&broker, session_id);
    do_hello(&broker, session_id, 1u, 50u);
    request_size = make_discover(request, sizeof(request), &broker, session,
                                 51u, "UVDBDEMO1");
    assert(vd_attach_broker_handle_record(
               &broker, session_id, 1u, (const uint8_t *)request, request_size,
               response, sizeof(response), &response_size) == 0);
    assert_contains(response, response_size,
                    "\nstate=error\ntarget_title_id=UVDBDEMO1\n"
                    "pid=00000000\nmain_modid=00000000\n"
                    "main_fingerprint=00000000\n"
                    "target_generation=0000000000000000\n"
                    "target_ticket=00000000000000000000000000000000"
                    "00000000000000000000000000000000\n");

    vd_attach_broker_shutdown(&broker);
    assert(vd_attach_broker_handle_record(
               &broker, session_id, 1u, (const uint8_t *)request, request_size,
               response, sizeof(response), &response_size) ==
           VD_ATTACH_BROKER_ERROR_SHUTDOWN);
    assert(vd_attach_broker_open_session(&broker, 1u, &session_id) ==
           VD_ATTACH_BROKER_ERROR_SHUTDOWN);
}

int main(void) {
    test_discover_release_and_replay();
    test_ticket_expiration_is_missing_and_allows_rediscovery();
    test_wrong_peer_and_session_cannot_consume_ticket();
    test_changed_release_is_consumed_once();
    test_transport_bounds_and_absolute_deadline();
    test_fail_closed_vita_adapter_and_shutdown();
    puts("attach broker host tests: PASS");
    return 0;
}

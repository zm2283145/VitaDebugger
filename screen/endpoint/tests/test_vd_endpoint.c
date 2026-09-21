#include "vd_endpoint.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                       \
    do {                                                                \
        if (!(condition)) {                                             \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

struct io_fixture {
    uint8_t input[VD_COMPANION_MAX_RECORD];
    size_t input_size;
    size_t input_offset;
    size_t chunk;
    uint8_t output[64];
    size_t output_size;
    uint64_t now;
    int receive_result;
    int send_result;
    unsigned int yields;
    unsigned int progress_calls;
    unsigned int neutralizations;
    uint32_t send_advance_ms;
    uint32_t persistent_again;
    uint32_t input_active;
    uint64_t input_lease_ms;
    uint64_t cancel_at_ms;
};

static void store_u16(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void store_u32(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void store_u64(uint8_t* output, uint64_t value)
{
    store_u32(output, (uint32_t)(value >> 32));
    store_u32(output + 4u, (uint32_t)value);
}

static void valid_config(uint8_t data[VD_ENDPOINT_CONFIG_SIZE])
{
    size_t index;
    const uint64_t capabilities =
        VD_COMPANION_CAP_STATUS | VD_COMPANION_CAP_APP_INPUT |
        VD_COMPANION_CAP_DEBUG_FILES | VD_COMPANION_CAP_SCREEN |
        VD_COMPANION_CAP_INPUT_RECORD |
        VD_COMPANION_CAP_INPUT_PLAYBACK;

    memset(data, 0, VD_ENDPOINT_CONFIG_SIZE);
    memcpy(data, "VDCG", 4u);
    store_u16(data + 4u, VD_ENDPOINT_CONFIG_VERSION);
    store_u16(data + 6u, VD_ENDPOINT_CONFIG_SIZE);
    store_u32(data + 8u, VD_COMPANION_EXPLICIT_CONSENT);
    store_u32(data + 16u, VD_COMPANION_MUTATION_CONSENT);
    store_u32(data + 20u, VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT);
    store_u32(data + 24u, VD_INPUT_TRACE_EXPLICIT_PLAYBACK_CONSENT);
    store_u32(data + 28u, VD_COMPANION_NETWORK_LOOPBACK);
    store_u64(data + 32u, capabilities);
    store_u64(data + 40u, capabilities);
    data[48] = 127u;
    data[51] = 1u;
    data[120] = 127u;
    data[123] = 1u;
    store_u16(data + 52u, VD_COMPANION_DEFAULT_CONTROL_PORT);
    store_u16(data + 54u, VD_COMPANION_DEFAULT_SCREEN_PORT);
    for (index = 0u; index < 32u; ++index) {
        data[56u + index] = (uint8_t)(index + 1u);
        data[88u + index] = (uint8_t)(index + 33u);
    }
}

static int receive_fake(void* user, uint8_t* output, size_t capacity,
                        size_t* received)
{
    struct io_fixture* fixture = (struct io_fixture*)user;
    size_t amount;

    if (fixture->receive_result != VD_ENDPOINT_IO_OK)
        return fixture->receive_result;
    if (fixture->input_offset == fixture->input_size)
        return VD_ENDPOINT_IO_AGAIN;
    amount = fixture->input_size - fixture->input_offset;
    if (amount > fixture->chunk)
        amount = fixture->chunk;
    if (amount > capacity)
        amount = capacity;
    memcpy(output, fixture->input + fixture->input_offset, amount);
    fixture->input_offset += amount;
    *received = amount;
    return VD_ENDPOINT_IO_OK;
}

static int send_fake(void* user, const uint8_t* input, size_t size,
                     size_t* sent)
{
    struct io_fixture* fixture = (struct io_fixture*)user;
    size_t amount;

    if (fixture->send_result != VD_ENDPOINT_IO_OK)
        return fixture->send_result;
    amount = size > fixture->chunk ? fixture->chunk : size;
    if (fixture->output_size + amount > sizeof(fixture->output))
        return VD_ENDPOINT_IO_ERROR;
    memcpy(fixture->output + fixture->output_size, input, amount);
    fixture->output_size += amount;
    *sent = amount;
    fixture->now += fixture->send_advance_ms;
    return VD_ENDPOINT_IO_OK;
}

static uint64_t now_fake(void* user)
{
    return ((struct io_fixture*)user)->now;
}

static void yield_fake(void* user)
{
    struct io_fixture* fixture = (struct io_fixture*)user;
    ++fixture->yields;
    ++fixture->now;
    if (fixture->persistent_again == 0u)
        fixture->send_result = VD_ENDPOINT_IO_OK;
}

static int progress_fake(void* user)
{
    struct io_fixture* fixture = (struct io_fixture*)user;

    ++fixture->progress_calls;
    if (fixture->input_active != 0u &&
        fixture->now >= fixture->input_lease_ms) {
        fixture->input_active = 0u;
        ++fixture->neutralizations;
    }
    if (fixture->cancel_at_ms != 0u &&
        fixture->now >= fixture->cancel_at_ms)
        return VD_ENDPOINT_IO_ERROR;
    return VD_ENDPOINT_IO_OK;
}

static int service_bind(void* user, uint32_t scope, uint16_t port)
{
    (void)user;
    return scope == VD_COMPANION_NETWORK_LOOPBACK &&
                   port == VD_COMPANION_DEFAULT_CONTROL_PORT
               ? 0
               : -1;
}

static int service_send(void* user, const uint8_t* data, size_t size)
{
    (void)user;
    (void)data;
    (void)size;
    return 0;
}

static int service_close(void* user)
{
    (void)user;
    return 0;
}

static uint64_t service_now(void* user)
{
    (void)user;
    return 100u;
}

static void test_config(void)
{
    uint8_t data[VD_ENDPOINT_CONFIG_SIZE];
    struct vd_endpoint_config config;
    size_t index;

    valid_config(data);
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) == 0,
          "strict loopback config accepted");
    CHECK(config.control_port == 18198u &&
              config.screen_port == 18197u,
          "fixed endpoint ports parsed");
    store_u64(data + 32u, VD_COMPANION_CAP_STATUS);
    store_u64(data + 40u, VD_COMPANION_CAP_STATUS);
    store_u32(data + 16u, 0u);
    store_u32(data + 20u, 0u);
    store_u32(data + 24u, 0u);
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) == 0 &&
              config.enabled_capabilities == VD_COMPANION_CAP_STATUS,
          "status-only config keeps all mutation consents off");
    store_u32(data + 20u,
              VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT);
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) != 0,
          "unused recording consent is rejected");
    store_u32(data + 20u, 0u);
    store_u32(data + 16u, 1u);
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) != 0,
          "arbitrary disabled mutation consent is rejected");
    store_u32(data + 16u, 0u);
    store_u32(data + 20u, 1u);
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) != 0,
          "arbitrary disabled recording consent is rejected");
    store_u32(data + 20u, 0u);
    store_u32(data + 24u, 1u);
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) != 0,
          "arbitrary disabled playback consent is rejected");
    valid_config(data);

    for (index = 0u; index < sizeof(data); ++index) {
        uint8_t malformed[VD_ENDPOINT_CONFIG_SIZE];
        memcpy(malformed, data, sizeof(malformed));
        if (index < 8u || (index >= 124u && index < 128u)) {
            malformed[index] ^= 1u;
            memset(&config, 0xa5, sizeof(config));
            CHECK(vd_endpoint_config_parse(
                      &config, malformed, sizeof(malformed)) != 0,
                  "header and reserved corruption rejected");
            CHECK(config.enabled_capabilities == 0u,
                  "invalid config remains default off");
        }
    }
    CHECK(vd_endpoint_config_parse(
              &config, data, sizeof(data) - 1u) != 0,
          "truncated config rejected");

    memcpy(data + 88u, data + 56u, 32u);
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) != 0,
          "reused transport secrets rejected");
    valid_config(data);
    store_u32(data + 28u, VD_COMPANION_NETWORK_PRIVATE_LAN);
    data[48] = 192u;
    data[49] = 168u;
    data[50] = 1u;
    data[51] = 20u;
    data[120] = 192u;
    data[121] = 168u;
    data[122] = 1u;
    data[123] = 30u;
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) != 0,
          "LAN config requires explicit consent");
    store_u32(data + 12u, VD_COMPANION_LAN_CONSENT);
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) == 0,
          "explicit RFC1918 LAN config accepted");
    memcpy(data + 120u, data + 48u, 4u);
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) != 0,
          "remote host cannot be reused as local bind address");
    data[123] = 30u;
    data[48] = 8u;
    data[49] = 8u;
    data[50] = 8u;
    data[51] = 8u;
    CHECK(vd_endpoint_config_parse(&config, data, sizeof(data)) != 0,
          "public endpoint rejected");
}

static void test_receive(void)
{
    struct io_fixture fixture;
    struct vd_endpoint_io io;
    struct vd_endpoint_receiver receiver;
    const uint8_t* record;
    size_t size;
    int result = 0;

    memset(&fixture, 0, sizeof(fixture));
    memset(&io, 0, sizeof(io));
    memcpy(fixture.input, "VDCP", 4u);
    store_u16(fixture.input + 4u, 1u);
    store_u16(fixture.input + 6u, VD_COMPANION_HEADER_SIZE);
    store_u32(fixture.input + 12u, 3u);
    fixture.input[80] = 1u;
    fixture.input[81] = 2u;
    fixture.input[82] = 3u;
    fixture.input_size = 83u;
    fixture.chunk = 7u;
    fixture.now = 100u;
    io.user = &fixture;
    io.receive = receive_fake;
    io.now_ms = now_fake;
    vd_endpoint_receiver_init(&receiver);
    while (result == VD_ENDPOINT_POLL_IDLE)
        result = vd_endpoint_receiver_poll(
            &receiver, &io, &record, &size);
    CHECK(result == VD_ENDPOINT_POLL_RECORD && size == 83u &&
              record[82] == 3u,
          "partial nonblocking record assembled");

    vd_endpoint_receiver_init(&receiver);
    fixture.input_offset = fixture.input_size;
    fixture.receive_result = VD_ENDPOINT_IO_AGAIN;
    fixture.now = 150u;
    CHECK(vd_endpoint_receiver_poll(
              &receiver, &io, &record, &size) ==
              VD_ENDPOINT_POLL_IDLE,
          "unarmed authenticated receiver may remain idle");
    fixture.now += VD_ENDPOINT_RECEIVE_DEADLINE_MS;
    CHECK(vd_endpoint_receiver_poll(
              &receiver, &io, &record, &size) ==
              VD_ENDPOINT_POLL_IDLE,
          "authenticated idle does not consume a framing deadline");

    vd_endpoint_receiver_init(&receiver);
    fixture.now = 150u;
    CHECK(vd_endpoint_receiver_arm(
              &receiver, fixture.now,
              VD_ENDPOINT_RECEIVE_DEADLINE_MS) == 0 &&
              vd_endpoint_receiver_poll(
                  &receiver, &io, &record, &size) ==
                  VD_ENDPOINT_POLL_IDLE,
          "accepted client arms authentication deadline");
    fixture.now += VD_ENDPOINT_RECEIVE_DEADLINE_MS;
    CHECK(vd_endpoint_receiver_poll(
              &receiver, &io, &record, &size) ==
              VD_ENDPOINT_ERROR_DEADLINE,
          "silent accepted client cannot monopolize service");

    vd_endpoint_receiver_init(&receiver);
    fixture.input_offset = 0u;
    fixture.chunk = 1u;
    fixture.receive_result = VD_ENDPOINT_IO_OK;
    fixture.now = 200u;
    CHECK(vd_endpoint_receiver_poll(
              &receiver, &io, &record, &size) ==
              VD_ENDPOINT_POLL_IDLE,
          "partial header starts deadline");
    fixture.receive_result = VD_ENDPOINT_IO_AGAIN;
    fixture.now += VD_ENDPOINT_RECEIVE_DEADLINE_MS;
    CHECK(vd_endpoint_receiver_poll(
              &receiver, &io, &record, &size) ==
              VD_ENDPOINT_ERROR_DEADLINE,
          "partial record deadline enforced");

    vd_endpoint_receiver_init(&receiver);
    memset(&fixture, 0, sizeof(fixture));
    fixture.chunk = VD_COMPANION_HEADER_SIZE;
    fixture.input_size = VD_COMPANION_HEADER_SIZE;
    store_u32(fixture.input + 12u,
              VD_COMPANION_MAX_PAYLOAD + 1u);
    CHECK(vd_endpoint_receiver_poll(
              &receiver, &io, &record, &size) ==
              VD_ENDPOINT_ERROR_LIMIT,
          "oversized payload rejected before allocation");

    vd_endpoint_receiver_init(&receiver);
    fixture.receive_result = VD_ENDPOINT_IO_EOF;
    CHECK(vd_endpoint_receiver_poll(
              &receiver, &io, &record, &size) ==
              VD_ENDPOINT_ERROR_IO,
          "EOF fails closed");
}

static void test_send(void)
{
    static const uint8_t data[] = {1u, 2u, 3u, 4u, 5u};
    struct io_fixture fixture;
    struct vd_endpoint_io io;

    memset(&fixture, 0, sizeof(fixture));
    memset(&io, 0, sizeof(io));
    fixture.chunk = 2u;
    io.user = &fixture;
    io.send = send_fake;
    io.now_ms = now_fake;
    io.yield = yield_fake;
    CHECK(vd_endpoint_send_all(&io, data, sizeof(data), 5u) == 0 &&
              fixture.output_size == sizeof(data) &&
              memcmp(fixture.output, data, sizeof(data)) == 0,
          "short sends complete exactly");

    memset(&fixture, 0, sizeof(fixture));
    fixture.chunk = 2u;
    fixture.send_result = VD_ENDPOINT_IO_AGAIN;
    io.user = &fixture;
    CHECK(vd_endpoint_send_all(&io, data, sizeof(data), 5u) == 0 &&
              fixture.yields == 1u,
          "would-block send retries");

    memset(&fixture, 0, sizeof(fixture));
    fixture.chunk = 2u;
    fixture.send_result = VD_ENDPOINT_IO_AGAIN;
    fixture.persistent_again = 1u;
    fixture.input_active = 1u;
    fixture.input_lease_ms = 3u;
    io.user = &fixture;
    io.progress = progress_fake;
    CHECK(vd_endpoint_send_all(&io, data, sizeof(data), 5u) ==
              VD_ENDPOINT_ERROR_DEADLINE &&
              fixture.neutralizations == 1u &&
              fixture.input_active == 0u &&
              fixture.now == 5u,
          "backpressure services lease watchdog before deadline");

    memset(&fixture, 0, sizeof(fixture));
    fixture.chunk = 1u;
    fixture.send_advance_ms = 1u;
    fixture.input_active = 1u;
    fixture.input_lease_ms = 2u;
    io.user = &fixture;
    CHECK(vd_endpoint_send_all(&io, data, sizeof(data), 10u) == 0 &&
              fixture.neutralizations == 1u &&
              fixture.progress_calls == sizeof(data),
          "short successful writes service lease watchdog");

    memset(&fixture, 0, sizeof(fixture));
    fixture.chunk = 1u;
    fixture.send_advance_ms = 1u;
    fixture.cancel_at_ms = 2u;
    io.user = &fixture;
    CHECK(vd_endpoint_send_all(&io, data, sizeof(data), 10u) ==
              VD_ENDPOINT_ERROR_IO &&
              fixture.output_size == 2u,
          "progress cancellation bounds shutdown during short writes");

    memset(&fixture, 0, sizeof(fixture));
    fixture.chunk = 2u;
    fixture.send_result = VD_ENDPOINT_IO_AGAIN;
    fixture.now = UINT64_MAX - 2u;
    io.user = &fixture;
    CHECK(vd_endpoint_send_all(&io, data, sizeof(data), 5u) ==
              VD_ENDPOINT_ERROR_DEADLINE,
          "deadline overflow fails closed");
}

static void test_virtual_filesystem(void)
{
    uint8_t trace_buffer[
        VD_INPUT_TRACE_HEADER_SIZE + 2u * VD_INPUT_TRACE_EVENT_SIZE];
    uint8_t output[VD_ENDPOINT_STATUS_SIZE];
    struct vd_companion_file_entry entries[2];
    struct vd_endpoint_virtual_fs filesystem;
    struct vd_companion_config config;
    struct vd_companion_service service;
    struct vd_companion_input input;
    char path[VD_COMPANION_MAX_PATH + 1u];
    const uint8_t* trace;
    size_t output_size;
    size_t entry_count;
    size_t trace_size;
    size_t record_size;
    uint32_t flags;
    uint64_t file_size;
    uint8_t record[VD_COMPANION_MAX_RECORD];
    size_t index;

    memset(&service, 0, sizeof(service));
    memset(&input, 0, sizeof(input));
    vd_companion_config_init(&config);
    config.explicit_consent = VD_COMPANION_EXPLICIT_CONSENT;
    config.record_consent =
        VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT;
    config.enabled_capabilities =
        VD_COMPANION_CAP_STATUS |
        VD_COMPANION_CAP_INPUT_RECORD;
    for (index = 0u; index < sizeof(config.secret); ++index)
        config.secret[index] = (uint8_t)(index + 1u);
    memcpy(config.title_id, "VDSCRN001", sizeof("VDSCRN001"));
    config.process_id = 1u;
    config.process_generation = 2u;
    config.session_id = 3u;
    config.bind = service_bind;
    config.send = service_send;
    config.close = service_close;
    config.now_ms = service_now;
    config.trace_buffer = trace_buffer;
    config.trace_buffer_capacity = sizeof(trace_buffer);
    config.trace_max_events = 2u;
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK,
          "service fixture initializes");
    CHECK(vd_companion_encode_record(
              config.secret, VD_COMPANION_MESSAGE_HELLO, 0u, 1u, 500u,
              config.session_id, config.process_generation,
              config.enabled_capabilities, NULL, 0u, record,
              sizeof(record), &record_size) == VD_COMPANION_OK &&
              vd_companion_service_process(
                  &service, record, record_size) == VD_COMPANION_OK,
          "service fixture pairs before local trace operations");
    vd_endpoint_virtual_fs_init(&filesystem, &service);

    CHECK(vd_endpoint_fs_resolve(
              &filesystem, "../status.bin", path, sizeof(path),
              &flags, &file_size) != 0,
          "virtual root rejects traversal");
    CHECK(vd_endpoint_fs_resolve(
              &filesystem, "status.bin", path, sizeof(path),
              &flags, &file_size) == 0 &&
              strcmp(path, "status.bin") == 0 &&
              flags == VD_COMPANION_FS_REQUIRED_REGULAR &&
              file_size == VD_ENDPOINT_STATUS_SIZE,
          "status resolves only inside virtual root");
    CHECK(vd_endpoint_fs_read(
              &filesystem, "status.bin", 0u, output,
              sizeof(output), &output_size, &flags,
              &file_size) == 0 &&
              output_size == VD_ENDPOINT_STATUS_SIZE &&
              memcmp(output, "VDST", 4u) == 0,
          "status read is bounded and synthetic");
    CHECK(vd_endpoint_fs_read(
              &filesystem, "status.bin",
              VD_ENDPOINT_STATUS_SIZE + 1u, output, sizeof(output),
              &output_size, &flags, &file_size) != 0,
          "status offset overflow rejected");
    CHECK(vd_endpoint_fs_list(
              &filesystem, "", entries, 2u, &entry_count) == 0 &&
              entry_count == 1u,
          "trace absent until a complete recording exists");

    CHECK(vd_companion_input_record_begin(
              &service, 4u, 1000u) == VD_INPUT_TRACE_OK &&
              vd_companion_input_record_physical(
                  &service, &input, 1000u, 0u) ==
                  VD_INPUT_TRACE_OK &&
              vd_companion_input_record_end(
                  &service, VD_INPUT_TRACE_END_COMPLETE) ==
                  VD_INPUT_TRACE_OK,
          "recording fixture creates virtual trace");
    CHECK(vd_companion_input_trace_data(
              &service, &trace, &trace_size) == VD_INPUT_TRACE_OK &&
              trace_size > VD_INPUT_TRACE_HEADER_SIZE,
          "completed trace is available");
    CHECK(vd_endpoint_fs_list(
              &filesystem, "", entries, 2u, &entry_count) == 0 &&
              entry_count == 2u &&
              strcmp(entries[1].name, "input.vdtr") == 0,
          "virtual trace is listed");
    CHECK(vd_endpoint_fs_read(
              &filesystem, "input.vdtr", 0u, output,
              sizeof(output), &output_size, &flags,
              &file_size) == 0 &&
              output_size == sizeof(output) &&
              file_size == trace_size &&
              memcmp(output, trace, output_size) == 0,
          "virtual trace read uses immutable in-memory object");
    CHECK(vd_companion_service_close(&service) == VD_COMPANION_OK,
          "service fixture cleanup succeeds");
}

int main(void)
{
    test_config();
    test_receive();
    test_send();
    test_virtual_filesystem();
    if (failures != 0) {
        fprintf(stderr, "%d endpoint test(s) failed\n", failures);
        return 1;
    }
    puts("vitadebug endpoint tests passed");
    return 0;
}

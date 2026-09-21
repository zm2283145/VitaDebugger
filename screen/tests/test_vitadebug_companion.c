#include "vitadebug_companion.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

struct fixture {
    uint8_t sent[VD_COMPANION_MAX_RECORD];
    size_t sent_size;
    size_t send_count;
    int bind_result;
    int bind_calls;
    uint32_t bound_scope;
    uint16_t bound_port;
    int close_calls;
    int input_calls;
    int input_fail_call;
    struct vd_companion_input last_input;
    uint32_t resolve_flags;
    int fs_calls;
};

static uint16_t read_u16(const uint8_t* input)
{
    return (uint16_t)(((uint16_t)input[0] << 8) | input[1]);
}

static uint32_t read_u32(const uint8_t* input)
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | input[3];
}

static uint64_t read_u64(const uint8_t* input)
{
    return ((uint64_t)read_u32(input) << 32) | read_u32(input + 4);
}

static void write_u16(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void write_u32(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void write_u64(uint8_t* output, uint64_t value)
{
    write_u32(output, (uint32_t)(value >> 32));
    write_u32(output + 4, (uint32_t)value);
}

static int bind_fake(void* user, uint32_t scope, uint16_t port)
{
    struct fixture* fixture = (struct fixture*)user;
    ++fixture->bind_calls;
    fixture->bound_scope = scope;
    fixture->bound_port = port;
    return fixture->bind_result;
}

static int send_fake(void* user, const uint8_t* data, size_t size)
{
    struct fixture* fixture = (struct fixture*)user;
    if (size > sizeof(fixture->sent))
        return -1;
    memcpy(fixture->sent, data, size);
    fixture->sent_size = size;
    ++fixture->send_count;
    return 0;
}

static int close_fake(void* user)
{
    struct fixture* fixture = (struct fixture*)user;
    ++fixture->close_calls;
    return 0;
}

static int input_fake(void* user,
                      const struct vd_companion_input* input)
{
    struct fixture* fixture = (struct fixture*)user;
    fixture->last_input = *input;
    ++fixture->input_calls;
    if (fixture->input_fail_call != 0 &&
        fixture->input_calls == fixture->input_fail_call)
        return -1;
    return 0;
}

static int resolve_fake(void* user, const char* relative_path,
                        char* canonical_path, size_t canonical_capacity,
                        uint32_t* flags, uint64_t* size)
{
    struct fixture* fixture = (struct fixture*)user;
    const size_t length = strlen(relative_path);
    ++fixture->fs_calls;
    if (length + 1u > canonical_capacity)
        return -1;
    memcpy(canonical_path, relative_path, length + 1u);
    *flags = fixture->resolve_flags;
    *size = strcmp(relative_path, "log.txt") == 0 ? 6u : 0u;
    return 0;
}

static int list_fake(void* user, const char* canonical_directory,
                     struct vd_companion_file_entry* entries,
                     size_t entry_capacity, size_t* entry_count)
{
    struct fixture* fixture = (struct fixture*)user;
    ++fixture->fs_calls;
    if (canonical_directory[0] != '\0' || entry_capacity < 1u)
        return -1;
    memset(entries, 0, sizeof(entries[0]));
    memcpy(entries[0].name, "log.txt", 8u);
    entries[0].size = 6u;
    entries[0].flags = VD_COMPANION_FS_REQUIRED_REGULAR;
    *entry_count = 1u;
    return 0;
}

static int read_fake(void* user, const char* canonical_file,
                     uint64_t offset, uint8_t* output,
                     size_t output_capacity, size_t* output_size,
                     uint32_t* flags, uint64_t* file_size)
{
    static const uint8_t content[] = "hello\n";
    struct fixture* fixture = (struct fixture*)user;
    size_t remaining;
    ++fixture->fs_calls;
    if (strcmp(canonical_file, "log.txt") != 0 || offset > 6u)
        return -1;
    remaining = 6u - (size_t)offset;
    if (remaining > output_capacity)
        remaining = output_capacity;
    memcpy(output, content + offset, remaining);
    *output_size = remaining;
    *flags = fixture->resolve_flags;
    *file_size = 6u;
    return 0;
}

static int screen_write(void* user, const uint8_t* data, size_t size)
{
    (void)user;
    (void)data;
    (void)size;
    return 0;
}

static void configure(struct vd_companion_config* config,
                      struct fixture* fixture)
{
    size_t index;

    vd_companion_config_init(config);
    config->explicit_consent = VD_COMPANION_EXPLICIT_CONSENT;
    config->mutation_consent = VD_COMPANION_MUTATION_CONSENT;
    config->enabled_capabilities =
        VD_COMPANION_CAP_STATUS | VD_COMPANION_CAP_APP_INPUT |
        VD_COMPANION_CAP_DEBUG_FILES;
    for (index = 0u; index < sizeof(config->secret); ++index)
        config->secret[index] = (uint8_t)(index + 1u);
    memcpy(config->title_id, "VDSCRN001", 10u);
    config->process_id = 42u;
    config->process_generation = 7u;
    config->session_id = UINT64_C(0x1122334455667788);
    config->debug_root = "ux0:/data/VDSCRN001/debug";
    config->bind = bind_fake;
    config->send = send_fake;
    config->close = close_fake;
    config->transport_user = fixture;
    config->apply_input = input_fake;
    config->input_user = fixture;
    config->filesystem.user = fixture;
    config->filesystem.resolve = resolve_fake;
    config->filesystem.list = list_fake;
    config->filesystem.read_regular = read_fake;
    fixture->resolve_flags = VD_COMPANION_FS_REQUIRED_DIRECTORY;
}

static int request(struct vd_companion_service* service,
                   struct vd_companion_config* config, uint16_t type,
                   uint64_t sequence, uint64_t now_ms, uint64_t capabilities,
                   const uint8_t* payload, size_t payload_size,
                   uint8_t* record, size_t* record_size)
{
    int result = vd_companion_encode_record(
        config->secret, type, 0u, sequence, now_ms + 500u,
        config->session_id, config->process_generation, capabilities,
        payload, payload_size, record, VD_COMPANION_MAX_RECORD,
        record_size);
    if (result != VD_COMPANION_OK)
        return result;
    return vd_companion_service_process(service, record, *record_size,
                                        now_ms);
}

static void test_defaults_ports_and_bind(void)
{
    struct vd_companion_config config;
    struct vd_companion_service service = {0};
    struct fixture fixture = {0};

    vd_companion_config_init(&config);
    CHECK(config.explicit_consent == 0u &&
              config.control_port == 18198u &&
              config.screen_port == 18197u,
          "companion defaults off on distinct ports");
    CHECK(vd_companion_validate_control_port(1337u) ==
              VD_COMPANION_ERROR_PORT &&
              vd_companion_validate_control_port(1338u) ==
                  VD_COMPANION_ERROR_PORT &&
              vd_companion_validate_control_port(1348u) ==
                  VD_COMPANION_ERROR_PORT &&
              vd_companion_validate_control_port(18194u) ==
                  VD_COMPANION_ERROR_PORT &&
              vd_companion_validate_control_port(18197u) ==
                  VD_COMPANION_ERROR_PORT &&
              vd_companion_validate_control_port(18198u) ==
                  VD_COMPANION_OK &&
              vd_companion_validate_screen_port(18197u) ==
                  VD_COMPANION_OK &&
              vd_companion_validate_screen_port(18198u) ==
                  VD_COMPANION_ERROR_PORT,
          "control port rejects fallback, bridge, and reserved services");
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_ERROR_DISABLED,
          "initialization requires explicit consent");

    configure(&config, &fixture);
    config.network_scope = VD_COMPANION_NETWORK_PRIVATE_LAN;
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_ERROR_DISABLED,
          "private LAN requires separate consent");
    configure(&config, &fixture);
    config.mutation_consent = 0u;
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_ERROR_DISABLED,
          "input mutation requires separate explicit consent");
    configure(&config, &fixture);
    config.network_scope = VD_COMPANION_NETWORK_PRIVATE_LAN;
    config.lan_consent = VD_COMPANION_LAN_CONSENT;
    fixture.bind_result = -1;
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_ERROR_BIND &&
              service.state == VD_COMPANION_STATE_FAILED &&
              fixture.bind_calls == 1 && fixture.bound_port == 18198u,
          "bind conflict is terminal without port fallback");
}

static void test_pair_status_input_and_cleanup(void)
{
    uint8_t record[VD_COMPANION_MAX_RECORD];
    uint8_t payload[36] = {0};
    size_t record_size = 0u;
    const uint64_t capabilities =
        VD_COMPANION_CAP_STATUS | VD_COMPANION_CAP_APP_INPUT |
        VD_COMPANION_CAP_DEBUG_FILES;
    struct vd_companion_config config;
    struct vd_companion_service service = {0};
    struct fixture fixture = {0};

    configure(&config, &fixture);
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK &&
              fixture.bound_scope == VD_COMPANION_NETWORK_LOOPBACK,
          "valid service binds once to explicit loopback scope");
    CHECK(request(&service, &config, VD_COMPANION_MESSAGE_HELLO, 1u,
                  1000u, capabilities | VD_COMPANION_CAP_TITLE_LAUNCH,
                  NULL, 0u, record, &record_size) == VD_COMPANION_OK &&
              service.state == VD_COMPANION_STATE_PAIRED &&
              read_u64(fixture.sent + VD_COMPANION_HEADER_SIZE + 8u) ==
                  capabilities,
          "hello negotiates only implemented capabilities");
    CHECK(request(&service, &config, VD_COMPANION_MESSAGE_STATUS, 2u,
                  1001u, capabilities, NULL, 0u, record, &record_size) ==
              VD_COMPANION_OK &&
              memcmp(fixture.sent + VD_COMPANION_HEADER_SIZE,
                     "VDSCRN001", 9u) == 0 &&
              read_u32(fixture.sent + VD_COMPANION_HEADER_SIZE + 12u) ==
                  42u,
          "status reports only the configured source-owned target");

    write_u32(payload,
              VD_INPUT_BUTTON_CROSS | VD_INPUT_BUTTON_RIGHT);
    write_u16(payload + 4, (uint16_t)(int16_t)-100);
    write_u16(payload + 6, 200u);
    write_u16(payload + 8, 300u);
    write_u16(payload + 10, (uint16_t)(int16_t)-400);
    write_u32(payload + 32, 100u);
    CHECK(request(&service, &config, VD_COMPANION_MESSAGE_INPUT, 3u,
                  1010u, capabilities, payload, sizeof(payload), record,
                  &record_size) == VD_COMPANION_OK &&
              fixture.input_calls == 1 &&
              fixture.last_input.buttons ==
                  (VD_INPUT_BUTTON_CROSS | VD_INPUT_BUTTON_RIGHT) &&
              fixture.last_input.left_x == -100 &&
              service.input_active != 0u,
          "cooperative input callback receives bounded leased state");
    CHECK(vd_companion_service_tick(&service, 1109u) == VD_COMPANION_OK &&
              fixture.input_calls == 1,
          "input remains active before lease expiry");
    CHECK(vd_companion_service_tick(&service, 1110u) == VD_COMPANION_OK &&
              fixture.input_calls == 2 &&
              fixture.last_input.buttons == 0u &&
              service.input_active == 0u,
          "watchdog forces neutral input at lease expiry");

    CHECK(request(&service, &config, VD_COMPANION_MESSAGE_INPUT, 4u,
                  1120u, capabilities, payload, sizeof(payload), record,
                  &record_size) == VD_COMPANION_OK,
          "second input lease starts");
    CHECK(vd_companion_service_disconnect(&service) == VD_COMPANION_OK &&
              fixture.input_calls == 4 &&
              service.state == VD_COMPANION_STATE_CLOSED &&
              fixture.close_calls == 1,
          "disconnect releases input and terminally closes the session");
    CHECK(vd_companion_service_close(&service) == VD_COMPANION_OK &&
              fixture.close_calls == 1 &&
              service.state == VD_COMPANION_STATE_CLOSED,
          "shutdown closes the bound transport exactly once");
}

static void test_files_and_unsupported(void)
{
    uint8_t record[VD_COMPANION_MAX_RECORD];
    uint8_t payload[64] = {0};
    size_t record_size = 0u;
    const uint64_t capabilities =
        VD_COMPANION_CAP_STATUS | VD_COMPANION_CAP_DEBUG_FILES;
    struct vd_companion_config config;
    struct vd_companion_service service = {0};
    struct fixture fixture = {0};

    configure(&config, &fixture);
    config.enabled_capabilities = capabilities;
    config.mutation_consent = 0u;
    config.apply_input = NULL;
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK,
          "read-only companion initializes without mutation consent");
    CHECK(request(&service, &config, VD_COMPANION_MESSAGE_HELLO, 1u,
                  2000u, capabilities, NULL, 0u, record, &record_size) ==
              VD_COMPANION_OK,
          "read-only session pairs");

    fixture.resolve_flags = VD_COMPANION_FS_REQUIRED_DIRECTORY;
    write_u16(payload, 0u);
    CHECK(request(&service, &config, VD_COMPANION_MESSAGE_FILE_LIST, 2u,
                  2010u, capabilities, payload, 2u, record,
                  &record_size) == VD_COMPANION_OK &&
              read_u16(fixture.sent + VD_COMPANION_HEADER_SIZE) == 1u &&
              fixture.sent[VD_COMPANION_HEADER_SIZE + 2u] == 7u,
          "root listing emits one bounded validated regular file");

    memset(payload, 0, sizeof(payload));
    write_u16(payload, 7u);
    memcpy(payload + 2, "log.txt", 7u);
    write_u64(payload + 9, 0u);
    write_u32(payload + 17, 6u);
    fixture.resolve_flags = VD_COMPANION_FS_REQUIRED_REGULAR;
    CHECK(request(&service, &config, VD_COMPANION_MESSAGE_FILE_READ, 3u,
                  2020u, capabilities, payload, 21u, record,
                  &record_size) == VD_COMPANION_OK &&
              read_u32(fixture.sent + VD_COMPANION_HEADER_SIZE + 8u) ==
                  6u &&
              memcmp(fixture.sent + VD_COMPANION_HEADER_SIZE + 16u,
                     "hello\n", 6u) == 0,
          "regular read is bounded and tied to resolved root object");

    fixture.resolve_flags =
        VD_COMPANION_FS_INSIDE_ROOT | VD_COMPANION_FS_REGULAR;
    CHECK(request(&service, &config, VD_COMPANION_MESSAGE_FILE_READ, 4u,
                  2025u, capabilities, payload, 21u, record,
                  &record_size) == VD_COMPANION_ERROR_PATH,
          "provider objects without no-symlink attestation are rejected");

    memset(payload, 0, sizeof(payload));
    write_u16(payload, 9u);
    memcpy(payload + 2, "../secret", 9u);
    write_u64(payload + 11, 0u);
    write_u32(payload + 19, 1u);
    CHECK(request(&service, &config, VD_COMPANION_MESSAGE_FILE_READ, 5u,
                  2030u, capabilities, payload, 23u, record,
                  &record_size) == VD_COMPANION_ERROR_PATH &&
              read_u16(fixture.sent + 10u) ==
                  (uint16_t)-VD_COMPANION_ERROR_PATH,
          "path traversal returns an authenticated typed error");

    CHECK(request(&service, &config,
                  VD_COMPANION_MESSAGE_TITLE_INVENTORY, 6u, 2040u,
                  capabilities, NULL, 0u, record, &record_size) ==
              VD_COMPANION_ERROR_UNSUPPORTED &&
              read_u16(fixture.sent + 10u) ==
                  (uint16_t)-VD_COMPANION_ERROR_UNSUPPORTED,
          "title inventory is explicitly unsupported");
    CHECK(request(&service, &config,
                  VD_COMPANION_MESSAGE_TITLE_LAUNCH, 7u, 2050u,
                  capabilities, NULL, 0u, record, &record_size) ==
              VD_COMPANION_ERROR_UNSUPPORTED,
          "title launch is explicitly unsupported");
    (void)vd_companion_service_close(&service);
}

static void test_replay_deadline_auth_and_malformed(void)
{
    uint8_t record[VD_COMPANION_MAX_RECORD];
    uint8_t mutated[VD_COMPANION_MAX_RECORD];
    size_t record_size = 0u;
    size_t length;
    struct vd_companion_config config;
    struct vd_companion_service service = {0};
    struct fixture fixture = {0};

    configure(&config, &fixture);
    config.enabled_capabilities = VD_COMPANION_CAP_STATUS;
    config.mutation_consent = 0u;
    config.apply_input = NULL;
    CHECK(vd_companion_encode_record(
              config.secret, VD_COMPANION_MESSAGE_HELLO, 0u, 1u, 3000u,
              config.session_id, config.process_generation,
              VD_COMPANION_CAP_STATUS, NULL, 0u, record, sizeof(record),
              &record_size) == VD_COMPANION_OK,
          "canonical hello encodes");
    for (length = 0u; length < VD_COMPANION_HEADER_SIZE; ++length) {
        memset(&service, 0, sizeof(service));
        CHECK(vd_companion_service_init(&service, &config) ==
                  VD_COMPANION_OK,
              "truncated-record fixture initializes");
        CHECK(vd_companion_service_process(&service, record, length,
                                           2500u) ==
                  VD_COMPANION_ERROR_PROTOCOL,
              "every truncated header fails closed");
        CHECK(service.state == VD_COMPANION_STATE_FAILED &&
                  fixture.close_calls == (int)(length + 1u),
              "malformed record terminally closes its session");
    }
    memset(&service, 0, sizeof(service));
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK,
          "bad-tag fixture initializes");
    memcpy(mutated, record, record_size);
    mutated[64] ^= 1u;
    CHECK(vd_companion_service_process(&service, mutated, record_size,
                                       2500u) ==
              VD_COMPANION_ERROR_AUTH,
          "bad record authentication fails closed");
    memset(&service, 0, sizeof(service));
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK,
          "bad-magic fixture initializes");
    memcpy(mutated, record, record_size);
    mutated[0] ^= 1u;
    CHECK(vd_companion_service_process(&service, mutated, record_size,
                                       2500u) ==
              VD_COMPANION_ERROR_PROTOCOL,
          "bad framing magic fails before dispatch");
    memset(&service, 0, sizeof(service));
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK,
          "deadline fixture initializes");
    CHECK(vd_companion_service_process(&service, record, record_size,
                                       3001u) ==
              VD_COMPANION_ERROR_DEADLINE,
          "expired request is rejected");
    memset(&service, 0, sizeof(service));
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK,
          "replay fixture initializes");
    CHECK(vd_companion_service_process(&service, record, record_size,
                                       2500u) == VD_COMPANION_OK,
          "valid authenticated hello pairs");
    CHECK(vd_companion_service_process(&service, record, record_size,
                                       2500u) ==
              VD_COMPANION_ERROR_REPLAY,
          "duplicate sequence is rejected");

    for (length = 0u; length < 256u; ++length) {
        size_t index;
        const size_t fuzz_size = length % (VD_COMPANION_HEADER_SIZE + 1u);
        memset(&service, 0, sizeof(service));
        CHECK(vd_companion_service_init(&service, &config) ==
                  VD_COMPANION_OK,
              "deterministic malformed fixture initializes");
        for (index = 0u; index < fuzz_size; ++index)
            mutated[index] =
                (uint8_t)(index * 33u + length * 17u + 11u);
        CHECK(vd_companion_service_process(&service, mutated, fuzz_size,
                                           2500u) < 0,
              "deterministic malformed corpus never dispatches");
    }
    (void)vd_companion_service_close(&service);
}

static void test_integrated_screen(void)
{
    uint8_t pixels[4] = {1u, 2u, 3u, 4u};
    struct vd_screen_source source = {pixels, sizeof(pixels)};
    struct vd_companion_screen_config screen = {0};
    struct vd_companion_config config;
    struct vd_companion_service service = {0};
    struct vd_screen_frame frame = {
        pixels, 1u, 1u, 4u, VD_SCREEN_PIXEL_RGBA8888, 1000u,
    };
    struct fixture fixture = {0};
    size_t index;

    configure(&config, &fixture);
    screen.write = screen_write;
    screen.sources = &source;
    screen.source_count = 1u;
    screen.max_width = 1u;
    screen.max_height = 1u;
    screen.max_payload_bytes = 4u;
    screen.min_frame_interval_us =
        VD_SCREEN_DEFAULT_MIN_FRAME_INTERVAL_US;
    for (index = 0u; index < sizeof(screen.auth_token); ++index)
        screen.auth_token[index] = (uint8_t)(0xa0u + index);
    config.enabled_capabilities =
        VD_COMPANION_CAP_STATUS | VD_COMPANION_CAP_SCREEN;
    config.mutation_consent = 0u;
    config.apply_input = NULL;
    config.screen = &screen;
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK &&
              service.screen_initialized != 0u,
          "same companion initializes source-owned framebuffer stream");
    CHECK(vd_companion_screen_begin(&service) == VD_COMPANION_OK &&
              vd_companion_submit_displayed_frame(&service, &frame) ==
                  VD_COMPANION_OK,
          "integrated stream begins and submits registered display buffer");
    (void)vd_companion_service_close(&service);
}

static void test_integrated_input_trace(void)
{
    uint8_t trace_buffer[VD_INPUT_TRACE_HEADER_SIZE +
                         8u * VD_INPUT_TRACE_EVENT_SIZE];
    uint8_t record[VD_COMPANION_MAX_RECORD];
    size_t record_size = 0u;
    const uint64_t capabilities =
        VD_COMPANION_CAP_STATUS | VD_COMPANION_CAP_INPUT_RECORD |
        VD_COMPANION_CAP_INPUT_PLAYBACK;
    struct vd_companion_config config;
    struct vd_companion_service service = {0};
    struct vd_companion_input input = {0};
    struct vd_input_trace_info info;
    struct fixture fixture = {0};
    const uint8_t* trace_data;
    size_t trace_size;

    configure(&config, &fixture);
    config.enabled_capabilities = capabilities;
    config.record_consent = VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT;
    config.playback_consent =
        VD_INPUT_TRACE_EXPLICIT_PLAYBACK_CONSENT;
    config.trace_buffer = trace_buffer;
    config.trace_buffer_capacity = sizeof(trace_buffer);
    config.trace_max_events = 8u;
    config.trace_max_duration_us = 1000000u;
    config.trace_max_scheduling_drift_us = 100u;
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK &&
              request(&service, &config, VD_COMPANION_MESSAGE_HELLO, 1u,
                      4000u, capabilities, NULL, 0u, record,
                      &record_size) == VD_COMPANION_OK,
          "trace-enabled companion pairs with separate capabilities");
    CHECK(vd_companion_input_record_begin(&service, 99u, 1000u) ==
              VD_INPUT_TRACE_OK,
          "recording requires an explicit paired-session start");
    CHECK(request(&service, &config, VD_COMPANION_MESSAGE_STATUS, 2u,
                  4001u, capabilities, NULL, 0u, record, &record_size) ==
              VD_COMPANION_OK &&
              read_u32(fixture.sent + VD_COMPANION_HEADER_SIZE + 40u) ==
                  VD_INPUT_TRACE_STATE_RECORDING,
          "paired status exposes visible recording state");
    CHECK(vd_companion_input_playback_begin(&service, 1000u) ==
              VD_INPUT_TRACE_ERROR_STATE,
          "recording and playback are mutually exclusive");
    input.buttons = VD_INPUT_BUTTON_CROSS;
    input.left_x = -12;
    input.right_y = 34;
    input.touch_count = 1u;
    input.touches[0] =
        (struct vd_input_touch){3u, 100u, 200u, 300u};
    CHECK(vd_companion_input_record_physical(
              &service, &input, 1000u, 5u) == VD_INPUT_TRACE_OK &&
              vd_companion_input_record_checkpoint(
                  &service, VD_INPUT_TRACE_CHECKPOINT_FRAME, 1100u,
                  6u) == VD_INPUT_TRACE_OK &&
              vd_companion_input_record_end(
                  &service, VD_INPUT_TRACE_END_COMPLETE) ==
                  VD_INPUT_TRACE_OK,
          "cooperative physical state and checkpoint record exactly");
    CHECK(vd_companion_input_trace_data(
              &service, &trace_data, &trace_size) ==
              VD_INPUT_TRACE_OK &&
              vd_input_trace_verify(trace_data, trace_size,
                                    &service.trace.identity,
                                    &info) == VD_INPUT_TRACE_OK &&
              info.event_count == 2u,
          "completed companion trace is host-verifiable");
    CHECK(vd_companion_input_playback_begin(&service, 5000u) ==
              VD_INPUT_TRACE_OK &&
              vd_companion_input_playback_tick(&service, 5000u) ==
                  VD_INPUT_TRACE_OK &&
              fixture.input_calls == 1 &&
              fixture.last_input.touch_count == 1u &&
              fixture.last_input.touches[0].x == 100u,
          "playback uses only the registered cooperative callback");
    CHECK(vd_companion_input_playback_tick(&service, 5100u) ==
              VD_INPUT_TRACE_COMPLETE &&
              fixture.input_calls == 2 &&
              fixture.last_input.buttons == 0u &&
              fixture.last_input.touch_count == 0u,
          "playback completion forces exact neutral input");
    CHECK(vd_companion_input_playback_begin(&service, 6000u) ==
              VD_INPUT_TRACE_OK &&
              vd_companion_input_trace_abort(
                  &service, VD_INPUT_TRACE_END_CANCELLED) ==
                  VD_COMPANION_OK &&
              fixture.input_calls == 3 &&
              fixture.last_input.buttons == 0u,
          "playback cancel forces neutral cleanup");
    CHECK(vd_companion_input_playback_begin(&service, 7000u) ==
              VD_INPUT_TRACE_OK &&
              vd_companion_service_disconnect(&service) ==
                  VD_COMPANION_OK &&
              fixture.input_calls == 4 &&
              fixture.last_input.buttons == 0u &&
              service.state == VD_COMPANION_STATE_CLOSED,
          "disconnect mid-replay forces neutral and never auto-resumes");
}

static void test_trace_disconnect_callback_and_timeout(void)
{
    uint8_t trace_buffer[VD_INPUT_TRACE_HEADER_SIZE +
                         2u * VD_INPUT_TRACE_EVENT_SIZE];
    uint8_t record[VD_COMPANION_MAX_RECORD];
    size_t record_size = 0u;
    const uint64_t capabilities =
        VD_COMPANION_CAP_STATUS | VD_COMPANION_CAP_INPUT_RECORD |
        VD_COMPANION_CAP_INPUT_PLAYBACK;
    struct vd_companion_config config;
    struct vd_companion_service service = {0};
    struct vd_companion_input input = {0};
    struct vd_input_trace_info info;
    struct fixture fixture = {0};

    configure(&config, &fixture);
    config.enabled_capabilities = capabilities;
    config.record_consent = VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT;
    config.playback_consent =
        VD_INPUT_TRACE_EXPLICIT_PLAYBACK_CONSENT;
    config.trace_buffer = trace_buffer;
    config.trace_buffer_capacity = sizeof(trace_buffer);
    config.trace_max_events = 2u;
    config.trace_max_duration_us = 1000000u;
    config.trace_max_scheduling_drift_us = 100u;
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK &&
              request(&service, &config, VD_COMPANION_MESSAGE_HELLO, 1u,
                      7000u, capabilities, NULL, 0u, record,
                      &record_size) == VD_COMPANION_OK &&
              vd_companion_input_record_begin(&service, 100u, 1000u) ==
                  VD_INPUT_TRACE_OK &&
              vd_companion_input_record_physical(
                  &service, &input, 1000u, 1u) ==
                  VD_INPUT_TRACE_OK &&
              vd_companion_service_disconnect(&service) ==
                  VD_COMPANION_OK &&
              vd_companion_input_trace_get_info(&service, &info) ==
                  VD_INPUT_TRACE_OK &&
              info.end_reason == VD_INPUT_TRACE_END_DISCONNECT,
          "disconnect mid-record finalizes an explicit aborted trace");

    memset(&service, 0, sizeof(service));
    memset(&fixture, 0, sizeof(fixture));
    configure(&config, &fixture);
    config.enabled_capabilities = capabilities;
    config.record_consent = VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT;
    config.playback_consent =
        VD_INPUT_TRACE_EXPLICIT_PLAYBACK_CONSENT;
    config.trace_buffer = trace_buffer;
    config.trace_buffer_capacity = sizeof(trace_buffer);
    config.trace_max_events = 2u;
    config.trace_max_duration_us = 1000000u;
    config.trace_max_scheduling_drift_us = 100u;
    CHECK(vd_companion_service_init(&service, &config) ==
              VD_COMPANION_OK &&
              request(&service, &config, VD_COMPANION_MESSAGE_HELLO, 1u,
                      8000u, capabilities, NULL, 0u, record,
                      &record_size) == VD_COMPANION_OK &&
              vd_companion_input_record_begin(&service, 101u, 0u) ==
                  VD_INPUT_TRACE_OK &&
              vd_companion_input_record_physical(
                  &service, &input, 0u, 1u) ==
                  VD_INPUT_TRACE_OK &&
              vd_companion_input_record_end(
                  &service, VD_INPUT_TRACE_END_COMPLETE) ==
                  VD_INPUT_TRACE_OK &&
              vd_companion_input_playback_begin(&service, 9000u) ==
                  VD_INPUT_TRACE_OK,
          "callback failure fixture starts playback");
    fixture.input_fail_call = 1;
    CHECK(vd_companion_input_playback_tick(&service, 9000u) ==
              VD_INPUT_TRACE_ERROR_CALLBACK &&
              fixture.input_calls == 2 &&
              fixture.last_input.buttons == 0u &&
              service.state == VD_COMPANION_STATE_FAILED,
          "callback failure forces neutral and terminal cleanup");
}

int main(void)
{
    test_defaults_ports_and_bind();
    test_pair_status_input_and_cleanup();
    test_files_and_unsupported();
    test_replay_deadline_auth_and_malformed();
    test_integrated_screen();
    test_integrated_input_trace();
    test_trace_disconnect_callback_and_timeout();
    if (failures != 0) {
        fprintf(stderr, "%d companion test(s) failed\n", failures);
        return 1;
    }
    puts("companion control tests passed");
    return 0;
}

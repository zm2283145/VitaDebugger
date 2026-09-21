#include "vd_endpoint.h"

#include <string.h>

static uint16_t vd_load_u16(const uint8_t* input)
{
    return (uint16_t)(((uint16_t)input[0] << 8) | input[1]);
}

static uint32_t vd_load_u32(const uint8_t* input)
{
    return ((uint32_t)input[0] << 24) |
           ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) |
           (uint32_t)input[3];
}

static uint64_t vd_load_u64(const uint8_t* input)
{
    return ((uint64_t)vd_load_u32(input) << 32) |
           vd_load_u32(input + 4u);
}

static void vd_store_u32(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void vd_store_u64(uint8_t* output, uint64_t value)
{
    vd_store_u32(output, (uint32_t)(value >> 32));
    vd_store_u32(output + 4u, (uint32_t)value);
}

static int vd_nonzero(const uint8_t* bytes, size_t size)
{
    uint8_t value = 0u;
    size_t index;

    for (index = 0u; index < size; ++index)
        value |= bytes[index];
    return value != 0u;
}

static int vd_private_ipv4(const uint8_t address[4])
{
    return address[0] == 10u ||
           (address[0] == 172u &&
            address[1] >= 16u && address[1] <= 31u) ||
           (address[0] == 192u && address[1] == 168u);
}

int vd_endpoint_ipv4_text_matches(const uint8_t expected[4],
                                  const char* actual)
{
    uint8_t parsed[4];
    size_t component;
    const char* cursor;

    if (expected == NULL || actual == NULL)
        return 0;
    cursor = actual;
    for (component = 0u; component < 4u; ++component) {
        unsigned int value = 0u;
        size_t digits = 0u;

        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10u + (unsigned int)(*cursor - '0');
            if (value > 255u || ++digits > 3u)
                return 0;
            ++cursor;
        }
        if (digits == 0u ||
            (component < 3u ? *cursor != '.' : *cursor != '\0'))
            return 0;
        parsed[component] = (uint8_t)value;
        if (component < 3u)
            ++cursor;
    }
    return memcmp(parsed, expected, sizeof(parsed)) == 0;
}

void vd_endpoint_config_init(struct vd_endpoint_config* config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}

int vd_endpoint_config_parse(struct vd_endpoint_config* config,
                             const uint8_t* data, size_t size)
{
    struct vd_endpoint_config parsed;
    uint64_t capabilities;
    uint64_t consented_capabilities;
    uint32_t expected_mutation_consent;
    uint32_t expected_record_consent;
    uint32_t expected_playback_consent;
    size_t index;

    if (config == NULL || data == NULL)
        return VD_ENDPOINT_ERROR_ARGUMENT;
    vd_endpoint_config_init(config);
    if (size != VD_ENDPOINT_CONFIG_SIZE ||
        memcmp(data, "VDCG", 4u) != 0 ||
        vd_load_u16(data + 4u) != VD_ENDPOINT_CONFIG_VERSION ||
        vd_load_u16(data + 6u) != VD_ENDPOINT_CONFIG_SIZE ||
        vd_load_u32(data + 8u) != VD_COMPANION_EXPLICIT_CONSENT)
        return VD_ENDPOINT_ERROR_CONFIG;

    capabilities = vd_load_u64(data + 32u);
    consented_capabilities = vd_load_u64(data + 40u);
    if ((capabilities & VD_COMPANION_CAP_STATUS) == 0u ||
        (capabilities & ~VD_COMPANION_CAP_SUPPORTED) != 0u ||
        consented_capabilities != capabilities)
        return VD_ENDPOINT_ERROR_CONFIG;
    for (index = 124u; index < size; ++index) {
        if (data[index] != 0u)
            return VD_ENDPOINT_ERROR_CONFIG;
    }

    parsed.mutation_consent = vd_load_u32(data + 16u);
    parsed.record_consent = vd_load_u32(data + 20u);
    parsed.playback_consent = vd_load_u32(data + 24u);
    parsed.network_scope = vd_load_u32(data + 28u);
    parsed.enabled_capabilities = capabilities;
    memcpy(parsed.host_ipv4, data + 48u, sizeof(parsed.host_ipv4));
    parsed.control_port = vd_load_u16(data + 52u);
    parsed.screen_port = vd_load_u16(data + 54u);
    memcpy(parsed.control_secret, data + 56u,
           sizeof(parsed.control_secret));
    memcpy(parsed.screen_secret, data + 88u,
           sizeof(parsed.screen_secret));
    memcpy(parsed.bind_ipv4, data + 120u,
           sizeof(parsed.bind_ipv4));

    expected_mutation_consent =
        (capabilities & (VD_COMPANION_CAP_APP_INPUT |
                         VD_COMPANION_CAP_INPUT_PLAYBACK)) != 0u
            ? VD_COMPANION_MUTATION_CONSENT
            : 0u;
    expected_record_consent =
        (capabilities & VD_COMPANION_CAP_INPUT_RECORD) != 0u
            ? VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT
            : 0u;
    expected_playback_consent =
        (capabilities & VD_COMPANION_CAP_INPUT_PLAYBACK) != 0u
            ? VD_INPUT_TRACE_EXPLICIT_PLAYBACK_CONSENT
            : 0u;
    if (parsed.mutation_consent != expected_mutation_consent ||
        parsed.record_consent != expected_record_consent ||
        parsed.playback_consent != expected_playback_consent ||
        parsed.control_port != VD_COMPANION_DEFAULT_CONTROL_PORT ||
        parsed.screen_port != VD_COMPANION_DEFAULT_SCREEN_PORT ||
        !vd_nonzero(parsed.control_secret,
                    sizeof(parsed.control_secret)) ||
        !vd_nonzero(parsed.screen_secret,
                    sizeof(parsed.screen_secret)) ||
        memcmp(parsed.control_secret, parsed.screen_secret,
               sizeof(parsed.control_secret)) == 0)
        return VD_ENDPOINT_ERROR_CONFIG;

    if (parsed.network_scope == VD_COMPANION_NETWORK_LOOPBACK) {
        if (data[12u] != 0u || data[13u] != 0u ||
            data[14u] != 0u || data[15u] != 0u ||
            memcmp(parsed.host_ipv4, "\x7f\x00\x00\x01", 4u) != 0 ||
            memcmp(parsed.bind_ipv4, "\x7f\x00\x00\x01", 4u) != 0)
            return VD_ENDPOINT_ERROR_CONFIG;
    } else if (parsed.network_scope ==
               VD_COMPANION_NETWORK_PRIVATE_LAN) {
        if (vd_load_u32(data + 12u) != VD_COMPANION_LAN_CONSENT ||
            !vd_private_ipv4(parsed.host_ipv4) ||
            !vd_private_ipv4(parsed.bind_ipv4) ||
            memcmp(parsed.host_ipv4, parsed.bind_ipv4, 4u) == 0)
            return VD_ENDPOINT_ERROR_CONFIG;
    } else {
        return VD_ENDPOINT_ERROR_CONFIG;
    }

    *config = parsed;
    return 0;
}

void vd_endpoint_receiver_init(struct vd_endpoint_receiver* receiver)
{
    if (receiver != NULL) {
        memset(receiver, 0, sizeof(*receiver));
        receiver->expected = VD_COMPANION_HEADER_SIZE;
    }
}

int vd_endpoint_receiver_arm(struct vd_endpoint_receiver* receiver,
                             uint64_t now_ms, uint32_t deadline_ms)
{
    if (receiver == NULL || receiver->started != 0u ||
        deadline_ms == 0u || now_ms > UINT64_MAX - deadline_ms)
        return VD_ENDPOINT_ERROR_ARGUMENT;
    receiver->deadline_ms = now_ms + deadline_ms;
    receiver->started = 1u;
    return 0;
}

static int vd_deadline_expired(uint64_t now, uint64_t deadline)
{
    return now >= deadline;
}

int vd_endpoint_receiver_poll(struct vd_endpoint_receiver* receiver,
                              const struct vd_endpoint_io* io,
                              const uint8_t** record, size_t* record_size)
{
    size_t received = 0u;
    uint64_t now;
    int result;

    if (receiver == NULL || io == NULL || io->receive == NULL ||
        io->now_ms == NULL || record == NULL || record_size == NULL ||
        receiver->expected < VD_COMPANION_HEADER_SIZE ||
        receiver->expected > VD_COMPANION_MAX_RECORD ||
        receiver->offset > receiver->expected)
        return VD_ENDPOINT_ERROR_ARGUMENT;
    *record = NULL;
    *record_size = 0u;
    now = io->now_ms(io->user);
    if (receiver->started != 0u &&
        vd_deadline_expired(now, receiver->deadline_ms))
        return VD_ENDPOINT_ERROR_DEADLINE;

    result = io->receive(io->user, receiver->record + receiver->offset,
                         receiver->expected - receiver->offset, &received);
    if (result == VD_ENDPOINT_IO_AGAIN)
        return VD_ENDPOINT_POLL_IDLE;
    if (result == VD_ENDPOINT_IO_EOF || result == VD_ENDPOINT_IO_ERROR)
        return VD_ENDPOINT_ERROR_IO;
    if (result != VD_ENDPOINT_IO_OK || received == 0u ||
        received > receiver->expected - receiver->offset)
        return VD_ENDPOINT_ERROR_IO;

    if (receiver->started == 0u) {
        if (now > UINT64_MAX - VD_ENDPOINT_RECEIVE_DEADLINE_MS)
            return VD_ENDPOINT_ERROR_DEADLINE;
        receiver->deadline_ms =
            now + VD_ENDPOINT_RECEIVE_DEADLINE_MS;
        receiver->started = 1u;
    }
    receiver->offset += received;
    if (receiver->offset < receiver->expected)
        return VD_ENDPOINT_POLL_IDLE;

    if (receiver->expected == VD_COMPANION_HEADER_SIZE) {
        const uint32_t payload_size =
            vd_load_u32(receiver->record + 12u);
        if (payload_size > VD_COMPANION_MAX_PAYLOAD)
            return VD_ENDPOINT_ERROR_LIMIT;
        receiver->expected =
            VD_COMPANION_HEADER_SIZE + (size_t)payload_size;
        if (receiver->offset < receiver->expected)
            return VD_ENDPOINT_POLL_IDLE;
    }

    *record = receiver->record;
    *record_size = receiver->expected;
    return VD_ENDPOINT_POLL_RECORD;
}

int vd_endpoint_send_all(const struct vd_endpoint_io* io,
                         const uint8_t* data, size_t size,
                         uint32_t deadline_ms)
{
    size_t offset = 0u;
    uint64_t deadline;

    if (io == NULL || io->send == NULL || io->now_ms == NULL ||
        (size != 0u && data == NULL) || deadline_ms == 0u)
        return VD_ENDPOINT_ERROR_ARGUMENT;
    deadline = io->now_ms(io->user);
    if (deadline > UINT64_MAX - deadline_ms)
        return VD_ENDPOINT_ERROR_DEADLINE;
    deadline += deadline_ms;
    while (offset < size) {
        size_t sent = 0u;
        int result;

        if (io->progress != NULL &&
            io->progress(io->user) != VD_ENDPOINT_IO_OK)
            return VD_ENDPOINT_ERROR_IO;
        if (vd_deadline_expired(io->now_ms(io->user), deadline))
            return VD_ENDPOINT_ERROR_DEADLINE;
        result = io->send(
            io->user, data + offset, size - offset, &sent);
        if (result == VD_ENDPOINT_IO_OK) {
            if (sent == 0u || sent > size - offset)
                return VD_ENDPOINT_ERROR_IO;
            offset += sent;
            continue;
        }
        if (result != VD_ENDPOINT_IO_AGAIN)
            return VD_ENDPOINT_ERROR_IO;
        if (io->yield != NULL)
            io->yield(io->user);
    }
    return 0;
}

void vd_endpoint_virtual_fs_init(
    struct vd_endpoint_virtual_fs* filesystem,
    const struct vd_companion_service* service)
{
    if (filesystem != NULL) {
        memset(filesystem, 0, sizeof(*filesystem));
        filesystem->service = service;
    }
}

static int vd_copy_path(const char* path, char* output, size_t capacity)
{
    const size_t length = strlen(path);

    if (length + 1u > capacity)
        return -1;
    memcpy(output, path, length + 1u);
    return 0;
}

static size_t vd_status_data(const struct vd_companion_service* service,
                             uint8_t output[VD_ENDPOINT_STATUS_SIZE])
{
    struct vd_companion_status status;

    memset(output, 0, VD_ENDPOINT_STATUS_SIZE);
    if (service == NULL ||
        vd_companion_service_get_status(service, &status) !=
            VD_COMPANION_OK)
        return 0u;
    memcpy(output, "VDST", 4u);
    vd_store_u32(output + 4u, 1u);
    vd_store_u32(output + 8u, status.state);
    vd_store_u32(output + 12u, status.input_active);
    vd_store_u32(output + 16u, status.input_cleanup_pending);
    vd_store_u32(output + 20u, (uint32_t)status.last_error);
    vd_store_u32(output + 24u, (uint32_t)status.cleanup_error);
    vd_store_u32(output + 28u, status.trace_state);
    vd_store_u32(output + 32u, status.trace_end_reason);
    vd_store_u64(output + 36u, status.trace_event_count);
    vd_store_u64(output + 44u, status.trace_duration_us);
    vd_store_u64(output + 52u,
                 status.trace_max_scheduling_drift_us);
    vd_store_u32(output + 60u, 0u);
    return VD_ENDPOINT_STATUS_SIZE;
}

static int vd_trace_data(const struct vd_companion_service* service,
                         const uint8_t** data, size_t* size)
{
    if (service == NULL || data == NULL || size == NULL)
        return -1;
    return vd_companion_input_trace_data(service, data, size) ==
                   VD_INPUT_TRACE_OK
               ? 0
               : -1;
}

int vd_endpoint_fs_resolve(void* user, const char* relative_path,
                           char* canonical_path, size_t canonical_capacity,
                           uint32_t* flags, uint64_t* size)
{
    struct vd_endpoint_virtual_fs* filesystem =
        (struct vd_endpoint_virtual_fs*)user;
    const uint8_t* trace;
    size_t trace_size;

    if (filesystem == NULL || filesystem->service == NULL ||
        relative_path == NULL || canonical_path == NULL ||
        flags == NULL || size == NULL)
        return -1;
    if (relative_path[0] == '\0') {
        if (vd_copy_path("", canonical_path, canonical_capacity) != 0)
            return -1;
        *flags = VD_COMPANION_FS_REQUIRED_DIRECTORY;
        *size = 0u;
        return 0;
    }
    if (strcmp(relative_path, "status.bin") == 0) {
        if (vd_copy_path(relative_path, canonical_path,
                         canonical_capacity) != 0)
            return -1;
        *flags = VD_COMPANION_FS_REQUIRED_REGULAR;
        *size = VD_ENDPOINT_STATUS_SIZE;
        return 0;
    }
    if (strcmp(relative_path, "input.vdtr") == 0 &&
        vd_trace_data(filesystem->service, &trace, &trace_size) == 0) {
        if (vd_copy_path(relative_path, canonical_path,
                         canonical_capacity) != 0)
            return -1;
        *flags = VD_COMPANION_FS_REQUIRED_REGULAR;
        *size = trace_size;
        return 0;
    }
    return -1;
}

int vd_endpoint_fs_list(void* user, const char* canonical_directory,
                        struct vd_companion_file_entry* entries,
                        size_t entry_capacity, size_t* entry_count)
{
    struct vd_endpoint_virtual_fs* filesystem =
        (struct vd_endpoint_virtual_fs*)user;
    const uint8_t* trace;
    size_t trace_size;

    if (filesystem == NULL || filesystem->service == NULL ||
        canonical_directory == NULL || entries == NULL ||
        entry_count == NULL || canonical_directory[0] != '\0' ||
        entry_capacity < 1u)
        return -1;
    memset(entries, 0,
           entry_capacity * sizeof(struct vd_companion_file_entry));
    memcpy(entries[0].name, "status.bin", sizeof("status.bin"));
    entries[0].size = VD_ENDPOINT_STATUS_SIZE;
    entries[0].flags = VD_COMPANION_FS_REQUIRED_REGULAR;
    *entry_count = 1u;
    if (entry_capacity >= 2u &&
        vd_trace_data(filesystem->service, &trace, &trace_size) == 0) {
        memcpy(entries[1].name, "input.vdtr",
               sizeof("input.vdtr"));
        entries[1].size = trace_size;
        entries[1].flags = VD_COMPANION_FS_REQUIRED_REGULAR;
        *entry_count = 2u;
    }
    return 0;
}

int vd_endpoint_fs_read(void* user, const char* canonical_file,
                        uint64_t offset, uint8_t* output,
                        size_t output_capacity, size_t* output_size,
                        uint32_t* flags, uint64_t* file_size)
{
    struct vd_endpoint_virtual_fs* filesystem =
        (struct vd_endpoint_virtual_fs*)user;
    uint8_t status[VD_ENDPOINT_STATUS_SIZE];
    const uint8_t* source = NULL;
    size_t source_size = 0u;
    size_t copy_size;

    if (filesystem == NULL || filesystem->service == NULL ||
        canonical_file == NULL || output == NULL ||
        output_size == NULL || flags == NULL || file_size == NULL)
        return -1;
    if (strcmp(canonical_file, "status.bin") == 0) {
        source_size = vd_status_data(filesystem->service, status);
        source = status;
    } else if (strcmp(canonical_file, "input.vdtr") == 0) {
        if (vd_trace_data(filesystem->service, &source,
                          &source_size) != 0)
            return -1;
    } else {
        return -1;
    }
    if (offset > source_size)
        return -1;
    copy_size = source_size - (size_t)offset;
    if (copy_size > output_capacity)
        copy_size = output_capacity;
    if (copy_size != 0u)
        memcpy(output, source + (size_t)offset, copy_size);
    *output_size = copy_size;
    *flags = VD_COMPANION_FS_REQUIRED_REGULAR;
    *file_size = source_size;
    return 0;
}

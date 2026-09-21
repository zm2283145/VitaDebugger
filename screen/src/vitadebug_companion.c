#include "vitadebug_companion.h"

#include <monocypher.h>

#include <limits.h>
#include <string.h>

#define VD_COMPANION_MAGIC UINT32_C(0x56444350)
#define VD_COMPANION_SERVICE_MAGIC UINT32_C(0x56444353)
#define VD_COMPANION_MAC_DOMAIN "VITADEBUG-COMPANION/RECORD/v1"
#define VD_COMPANION_MAC_DOMAIN_SIZE 29u
#define VD_COMPANION_RESPONSE_BIT UINT16_C(0x8000)
#define VD_COMPANION_HELLO_RESPONSE_SIZE 32u
#define VD_COMPANION_STATUS_RESPONSE_SIZE 72u
#define VD_COMPANION_LIST_ENTRY_SIZE 76u
#define VD_COMPANION_READ_PREFIX_SIZE 16u

_Static_assert(sizeof(VD_COMPANION_MAC_DOMAIN) - 1u ==
                   VD_COMPANION_MAC_DOMAIN_SIZE,
               "companion MAC domain length drifted");

struct vd_companion_record {
    uint16_t type;
    uint16_t status;
    uint32_t payload_size;
    uint64_t sequence;
    uint64_t ttl_ms;
    uint64_t session_id;
    uint64_t generation;
    uint64_t capabilities;
    const uint8_t* payload;
};

static uint16_t vd_read_u16(const uint8_t* input)
{
    return (uint16_t)(((uint16_t)input[0] << 8) | input[1]);
}

static uint32_t vd_read_u32(const uint8_t* input)
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | input[3];
}

static uint64_t vd_read_u64(const uint8_t* input)
{
    return ((uint64_t)vd_read_u32(input) << 32) | vd_read_u32(input + 4);
}

static void vd_write_u16(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void vd_write_u32(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void vd_write_u64(uint8_t* output, uint64_t value)
{
    vd_write_u32(output, (uint32_t)(value >> 32));
    vd_write_u32(output + 4, (uint32_t)value);
}

static int vd_bytes_nonzero(const uint8_t* bytes, size_t size)
{
    size_t index;
    uint8_t combined = 0u;

    if (bytes == NULL)
        return 0;
    for (index = 0u; index < size; ++index)
        combined |= bytes[index];
    return combined != 0u;
}

static int vd_title_valid(const char title_id[VD_SCREEN_TITLE_ID_SIZE + 1u])
{
    size_t index;

    if (title_id == NULL ||
        title_id[VD_SCREEN_TITLE_ID_SIZE] != '\0')
        return 0;
    for (index = 0u; index < VD_SCREEN_TITLE_ID_SIZE; ++index) {
        const char value = title_id[index];
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9')))
            return 0;
    }
    return 1;
}

static int vd_service_valid(const struct vd_companion_service* service)
{
    return service != NULL &&
           service->initialized == VD_COMPANION_SERVICE_MAGIC &&
           service->send != NULL && service->close != NULL;
}

static void vd_record_tag(
    const uint8_t secret[VD_COMPANION_SECRET_SIZE], const uint8_t* header,
    const uint8_t* payload, size_t payload_size,
    uint8_t tag[VD_COMPANION_TAG_SIZE])
{
    crypto_blake2b_ctx context;

    crypto_blake2b_keyed_init(&context, VD_COMPANION_TAG_SIZE, secret,
                              VD_COMPANION_SECRET_SIZE);
    crypto_blake2b_update(
        &context, (const uint8_t*)VD_COMPANION_MAC_DOMAIN,
        VD_COMPANION_MAC_DOMAIN_SIZE);
    crypto_blake2b_update(&context, header, 64u);
    if (payload_size != 0u)
        crypto_blake2b_update(&context, payload, payload_size);
    crypto_blake2b_final(&context, tag);
}

static int vd_path_valid(const char* path, size_t length, int allow_empty)
{
    size_t segment_start = 0u;
    size_t index;

    if (path == NULL || length > VD_COMPANION_MAX_PATH ||
        (!allow_empty && length == 0u))
        return 0;
    if (length == 0u)
        return allow_empty;
    if (path[0] == '/' || path[length - 1u] == '/')
        return 0;
    for (index = 0u; index <= length; ++index) {
        if (index == length || path[index] == '/') {
            const size_t segment_size = index - segment_start;
            if (segment_size == 0u ||
                (segment_size == 1u && path[segment_start] == '.') ||
                (segment_size == 2u && path[segment_start] == '.' &&
                 path[segment_start + 1u] == '.'))
                return 0;
            segment_start = index + 1u;
        } else {
            const unsigned char value = (unsigned char)path[index];
            if (value < 0x21u || value > 0x7eu || value == '\\' ||
                value == ':' || value == '\0')
                return 0;
        }
    }
    return 1;
}

static int vd_name_valid(const char name[VD_COMPANION_MAX_NAME + 1u])
{
    size_t length = 0u;

    while (length <= VD_COMPANION_MAX_NAME && name[length] != '\0')
        ++length;
    return length != 0u && length <= VD_COMPANION_MAX_NAME &&
           vd_path_valid(name, length, 0) &&
           memchr(name, '/', length) == NULL;
}

static int vd_release_input(struct vd_companion_service* service)
{
    const struct vd_companion_input neutral = {0};

    if (service->input_active == 0u &&
        service->input_cleanup_pending == 0u)
        return VD_COMPANION_OK;
    if (service->apply_input(service->input_user, &neutral) != 0) {
        service->input_cleanup_pending = 1u;
        service->state = VD_COMPANION_STATE_FAILED;
        service->last_error = VD_COMPANION_ERROR_INPUT;
        return VD_COMPANION_ERROR_INPUT;
    }
    service->input_active = 0u;
    service->input_cleanup_pending = 0u;
    service->input_lease_expires_ms = 0u;
    return VD_COMPANION_OK;
}

static void vd_stop_trace(struct vd_companion_service* service,
                          uint32_t end_reason)
{
    if (service->trace_initialized != 0u)
        (void)vd_input_trace_abort(&service->trace, end_reason);
}

static int vd_close_screen_transport(
    struct vd_companion_service* service)
{
    if (service->screen_connected == 0u)
        return VD_COMPANION_OK;
    if (service->screen_close(service->screen_transport_user) != 0)
        return VD_COMPANION_ERROR_IO;
    service->screen_connected = 0u;
    return VD_COMPANION_OK;
}

static int vd_abort_session(struct vd_companion_service* service, int error)
{
    int release_result = vd_release_input(service);

    vd_stop_trace(service, VD_INPUT_TRACE_END_ABORTED);
    service->negotiated_capabilities = 0u;
    service->total_file_bytes = 0u;
    if (service->screen_initialized != 0u)
        (void)vd_screen_stream_close(&service->screen);
    if (vd_close_screen_transport(service) != VD_COMPANION_OK &&
        release_result == VD_COMPANION_OK)
        release_result = VD_COMPANION_ERROR_IO;
    if (service->bound != 0u) {
        service->bound = 0u;
        if (service->close(service->transport_user) != 0 &&
            release_result == VD_COMPANION_OK)
            release_result = VD_COMPANION_ERROR_IO;
    }
    crypto_wipe(service->secret, sizeof(service->secret));
    service->state = VD_COMPANION_STATE_FAILED;
    service->last_error =
        release_result != VD_COMPANION_OK ? release_result : error;
    return service->last_error;
}

static int vd_send_response(struct vd_companion_service* service,
                            const struct vd_companion_record* request,
                            int status, const uint8_t* payload,
                            size_t payload_size)
{
    uint8_t output[VD_COMPANION_MAX_RECORD];
    size_t output_size = 0u;
    int result;

    if (service->now_ms(service->clock_user) >
        service->request_expires_ms)
        return vd_abort_session(
            service, VD_COMPANION_ERROR_DEADLINE);
    result = vd_companion_encode_record(
        service->secret,
        (uint16_t)(request->type | VD_COMPANION_RESPONSE_BIT),
        (uint16_t)(status == VD_COMPANION_OK ? 0u : (uint16_t)(-status)),
        request->sequence, request->ttl_ms, service->session_id,
        service->process_generation, service->negotiated_capabilities,
        payload, payload_size, output, sizeof(output), &output_size);
    if (result != VD_COMPANION_OK)
        return result;
    if (service->send(service->transport_user, output, output_size) != 0)
        return vd_abort_session(service, VD_COMPANION_ERROR_IO);
    return status;
}

static int vd_decode_record(struct vd_companion_service* service,
                            const uint8_t* record, size_t record_size,
                            struct vd_companion_record* decoded)
{
    uint8_t tag[VD_COMPANION_TAG_SIZE];
    uint32_t payload_size;

    if (record == NULL || decoded == NULL ||
        record_size < VD_COMPANION_HEADER_SIZE ||
        record_size > VD_COMPANION_MAX_RECORD ||
        vd_read_u32(record) != VD_COMPANION_MAGIC ||
        vd_read_u16(record + 4) != VD_COMPANION_PROTOCOL_VERSION ||
        vd_read_u16(record + 6) != VD_COMPANION_HEADER_SIZE)
        return VD_COMPANION_ERROR_PROTOCOL;
    payload_size = vd_read_u32(record + 12);
    if (payload_size > VD_COMPANION_MAX_PAYLOAD ||
        record_size != VD_COMPANION_HEADER_SIZE + (size_t)payload_size ||
        vd_read_u16(record + 10) != 0u ||
        vd_read_u64(record + 56) != 0u)
        return VD_COMPANION_ERROR_PROTOCOL;
    vd_record_tag(service->secret, record, record + VD_COMPANION_HEADER_SIZE,
                  payload_size, tag);
    if (crypto_verify16(tag, record + 64) != 0) {
        crypto_wipe(tag, sizeof(tag));
        return VD_COMPANION_ERROR_AUTH;
    }
    crypto_wipe(tag, sizeof(tag));
    decoded->type = vd_read_u16(record + 8);
    decoded->status = 0u;
    decoded->payload_size = payload_size;
    decoded->sequence = vd_read_u64(record + 16);
    decoded->ttl_ms = vd_read_u64(record + 24);
    decoded->session_id = vd_read_u64(record + 32);
    decoded->generation = vd_read_u64(record + 40);
    decoded->capabilities = vd_read_u64(record + 48);
    decoded->payload = record + VD_COMPANION_HEADER_SIZE;
    return VD_COMPANION_OK;
}

static int vd_handle_hello(struct vd_companion_service* service,
                           const struct vd_companion_record* request)
{
    uint8_t payload[VD_COMPANION_HELLO_RESPONSE_SIZE] = {0};

    if (service->state != VD_COMPANION_STATE_LISTENING ||
        request->sequence != 1u || request->payload_size != 0u)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_STATE, NULL, 0u);
    service->negotiated_capabilities =
        request->capabilities & service->enabled_capabilities;
    vd_write_u64(payload, service->enabled_capabilities);
    vd_write_u64(payload + 8, service->negotiated_capabilities);
    vd_write_u32(payload + 16, VD_COMPANION_MAX_PAYLOAD);
    vd_write_u32(payload + 20, VD_COMPANION_MAX_READ_BYTES);
    vd_write_u32(payload + 24, VD_COMPANION_MAX_LIST_ENTRIES);
    vd_write_u32(payload + 28, VD_COMPANION_MAX_INPUT_LEASE_MS);
    service->state = VD_COMPANION_STATE_PAIRED;
    return vd_send_response(service, request, VD_COMPANION_OK, payload,
                            sizeof(payload));
}

static int vd_handle_status(struct vd_companion_service* service,
                            const struct vd_companion_record* request)
{
    uint8_t payload[VD_COMPANION_STATUS_RESPONSE_SIZE] = {0};
    struct vd_input_trace_info trace_info;

    if ((service->negotiated_capabilities & VD_COMPANION_CAP_STATUS) == 0u)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_CAPABILITY, NULL, 0u);
    if (request->payload_size != 0u)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_PROTOCOL, NULL, 0u);
    memcpy(payload, service->title_id, VD_SCREEN_TITLE_ID_SIZE);
    vd_write_u32(payload + 12, service->process_id);
    vd_write_u64(payload + 16, service->process_generation);
    vd_write_u64(payload + 24, service->session_id);
    vd_write_u64(payload + 32, service->negotiated_capabilities);
    if (service->trace_initialized != 0u &&
        vd_input_trace_get_info(&service->trace, &trace_info) ==
            VD_INPUT_TRACE_OK) {
        vd_write_u32(payload + 40, trace_info.state);
        vd_write_u32(payload + 44, trace_info.event_count);
        vd_write_u64(payload + 48, trace_info.duration_us);
        vd_write_u64(payload + 56,
                     trace_info.max_scheduling_drift_us);
    }
    vd_write_u32(payload + 64, service->input_cleanup_pending);
    vd_write_u32(payload + 68,
                 service->last_error < 0
                     ? (uint32_t)(-service->last_error)
                     : (uint32_t)service->last_error);
    return vd_send_response(service, request, VD_COMPANION_OK, payload,
                            sizeof(payload));
}

static int vd_handle_input(struct vd_companion_service* service,
                           const struct vd_companion_record* request,
                           uint64_t now_ms)
{
    struct vd_companion_input input;
    uint32_t lease_ms;

    if ((service->negotiated_capabilities &
         VD_COMPANION_CAP_APP_INPUT) == 0u)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_CAPABILITY, NULL, 0u);
    if (request->payload_size != 36u ||
        request->payload[13] != 0u ||
        request->payload[14] != 0u ||
        request->payload[15] != 0u)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_PROTOCOL, NULL, 0u);
    lease_ms = vd_read_u32(request->payload + 32);
    if (lease_ms < VD_COMPANION_MIN_INPUT_LEASE_MS ||
        lease_ms > VD_COMPANION_MAX_INPUT_LEASE_MS ||
        now_ms > UINT64_MAX - lease_ms)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_LIMIT, NULL, 0u);
    input.buttons = vd_read_u32(request->payload);
    input.left_x = (int16_t)vd_read_u16(request->payload + 4);
    input.left_y = (int16_t)vd_read_u16(request->payload + 6);
    input.right_x = (int16_t)vd_read_u16(request->payload + 8);
    input.right_y = (int16_t)vd_read_u16(request->payload + 10);
    input.touch_count = request->payload[12];
    input.touches[0].id = vd_read_u16(request->payload + 16);
    input.touches[0].x = vd_read_u16(request->payload + 18);
    input.touches[0].y = vd_read_u16(request->payload + 20);
    input.touches[0].force = vd_read_u16(request->payload + 22);
    input.touches[1].id = vd_read_u16(request->payload + 24);
    input.touches[1].x = vd_read_u16(request->payload + 26);
    input.touches[1].y = vd_read_u16(request->payload + 28);
    input.touches[1].force = vd_read_u16(request->payload + 30);
    if ((input.buttons & ~VD_INPUT_BUTTON_ALLOWED_MASK) != 0u ||
        input.touch_count > 2u ||
        (input.touch_count == 0u &&
         (vd_read_u64(request->payload + 16) != 0u ||
          vd_read_u64(request->payload + 24) != 0u)) ||
        (input.touch_count == 1u &&
         vd_read_u64(request->payload + 24) != 0u))
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_INPUT, NULL, 0u);
    if (service->trace_initialized != 0u &&
        service->trace.state == VD_INPUT_TRACE_STATE_PLAYING)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_STATE, NULL, 0u);
    service->input_active = 1u;
    service->input_lease_expires_ms = now_ms + lease_ms;
    if (service->apply_input(service->input_user, &input) != 0) {
        return vd_abort_session(service, VD_COMPANION_ERROR_INPUT);
    }
    return vd_send_response(service, request, VD_COMPANION_OK, NULL, 0u);
}

static int vd_extract_path(const struct vd_companion_record* request,
                           size_t suffix_size, int allow_empty, char* path,
                           size_t* path_size)
{
    uint16_t length;

    if (request->payload_size < 2u + suffix_size)
        return VD_COMPANION_ERROR_PROTOCOL;
    length = vd_read_u16(request->payload);
    if ((size_t)length + 2u + suffix_size != request->payload_size ||
        !vd_path_valid((const char*)request->payload + 2u, length,
                       allow_empty))
        return VD_COMPANION_ERROR_PATH;
    memcpy(path, request->payload + 2u, length);
    path[length] = '\0';
    *path_size = length;
    return VD_COMPANION_OK;
}

static int vd_resolve_path(struct vd_companion_service* service,
                           const char* path, uint32_t expected_flags,
                           char* canonical, uint64_t* size)
{
    uint32_t flags = 0u;
    int result = service->filesystem.resolve(
        service->filesystem.user, path, canonical,
        VD_COMPANION_MAX_PATH + 1u, &flags, size);

    if (result != 0)
        return VD_COMPANION_ERROR_FILESYSTEM;
    canonical[VD_COMPANION_MAX_PATH] = '\0';
    if (strcmp(path, canonical) != 0 ||
        (flags & expected_flags) != expected_flags ||
        (flags & (VD_COMPANION_FS_REGULAR |
                  VD_COMPANION_FS_DIRECTORY)) !=
            (expected_flags & (VD_COMPANION_FS_REGULAR |
                               VD_COMPANION_FS_DIRECTORY)))
        return VD_COMPANION_ERROR_PATH;
    return VD_COMPANION_OK;
}

static int vd_handle_list(struct vd_companion_service* service,
                          const struct vd_companion_record* request)
{
    char path[VD_COMPANION_MAX_PATH + 1u];
    char canonical[VD_COMPANION_MAX_PATH + 1u] = {0};
    struct vd_companion_file_entry entries[VD_COMPANION_MAX_LIST_ENTRIES];
    uint8_t response[2u + VD_COMPANION_MAX_LIST_ENTRIES *
                              VD_COMPANION_LIST_ENTRY_SIZE] = {0};
    uint64_t ignored_size = 0u;
    size_t path_size;
    size_t entry_count = 0u;
    size_t index;
    int result;

    if ((service->negotiated_capabilities &
         VD_COMPANION_CAP_DEBUG_FILES) == 0u)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_CAPABILITY, NULL, 0u);
    result = vd_extract_path(request, 0u, 1, path, &path_size);
    (void)path_size;
    if (result != VD_COMPANION_OK)
        return vd_send_response(service, request, result, NULL, 0u);
    result = vd_resolve_path(service, path,
                             VD_COMPANION_FS_REQUIRED_DIRECTORY,
                             canonical, &ignored_size);
    if (result != VD_COMPANION_OK)
        return vd_send_response(service, request, result, NULL, 0u);
    memset(entries, 0, sizeof(entries));
    if (service->filesystem.list(
            service->filesystem.user, canonical, entries,
            VD_COMPANION_MAX_LIST_ENTRIES, &entry_count) != 0)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_FILESYSTEM, NULL, 0u);
    if (entry_count > VD_COMPANION_MAX_LIST_ENTRIES)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_LIMIT, NULL, 0u);
    vd_write_u16(response, (uint16_t)entry_count);
    for (index = 0u; index < entry_count; ++index) {
        uint8_t* output =
            response + 2u + index * VD_COMPANION_LIST_ENTRY_SIZE;
        size_t name_size;
        const uint32_t type_flags =
            entries[index].flags &
            (VD_COMPANION_FS_REGULAR | VD_COMPANION_FS_DIRECTORY);
        if (!vd_name_valid(entries[index].name) ||
            (entries[index].flags &
             (VD_COMPANION_FS_INSIDE_ROOT |
              VD_COMPANION_FS_NO_SYMLINKS)) !=
                (VD_COMPANION_FS_INSIDE_ROOT |
                 VD_COMPANION_FS_NO_SYMLINKS) ||
            (type_flags != VD_COMPANION_FS_REGULAR &&
             type_flags != VD_COMPANION_FS_DIRECTORY))
            return vd_send_response(service, request,
                                    VD_COMPANION_ERROR_PATH, NULL, 0u);
        name_size = strlen(entries[index].name);
        output[0] = (uint8_t)name_size;
        output[1] =
            type_flags == VD_COMPANION_FS_REGULAR ? 1u : 2u;
        vd_write_u64(output + 4, entries[index].size);
        memcpy(output + 12, entries[index].name, name_size);
    }
    return vd_send_response(
        service, request, VD_COMPANION_OK, response,
        2u + entry_count * VD_COMPANION_LIST_ENTRY_SIZE);
}

static int vd_handle_read(struct vd_companion_service* service,
                          const struct vd_companion_record* request)
{
    char path[VD_COMPANION_MAX_PATH + 1u];
    char canonical[VD_COMPANION_MAX_PATH + 1u] = {0};
    uint8_t response[VD_COMPANION_READ_PREFIX_SIZE +
                     VD_COMPANION_MAX_READ_BYTES] = {0};
    uint64_t resolved_size = 0u;
    uint64_t file_size = 0u;
    uint64_t offset;
    uint32_t requested;
    uint32_t flags = 0u;
    size_t path_size;
    size_t output_size = 0u;
    int result;

    if ((service->negotiated_capabilities &
         VD_COMPANION_CAP_DEBUG_FILES) == 0u)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_CAPABILITY, NULL, 0u);
    result = vd_extract_path(request, 12u, 0, path, &path_size);
    if (result != VD_COMPANION_OK)
        return vd_send_response(service, request, result, NULL, 0u);
    offset = vd_read_u64(request->payload + 2u + path_size);
    requested = vd_read_u32(request->payload + 10u + path_size);
    if (requested == 0u || requested > VD_COMPANION_MAX_READ_BYTES ||
        service->total_file_bytes >
            VD_COMPANION_MAX_TOTAL_READ_BYTES - requested)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_LIMIT, NULL, 0u);
    result = vd_resolve_path(service, path,
                             VD_COMPANION_FS_REQUIRED_REGULAR,
                             canonical, &resolved_size);
    if (result != VD_COMPANION_OK)
        return vd_send_response(service, request, result, NULL, 0u);
    if (service->filesystem.read_regular(
            service->filesystem.user, canonical, offset,
            response + VD_COMPANION_READ_PREFIX_SIZE, requested,
            &output_size, &flags, &file_size) != 0)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_FILESYSTEM, NULL, 0u);
    if (output_size > requested || file_size != resolved_size ||
        offset > file_size || output_size > file_size - offset ||
        (flags & VD_COMPANION_FS_REQUIRED_REGULAR) !=
            VD_COMPANION_FS_REQUIRED_REGULAR ||
        (flags & VD_COMPANION_FS_DIRECTORY) != 0u)
        return vd_send_response(service, request,
                                VD_COMPANION_ERROR_PATH, NULL, 0u);
    service->total_file_bytes += output_size;
    vd_write_u64(response, offset);
    vd_write_u32(response + 8, (uint32_t)output_size);
    vd_write_u32(response + 12,
                 output_size == file_size - offset ? 1u : 0u);
    return vd_send_response(service, request, VD_COMPANION_OK, response,
                            VD_COMPANION_READ_PREFIX_SIZE + output_size);
}

void vd_companion_config_init(struct vd_companion_config* config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->network_scope = VD_COMPANION_NETWORK_LOOPBACK;
    config->screen_port = VD_COMPANION_DEFAULT_SCREEN_PORT;
    config->control_port = VD_COMPANION_DEFAULT_CONTROL_PORT;
    config->trace_max_events = VD_INPUT_TRACE_MAX_EVENTS;
    config->trace_max_duration_us = VD_INPUT_TRACE_MAX_DURATION_US;
    config->trace_max_scheduling_drift_us =
        VD_INPUT_TRACE_DEFAULT_MAX_DRIFT_US;
}

int vd_companion_validate_screen_port(uint16_t port)
{
    if (port < VD_COMPANION_MIN_PORT || port > VD_COMPANION_MAX_PORT ||
        (port >= 18194u && port <= 18196u) || port == 18198u)
        return VD_COMPANION_ERROR_PORT;
    return VD_COMPANION_OK;
}

int vd_companion_validate_control_port(uint16_t port)
{
    if (port < VD_COMPANION_MIN_PORT || port > VD_COMPANION_MAX_PORT ||
        (port >= 18194u && port <= 18197u))
        return VD_COMPANION_ERROR_PORT;
    return VD_COMPANION_OK;
}

int vd_companion_service_init(struct vd_companion_service* service,
                              const struct vd_companion_config* config)
{
    struct vd_screen_config screen_config;
    struct vd_input_trace_config trace_config;
    size_t root_size = 0u;
    int result;

    if (service == NULL || config == NULL)
        return VD_COMPANION_ERROR_INVALID_ARGUMENT;
    if (service->initialized == VD_COMPANION_SERVICE_MAGIC &&
        ((service->state != VD_COMPANION_STATE_CLOSED &&
          service->state != VD_COMPANION_STATE_FAILED) ||
         service->input_active != 0u ||
         service->input_cleanup_pending != 0u ||
         service->screen_connected != 0u))
        return VD_COMPANION_ERROR_STATE;
    if (config->explicit_consent != VD_COMPANION_EXPLICIT_CONSENT)
        return VD_COMPANION_ERROR_DISABLED;
    if (!vd_bytes_nonzero(config->secret, sizeof(config->secret)) ||
        !vd_title_valid(config->title_id) || config->process_id == 0u ||
        config->process_generation == 0u || config->session_id == 0u ||
        config->bind == NULL || config->send == NULL ||
        config->close == NULL || config->now_ms == NULL ||
        config->enabled_capabilities == 0u ||
        (config->enabled_capabilities & ~VD_COMPANION_CAP_SUPPORTED) != 0u ||
        (config->enabled_capabilities & VD_COMPANION_CAP_STATUS) == 0u)
        return VD_COMPANION_ERROR_INVALID_ARGUMENT;
    if (vd_companion_validate_screen_port(config->screen_port) !=
            VD_COMPANION_OK ||
        vd_companion_validate_control_port(config->control_port) !=
            VD_COMPANION_OK ||
        config->screen_port == config->control_port)
        return VD_COMPANION_ERROR_PORT;
    if (config->network_scope != VD_COMPANION_NETWORK_LOOPBACK &&
        config->network_scope != VD_COMPANION_NETWORK_PRIVATE_LAN)
        return VD_COMPANION_ERROR_INVALID_ARGUMENT;
    if (config->network_scope == VD_COMPANION_NETWORK_PRIVATE_LAN &&
        config->lan_consent != VD_COMPANION_LAN_CONSENT)
        return VD_COMPANION_ERROR_DISABLED;
    if ((config->enabled_capabilities & VD_COMPANION_CAP_APP_INPUT) != 0u &&
        (config->mutation_consent != VD_COMPANION_MUTATION_CONSENT ||
         config->apply_input == NULL))
        return VD_COMPANION_ERROR_DISABLED;
    if ((config->enabled_capabilities &
         VD_COMPANION_CAP_INPUT_RECORD) != 0u &&
        config->record_consent !=
            VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT)
        return VD_COMPANION_ERROR_DISABLED;
    if ((config->enabled_capabilities &
         VD_COMPANION_CAP_INPUT_PLAYBACK) != 0u &&
        (config->playback_consent !=
             VD_INPUT_TRACE_EXPLICIT_PLAYBACK_CONSENT ||
         config->mutation_consent != VD_COMPANION_MUTATION_CONSENT ||
         config->apply_input == NULL))
        return VD_COMPANION_ERROR_DISABLED;
    if ((config->enabled_capabilities &
         (VD_COMPANION_CAP_INPUT_RECORD |
          VD_COMPANION_CAP_INPUT_PLAYBACK)) != 0u &&
        (config->trace_buffer == NULL ||
         config->trace_buffer_capacity <
             VD_INPUT_TRACE_HEADER_SIZE + VD_INPUT_TRACE_EVENT_SIZE))
        return VD_COMPANION_ERROR_INVALID_ARGUMENT;
    if ((config->enabled_capabilities &
         VD_COMPANION_CAP_DEBUG_FILES) != 0u) {
        if (config->debug_root == NULL ||
            config->filesystem.resolve == NULL ||
            config->filesystem.list == NULL ||
            config->filesystem.read_regular == NULL)
            return VD_COMPANION_ERROR_INVALID_ARGUMENT;
        while (root_size <= VD_COMPANION_MAX_PATH &&
               config->debug_root[root_size] != '\0')
            ++root_size;
        if (root_size == 0u || root_size > VD_COMPANION_MAX_PATH)
            return VD_COMPANION_ERROR_INVALID_ARGUMENT;
    }
    if ((config->enabled_capabilities & VD_COMPANION_CAP_SCREEN) != 0u &&
        (config->screen == NULL || config->screen->connect == NULL ||
         config->screen->close == NULL ||
         config->screen->transport_user == NULL ||
         config->screen->write == NULL))
        return VD_COMPANION_ERROR_INVALID_ARGUMENT;
    if ((config->enabled_capabilities & VD_COMPANION_CAP_SCREEN) != 0u &&
        crypto_verify32(config->secret,
                        config->screen->auth_token) == 0)
        return VD_COMPANION_ERROR_SECRET_REUSE;

    memset(service, 0, sizeof(*service));
    memcpy(service->secret, config->secret, sizeof(service->secret));
    memcpy(service->title_id, config->title_id, sizeof(service->title_id));
    if (root_size != 0u)
        memcpy(service->debug_root, config->debug_root, root_size + 1u);
    service->process_id = config->process_id;
    service->process_generation = config->process_generation;
    service->session_id = config->session_id;
    service->network_scope = config->network_scope;
    service->screen_port = config->screen_port;
    service->control_port = config->control_port;
    service->enabled_capabilities = config->enabled_capabilities;
    service->send = config->send;
    service->close = config->close;
    service->transport_user = config->transport_user;
    service->now_ms = config->now_ms;
    service->clock_user = config->clock_user;
    service->apply_input = config->apply_input;
    service->input_user = config->input_user;
    service->filesystem = config->filesystem;
    service->record_consent = config->record_consent;
    service->playback_consent = config->playback_consent;
    service->state = VD_COMPANION_STATE_LISTENING;
    service->initialized = VD_COMPANION_SERVICE_MAGIC;

    if ((config->enabled_capabilities & VD_COMPANION_CAP_SCREEN) != 0u) {
        vd_screen_config_init(&screen_config);
        screen_config.explicit_consent = VD_SCREEN_EXPLICIT_CONSENT;
        screen_config.write = config->screen->write;
        screen_config.write_user = config->screen->write_user;
        screen_config.sources = config->screen->sources;
        screen_config.source_count = config->screen->source_count;
        memcpy(screen_config.auth_token, config->screen->auth_token,
               sizeof(screen_config.auth_token));
        memcpy(screen_config.title_id, config->title_id,
               sizeof(screen_config.title_id));
        screen_config.process_id = config->process_id;
        screen_config.process_generation = config->process_generation;
        screen_config.session_id = config->session_id;
        if (config->screen->max_width != 0u)
            screen_config.max_width = config->screen->max_width;
        if (config->screen->max_height != 0u)
            screen_config.max_height = config->screen->max_height;
        if (config->screen->max_payload_bytes != 0u)
            screen_config.max_payload_bytes =
                config->screen->max_payload_bytes;
        if (config->screen->min_frame_interval_us != 0u)
            screen_config.min_frame_interval_us =
                config->screen->min_frame_interval_us;
        result = vd_screen_stream_init(&service->screen, &screen_config);
        crypto_wipe(&screen_config, sizeof(screen_config));
        if (result != VD_SCREEN_OK) {
            crypto_wipe(service, sizeof(*service));
            return VD_COMPANION_ERROR_INVALID_ARGUMENT;
        }
        service->screen_initialized = 1u;
        service->screen_close = config->screen->close;
        service->screen_transport_user =
            config->screen->transport_user;
    }

    if ((config->enabled_capabilities &
         (VD_COMPANION_CAP_INPUT_RECORD |
          VD_COMPANION_CAP_INPUT_PLAYBACK)) != 0u) {
        vd_input_trace_config_init(&trace_config);
        trace_config.buffer = config->trace_buffer;
        trace_config.buffer_capacity = config->trace_buffer_capacity;
        trace_config.max_events = config->trace_max_events;
        trace_config.max_duration_us = config->trace_max_duration_us;
        trace_config.max_scheduling_drift_us =
            config->trace_max_scheduling_drift_us;
        memcpy(trace_config.identity.title_id, config->title_id,
               sizeof(trace_config.identity.title_id));
        trace_config.identity.process_id = config->process_id;
        trace_config.identity.process_generation =
            config->process_generation;
        trace_config.identity.session_id = config->session_id;
        result = vd_input_trace_init(&service->trace, &trace_config);
        if (result != VD_INPUT_TRACE_OK) {
            if (service->screen_initialized != 0u)
                (void)vd_screen_stream_close(&service->screen);
            crypto_wipe(service, sizeof(*service));
            return VD_COMPANION_ERROR_INVALID_ARGUMENT;
        }
        service->trace_initialized = 1u;
    }

    if (config->bind(config->transport_user, config->network_scope,
                     config->control_port) != 0) {
        if (service->screen_initialized != 0u)
            (void)vd_screen_stream_close(&service->screen);
        crypto_wipe(service->secret, sizeof(service->secret));
        service->state = VD_COMPANION_STATE_FAILED;
        return VD_COMPANION_ERROR_BIND;
    }
    service->bound = 1u;
    if (service->screen_initialized != 0u) {
        service->screen_connected = 1u;
    }
    if (service->screen_initialized != 0u &&
        config->screen->connect(config->screen->transport_user,
                                config->network_scope,
                                config->screen_port) != 0) {
        int close_result;
        service->bound = 0u;
        (void)service->close(service->transport_user);
        (void)vd_screen_stream_close(&service->screen);
        close_result = vd_close_screen_transport(service);
        crypto_wipe(service->secret, sizeof(service->secret));
        service->state = VD_COMPANION_STATE_FAILED;
        service->last_error =
            close_result == VD_COMPANION_OK
                ? VD_COMPANION_ERROR_CONNECT
                : close_result;
        return service->last_error;
    }
    return VD_COMPANION_OK;
}

int vd_companion_service_process(struct vd_companion_service* service,
                                 const uint8_t* record,
                                 size_t record_size)
{
    struct vd_companion_record request;
    uint64_t now_ms;
    int result;

    if (!vd_service_valid(service) ||
        (service->state != VD_COMPANION_STATE_LISTENING &&
         service->state != VD_COMPANION_STATE_PAIRED))
        return VD_COMPANION_ERROR_STATE;
    result = vd_decode_record(service, record, record_size, &request);
    if (result != VD_COMPANION_OK)
        return vd_abort_session(service, result);
    now_ms = service->now_ms(service->clock_user);
    if (request.type == 0u ||
        (request.type & VD_COMPANION_RESPONSE_BIT) != 0u ||
        request.session_id != service->session_id ||
        request.generation != service->process_generation)
        return vd_abort_session(service, VD_COMPANION_ERROR_PROTOCOL);
    if (request.sequence == 0u ||
        request.sequence != service->last_sequence + 1u)
        return vd_abort_session(service, VD_COMPANION_ERROR_REPLAY);
    if (request.ttl_ms == 0u ||
        request.ttl_ms > VD_COMPANION_MAX_DEADLINE_MS ||
        now_ms > UINT64_MAX - request.ttl_ms)
        return vd_abort_session(service, VD_COMPANION_ERROR_DEADLINE);
    service->request_received_ms = now_ms;
    service->request_expires_ms = now_ms + request.ttl_ms;
    if (service->state == VD_COMPANION_STATE_PAIRED &&
        request.capabilities != service->negotiated_capabilities)
        return vd_abort_session(service, VD_COMPANION_ERROR_CAPABILITY);
    service->last_sequence = request.sequence;

    switch (request.type) {
        case VD_COMPANION_MESSAGE_HELLO:
            return vd_handle_hello(service, &request);
        case VD_COMPANION_MESSAGE_STATUS:
            if (service->state != VD_COMPANION_STATE_PAIRED)
                return vd_send_response(service, &request,
                                        VD_COMPANION_ERROR_STATE, NULL, 0u);
            return vd_handle_status(service, &request);
        case VD_COMPANION_MESSAGE_INPUT:
            if (service->state != VD_COMPANION_STATE_PAIRED)
                return vd_send_response(service, &request,
                                        VD_COMPANION_ERROR_STATE, NULL, 0u);
            return vd_handle_input(service, &request, now_ms);
        case VD_COMPANION_MESSAGE_FILE_LIST:
            if (service->state != VD_COMPANION_STATE_PAIRED)
                return vd_send_response(service, &request,
                                        VD_COMPANION_ERROR_STATE, NULL, 0u);
            return vd_handle_list(service, &request);
        case VD_COMPANION_MESSAGE_FILE_READ:
            if (service->state != VD_COMPANION_STATE_PAIRED)
                return vd_send_response(service, &request,
                                        VD_COMPANION_ERROR_STATE, NULL, 0u);
            return vd_handle_read(service, &request);
        case VD_COMPANION_MESSAGE_TITLE_INVENTORY:
        case VD_COMPANION_MESSAGE_TITLE_LAUNCH:
            return vd_send_response(service, &request,
                                    VD_COMPANION_ERROR_UNSUPPORTED, NULL, 0u);
        default:
            return vd_send_response(service, &request,
                                    VD_COMPANION_ERROR_PROTOCOL, NULL, 0u);
    }
}

int vd_companion_service_tick(struct vd_companion_service* service,
                              uint64_t now_ms)
{
    if (!vd_service_valid(service))
        return VD_COMPANION_ERROR_STATE;
    if (service->input_cleanup_pending != 0u)
        return vd_release_input(service);
    if (service->state == VD_COMPANION_STATE_FAILED)
        return VD_COMPANION_ERROR_STATE;
    if (service->input_active != 0u &&
        (service->trace_initialized == 0u ||
         service->trace.state != VD_INPUT_TRACE_STATE_PLAYING) &&
        now_ms >= service->input_lease_expires_ms)
        return vd_release_input(service);
    return VD_COMPANION_OK;
}

int vd_companion_service_disconnect(struct vd_companion_service* service)
{
    if (!vd_service_valid(service))
        return VD_COMPANION_ERROR_STATE;
    vd_stop_trace(service, VD_INPUT_TRACE_END_DISCONNECT);
    return vd_companion_service_close(service);
}

int vd_companion_service_identity_changed(
    struct vd_companion_service* service)
{
    if (!vd_service_valid(service))
        return VD_COMPANION_ERROR_STATE;
    vd_stop_trace(service, VD_INPUT_TRACE_END_IDENTITY_CHANGE);
    return vd_abort_session(service, VD_COMPANION_ERROR_STATE);
}

int vd_companion_service_retry_neutral(
    struct vd_companion_service* service)
{
    if (!vd_service_valid(service))
        return VD_COMPANION_ERROR_STATE;
    if (service->input_cleanup_pending == 0u)
        return VD_COMPANION_OK;
    return vd_release_input(service);
}

int vd_companion_service_get_status(
    const struct vd_companion_service* service,
    struct vd_companion_status* status)
{
    struct vd_input_trace_info trace_info;

    if (!vd_service_valid(service) || status == NULL)
        return VD_COMPANION_ERROR_INVALID_ARGUMENT;
    memset(status, 0, sizeof(*status));
    status->state = service->state;
    status->input_active = service->input_active;
    status->input_cleanup_pending =
        service->input_cleanup_pending;
    status->last_error = service->last_error;
    if (service->trace_initialized != 0u &&
        vd_input_trace_get_info(&service->trace, &trace_info) ==
            VD_INPUT_TRACE_OK) {
        status->trace_state = trace_info.state;
        status->trace_end_reason = trace_info.end_reason;
        status->trace_event_count = trace_info.event_count;
        status->trace_duration_us = trace_info.duration_us;
        status->trace_max_scheduling_drift_us =
            trace_info.max_scheduling_drift_us;
    }
    return VD_COMPANION_OK;
}

int vd_companion_service_close(struct vd_companion_service* service)
{
    int primary = VD_COMPANION_OK;

    if (!vd_service_valid(service))
        return VD_COMPANION_ERROR_STATE;
    if (service->state == VD_COMPANION_STATE_CLOSED)
        return VD_COMPANION_OK;
    vd_stop_trace(service, VD_INPUT_TRACE_END_SHUTDOWN);
    if (vd_release_input(service) != VD_COMPANION_OK)
        primary = VD_COMPANION_ERROR_INPUT;
    if (service->screen_initialized != 0u)
        (void)vd_screen_stream_close(&service->screen);
    if (vd_close_screen_transport(service) != VD_COMPANION_OK &&
        primary == VD_COMPANION_OK)
        primary = VD_COMPANION_ERROR_IO;
    if (service->bound != 0u) {
        service->bound = 0u;
        if (service->close(service->transport_user) != 0 &&
            primary == VD_COMPANION_OK)
            primary = VD_COMPANION_ERROR_IO;
    }
    crypto_wipe(service->secret, sizeof(service->secret));
    if (primary == VD_COMPANION_OK) {
        service->state = VD_COMPANION_STATE_CLOSED;
        service->last_error = VD_COMPANION_OK;
    } else {
        service->state = VD_COMPANION_STATE_FAILED;
        service->last_error = primary;
    }
    return primary;
}

int vd_companion_screen_begin(struct vd_companion_service* service)
{
    if (!vd_service_valid(service) || service->screen_initialized == 0u)
        return VD_COMPANION_ERROR_CAPABILITY;
    return vd_screen_stream_begin(&service->screen) == VD_SCREEN_OK
               ? VD_COMPANION_OK
               : VD_COMPANION_ERROR_IO;
}

int vd_companion_submit_displayed_frame(
    struct vd_companion_service* service,
    const struct vd_screen_frame* frame)
{
    int result;

    if (!vd_service_valid(service) || service->screen_initialized == 0u)
        return VD_COMPANION_ERROR_CAPABILITY;
    result = vd_screen_submit_displayed_frame(&service->screen, frame);
    if (result == VD_SCREEN_OK)
        return VD_COMPANION_OK;
    if (result == VD_SCREEN_DROPPED)
        return VD_SCREEN_DROPPED;
    return VD_COMPANION_ERROR_IO;
}

static void vd_companion_to_trace_input(
    const struct vd_companion_input* input,
    struct vd_input_state* trace_input)
{
    memset(trace_input, 0, sizeof(*trace_input));
    trace_input->buttons = input->buttons;
    trace_input->left_x = input->left_x;
    trace_input->left_y = input->left_y;
    trace_input->right_x = input->right_x;
    trace_input->right_y = input->right_y;
    trace_input->touch_count = input->touch_count;
    memcpy(trace_input->touches, input->touches,
           sizeof(trace_input->touches));
}

static int vd_companion_trace_apply(
    void* user, const struct vd_input_state* trace_input)
{
    struct vd_companion_service* service =
        (struct vd_companion_service*)user;
    struct vd_companion_input input;

    memset(&input, 0, sizeof(input));
    input.buttons = trace_input->buttons;
    input.left_x = trace_input->left_x;
    input.left_y = trace_input->left_y;
    input.right_x = trace_input->right_x;
    input.right_y = trace_input->right_y;
    input.touch_count = trace_input->touch_count;
    memcpy(input.touches, trace_input->touches, sizeof(input.touches));
    service->input_active = 1u;
    return service->apply_input(service->input_user, &input);
}

int vd_companion_input_record_begin(struct vd_companion_service* service,
                                    uint64_t trace_id, uint64_t now_us)
{
    if (!vd_service_valid(service) ||
        service->state != VD_COMPANION_STATE_PAIRED ||
        service->trace_initialized == 0u ||
        service->record_consent !=
            VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT ||
        (service->negotiated_capabilities &
         VD_COMPANION_CAP_INPUT_RECORD) == 0u)
        return VD_COMPANION_ERROR_CAPABILITY;
    return vd_input_trace_record_begin(&service->trace, trace_id, now_us);
}

int vd_companion_input_record_physical(
    struct vd_companion_service* service,
    const struct vd_companion_input* physical_input, uint64_t now_us,
    uint64_t frame_index)
{
    struct vd_input_state trace_input;

    if (!vd_service_valid(service) || physical_input == NULL ||
        service->trace_initialized == 0u ||
        service->trace.state != VD_INPUT_TRACE_STATE_RECORDING)
        return VD_COMPANION_ERROR_STATE;
    vd_companion_to_trace_input(physical_input, &trace_input);
    {
        const int result = vd_input_trace_record_input(
            &service->trace, &trace_input, now_us, frame_index);
        if (result == VD_INPUT_TRACE_ERROR_TIME)
            (void)vd_input_trace_abort(
                &service->trace, VD_INPUT_TRACE_END_TIMEOUT);
        else if (result == VD_INPUT_TRACE_ERROR_ARGUMENT)
            (void)vd_input_trace_abort(
                &service->trace, VD_INPUT_TRACE_END_ABORTED);
        return result;
    }
}

int vd_companion_input_record_checkpoint(
    struct vd_companion_service* service, uint32_t marker,
    uint64_t now_us, uint64_t frame_index)
{
    int result;

    if (!vd_service_valid(service) ||
        service->trace_initialized == 0u)
        return VD_COMPANION_ERROR_STATE;
    result = vd_input_trace_record_checkpoint(
        &service->trace, marker, now_us, frame_index);
    if (result == VD_INPUT_TRACE_ERROR_TIME)
        (void)vd_input_trace_abort(
            &service->trace, VD_INPUT_TRACE_END_TIMEOUT);
    else if (result == VD_INPUT_TRACE_ERROR_ARGUMENT)
        (void)vd_input_trace_abort(
            &service->trace, VD_INPUT_TRACE_END_ABORTED);
    return result;
}

int vd_companion_input_record_end(struct vd_companion_service* service,
                                  uint32_t end_reason)
{
    if (!vd_service_valid(service) ||
        service->trace_initialized == 0u)
        return VD_COMPANION_ERROR_STATE;
    return vd_input_trace_record_end(&service->trace, end_reason);
}

int vd_companion_input_trace_import(struct vd_companion_service* service,
                                    const uint8_t* data,
                                    size_t data_size)
{
    if (!vd_service_valid(service) ||
        service->state != VD_COMPANION_STATE_PAIRED ||
        service->trace_initialized == 0u ||
        service->playback_consent !=
            VD_INPUT_TRACE_EXPLICIT_PLAYBACK_CONSENT ||
        (service->negotiated_capabilities &
         VD_COMPANION_CAP_INPUT_PLAYBACK) == 0u)
        return VD_COMPANION_ERROR_CAPABILITY;
    return vd_input_trace_import(&service->trace, data, data_size);
}

int vd_companion_input_playback_begin(
    struct vd_companion_service* service, uint64_t now_us)
{
    int result;

    if (!vd_service_valid(service) ||
        service->state != VD_COMPANION_STATE_PAIRED ||
        service->trace_initialized == 0u ||
        service->playback_consent !=
            VD_INPUT_TRACE_EXPLICIT_PLAYBACK_CONSENT ||
        (service->negotiated_capabilities &
         VD_COMPANION_CAP_INPUT_PLAYBACK) == 0u)
        return VD_COMPANION_ERROR_CAPABILITY;
    result = vd_release_input(service);
    if (result != VD_COMPANION_OK)
        return result;
    result = vd_input_trace_playback_begin(&service->trace, now_us);
    if (result == VD_INPUT_TRACE_OK)
        service->input_active = 1u;
    return result;
}

int vd_companion_input_playback_tick(
    struct vd_companion_service* service, uint64_t now_us)
{
    int result;

    if (!vd_service_valid(service) ||
        service->trace_initialized == 0u)
        return VD_COMPANION_ERROR_STATE;
    result = vd_input_trace_playback_tick(
        &service->trace, now_us, vd_companion_trace_apply, service);
    if (result == VD_INPUT_TRACE_COMPLETE) {
        service->input_active = 0u;
        service->input_cleanup_pending = 0u;
        service->input_lease_expires_ms = 0u;
        return VD_INPUT_TRACE_COMPLETE;
    }
    if (result < 0) {
        if (result == VD_INPUT_TRACE_ERROR_CALLBACK)
            return vd_abort_session(
                service, VD_COMPANION_ERROR_INPUT);
        if (result == VD_INPUT_TRACE_ERROR_TIME)
            return vd_abort_session(
                service, VD_COMPANION_ERROR_DEADLINE);
        return vd_release_input(service) == VD_COMPANION_OK
                   ? result
                   : VD_COMPANION_ERROR_INPUT;
    }
    return result;
}

int vd_companion_input_trace_abort(
    struct vd_companion_service* service, uint32_t end_reason)
{
    int result;
    int release_result;

    if (!vd_service_valid(service) ||
        service->trace_initialized == 0u)
        return VD_COMPANION_ERROR_STATE;
    result = vd_input_trace_abort(&service->trace, end_reason);
    release_result = vd_release_input(service);
    return result == VD_INPUT_TRACE_OK ? release_result : result;
}

int vd_companion_input_trace_pause(
    struct vd_companion_service* service)
{
    if (!vd_service_valid(service) ||
        service->trace_initialized == 0u)
        return VD_COMPANION_ERROR_STATE;
    return vd_input_trace_pause(&service->trace);
}

int vd_companion_input_trace_get_info(
    const struct vd_companion_service* service,
    struct vd_input_trace_info* info)
{
    if (!vd_service_valid(service) ||
        service->trace_initialized == 0u)
        return VD_COMPANION_ERROR_STATE;
    return vd_input_trace_get_info(&service->trace, info);
}

int vd_companion_input_trace_data(
    const struct vd_companion_service* service, const uint8_t** data,
    size_t* data_size)
{
    if (!vd_service_valid(service) ||
        service->trace_initialized == 0u)
        return VD_COMPANION_ERROR_STATE;
    return vd_input_trace_data(&service->trace, data, data_size);
}

int vd_companion_encode_record(
    const uint8_t secret[VD_COMPANION_SECRET_SIZE], uint16_t message_type,
    uint16_t status, uint64_t sequence, uint64_t ttl_ms,
    uint64_t session_id, uint64_t process_generation,
    uint64_t capabilities, const uint8_t* payload, size_t payload_size,
    uint8_t* output, size_t output_capacity, size_t* output_size)
{
    uint8_t tag[VD_COMPANION_TAG_SIZE];

    if (output_size != NULL)
        *output_size = 0u;
    if (!vd_bytes_nonzero(secret, VD_COMPANION_SECRET_SIZE) ||
        message_type == 0u || sequence == 0u || ttl_ms == 0u ||
        ttl_ms > VD_COMPANION_MAX_DEADLINE_MS ||
        session_id == 0u || process_generation == 0u ||
        payload_size > VD_COMPANION_MAX_PAYLOAD ||
        (payload_size != 0u && payload == NULL) || output == NULL ||
        output_size == NULL ||
        output_capacity < VD_COMPANION_HEADER_SIZE + payload_size)
        return VD_COMPANION_ERROR_INVALID_ARGUMENT;
    memset(output, 0, VD_COMPANION_HEADER_SIZE + payload_size);
    vd_write_u32(output, VD_COMPANION_MAGIC);
    vd_write_u16(output + 4, VD_COMPANION_PROTOCOL_VERSION);
    vd_write_u16(output + 6, VD_COMPANION_HEADER_SIZE);
    vd_write_u16(output + 8, message_type);
    vd_write_u16(output + 10, status);
    vd_write_u32(output + 12, (uint32_t)payload_size);
    vd_write_u64(output + 16, sequence);
    vd_write_u64(output + 24, ttl_ms);
    vd_write_u64(output + 32, session_id);
    vd_write_u64(output + 40, process_generation);
    vd_write_u64(output + 48, capabilities);
    if (payload_size != 0u)
        memcpy(output + VD_COMPANION_HEADER_SIZE, payload, payload_size);
    vd_record_tag(secret, output, output + VD_COMPANION_HEADER_SIZE,
                  payload_size, tag);
    memcpy(output + 64, tag, sizeof(tag));
    crypto_wipe(tag, sizeof(tag));
    *output_size = VD_COMPANION_HEADER_SIZE + payload_size;
    return VD_COMPANION_OK;
}

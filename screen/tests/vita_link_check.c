#include "vitadebug_companion.h"

#include <stddef.h>
#include <stdint.h>

static int discard(void* user, const uint8_t* data, size_t size)
{
    (void)user;
    (void)data;
    (void)size;
    return 0;
}

int main(void)
{
    static uint8_t pixels[4];
    uint8_t record[VD_COMPANION_HEADER_SIZE];
    size_t record_size = 0u;
    struct vd_screen_source source = {pixels, sizeof(pixels)};
    struct vd_screen_config config;
    struct vd_screen_stream stream = {0};
    struct vd_companion_config companion;
    size_t index;

    vd_companion_config_init(&companion);
    vd_screen_config_init(&config);
    config.explicit_consent = VD_SCREEN_EXPLICIT_CONSENT;
    config.write = discard;
    config.sources = &source;
    config.source_count = 1u;
    config.title_id[0] = 'V';
    config.title_id[1] = 'D';
    config.title_id[2] = 'S';
    config.title_id[3] = 'C';
    config.title_id[4] = 'R';
    config.title_id[5] = 'N';
    config.title_id[6] = '0';
    config.title_id[7] = '0';
    config.title_id[8] = '1';
    config.process_id = 1u;
    config.process_generation = 1u;
    config.session_id = 1u;
    for (index = 0u; index < VD_SCREEN_AUTH_TOKEN_SIZE; ++index)
        config.auth_token[index] = (uint8_t)(index + 1u);
    if (vd_screen_stream_init(&stream, &config) != VD_SCREEN_OK)
        return 1;
    return vd_companion_encode_record(
               config.auth_token, VD_COMPANION_MESSAGE_HELLO, 0u, 1u,
               1000u, config.session_id, config.process_generation,
               VD_COMPANION_CAP_STATUS, NULL, 0u, record, sizeof(record),
               &record_size) == VD_COMPANION_OK &&
                   record_size == sizeof(record) &&
                   companion.control_port ==
                       VD_COMPANION_DEFAULT_CONTROL_PORT
               ? 0
               : 1;
}

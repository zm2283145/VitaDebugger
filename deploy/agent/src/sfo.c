#include "sfo.h"

#include "io.h"

#include <stdlib.h>
#include <string.h>

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}
static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

int vdev_read_sfo_title_id(const char *path,
                           char title_id[VDEV_TITLE_ID_LEN + 1u])
{
    uint8_t *data = NULL;
    size_t size = 0;
    uint32_t key_table;
    uint32_t data_table;
    uint32_t entry_count;
    uint32_t index;
    int found = 0;
    int result = vdev_read_file_limited(path, 1024u * 1024u, &data, &size);
    if (result < 0) return result;
    if (size < 20u || read_u32_le(data) != 0x46535000u) {
        result = VDEV_ERR_FORMAT; goto done;
    }
    key_table = read_u32_le(data + 8u);
    data_table = read_u32_le(data + 12u);
    entry_count = read_u32_le(data + 16u);
    if (entry_count == 0u || entry_count > 4096u ||
        20u + (uint64_t)entry_count * 16u > size ||
        key_table >= size || data_table >= size || key_table >= data_table) {
        result = VDEV_ERR_FORMAT; goto done;
    }
    for (index = 0; index < entry_count; ++index) {
        const uint8_t *entry = data + 20u + (size_t)index * 16u;
        const uint16_t key_offset = read_u16_le(entry);
        const uint32_t data_length = read_u32_le(entry + 4u);
        const uint32_t data_offset = read_u32_le(entry + 12u);
        const uint64_t key_position = (uint64_t)key_table + key_offset;
        const uint64_t value_position = (uint64_t)data_table + data_offset;
        const char *key;
        size_t key_remaining;
        if (key_position >= data_table || value_position > size ||
            data_length > size - value_position) {
            result = VDEV_ERR_FORMAT; goto done;
        }
        key = (const char *)(data + key_position);
        key_remaining = (size_t)((uint64_t)data_table - key_position);
        if (memchr(key, '\0', key_remaining) == NULL) {
            result = VDEV_ERR_FORMAT; goto done;
        }
        if (strcmp(key, "TITLE_ID") == 0) {
            const uint8_t *value = data + value_position;
            if (found || data_length != VDEV_TITLE_ID_LEN + 1u ||
                value[VDEV_TITLE_ID_LEN] != '\0' ||
                memchr(value, '\0', VDEV_TITLE_ID_LEN) != NULL) {
                result = VDEV_ERR_TITLE; goto done;
            }
            memcpy(title_id, value, VDEV_TITLE_ID_LEN);
            title_id[VDEV_TITLE_ID_LEN] = '\0';
            if (!vdev_is_title_id(title_id)) {
                result = VDEV_ERR_TITLE; goto done;
            }
            found = 1;
        }
    }
    result = found ? VDEV_OK : VDEV_ERR_TITLE;
done:
    if (data != NULL) {
        memset(data, 0, size);
        free(data);
    }
    return result;
}

#include "vitaprofiler.h"

#include <limits.h>
#include <string.h>

#define VP_NAME_DICTIONARY_MAGIC UINT32_C(0x564e414d)

struct vp_builtin_name {
    uint32_t name_id;
    const char* name;
    uint16_t name_length;
};

#define VP_BUILTIN(id_value, text_value)                                     \
    {                                                                         \
        (id_value), (text_value), (uint16_t)(sizeof(text_value) - 1u)         \
    }

static const struct vp_builtin_name vp_builtin_names[VP_BUILTIN_NAME_COUNT] = {
    VP_BUILTIN(VP_METRIC_FREE_USER_BYTES, "vita.memory.free_user_bytes"),
    VP_BUILTIN(VP_METRIC_FREE_CDRAM_BYTES, "vita.memory.free_cdram_bytes"),
    VP_BUILTIN(VP_METRIC_FREE_PHYCONT_BYTES,
               "vita.memory.free_phycont_bytes"),
    VP_BUILTIN(VP_METRIC_PROCESS_TIME_US, "vita.process.time_us"),
    VP_BUILTIN(VP_METRIC_THREAD_RUN_CLOCKS, "vita.thread.run_clocks"),
    VP_BUILTIN(VP_METRIC_THREAD_STACK_FREE_BYTES,
               "vita.thread.stack_free_bytes"),
    VP_BUILTIN(VP_METRIC_THREAD_PREEMPTIONS,
               "vita.thread.preemptions"),
    VP_BUILTIN(VP_METRIC_INTERRUPT_PREEMPTIONS,
               "vita.thread.interrupt_preemptions"),
};

_Static_assert(sizeof(vp_builtin_names) / sizeof(vp_builtin_names[0]) ==
                   VP_BUILTIN_NAME_COUNT,
               "built-in name count changed");

#undef VP_BUILTIN

static int vp_name_dictionary_is_valid(
    const struct vp_name_dictionary* dictionary)
{
    return dictionary != NULL &&
           dictionary->initialized == VP_NAME_DICTIONARY_MAGIC &&
           dictionary->entry_count <= dictionary->entry_capacity &&
           dictionary->entry_capacity <= VP_NAME_DICTIONARY_MAX_ENTRIES &&
           dictionary->text_used <= dictionary->text_capacity &&
           (dictionary->entry_capacity == 0u ||
            (dictionary->entries != NULL && dictionary->text != NULL));
}

static uint16_t vp_read_u16_le(const uint8_t* input)
{
    return (uint16_t)((uint16_t)input[0] |
                      (uint16_t)((uint16_t)input[1] << 8));
}

static uint32_t vp_read_u32_le(const uint8_t* input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

static void vp_write_u16_le(uint8_t* output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void vp_write_u32_le(uint8_t* output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static uint32_t vp_name_id_bytes(const char* name, uint16_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    uint16_t i;
    for (i = 0u; i < length; ++i) {
        hash ^= (uint8_t)name[i];
        hash *= UINT32_C(16777619);
    }
    return hash == 0u ? 1u : hash;
}

static int vp_bounded_name_length(const char* name, uint16_t* length)
{
    uint32_t i;
    if (name == NULL || length == NULL || name[0] == '\0')
        return VP_ERROR_INVALID_ARGUMENT;
    for (i = 0u; i <= VP_NAME_MAX_LENGTH; ++i) {
        if (name[i] == '\0') {
            *length = (uint16_t)i;
            return VP_RESULT_OK;
        }
    }
    return VP_ERROR_INVALID_ARGUMENT;
}

static int vp_builtin_index(uint32_t name_id)
{
    uint32_t first = VP_METRIC_FREE_USER_BYTES;
    uint32_t last = VP_METRIC_INTERRUPT_PREEMPTIONS;
    if (name_id < first || name_id > last)
        return -1;
    return (int)(name_id - first);
}

static void vp_builtin_view(uint32_t index, struct vp_name_view* view)
{
    view->name = vp_builtin_names[index].name;
    view->name_id = vp_builtin_names[index].name_id;
    view->name_length = vp_builtin_names[index].name_length;
    view->flags = VP_NAME_FLAG_BUILTIN;
}

static uint32_t vp_user_lower_bound(
    const struct vp_name_dictionary* dictionary, uint32_t name_id)
{
    uint32_t first = 0u;
    uint32_t count = dictionary->entry_count;
    while (count != 0u) {
        uint32_t step = count / 2u;
        uint32_t middle = first + step;
        if (dictionary->entries[middle].name_id < name_id) {
            first = middle + 1u;
            count -= step + 1u;
        } else {
            count = step;
        }
    }
    return first;
}

static void vp_user_view(const struct vp_name_dictionary* dictionary,
                         uint32_t index, struct vp_name_view* view)
{
    const struct vp_name_entry* entry = &dictionary->entries[index];
    view->name = dictionary->text + entry->text_offset;
    view->name_id = entry->name_id;
    view->name_length = entry->text_length;
    view->flags = VP_NAME_FLAG_NONE;
}

static int vp_merged_view(const struct vp_name_dictionary* dictionary,
                          uint32_t* user_index, uint32_t* builtin_index,
                          struct vp_name_view* view)
{
    if (*user_index >= dictionary->entry_count &&
        *builtin_index >= VP_BUILTIN_NAME_COUNT)
        return VP_RESULT_END;

    if (*builtin_index >= VP_BUILTIN_NAME_COUNT ||
        (*user_index < dictionary->entry_count &&
         dictionary->entries[*user_index].name_id <
             vp_builtin_names[*builtin_index].name_id)) {
        vp_user_view(dictionary, *user_index, view);
        ++*user_index;
    } else {
        vp_builtin_view(*builtin_index, view);
        ++*builtin_index;
    }
    return VP_RESULT_OK;
}

static uint32_t vp_padded_name_size(uint16_t name_length)
{
    return ((uint32_t)name_length + 3u) & ~UINT32_C(3);
}

int vp_name_dictionary_init(
    struct vp_name_dictionary* dictionary,
    const struct vp_name_dictionary_config* config)
{
    if (dictionary == NULL || config == NULL ||
        config->entry_capacity > VP_NAME_DICTIONARY_MAX_ENTRIES ||
        (config->entry_capacity != 0u && config->entries == NULL) ||
        (config->text_capacity != 0u && config->text == NULL) ||
        (config->entry_capacity != 0u && config->text_capacity == 0u))
        return VP_ERROR_INVALID_ARGUMENT;

    memset(dictionary, 0, sizeof(*dictionary));
    dictionary->entries = config->entries;
    dictionary->text = config->text;
    dictionary->entry_capacity = config->entry_capacity;
    dictionary->text_capacity = config->text_capacity;
    dictionary->initialized = VP_NAME_DICTIONARY_MAGIC;
    return VP_RESULT_OK;
}

void vp_name_dictionary_deinit(struct vp_name_dictionary* dictionary)
{
    if (dictionary != NULL)
        memset(dictionary, 0, sizeof(*dictionary));
}

int vp_name_dictionary_register(struct vp_name_dictionary* dictionary,
                                const char* name, uint32_t* name_id)
{
    struct vp_name_entry* entry;
    uint32_t id;
    uint32_t index;
    uint16_t length;
    int result;

    if (name_id != NULL)
        *name_id = 0u;
    if (!vp_name_dictionary_is_valid(dictionary))
        return VP_ERROR_NOT_INITIALIZED;
    if (dictionary->sealed != 0u)
        return VP_ERROR_SEALED;

    result = vp_bounded_name_length(name, &length);
    if (result != VP_RESULT_OK)
        return result;
    id = vp_name_id_bytes(name, length);
    if (vp_builtin_index(id) >= 0)
        return VP_ERROR_NAME_CONFLICT;

    index = vp_user_lower_bound(dictionary, id);
    if (index < dictionary->entry_count &&
        dictionary->entries[index].name_id == id) {
        entry = &dictionary->entries[index];
        if (entry->text_length != length ||
            memcmp(dictionary->text + entry->text_offset, name, length) != 0)
            return VP_ERROR_NAME_CONFLICT;
        if (name_id != NULL)
            *name_id = id;
        return VP_RESULT_OK;
    }

    if (dictionary->entry_count == dictionary->entry_capacity ||
        (uint32_t)length + 1u >
            dictionary->text_capacity - dictionary->text_used)
        return VP_ERROR_CAPACITY;

    if (index < dictionary->entry_count) {
        memmove(&dictionary->entries[index + 1u],
                &dictionary->entries[index],
                (dictionary->entry_count - index) *
                    sizeof(dictionary->entries[0]));
    }
    memcpy(dictionary->text + dictionary->text_used, name,
           (size_t)length + 1u);
    entry = &dictionary->entries[index];
    entry->name_id = id;
    entry->text_offset = dictionary->text_used;
    entry->text_length = length;
    entry->reserved = 0u;
    dictionary->text_used += (uint32_t)length + 1u;
    ++dictionary->entry_count;
    if (name_id != NULL)
        *name_id = id;
    return VP_RESULT_OK;
}

int vp_name_dictionary_seal(struct vp_name_dictionary* dictionary)
{
    if (!vp_name_dictionary_is_valid(dictionary))
        return VP_ERROR_NOT_INITIALIZED;
    dictionary->sealed = 1u;
    return VP_RESULT_OK;
}

int vp_name_dictionary_get_stats(
    const struct vp_name_dictionary* dictionary,
    struct vp_name_dictionary_stats* stats)
{
    if (!vp_name_dictionary_is_valid(dictionary))
        return VP_ERROR_NOT_INITIALIZED;
    if (stats == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    stats->user_entries = dictionary->entry_count;
    stats->builtin_entries = VP_BUILTIN_NAME_COUNT;
    stats->total_entries = dictionary->entry_count + VP_BUILTIN_NAME_COUNT;
    stats->entry_capacity = dictionary->entry_capacity;
    stats->text_used = dictionary->text_used;
    stats->text_capacity = dictionary->text_capacity;
    stats->sealed = dictionary->sealed != 0u ? 1u : 0u;
    return VP_RESULT_OK;
}

int vp_name_dictionary_lookup(const struct vp_name_dictionary* dictionary,
                              uint32_t name_id, struct vp_name_view* view)
{
    uint32_t index;
    int builtin;
    if (view != NULL)
        memset(view, 0, sizeof(*view));
    if (!vp_name_dictionary_is_valid(dictionary))
        return VP_ERROR_NOT_INITIALIZED;
    if (view == NULL || name_id == 0u)
        return VP_ERROR_INVALID_ARGUMENT;
    if (dictionary->sealed == 0u)
        return VP_ERROR_SEALED;

    builtin = vp_builtin_index(name_id);
    if (builtin >= 0) {
        vp_builtin_view((uint32_t)builtin, view);
        return VP_RESULT_OK;
    }
    index = vp_user_lower_bound(dictionary, name_id);
    if (index >= dictionary->entry_count ||
        dictionary->entries[index].name_id != name_id)
        return VP_ERROR_NOT_FOUND;
    vp_user_view(dictionary, index, view);
    return VP_RESULT_OK;
}

int vp_name_dictionary_entry_at(
    const struct vp_name_dictionary* dictionary, uint32_t index,
    struct vp_name_view* view)
{
    uint32_t user_index = 0u;
    uint32_t builtin_index = 0u;
    uint32_t current;
    if (view != NULL)
        memset(view, 0, sizeof(*view));
    if (!vp_name_dictionary_is_valid(dictionary))
        return VP_ERROR_NOT_INITIALIZED;
    if (view == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (dictionary->sealed == 0u)
        return VP_ERROR_SEALED;
    if (index >= dictionary->entry_count + VP_BUILTIN_NAME_COUNT)
        return VP_ERROR_NOT_FOUND;

    for (current = 0u; current <= index; ++current) {
        int result = vp_merged_view(dictionary, &user_index, &builtin_index,
                                    view);
        if (result != VP_RESULT_OK)
            return VP_ERROR_NOT_FOUND;
    }
    return VP_RESULT_OK;
}

int vp_name_dictionary_wire_size(
    const struct vp_name_dictionary* dictionary, size_t* required)
{
    size_t total = VP_NAME_WIRE_HEADER_SIZE;
    uint32_t i;
    if (required != NULL)
        *required = 0u;
    if (!vp_name_dictionary_is_valid(dictionary))
        return VP_ERROR_NOT_INITIALIZED;
    if (required == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (dictionary->sealed == 0u)
        return VP_ERROR_SEALED;

    for (i = 0u; i < dictionary->entry_count; ++i) {
        total += VP_NAME_WIRE_ENTRY_HEADER_SIZE +
                 vp_padded_name_size(dictionary->entries[i].text_length);
    }
    for (i = 0u; i < VP_BUILTIN_NAME_COUNT; ++i) {
        total += VP_NAME_WIRE_ENTRY_HEADER_SIZE +
                 vp_padded_name_size(vp_builtin_names[i].name_length);
    }
    if (total > UINT32_MAX)
        return VP_ERROR_CAPACITY;
    *required = total;
    return VP_RESULT_OK;
}

int vp_encode_name_dictionary_le(
    const struct vp_name_dictionary* dictionary, uint8_t* output,
    size_t output_capacity, size_t* written)
{
    struct vp_name_view view;
    size_t required;
    uint32_t user_index = 0u;
    uint32_t builtin_index = 0u;
    uint32_t entry_count;
    uint32_t offset = VP_NAME_WIRE_HEADER_SIZE;
    int result;

    if (written != NULL)
        *written = 0u;
    if (output == NULL || written == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    result = vp_name_dictionary_wire_size(dictionary, &required);
    if (result != VP_RESULT_OK)
        return result;
    if (output_capacity < required) {
        *written = required;
        return VP_ERROR_BUFFER_TOO_SMALL;
    }

    entry_count = dictionary->entry_count + VP_BUILTIN_NAME_COUNT;
    vp_write_u32_le(output, VP_NAME_WIRE_MAGIC);
    vp_write_u16_le(output + 4u, VP_NAME_WIRE_VERSION);
    vp_write_u16_le(output + 6u, VP_NAME_WIRE_HEADER_SIZE);
    vp_write_u16_le(output + 8u, VP_NAME_WIRE_ENTRY_HEADER_SIZE);
    vp_write_u16_le(output + 10u, VP_NAME_WIRE_FLAG_LITTLE_ENDIAN);
    vp_write_u32_le(output + 12u, entry_count);
    vp_write_u32_le(output + 16u, (uint32_t)required);
    vp_write_u32_le(output + 20u, 0u);

    while (vp_merged_view(dictionary, &user_index, &builtin_index, &view) ==
           VP_RESULT_OK) {
        uint32_t padded = vp_padded_name_size(view.name_length);
        vp_write_u32_le(output + offset, view.name_id);
        vp_write_u16_le(output + offset + 4u, view.name_length);
        vp_write_u16_le(output + offset + 6u, view.flags);
        memcpy(output + offset + VP_NAME_WIRE_ENTRY_HEADER_SIZE, view.name,
               view.name_length);
        memset(output + offset + VP_NAME_WIRE_ENTRY_HEADER_SIZE +
                   view.name_length,
               0, padded - view.name_length);
        offset += VP_NAME_WIRE_ENTRY_HEADER_SIZE + padded;
    }
    *written = offset;
    return VP_RESULT_OK;
}

static int vp_validate_wire_name(uint32_t name_id, uint16_t flags,
                                 const char* name, uint16_t name_length,
                                 uint32_t* builtin_mask)
{
    int builtin = vp_builtin_index(name_id);
    uint16_t i;
    for (i = 0u; i < name_length; ++i) {
        if (name[i] == '\0')
            return VP_ERROR_MALFORMED;
    }

    if (builtin >= 0) {
        const struct vp_builtin_name* expected = &vp_builtin_names[builtin];
        if (flags != VP_NAME_FLAG_BUILTIN ||
            name_length != expected->name_length ||
            memcmp(name, expected->name, name_length) != 0)
            return VP_ERROR_MALFORMED;
        *builtin_mask |= UINT32_C(1) << (uint32_t)builtin;
        return VP_RESULT_OK;
    }
    if (flags != VP_NAME_FLAG_NONE ||
        vp_name_id_bytes(name, name_length) != name_id)
        return VP_ERROR_MALFORMED;
    return VP_RESULT_OK;
}

int vp_name_wire_validate_le(const uint8_t* input, size_t input_size,
                             struct vp_name_wire_info* info)
{
    uint32_t entry_count;
    uint32_t total_size;
    uint32_t offset;
    uint32_t previous_id = 0u;
    uint32_t builtin_mask = 0u;
    uint32_t i;
    uint16_t version;
    uint16_t flags;

    if (info != NULL)
        memset(info, 0, sizeof(*info));
    if (input == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (input_size < VP_NAME_WIRE_HEADER_SIZE)
        return VP_ERROR_MALFORMED;
    if (vp_read_u32_le(input) != VP_NAME_WIRE_MAGIC)
        return VP_ERROR_MALFORMED;

    version = vp_read_u16_le(input + 4u);
    if (version != VP_NAME_WIRE_VERSION)
        return VP_ERROR_UNSUPPORTED;
    if (vp_read_u16_le(input + 6u) != VP_NAME_WIRE_HEADER_SIZE ||
        vp_read_u16_le(input + 8u) != VP_NAME_WIRE_ENTRY_HEADER_SIZE)
        return VP_ERROR_UNSUPPORTED;
    flags = vp_read_u16_le(input + 10u);
    if (flags != VP_NAME_WIRE_FLAG_LITTLE_ENDIAN)
        return VP_ERROR_UNSUPPORTED;

    entry_count = vp_read_u32_le(input + 12u);
    total_size = vp_read_u32_le(input + 16u);
    if (vp_read_u32_le(input + 20u) != 0u ||
        entry_count < VP_BUILTIN_NAME_COUNT ||
        entry_count > VP_NAME_DICTIONARY_MAX_ENTRIES +
                          VP_BUILTIN_NAME_COUNT ||
        total_size < VP_NAME_WIRE_HEADER_SIZE || total_size > input_size ||
        entry_count >
            (total_size - VP_NAME_WIRE_HEADER_SIZE) /
                (VP_NAME_WIRE_ENTRY_HEADER_SIZE + 4u))
        return VP_ERROR_MALFORMED;

    offset = VP_NAME_WIRE_HEADER_SIZE;
    for (i = 0u; i < entry_count; ++i) {
        uint32_t name_id;
        uint32_t padded;
        uint16_t name_length;
        uint16_t name_flags;
        uint32_t j;
        int result;

        if (offset > total_size ||
            total_size - offset < VP_NAME_WIRE_ENTRY_HEADER_SIZE)
            return VP_ERROR_MALFORMED;
        name_id = vp_read_u32_le(input + offset);
        name_length = vp_read_u16_le(input + offset + 4u);
        name_flags = vp_read_u16_le(input + offset + 6u);
        padded = vp_padded_name_size(name_length);
        if (name_id == 0u || name_length == 0u ||
            name_length > VP_NAME_MAX_LENGTH ||
            (i != 0u && name_id <= previous_id) ||
            padded > total_size - offset - VP_NAME_WIRE_ENTRY_HEADER_SIZE)
            return VP_ERROR_MALFORMED;

        result = vp_validate_wire_name(
            name_id, name_flags,
            (const char*)(input + offset + VP_NAME_WIRE_ENTRY_HEADER_SIZE),
            name_length, &builtin_mask);
        if (result != VP_RESULT_OK)
            return result;
        for (j = name_length; j < padded; ++j) {
            if (input[offset + VP_NAME_WIRE_ENTRY_HEADER_SIZE + j] != 0u)
                return VP_ERROR_MALFORMED;
        }
        previous_id = name_id;
        offset += VP_NAME_WIRE_ENTRY_HEADER_SIZE + padded;
    }
    if (offset != total_size ||
        builtin_mask != (UINT32_C(1) << VP_BUILTIN_NAME_COUNT) - 1u)
        return VP_ERROR_MALFORMED;

    if (info != NULL) {
        info->entry_count = entry_count;
        info->total_size = total_size;
        info->version = version;
        info->flags = flags;
    }
    return VP_RESULT_OK;
}

int vp_name_wire_cursor_init(struct vp_name_wire_cursor* cursor,
                             const uint8_t* input, size_t input_size,
                             struct vp_name_wire_info* info)
{
    struct vp_name_wire_info parsed;
    int result;
    if (cursor == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    memset(cursor, 0, sizeof(*cursor));
    result = vp_name_wire_validate_le(input, input_size, &parsed);
    if (result != VP_RESULT_OK)
        return result;
    cursor->data = input;
    cursor->total_size = parsed.total_size;
    cursor->offset = VP_NAME_WIRE_HEADER_SIZE;
    cursor->remaining = parsed.entry_count;
    if (info != NULL)
        *info = parsed;
    return VP_RESULT_OK;
}

int vp_name_wire_cursor_next(struct vp_name_wire_cursor* cursor,
                             struct vp_name_view* view)
{
    uint16_t length;
    uint32_t padded;
    if (view != NULL)
        memset(view, 0, sizeof(*view));
    if (cursor == NULL || view == NULL || cursor->data == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (cursor->remaining == 0u)
        return VP_RESULT_END;
    if (cursor->offset > cursor->total_size ||
        cursor->total_size - cursor->offset <
            VP_NAME_WIRE_ENTRY_HEADER_SIZE)
        return VP_ERROR_MALFORMED;

    length = vp_read_u16_le(cursor->data + cursor->offset + 4u);
    padded = vp_padded_name_size(length);
    if (padded > cursor->total_size - cursor->offset -
                     VP_NAME_WIRE_ENTRY_HEADER_SIZE)
        return VP_ERROR_MALFORMED;
    view->name_id = vp_read_u32_le(cursor->data + cursor->offset);
    view->name_length = length;
    view->flags = vp_read_u16_le(cursor->data + cursor->offset + 6u);
    view->name = (const char*)(cursor->data + cursor->offset +
                               VP_NAME_WIRE_ENTRY_HEADER_SIZE);
    cursor->offset += VP_NAME_WIRE_ENTRY_HEADER_SIZE + padded;
    --cursor->remaining;
    return VP_RESULT_OK;
}

int vp_name_wire_lookup_le(const uint8_t* input, size_t input_size,
                           uint32_t name_id, struct vp_name_view* view)
{
    struct vp_name_wire_cursor cursor;
    struct vp_name_view current;
    int result;
    if (view != NULL)
        memset(view, 0, sizeof(*view));
    if (view == NULL || name_id == 0u)
        return VP_ERROR_INVALID_ARGUMENT;
    result = vp_name_wire_cursor_init(&cursor, input, input_size, NULL);
    if (result != VP_RESULT_OK)
        return result;
    while ((result = vp_name_wire_cursor_next(&cursor, &current)) ==
           VP_RESULT_OK) {
        if (current.name_id == name_id) {
            *view = current;
            return VP_RESULT_OK;
        }
        if (current.name_id > name_id)
            break;
    }
    return VP_ERROR_NOT_FOUND;
}

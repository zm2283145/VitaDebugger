#include "protocol.h"

#include "io.h"

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct VdevLine {
    const uint8_t *data;
    size_t size;
} VdevLine;

static int next_line(const uint8_t *data, size_t size, size_t *position,
                     VdevLine *line)
{
    size_t end;
    if (*position >= size) {
        return VDEV_ERR_FORMAT;
    }
    end = *position;
    while (end < size && data[end] != '\n') {
        if (data[end] == '\r' || data[end] == '\0') {
            return VDEV_ERR_FORMAT;
        }
        ++end;
    }
    if (end == size) {
        return VDEV_ERR_FORMAT;
    }
    line->data = data + *position;
    line->size = end - *position;
    *position = end + 1u;
    return VDEV_OK;
}

static int line_equals(const VdevLine *line, const char *literal)
{
    const size_t length = strlen(literal);
    return line->size == length && memcmp(line->data, literal, length) == 0;
}

static int copy_value(const VdevLine *line, const char *key,
                      char *output, size_t output_size)
{
    const size_t key_length = strlen(key);
    const size_t value_length = line->size > key_length ? line->size - key_length : 0u;
    if (line->size <= key_length || memcmp(line->data, key, key_length) != 0 ||
        value_length + 1u > output_size) {
        return VDEV_ERR_FORMAT;
    }
    memcpy(output, line->data + key_length, value_length);
    output[value_length] = '\0';
    return VDEV_OK;
}

static int parse_decimal(const uint8_t *text, size_t length, uint64_t *value)
{
    size_t index;
    uint64_t parsed = 0;
    if (length == 0u || (length > 1u && text[0] == '0')) {
        return VDEV_ERR_FORMAT;
    }
    for (index = 0; index < length; ++index) {
        const uint8_t digit = text[index];
        if (digit < '0' || digit > '9') {
            return VDEV_ERR_FORMAT;
        }
        if (parsed > (UINT64_MAX - (uint64_t)(digit - '0')) / 10u) {
            return VDEV_ERR_LIMIT;
        }
        parsed = parsed * 10u + (uint64_t)(digit - '0');
    }
    *value = parsed;
    return VDEV_OK;
}

static int parse_decimal_line(const VdevLine *line, const char *key,
                              uint64_t *value)
{
    const size_t key_length = strlen(key);
    if (line->size <= key_length || memcmp(line->data, key, key_length) != 0) {
        return VDEV_ERR_FORMAT;
    }
    return parse_decimal(line->data + key_length, line->size - key_length, value);
}

int vdev_parse_request(const uint8_t *data, size_t size, VdevRequest *request)
{
    VdevLine line;
    size_t position = 0;
    char manifest_hex[VDEV_SHA256_HEX_LEN + 1u];
    char action[32];
    uint64_t count;
    int result;

    if (data == NULL || request == NULL || size == 0u || size > VDEV_REQUEST_MAX) {
        return VDEV_ERR_FORMAT;
    }
    memset(request, 0, sizeof(*request));
#define NEXT() do { result = next_line(data, size, &position, &line); if (result < 0) return result; } while (0)
    NEXT();
    if (!line_equals(&line, "VITADEVDEPLOY-REQUEST-1")) {
        return VDEV_ERR_FORMAT;
    }
    NEXT();
    if (copy_value(&line, "job=", request->job, sizeof(request->job)) < 0 ||
        !vdev_is_lower_hex(request->job, VDEV_JOB_ID_HEX_LEN)) {
        return VDEV_ERR_FORMAT;
    }
    NEXT();
    if (copy_value(&line, "nonce=", request->nonce, sizeof(request->nonce)) < 0 ||
        !vdev_is_lower_hex(request->nonce, VDEV_NONCE_HEX_LEN)) {
        return VDEV_ERR_FORMAT;
    }
    NEXT();
    if (copy_value(&line, "action=", action, sizeof(action)) < 0) {
        return VDEV_ERR_FORMAT;
    }
    if (strcmp(action, "verify") == 0) {
        request->action = VDEV_ACTION_VERIFY;
    } else if (strcmp(action, "install") == 0) {
        request->action = VDEV_ACTION_INSTALL;
    } else if (strcmp(action, "install_launch") == 0) {
        request->action = VDEV_ACTION_INSTALL_LAUNCH;
    } else {
        return VDEV_ERR_FORMAT;
    }
    NEXT();
    if (copy_value(&line, "title_id=", request->title_id,
                   sizeof(request->title_id)) < 0 ||
        !vdev_is_allowed_target_title_id(request->title_id)) {
        return VDEV_ERR_TITLE;
    }
    NEXT();
    if (copy_value(&line, "manifest_sha256=", manifest_hex,
                   sizeof(manifest_hex)) < 0 ||
        !vdev_is_lower_hex(manifest_hex, VDEV_SHA256_HEX_LEN) ||
        vdev_hex_decode(manifest_hex, VDEV_SHA256_HEX_LEN,
                        request->manifest_sha256,
                        sizeof(request->manifest_sha256)) < 0) {
        return VDEV_ERR_FORMAT;
    }
    NEXT();
    result = parse_decimal_line(&line, "file_count=", &count);
    if (result < 0 || count == 0u || count > VDEV_MAX_FILES) {
        return result < 0 ? result : VDEV_ERR_LIMIT;
    }
    request->file_count = (uint32_t)count;
    NEXT();
    result = parse_decimal_line(&line, "total_size=", &request->total_size);
    if (result < 0 || request->total_size > VDEV_MAX_TOTAL_SIZE) {
        return result < 0 ? result : VDEV_ERR_LIMIT;
    }
    if (position != size) {
        return VDEV_ERR_FORMAT;
    }
#undef NEXT
    return VDEV_OK;
}

static int validate_relative_path(const char *path)
{
    const char *segment = path;
    const char *cursor = path;
    const size_t length = strlen(path);
    if (length == 0u || length > VDEV_REL_PATH_MAX || path[0] == '/' ||
        path[length - 1u] == '/') {
        return VDEV_ERR_PATH;
    }
    for (;;) {
        const unsigned char value = (unsigned char)*cursor;
        if (value == '\0' || value == '/') {
            const size_t segment_length = (size_t)(cursor - segment);
            if (segment_length == 0u ||
                (segment_length == 1u && segment[0] == '.') ||
                (segment_length == 2u && segment[0] == '.' && segment[1] == '.')) {
                return VDEV_ERR_PATH;
            }
            if (value == '\0') {
                break;
            }
            segment = cursor + 1;
        } else if (value < 0x20u || value > 0x7eu || value == '\t' ||
                   value == '\\' || value == ':') {
            return VDEV_ERR_PATH;
        }
        ++cursor;
    }
    return VDEV_OK;
}

static int ascii_case_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return *left == *right;
}

int vdev_parse_manifest(const uint8_t *data, size_t size,
                        VdevManifest *manifest)
{
    VdevLine line;
    size_t position = 0;
    uint32_t capacity = 0;
    int result;

    if (data == NULL || manifest == NULL || size == 0u || size > VDEV_MANIFEST_MAX) {
        return VDEV_ERR_FORMAT;
    }
    memset(manifest, 0, sizeof(*manifest));
    result = next_line(data, size, &position, &line);
    if (result < 0 || !line_equals(&line, "VITADEVDEPLOY-MANIFEST-1")) {
        return VDEV_ERR_FORMAT;
    }
    while (position < size) {
        size_t first_tab = SIZE_MAX;
        size_t second_tab = SIZE_MAX;
        size_t index;
        uint64_t file_size;
        char *path;
        VdevManifestEntry *entry;

        result = next_line(data, size, &position, &line);
        if (result < 0) goto failure;
        for (index = 0; index < line.size; ++index) {
            if (line.data[index] == '\t') {
                if (first_tab == SIZE_MAX) first_tab = index;
                else if (second_tab == SIZE_MAX) second_tab = index;
                else { result = VDEV_ERR_FORMAT; goto failure; }
            }
        }
        if (first_tab != VDEV_SHA256_HEX_LEN || second_tab == SIZE_MAX ||
            second_tab <= first_tab + 1u || second_tab + 1u >= line.size) {
            result = VDEV_ERR_FORMAT;
            goto failure;
        }
        if (manifest->count >= VDEV_MAX_FILES) {
            result = VDEV_ERR_LIMIT;
            goto failure;
        }
        if (manifest->count == capacity) {
            uint32_t new_capacity = capacity == 0u ? 64u : capacity * 2u;
            VdevManifestEntry *entries;
            if (new_capacity > VDEV_MAX_FILES) new_capacity = VDEV_MAX_FILES;
            entries = (VdevManifestEntry *)realloc(
                manifest->entries, sizeof(*entries) * new_capacity);
            if (entries == NULL) { result = VDEV_ERR_OOM; goto failure; }
            manifest->entries = entries;
            capacity = new_capacity;
        }
        entry = &manifest->entries[manifest->count];
        memset(entry, 0, sizeof(*entry));
        {
            char hash_hex[VDEV_SHA256_HEX_LEN + 1u];
            memcpy(hash_hex, line.data, VDEV_SHA256_HEX_LEN);
            hash_hex[VDEV_SHA256_HEX_LEN] = '\0';
            if (!vdev_is_lower_hex(hash_hex, VDEV_SHA256_HEX_LEN) ||
                vdev_hex_decode(hash_hex, VDEV_SHA256_HEX_LEN,
                                entry->sha256, sizeof(entry->sha256)) < 0) {
                result = VDEV_ERR_FORMAT;
                goto failure;
            }
        }
        result = parse_decimal(line.data + first_tab + 1u,
                               second_tab - first_tab - 1u, &file_size);
        if (result < 0) goto failure;
        if (file_size > VDEV_MAX_FILE_SIZE) {
            result = VDEV_ERR_LIMIT;
            goto failure;
        }
        path = (char *)malloc(line.size - second_tab);
        if (path == NULL) { result = VDEV_ERR_OOM; goto failure; }
        memcpy(path, line.data + second_tab + 1u, line.size - second_tab - 1u);
        path[line.size - second_tab - 1u] = '\0';
        entry->path = path;
        entry->size = file_size;
        ++manifest->count;
        result = validate_relative_path(path);
        if (result < 0) goto failure;
        if (manifest->count > 1u &&
            strcmp(manifest->entries[manifest->count - 2u].path, path) >= 0) {
            result = VDEV_ERR_FORMAT;
            goto failure;
        }
        for (index = 0; index + 1u < manifest->count; ++index) {
            if (ascii_case_equal(manifest->entries[index].path, path)) {
                result = VDEV_ERR_PATH;
                goto failure;
            }
        }
        if (UINT64_MAX - manifest->total_size < file_size) {
            result = VDEV_ERR_LIMIT;
            goto failure;
        }
        manifest->total_size += file_size;
        if (manifest->total_size > VDEV_MAX_TOTAL_SIZE) {
            result = VDEV_ERR_LIMIT;
            goto failure;
        }
    }
    if (manifest->count == 0u) {
        result = VDEV_ERR_FORMAT;
        goto failure;
    }
    return VDEV_OK;

failure:
    vdev_free_manifest(manifest);
    return result;
}

void vdev_free_manifest(VdevManifest *manifest)
{
    uint32_t index;
    if (manifest == NULL) return;
    for (index = 0; index < manifest->count; ++index) {
        free(manifest->entries[index].path);
    }
    free(manifest->entries);
    memset(manifest, 0, sizeof(*manifest));
}

int vdev_validate_job_layout(const char *job_directory)
{
    SceUID directory;
    SceIoDirent entry;
    unsigned mask = 0u;
    int read_result;

    directory = sceIoDopen(job_directory);
    if (directory < 0) return directory;
    for (;;) {
        char path[VDEV_PATH_MAX];
        unsigned bit;
        int expected_directory;
        memset(&entry, 0, sizeof(entry));
        read_result = sceIoDread(directory, &entry);
        if (read_result <= 0) break;
        if (strcmp(entry.d_name, ".") == 0 || strcmp(entry.d_name, "..") == 0) continue;
        if (strcmp(entry.d_name, "package") == 0) {
            bit = 1u; expected_directory = 1;
        } else if (strcmp(entry.d_name, "manifest.v1") == 0) {
            bit = 2u; expected_directory = 0;
        } else if (strcmp(entry.d_name, "request.v1") == 0) {
            bit = 4u; expected_directory = 0;
        } else if (strcmp(entry.d_name, "signature.bin") == 0) {
            bit = 8u; expected_directory = 0;
        } else {
            sceIoDclose(directory);
            return VDEV_ERR_EXTRA;
        }
        if ((mask & bit) != 0u || vdev_path_join(path, sizeof(path),
                                                  job_directory, entry.d_name) < 0 ||
            (expected_directory ? !vdev_is_directory(path) : !vdev_is_regular_file(path))) {
            sceIoDclose(directory);
            return VDEV_ERR_PATH;
        }
        mask |= bit;
    }
    sceIoDclose(directory);
    if (read_result < 0) return read_result;
    return mask == 15u ? VDEV_OK : VDEV_ERR_MISSING;
}

static int find_manifest_entry(VdevManifest *manifest, const char *path)
{
    uint32_t low = 0u;
    uint32_t high = manifest->count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2u;
        const int comparison = strcmp(path, manifest->entries[middle].path);
        if (comparison == 0) return (int)middle;
        if (comparison < 0) high = middle;
        else low = middle + 1u;
    }
    return -1;
}

static int manifest_has_descendant(const VdevManifest *manifest,
                                   const char *directory)
{
    uint32_t index;
    const size_t length = strlen(directory);
    for (index = 0; index < manifest->count; ++index) {
        if (strncmp(manifest->entries[index].path, directory, length) == 0 &&
            manifest->entries[index].path[length] == '/') {
            return 1;
        }
    }
    return 0;
}

static int verify_directory(const char *package_directory, const char *relative,
                            VdevManifest *manifest, unsigned depth,
                            uint32_t *actual_count, uint64_t *actual_size)
{
    char directory_path[VDEV_PATH_MAX];
    SceUID directory;
    SceIoDirent item;
    int read_result;
    int result;

    if (depth > VDEV_TREE_MAX_DEPTH) return VDEV_ERR_LIMIT;
    if (relative[0] == '\0') {
        if (strlen(package_directory) + 1u > sizeof(directory_path)) return VDEV_ERR_LIMIT;
        strcpy(directory_path, package_directory);
    } else if ((result = vdev_path_join(directory_path, sizeof(directory_path),
                                        package_directory, relative)) < 0) {
        return result;
    }
    directory = sceIoDopen(directory_path);
    if (directory < 0) return directory;
    for (;;) {
        char relative_path[VDEV_REL_PATH_MAX + 1u];
        char absolute_path[VDEV_PATH_MAX];
        size_t relative_length;
        size_t name_length;

        memset(&item, 0, sizeof(item));
        read_result = sceIoDread(directory, &item);
        if (read_result <= 0) break;
        if (strcmp(item.d_name, ".") == 0 || strcmp(item.d_name, "..") == 0) continue;
        name_length = strlen(item.d_name);
        relative_length = strlen(relative);
        if (name_length == 0u || relative_length + (relative_length ? 1u : 0u) +
            name_length + 1u > sizeof(relative_path)) {
            sceIoDclose(directory); return VDEV_ERR_LIMIT;
        }
        if (relative_length != 0u) {
            memcpy(relative_path, relative, relative_length);
            relative_path[relative_length++] = '/';
        }
        memcpy(relative_path + relative_length, item.d_name, name_length + 1u);
        result = validate_relative_path(relative_path);
        if (result < 0 ||
            vdev_path_join(absolute_path, sizeof(absolute_path),
                           package_directory, relative_path) < 0) {
            sceIoDclose(directory); return result < 0 ? result : VDEV_ERR_LIMIT;
        }
        if (SCE_S_ISDIR(item.d_stat.st_mode)) {
            if (!manifest_has_descendant(manifest, relative_path)) {
                sceIoDclose(directory); return VDEV_ERR_EXTRA;
            }
            result = verify_directory(package_directory, relative_path, manifest,
                                      depth + 1u, actual_count, actual_size);
            if (result < 0) { sceIoDclose(directory); return result; }
        } else if (SCE_S_ISREG(item.d_stat.st_mode)) {
            uint8_t digest[VDEV_SHA256_LEN];
            const int entry_index = find_manifest_entry(manifest, relative_path);
            VdevManifestEntry *expected;
            if (entry_index < 0) { sceIoDclose(directory); return VDEV_ERR_EXTRA; }
            expected = &manifest->entries[(uint32_t)entry_index];
            if (expected->seen) { sceIoDclose(directory); return VDEV_ERR_EXTRA; }
            result = vdev_sha256_file(absolute_path, expected->size, digest);
            if (result < 0) { sceIoDclose(directory); return result; }
            if (!vdev_constant_time_equal(digest, expected->sha256, sizeof(digest))) {
                sceIoDclose(directory); return VDEV_ERR_HASH;
            }
            expected->seen = 1;
            ++*actual_count;
            if (UINT64_MAX - *actual_size < expected->size) {
                sceIoDclose(directory); return VDEV_ERR_LIMIT;
            }
            *actual_size += expected->size;
        } else {
            sceIoDclose(directory); return VDEV_ERR_EXTRA;
        }
    }
    sceIoDclose(directory);
    return read_result < 0 ? read_result : VDEV_OK;
}

int vdev_verify_package_tree(const char *package_directory,
                             VdevManifest *manifest)
{
    uint32_t index;
    uint32_t actual_count = 0u;
    uint64_t actual_size = 0u;
    int result;
    if (!vdev_is_directory(package_directory)) return VDEV_ERR_MISSING;
    for (index = 0; index < manifest->count; ++index) manifest->entries[index].seen = 0;
    result = verify_directory(package_directory, "", manifest, 0u,
                              &actual_count, &actual_size);
    if (result < 0) return result;
    if (actual_count != manifest->count || actual_size != manifest->total_size) {
        return VDEV_ERR_MISSING;
    }
    for (index = 0; index < manifest->count; ++index) {
        if (!manifest->entries[index].seen) return VDEV_ERR_MISSING;
    }
    return VDEV_OK;
}

int vdev_find_committed_job(char job_id[VDEV_JOB_ID_HEX_LEN + 1u],
                            char job_directory[VDEV_PATH_MAX])
{
    SceUID directory;
    SceIoDirent item;
    int read_result;
    char selected[VDEV_JOB_ID_HEX_LEN + 1u] = {0};

    directory = sceIoDopen(VDEV_INBOX);
    if (directory < 0) return directory;
    for (;;) {
        char candidate[VDEV_PATH_MAX];
        char request_path[VDEV_PATH_MAX];
        char result_path[VDEV_PATH_MAX];
        memset(&item, 0, sizeof(item));
        read_result = sceIoDread(directory, &item);
        if (read_result <= 0) break;
        if (!SCE_S_ISDIR(item.d_stat.st_mode) ||
            !vdev_is_lower_hex(item.d_name, VDEV_JOB_ID_HEX_LEN)) continue;
        if (vdev_path_join(candidate, sizeof(candidate), VDEV_INBOX, item.d_name) < 0 ||
            vdev_path_join(request_path, sizeof(request_path), candidate, "request.v1") < 0 ||
            !vdev_is_regular_file(request_path)) continue;
        if (strlen(VDEV_RESULTS) + 1u + VDEV_JOB_ID_HEX_LEN + sizeof(".result") > sizeof(result_path)) continue;
        strcpy(result_path, VDEV_RESULTS "/");
        strcat(result_path, item.d_name);
        strcat(result_path, ".result");
        if (vdev_is_regular_file(result_path)) continue;
        if (selected[0] == '\0' || strcmp(item.d_name, selected) < 0) {
            strcpy(selected, item.d_name);
        }
    }
    sceIoDclose(directory);
    if (read_result < 0) return read_result;
    if (selected[0] == '\0') return 1;
    strcpy(job_id, selected);
    return vdev_path_join(job_directory, VDEV_PATH_MAX, VDEV_INBOX, selected);
}

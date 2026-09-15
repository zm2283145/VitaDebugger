#include "common.h"
#include "io.h"

#include <openssl/sha.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define VDEV_SCE_ERROR_ERRNO_EACCES UINT32_C(0x8001000D)

static void trace_atomic_step(VdevAtomicTraceCallback callback, void *context,
                              VdevAtomicTraceStep step,
                              VdevAtomicTraceEvent event, int code)
{
    if (callback != NULL) callback(step, event, code, context);
}

int vdev_is_lower_hex(const char *text, size_t length)
{
    size_t index;
    if (text == NULL) {
        return 0;
    }
    for (index = 0; index < length; ++index) {
        const char value = text[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return 0;
        }
    }
    return text[length] == '\0';
}

int vdev_is_title_id(const char *text)
{
    size_t index;
    if (text == NULL || strlen(text) != VDEV_TITLE_ID_LEN) {
        return 0;
    }
    for (index = 0; index < VDEV_TITLE_ID_LEN; ++index) {
        const char value = text[index];
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9'))) {
            return 0;
        }
    }
    return 1;
}

int vdev_is_allowed_target_title_id(const char *text)
{
    if (!vdev_is_title_id(text) || strcmp(text, VDEV_AGENT_TITLE_ID) == 0) {
        return 0;
    }
    return VDEV_ONLY_TARGET_TITLE_ID[0] == '\0' ||
           strcmp(text, VDEV_ONLY_TARGET_TITLE_ID) == 0;
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

int vdev_hex_decode(const char *hex, size_t hex_length,
                    uint8_t *output, size_t output_length)
{
    size_t index;
    if (hex == NULL || output == NULL || hex_length != output_length * 2u) {
        return VDEV_ERR_FORMAT;
    }
    for (index = 0; index < output_length; ++index) {
        const int high = hex_nibble(hex[index * 2u]);
        const int low = hex_nibble(hex[index * 2u + 1u]);
        if (high < 0 || low < 0) {
            return VDEV_ERR_FORMAT;
        }
        output[index] = (uint8_t)((high << 4) | low);
    }
    return VDEV_OK;
}

void vdev_hex_encode(const uint8_t *input, size_t input_length, char *output)
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0; index < input_length; ++index) {
        output[index * 2u] = digits[input[index] >> 4];
        output[index * 2u + 1u] = digits[input[index] & 15u];
    }
    output[input_length * 2u] = '\0';
}

int vdev_constant_time_equal(const uint8_t *left, const uint8_t *right,
                             size_t length)
{
    size_t index;
    uint8_t difference = 0;
    for (index = 0; index < length; ++index) {
        difference |= (uint8_t)(left[index] ^ right[index]);
    }
    return difference == 0;
}

int vdev_path_join(char *output, size_t output_size,
                   const char *left, const char *right)
{
    const size_t left_length = strlen(left);
    const size_t right_length = strlen(right);
    const int slash = left_length > 0u && left[left_length - 1u] != '/';
    if (left_length + (size_t)slash + right_length + 1u > output_size) {
        return VDEV_ERR_LIMIT;
    }
    memcpy(output, left, left_length);
    if (slash) {
        output[left_length] = '/';
    }
    memcpy(output + left_length + (size_t)slash, right, right_length + 1u);
    return VDEV_OK;
}

int vdev_ensure_directory(const char *path)
{
    SceIoStat stat;
    int result;
    memset(&stat, 0, sizeof(stat));
    result = sceIoGetstat(path, &stat);
    if (result >= 0) {
        return SCE_S_ISDIR(stat.st_mode) ? VDEV_OK : VDEV_ERR_PATH;
    }
    result = sceIoMkdir(path, VDEV_CREATE_DIRECTORY_MODE);
    return result < 0 ? result : VDEV_OK;
}

int vdev_is_regular_file(const char *path)
{
    SceIoStat stat;
    memset(&stat, 0, sizeof(stat));
    return sceIoGetstat(path, &stat) >= 0 && SCE_S_ISREG(stat.st_mode);
}

int vdev_is_directory(const char *path)
{
    SceIoStat stat;
    memset(&stat, 0, sizeof(stat));
    return sceIoGetstat(path, &stat) >= 0 && SCE_S_ISDIR(stat.st_mode);
}

static int write_all(SceUID descriptor, const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const size_t remaining = size - offset;
        const SceSize chunk = remaining > UINT_MAX ? UINT_MAX : (SceSize)remaining;
        const SceSSize written = sceIoWrite(descriptor, data + offset, chunk);
        if (written < 0) {
            return (int)written;
        }
        if (written == 0) {
            return VDEV_ERR_CHANGED;
        }
        offset += (size_t)written;
    }
    return VDEV_OK;
}

static int parent_directory_for_path(const char *path,
                                     char parent[VDEV_PATH_MAX])
{
    const char *colon;
    const char *slash;
    const char *cursor;
    size_t length;
    size_t path_length;

    if (path == NULL || parent == NULL) return VDEV_ERR_PATH;
    colon = strchr(path, ':');
    if (colon == NULL || colon == path) return VDEV_ERR_PATH;
    path_length = strlen(path);
    while (path_length > (size_t)(colon - path) + 1u &&
           path[path_length - 1u] == '/') {
        --path_length;
    }
    slash = NULL;
    cursor = path + path_length;
    while (cursor > colon + 1) {
        --cursor;
        if (*cursor == '/') {
            slash = cursor;
            break;
        }
    }
    if (slash == NULL || slash <= colon + 1) {
        length = (size_t)(colon - path) + 1u;
    } else {
        length = (size_t)(slash - path);
    }
    if (length == 0u || length + 1u > VDEV_PATH_MAX) return VDEV_ERR_LIMIT;
    memcpy(parent, path, length);
    parent[length] = '\0';
    return VDEV_OK;
}

static int sync_directory(const char *directory, int *sync_eacces,
                          VdevAtomicTraceCallback trace_callback,
                          void *trace_context)
{
    SceUID descriptor;
    int result;
    int close_result;

    if (sync_eacces == NULL) return VDEV_ERR_INTERNAL;
    *sync_eacces = 0;
    /* Probe the direct Vita directory descriptor sequence. Retail firmware may
     * reject SyncByFd here; callers decide whether that is fatal. */
    trace_atomic_step(trace_callback, trace_context,
                      VDEV_ATOMIC_STEP_PARENT_DOPEN,
                      VDEV_ATOMIC_TRACE_ENTER, VDEV_OK);
    descriptor = sceIoDopen(directory);
    trace_atomic_step(trace_callback, trace_context,
                      VDEV_ATOMIC_STEP_PARENT_DOPEN,
                      VDEV_ATOMIC_TRACE_RESULT,
                      descriptor < 0 ? descriptor : VDEV_OK);
    if (descriptor < 0) return descriptor;
    trace_atomic_step(trace_callback, trace_context,
                      VDEV_ATOMIC_STEP_PARENT_SYNC,
                      VDEV_ATOMIC_TRACE_ENTER, VDEV_OK);
    result = sceIoSyncByFd(descriptor, 0);
    trace_atomic_step(trace_callback, trace_context,
                      VDEV_ATOMIC_STEP_PARENT_SYNC,
                      VDEV_ATOMIC_TRACE_RESULT, result);
    trace_atomic_step(trace_callback, trace_context,
                      VDEV_ATOMIC_STEP_PARENT_DCLOSE,
                      VDEV_ATOMIC_TRACE_ENTER, VDEV_OK);
    close_result = sceIoDclose(descriptor);
    trace_atomic_step(trace_callback, trace_context,
                      VDEV_ATOMIC_STEP_PARENT_DCLOSE,
                      VDEV_ATOMIC_TRACE_RESULT, close_result);
    if (close_result < 0) return close_result;
    if (result >= 0) return VDEV_OK;
    if ((uint32_t)result == VDEV_SCE_ERROR_ERRNO_EACCES) *sync_eacces = 1;

    /* SyncByFd on a directory descriptor is rejected by the tested retail
     * firmware. sceIoSync is VitaSDK's documented device-wide sync primitive,
     * so use it as a checked barrier before considering any weaker fallback. */
    trace_atomic_step(trace_callback, trace_context,
                      VDEV_ATOMIC_STEP_DEVICE_SYNC,
                      VDEV_ATOMIC_TRACE_ENTER, VDEV_OK);
    result = vdev_sync_device();
    trace_atomic_step(trace_callback, trace_context,
                      VDEV_ATOMIC_STEP_DEVICE_SYNC,
                      VDEV_ATOMIC_TRACE_RESULT, result);
    return result < 0 ? result : VDEV_OK;
}

int vdev_sync_device(void)
{
    return sceIoSync(VDEV_STORAGE_DEVICE, 0);
}

#if !VDEV_ENABLE_INSTALL
static int sync_committed_regular_file(const char *path)
{
    SceIoStat before;
    SceUID descriptor;
    uint8_t digest[VDEV_SHA256_LEN];
    int result;

    memset(&before, 0, sizeof(before));
    result = sceIoGetstat(path, &before);
    if (result < 0) return result;
    if (!SCE_S_ISREG(before.st_mode) || before.st_size < 0) {
        return VDEV_ERR_CHANGED;
    }
    /* SyncByFd requires a writable descriptor on the tested retail Vita.
     * Close it before the independent read-only integrity pass below. */
    descriptor = sceIoOpen(path, SCE_O_WRONLY, 0);
    if (descriptor < 0) return descriptor;
    result = sceIoSyncByFd(descriptor, 0);
    {
        const int close_result = sceIoClose(descriptor);
        if (result >= 0 && close_result < 0) result = close_result;
    }
    if (result < 0) return result;

    /* Re-read the complete final file and require a stable size before/after. */
    result = vdev_sha256_file(path, (uint64_t)before.st_size, digest);
    memset(digest, 0, sizeof(digest));
    return result;
}
#endif

static int sync_parent_directory(const char *path,
                                 VdevAtomicTraceCallback trace_callback,
                                 void *trace_context)
{
    char parent[VDEV_PATH_MAX];
    int sync_eacces = 0;
    int result = parent_directory_for_path(path, parent);
    if (result < 0) return result;
    result = sync_directory(parent, &sync_eacces,
                            trace_callback, trace_context);
#if !VDEV_ENABLE_INSTALL
    /* Safe verification builds cannot persist namespace metadata on tested
     * retail hardware.  Reopen, resync, and re-stat the committed file as an
     * explicit weak fallback; this does not promise power-loss durability. */
    if (result < 0 && sync_eacces) {
        return sync_committed_regular_file(path);
    }
#else
    (void)sync_eacces;
#endif
    return result;
}

int vdev_sync_parent_directory(const char *path)
{
    return sync_parent_directory(path, NULL, NULL);
}

int vdev_sync_rename_parents(const char *source_path,
                             const char *destination_path)
{
    char source_parent[VDEV_PATH_MAX];
    char destination_parent[VDEV_PATH_MAX];
    int sync_eacces = 0;
    int result;

    result = parent_directory_for_path(source_path, source_parent);
    if (result < 0) return result;
    result = parent_directory_for_path(destination_path, destination_parent);
    if (result < 0) return result;

    if (strcmp(source_parent, destination_parent) == 0) {
        result = sync_directory(destination_parent, &sync_eacces, NULL, NULL);
#if !VDEV_ENABLE_INSTALL
        if (result < 0 && sync_eacces) {
            return sync_committed_regular_file(destination_path);
        }
#else
        (void)sync_eacces;
#endif
        return result;
    }

    /* Cross-directory promotion must never use the weak file fallback.
     * Commit the new name first, then the removal from the old directory. */
    result = sync_directory(destination_parent, &sync_eacces, NULL, NULL);
    if (result < 0) return result;
    return sync_directory(source_parent, &sync_eacces, NULL, NULL);
}

int vdev_read_file_limited(const char *path, size_t maximum,
                           uint8_t **data, size_t *size)
{
    SceIoStat before;
    SceIoStat after;
    SceUID descriptor;
    uint8_t *buffer;
    size_t offset = 0;
    int result = VDEV_OK;
    uint8_t extra;

    if (data == NULL || size == NULL) {
        return VDEV_ERR_INTERNAL;
    }
    *data = NULL;
    *size = 0;
    memset(&before, 0, sizeof(before));
    if ((result = sceIoGetstat(path, &before)) < 0) {
        return result;
    }
    if (!SCE_S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size > maximum) {
        return VDEV_ERR_LIMIT;
    }
    buffer = (uint8_t *)malloc((size_t)before.st_size + 1u);
    if (buffer == NULL) {
        return VDEV_ERR_OOM;
    }
    descriptor = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (descriptor < 0) {
        free(buffer);
        return descriptor;
    }
    while (offset < (size_t)before.st_size) {
        const size_t remaining = (size_t)before.st_size - offset;
        const SceSize chunk = remaining > 65536u ? 65536u : (SceSize)remaining;
        const SceSSize read_size = sceIoRead(descriptor, buffer + offset, chunk);
        if (read_size < 0) {
            result = (int)read_size;
            break;
        }
        if (read_size == 0) {
            result = VDEV_ERR_CHANGED;
            break;
        }
        offset += (size_t)read_size;
    }
    if (result == VDEV_OK) {
        const SceSSize extra_size = sceIoRead(descriptor, &extra, 1);
        if (extra_size < 0) {
            result = (int)extra_size;
        } else if (extra_size != 0) {
            result = VDEV_ERR_CHANGED;
        }
    }
    sceIoClose(descriptor);
    memset(&after, 0, sizeof(after));
    if (result == VDEV_OK &&
        (sceIoGetstat(path, &after) < 0 || after.st_size != before.st_size)) {
        result = VDEV_ERR_CHANGED;
    }
    if (result != VDEV_OK) {
        free(buffer);
        return result;
    }
    buffer[offset] = 0;
    *data = buffer;
    *size = offset;
    return VDEV_OK;
}

int vdev_write_atomic(const char *final_path, const void *data, size_t size,
                      int replace_existing,
                      VdevAtomicTraceCallback trace_callback,
                      void *trace_context)
{
    char part_path[VDEV_PATH_MAX];
    SceUID descriptor;
    int result;
    const size_t final_length = strlen(final_path);

    trace_atomic_step(trace_callback, trace_context, VDEV_ATOMIC_STEP_NONE,
                      VDEV_ATOMIC_TRACE_RESET, VDEV_OK);

    if (final_length + sizeof(".part") > sizeof(part_path)) {
        return VDEV_ERR_LIMIT;
    }
    memcpy(part_path, final_path, final_length);
    memcpy(part_path + final_length, ".part", sizeof(".part"));

    if (!replace_existing && vdev_is_regular_file(final_path)) {
        return VDEV_ERR_REPLAY;
    }
    sceIoRemove(part_path);
    if (replace_existing) {
        sceIoRemove(final_path);
    }
    trace_atomic_step(trace_callback, trace_context, VDEV_ATOMIC_STEP_OPEN,
                      VDEV_ATOMIC_TRACE_ENTER, VDEV_OK);
    descriptor = sceIoOpen(part_path,
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                           VDEV_CREATE_FILE_MODE);
    trace_atomic_step(trace_callback, trace_context, VDEV_ATOMIC_STEP_OPEN,
                      VDEV_ATOMIC_TRACE_RESULT,
                      descriptor < 0 ? descriptor : VDEV_OK);
    if (descriptor < 0) {
        return descriptor;
    }
    trace_atomic_step(trace_callback, trace_context, VDEV_ATOMIC_STEP_WRITE,
                      VDEV_ATOMIC_TRACE_ENTER, VDEV_OK);
    result = write_all(descriptor, (const uint8_t *)data, size);
    trace_atomic_step(trace_callback, trace_context, VDEV_ATOMIC_STEP_WRITE,
                      VDEV_ATOMIC_TRACE_RESULT, result);
    if (result == VDEV_OK) {
        trace_atomic_step(trace_callback, trace_context,
                          VDEV_ATOMIC_STEP_FILE_SYNC,
                          VDEV_ATOMIC_TRACE_ENTER, VDEV_OK);
        const int sync_result = sceIoSyncByFd(descriptor, 0);
        trace_atomic_step(trace_callback, trace_context,
                          VDEV_ATOMIC_STEP_FILE_SYNC,
                          VDEV_ATOMIC_TRACE_RESULT, sync_result);
        if (sync_result < 0) {
            result = sync_result;
        }
    }
    {
        trace_atomic_step(trace_callback, trace_context, VDEV_ATOMIC_STEP_CLOSE,
                          VDEV_ATOMIC_TRACE_ENTER, VDEV_OK);
        const int close_result = sceIoClose(descriptor);
        trace_atomic_step(trace_callback, trace_context, VDEV_ATOMIC_STEP_CLOSE,
                          VDEV_ATOMIC_TRACE_RESULT, close_result);
        if (result == VDEV_OK && close_result < 0) {
            result = close_result;
        }
    }
    if (result != VDEV_OK) {
        sceIoRemove(part_path);
        return result;
    }
    trace_atomic_step(trace_callback, trace_context, VDEV_ATOMIC_STEP_RENAME,
                      VDEV_ATOMIC_TRACE_ENTER, VDEV_OK);
    result = sceIoRename(part_path, final_path);
    trace_atomic_step(trace_callback, trace_context, VDEV_ATOMIC_STEP_RENAME,
                      VDEV_ATOMIC_TRACE_RESULT, result);
    if (result < 0) {
        sceIoRemove(part_path);
        return result;
    }
    {
        SceIoStat committed;
        memset(&committed, 0, sizeof(committed));
        trace_atomic_step(trace_callback, trace_context,
                          VDEV_ATOMIC_STEP_POSTSTAT,
                          VDEV_ATOMIC_TRACE_ENTER, VDEV_OK);
        result = sceIoGetstat(final_path, &committed);
        if (result >= 0 &&
            (!SCE_S_ISREG(committed.st_mode) || committed.st_size < 0 ||
             (uint64_t)committed.st_size != (uint64_t)size)) {
            result = VDEV_ERR_CHANGED;
        }
        trace_atomic_step(trace_callback, trace_context,
                          VDEV_ATOMIC_STEP_POSTSTAT,
                          VDEV_ATOMIC_TRACE_RESULT, result);
        if (result < 0) return result;
    }
    return sync_parent_directory(final_path, trace_callback, trace_context);
}

int vdev_consume_challenge(void)
{
    int result;
    sceIoRemove(VDEV_CHALLENGE_USED);
    result = sceIoRename(VDEV_CHALLENGE, VDEV_CHALLENGE_USED);
    if (result < 0) return result;
    if (!vdev_is_regular_file(VDEV_CHALLENGE_USED) ||
        vdev_is_regular_file(VDEV_CHALLENGE) ||
        vdev_is_directory(VDEV_CHALLENGE)) {
        return VDEV_ERR_CHANGED;
    }
    return vdev_sync_rename_parents(VDEV_CHALLENGE, VDEV_CHALLENGE_USED);
}

int vdev_sha256_buffer(const void *data, size_t size, uint8_t digest[32])
{
    SHA256_CTX context;
    if (SHA256_Init(&context) != 1 ||
        SHA256_Update(&context, data, size) != 1 ||
        SHA256_Final(digest, &context) != 1) {
        memset(&context, 0, sizeof(context));
        return VDEV_ERR_INTERNAL;
    }
    memset(&context, 0, sizeof(context));
    return VDEV_OK;
}

int vdev_sha256_file(const char *path, uint64_t expected_size,
                     uint8_t digest[32])
{
    uint8_t buffer[65536];
    SHA256_CTX context;
    SceIoStat before;
    SceIoStat after;
    SceUID descriptor;
    uint64_t total = 0;
    int result = VDEV_OK;

    memset(&before, 0, sizeof(before));
    if ((result = sceIoGetstat(path, &before)) < 0) {
        return result;
    }
    if (!SCE_S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size != expected_size) {
        return VDEV_ERR_CHANGED;
    }
    descriptor = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (descriptor < 0) {
        return descriptor;
    }
    if (SHA256_Init(&context) != 1) {
        sceIoClose(descriptor);
        return VDEV_ERR_INTERNAL;
    }
    for (;;) {
        const SceSSize read_size = sceIoRead(descriptor, buffer, sizeof(buffer));
        if (read_size < 0) {
            result = (int)read_size;
            break;
        }
        if (read_size == 0) {
            break;
        }
        total += (uint64_t)read_size;
        if (total > expected_size ||
            SHA256_Update(&context, buffer, (size_t)read_size) != 1) {
            result = VDEV_ERR_CHANGED;
            break;
        }
    }
    sceIoClose(descriptor);
    memset(&after, 0, sizeof(after));
    if (result == VDEV_OK &&
        (total != expected_size || sceIoGetstat(path, &after) < 0 ||
         after.st_size != before.st_size)) {
        result = VDEV_ERR_CHANGED;
    }
    if (result == VDEV_OK && SHA256_Final(digest, &context) != 1) {
        result = VDEV_ERR_INTERNAL;
    }
    memset(buffer, 0, sizeof(buffer));
    memset(&context, 0, sizeof(context));
    return result;
}

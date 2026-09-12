#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

#define TEST_TITLE_ID "VDDT00001"
#define TEST_DIRECTORY "ux0:data/VitaDevDeploy"
#define TEST_MARKER TEST_DIRECTORY "/disposable-target.last-run"
#define TEST_MARKER_PART TEST_DIRECTORY "/disposable-target.last-run.part"
#define TEST_MARKER_PREFIX "launch_count="
#define TEST_SCE_ERROR_ERRNO_EACCES UINT32_C(0x8001000D)
#define TEST_CREATE_FILE_MODE \
    (SCE_S_IRUSR | SCE_S_IWUSR | SCE_S_IRSYS | SCE_S_IWSYS)
#define TEST_CREATE_DIRECTORY_MODE (SCE_S_IRWXU | SCE_S_IRWXS)

static int ensure_test_directory(void)
{
    SceIoStat status;
    int result;

    memset(&status, 0, sizeof(status));
    result = sceIoGetstat(TEST_DIRECTORY, &status);
    if (result >= 0) {
        return SCE_S_ISDIR(status.st_mode) ? 0 : -1;
    }

    result = sceIoMkdir(TEST_DIRECTORY, TEST_CREATE_DIRECTORY_MODE);
    return result < 0 ? result : 0;
}

static uint64_t read_previous_launch_count(void)
{
    char marker[160];
    const size_t prefix_length = sizeof(TEST_MARKER_PREFIX) - 1u;
    SceUID descriptor;
    SceSSize read_size;
    uint64_t value = 0;
    size_t index;

    descriptor = sceIoOpen(TEST_MARKER, SCE_O_RDONLY, 0);
    if (descriptor < 0) {
        return 0;
    }
    read_size = sceIoRead(descriptor, marker, sizeof(marker) - 1u);
    sceIoClose(descriptor);
    if (read_size <= (SceSSize)prefix_length) {
        return 0;
    }
    marker[read_size] = '\0';
    if (memcmp(marker, TEST_MARKER_PREFIX, prefix_length) != 0) {
        return 0;
    }

    for (index = prefix_length; index < (size_t)read_size; ++index) {
        const char character = marker[index];
        const unsigned int digit = (unsigned int)(character - '0');

        if (character == '\n') {
            return index > prefix_length ? value : 0;
        }
        if (character < '0' || character > '9' ||
            value > (UINT64_MAX - digit) / 10u) {
            return 0;
        }
        value = value * 10u + digit;
    }
    return 0;
}

static int write_all(SceUID descriptor, const char *data, size_t size)
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
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static int weak_sync_test_marker(size_t expected_size)
{
    uint8_t marker[160];
    uint8_t extra;
    SceIoStat status;
    SceUID descriptor;
    size_t offset = 0;
    int result;

    if (expected_size > sizeof(marker)) return -1;
    descriptor = sceIoOpen(TEST_MARKER, SCE_O_WRONLY, 0);
    if (descriptor < 0) return descriptor;
    result = sceIoSyncByFd(descriptor, 0);
    {
        const int close_result = sceIoClose(descriptor);
        if (result >= 0 && close_result < 0) result = close_result;
    }
    if (result < 0) return result;

    descriptor = sceIoOpen(TEST_MARKER, SCE_O_RDONLY, 0);
    if (descriptor < 0) return descriptor;
    while (result >= 0 && offset < expected_size) {
        const SceSSize read_size = sceIoRead(
            descriptor, marker + offset, (SceSize)(expected_size - offset));
        if (read_size < 0) {
            result = (int)read_size;
        } else if (read_size == 0) {
            result = -1;
        } else {
            offset += (size_t)read_size;
        }
    }
    if (result >= 0) {
        const SceSSize extra_size = sceIoRead(descriptor, &extra, 1);
        if (extra_size < 0) {
            result = (int)extra_size;
        } else if (extra_size != 0) {
            result = -1;
        }
    }
    {
        const int close_result = sceIoClose(descriptor);
        if (result >= 0 && close_result < 0) result = close_result;
    }
    if (result < 0) return result;
    memset(&status, 0, sizeof(status));
    result = sceIoGetstat(TEST_MARKER, &status);
    if (result < 0) return result;
    if (!SCE_S_ISREG(status.st_mode) || status.st_size < 0 ||
        (uint64_t)status.st_size != (uint64_t)expected_size) {
        return -1;
    }
    return 0;
}

static int sync_test_directory(size_t expected_size)
{
    SceUID descriptor;
    int result;
    int close_result;

    descriptor = sceIoDopen(TEST_DIRECTORY);
    if (descriptor < 0) return descriptor;
    result = sceIoSyncByFd(descriptor, 0);
    close_result = sceIoDclose(descriptor);
    if (close_result < 0) return close_result;
    if (result >= 0) return 0;
    if ((uint32_t)result == TEST_SCE_ERROR_ERRNO_EACCES) {
        return weak_sync_test_marker(expected_size);
    }
    return result;
}

static int append_text(char *output, size_t capacity, size_t *length,
                       const char *text)
{
    size_t index = 0;

    while (text[index] != '\0') {
        if (*length >= capacity) {
            return -1;
        }
        output[*length] = text[index];
        ++*length;
        ++index;
    }
    return 0;
}

static int append_decimal(char *output, size_t capacity, size_t *length,
                          uint64_t value)
{
    char reversed[20];
    size_t digits = 0;

    do {
        reversed[digits++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);

    if (*length > capacity || digits > capacity - *length) {
        return -1;
    }
    while (digits > 0u) {
        output[(*length)++] = reversed[--digits];
    }
    return 0;
}

static int write_launch_marker(uint64_t launch_count)
{
    char marker[160];
    SceUID descriptor;
    SceIoStat old_status;
    int result;
    size_t length = 0;

    if (append_text(marker, sizeof(marker), &length, TEST_MARKER_PREFIX) < 0 ||
        append_decimal(marker, sizeof(marker), &length, launch_count) < 0 ||
        append_text(marker, sizeof(marker), &length,
                    "\ntitle_id=" TEST_TITLE_ID
                    "\nstatus=marker_committed"
                    "\nmarker_version=1\n") < 0) {
        return -1;
    }

    sceIoRemove(TEST_MARKER_PART);
    descriptor = sceIoOpen(TEST_MARKER_PART,
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                           TEST_CREATE_FILE_MODE);
    if (descriptor < 0) {
        return descriptor;
    }

    result = write_all(descriptor, marker, length);
    if (result == 0) {
        const int sync_result = sceIoSyncByFd(descriptor, 0);
        if (sync_result < 0) {
            result = sync_result;
        }
    }
    {
        const int close_result = sceIoClose(descriptor);
        if (result == 0 && close_result < 0) {
            result = close_result;
        }
    }
    if (result < 0) {
        sceIoRemove(TEST_MARKER_PART);
        return result;
    }

    memset(&old_status, 0, sizeof(old_status));
    if (sceIoGetstat(TEST_MARKER, &old_status) >= 0) {
        if (!SCE_S_ISREG(old_status.st_mode)) {
            sceIoRemove(TEST_MARKER_PART);
            return -1;
        }
        result = sceIoRemove(TEST_MARKER);
        if (result < 0) {
            sceIoRemove(TEST_MARKER_PART);
            return result;
        }
    }

    result = sceIoRename(TEST_MARKER_PART, TEST_MARKER);
    if (result < 0) {
        sceIoRemove(TEST_MARKER_PART);
        return result;
    }
    {
        SceIoStat committed;
        memset(&committed, 0, sizeof(committed));
        result = sceIoGetstat(TEST_MARKER, &committed);
        if (result < 0) return result;
        if (!SCE_S_ISREG(committed.st_mode) || committed.st_size < 0 ||
            (uint64_t)committed.st_size != (uint64_t)length) {
            return -1;
        }
    }
    return sync_test_directory(length);
}

int main(void)
{
    uint64_t previous_count;
    uint64_t next_count;

    if (ensure_test_directory() < 0) {
        return 1;
    }

    previous_count = read_previous_launch_count();
    next_count = previous_count == UINT64_MAX ? 1u : previous_count + 1u;
    return write_launch_marker(next_count) < 0 ? 1 : 0;
}

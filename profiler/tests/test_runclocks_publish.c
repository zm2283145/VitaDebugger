#include "../runclocks-experiment/publish.h"

#include <stdio.h>
#include <string.h>

enum failure_step {
    FAIL_NONE = 0,
    FAIL_OPEN,
    FAIL_WRITE,
    FAIL_SYNC_FD,
    FAIL_CLOSE,
    FAIL_SYNC_VOLUME,
    FAIL_EXISTS,
    FAIL_RENAME,
};

struct fake_io {
    enum failure_step failure;
    int part_exists;
    int final_exists;
    int close_calls;
};

static int failures;

#define CHECK(condition, message)                                        \
    do {                                                                 \
        if (!(condition)) {                                              \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static int fake_open(void* context, const char* path)
{
    struct fake_io* fake = (struct fake_io*)context;
    CHECK(strstr(path, ".json.part") != NULL, "opens only the part path");
    if (fake->failure == FAIL_OPEN)
        return -1;
    fake->part_exists = 1;
    return 7;
}

static int fake_write(void* context, int fd)
{
    struct fake_io* fake = (struct fake_io*)context;
    CHECK(fd == 7, "writer receives opened descriptor");
    return fake->failure == FAIL_WRITE ? -1 : 0;
}

static int fake_sync_fd(void* context, int fd)
{
    struct fake_io* fake = (struct fake_io*)context;
    CHECK(fd == 7, "file sync receives opened descriptor");
    return fake->failure == FAIL_SYNC_FD ? -1 : 0;
}

static int fake_close(void* context, int fd)
{
    struct fake_io* fake = (struct fake_io*)context;
    CHECK(fd == 7, "close receives opened descriptor");
    ++fake->close_calls;
    return fake->failure == FAIL_CLOSE ? -1 : 0;
}

static int fake_sync_volume(void* context)
{
    struct fake_io* fake = (struct fake_io*)context;
    return fake->failure == FAIL_SYNC_VOLUME ? -1 : 0;
}

static int fake_exists(void* context, const char* path)
{
    struct fake_io* fake = (struct fake_io*)context;
    CHECK(strstr(path, ".json.part") == NULL, "checks only the final path");
    if (fake->failure == FAIL_EXISTS)
        return -1;
    return fake->final_exists;
}

static int fake_rename(
    void* context, const char* source, const char* destination)
{
    struct fake_io* fake = (struct fake_io*)context;
    CHECK(strstr(source, ".json.part") != NULL,
          "rename source is the part path");
    CHECK(strstr(destination, ".json.part") == NULL,
          "rename destination is the final path");
    if (fake->failure == FAIL_RENAME)
        return -1;
    fake->part_exists = 0;
    fake->final_exists = 1;
    return 0;
}

static int fake_remove(void* context, const char* path)
{
    struct fake_io* fake = (struct fake_io*)context;
    CHECK(strstr(path, ".json.part") != NULL, "removes only the part path");
    fake->part_exists = 0;
    return 0;
}

static const struct rc_publish_io operations = {
    fake_open,
    fake_sync_fd,
    fake_close,
    fake_sync_volume,
    fake_exists,
    fake_rename,
    fake_remove,
};

static void test_success(void)
{
    struct fake_io fake;
    memset(&fake, 0, sizeof(fake));
    CHECK(rc_publish_atomic(
              "runclocks-test.json", fake_write, &fake, &operations,
              &fake) == 0,
          "successful publication returns success");
    CHECK(fake.final_exists && !fake.part_exists,
          "success publishes only the final path");
    CHECK(fake.close_calls == 1, "success closes the part file");
}

static void test_failure(enum failure_step step)
{
    struct fake_io fake;
    memset(&fake, 0, sizeof(fake));
    fake.failure = step;
    CHECK(rc_publish_atomic(
              "runclocks-test.json", fake_write, &fake, &operations,
              &fake) < 0,
          "injected publication failure is reported");
    CHECK(!fake.final_exists,
          "reported publication failure never creates the final path");
    CHECK(!fake.part_exists, "reported failure cleans the created part path");
    CHECK(fake.close_calls == (step == FAIL_OPEN ? 0 : 1),
          "opened part file is closed exactly once");
}

static void test_existing_final(void)
{
    struct fake_io fake;
    memset(&fake, 0, sizeof(fake));
    fake.final_exists = 1;
    CHECK(rc_publish_atomic(
              "runclocks-test.json", fake_write, &fake, &operations,
              &fake) < 0,
          "existing final path rejects publication");
    CHECK(fake.final_exists && !fake.part_exists,
          "existing final path is preserved and part is removed");
}

int main(void)
{
    enum failure_step step;
    test_success();
    for (step = FAIL_OPEN; step <= FAIL_RENAME; ++step)
        test_failure(step);
    test_existing_final();
    if (failures != 0) {
        fprintf(stderr, "runClocks publication: %d failure(s)\n", failures);
        return 1;
    }
    puts("runClocks publication: all failure paths passed");
    return 0;
}

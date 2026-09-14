#include "vitaprofiler.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

static int failures;

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__);     \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static uint16_t read_u16_le(const uint8_t* input)
{
    return (uint16_t)((uint16_t)input[0] |
                      (uint16_t)((uint16_t)input[1] << 8));
}

static uint32_t read_u32_le(const uint8_t* input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

static int view_equals(const struct vp_name_view* view, const char* expected)
{
    size_t length = strlen(expected);
    return length == view->name_length &&
           memcmp(view->name, expected, length) == 0;
}

static void init_dictionary(struct vp_name_dictionary* dictionary,
                            struct vp_name_entry* entries,
                            uint32_t entry_capacity, char* text,
                            uint32_t text_capacity)
{
    struct vp_name_dictionary_config config;
    config.entries = entries;
    config.entry_capacity = entry_capacity;
    config.text = text;
    config.text_capacity = text_capacity;
    CHECK(vp_name_dictionary_init(dictionary, &config) == VP_RESULT_OK,
          "initialize name dictionary");
}

static void test_validation_and_lifecycle(void)
{
    struct vp_name_dictionary dictionary;
    struct vp_name_dictionary_config config;
    struct vp_name_dictionary_stats stats;
    struct vp_name_view view;
    struct vp_name_entry entry;
    char text[8];
    uint32_t name_id = UINT32_C(0xdeadbeef);

    memset(&dictionary, 0, sizeof(dictionary));
    memset(&config, 0, sizeof(config));
    CHECK(vp_name_dictionary_register(&dictionary, "x", &name_id) ==
              VP_ERROR_NOT_INITIALIZED &&
              name_id == 0u,
          "registration rejects uninitialized dictionary");
    config.entries = &entry;
    config.entry_capacity = 1u;
    CHECK(vp_name_dictionary_init(&dictionary, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "entry storage requires text storage");
    config.entry_capacity = VP_NAME_DICTIONARY_MAX_ENTRIES + 1u;
    config.text = text;
    config.text_capacity = sizeof(text);
    CHECK(vp_name_dictionary_init(&dictionary, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "reject excessive entry capacity");

    memset(&config, 0, sizeof(config));
    CHECK(vp_name_dictionary_init(&dictionary, &config) == VP_RESULT_OK,
          "built-in-only dictionary needs no caller storage");
    CHECK(vp_name_dictionary_get_stats(&dictionary, &stats) == VP_RESULT_OK &&
              stats.user_entries == 0u &&
              stats.builtin_entries == VP_BUILTIN_NAME_COUNT &&
              stats.total_entries == VP_BUILTIN_NAME_COUNT &&
              stats.sealed == 0u,
          "built-in-only dictionary stats");
    CHECK(vp_name_dictionary_lookup(&dictionary,
                                     VP_METRIC_FREE_USER_BYTES, &view) ==
              VP_ERROR_SEALED,
          "lookup requires a sealed dictionary");
    CHECK(vp_name_dictionary_seal(&dictionary) == VP_RESULT_OK &&
              vp_name_dictionary_seal(&dictionary) == VP_RESULT_OK,
          "sealing is idempotent");
    CHECK(vp_name_dictionary_lookup(&dictionary,
                                     VP_METRIC_FREE_USER_BYTES, &view) ==
                  VP_RESULT_OK &&
              view.name_id == VP_METRIC_FREE_USER_BYTES &&
              view.flags == VP_NAME_FLAG_BUILTIN &&
              view_equals(&view, "vita.memory.free_user_bytes"),
          "built-in metric resolves without caller storage");
    CHECK(vp_name_dictionary_register(&dictionary, "late", NULL) ==
              VP_ERROR_SEALED,
          "sealed dictionary rejects registration");
    vp_name_dictionary_deinit(&dictionary);
    CHECK(vp_name_dictionary_get_stats(&dictionary, &stats) ==
              VP_ERROR_NOT_INITIALIZED,
          "deinitialized dictionary rejects access");
}

static void test_registration_bounds_and_collisions(void)
{
    struct vp_name_dictionary dictionary;
    struct vp_name_dictionary_stats stats;
    struct vp_name_entry entries[4];
    char text[64];
    char too_long[VP_NAME_MAX_LENGTH + 2u];
    uint32_t update_id;
    uint32_t duplicate_id;
    uint32_t collision_id;

    init_dictionary(&dictionary, entries, 4u, text, sizeof(text));
    CHECK(vp_name_dictionary_register(&dictionary, "update", &update_id) ==
                  VP_RESULT_OK &&
              update_id == vp_name_id("update"),
          "register a stable user name");
    CHECK(vp_name_dictionary_register(&dictionary, "update", &duplicate_id) ==
                  VP_RESULT_OK &&
              duplicate_id == update_id,
          "identical registration is idempotent");
    CHECK(vp_name_dictionary_get_stats(&dictionary, &stats) == VP_RESULT_OK &&
              stats.user_entries == 1u &&
              stats.text_used == sizeof("update"),
          "duplicate registration consumes no storage");

    CHECK(vp_name_id("costarring") == vp_name_id("liquid"),
          "known FNV-1a collision fixture remains valid");
    CHECK(vp_name_dictionary_register(&dictionary, "costarring",
                                       &collision_id) == VP_RESULT_OK,
          "register first colliding name");
    collision_id = UINT32_C(0xdeadbeef);
    CHECK(vp_name_dictionary_register(&dictionary, "liquid", &collision_id) ==
                  VP_ERROR_NAME_CONFLICT &&
              collision_id == 0u,
          "reject a different name with the same ID");
    CHECK(vp_name_dictionary_get_stats(&dictionary, &stats) == VP_RESULT_OK &&
              stats.user_entries == 2u,
          "collision failure leaves dictionary unchanged");

    memset(too_long, 'a', sizeof(too_long));
    too_long[sizeof(too_long) - 1u] = '\0';
    CHECK(vp_name_dictionary_register(&dictionary, too_long, NULL) ==
              VP_ERROR_INVALID_ARGUMENT,
          "reject names longer than the wire limit");
    CHECK(vp_name_dictionary_register(&dictionary, "", NULL) ==
              VP_ERROR_INVALID_ARGUMENT,
          "reject an empty dictionary name");
    CHECK(vp_name_dictionary_register(&dictionary, NULL, NULL) ==
              VP_ERROR_INVALID_ARGUMENT,
          "reject a NULL dictionary name");
    vp_name_dictionary_deinit(&dictionary);

    {
        struct vp_name_entry small_entries[1];
        char small_text[4];
        init_dictionary(&dictionary, small_entries, 1u, small_text,
                        sizeof(small_text));
        CHECK(vp_name_dictionary_register(&dictionary, "abc", NULL) ==
                  VP_RESULT_OK,
              "exact text capacity succeeds");
        CHECK(vp_name_dictionary_register(&dictionary, "d", NULL) ==
                  VP_ERROR_CAPACITY,
              "entry capacity is bounded");
        CHECK(vp_name_dictionary_get_stats(&dictionary, &stats) ==
                      VP_RESULT_OK &&
                  stats.user_entries == 1u && stats.text_used == 4u,
              "entry capacity failure does not mutate state");
    }
    {
        struct vp_name_entry small_entries[2];
        char small_text[3];
        init_dictionary(&dictionary, small_entries, 2u, small_text,
                        sizeof(small_text));
        CHECK(vp_name_dictionary_register(&dictionary, "abc", NULL) ==
                  VP_ERROR_CAPACITY,
              "text capacity includes the terminator");
        CHECK(vp_name_dictionary_get_stats(&dictionary, &stats) ==
                      VP_RESULT_OK &&
                  stats.user_entries == 0u && stats.text_used == 0u,
              "text capacity failure does not mutate state");
    }
}

static void build_test_dictionary(struct vp_name_dictionary* dictionary,
                                  struct vp_name_entry entries[8],
                                  char text[128], uint32_t* update_id,
                                  uint32_t* frame_id,
                                  uint32_t* draw_id)
{
    init_dictionary(dictionary, entries, 8u, text, 128u);
    CHECK(vp_name_dictionary_register(dictionary, "update", update_id) ==
              VP_RESULT_OK,
          "register update name");
    CHECK(vp_name_dictionary_register(dictionary, "main frame", frame_id) ==
              VP_RESULT_OK,
          "register frame name");
    CHECK(vp_name_dictionary_register(dictionary, "draw calls", draw_id) ==
              VP_RESULT_OK,
          "register counter name");
    CHECK(vp_name_dictionary_seal(dictionary) == VP_RESULT_OK,
          "seal test dictionary");
}

static void test_snapshot_and_wire_round_trip(void)
{
    struct vp_name_dictionary dictionary;
    struct vp_name_entry entries[8];
    struct vp_name_dictionary_stats stats;
    struct vp_name_wire_info info;
    struct vp_name_wire_cursor cursor;
    struct vp_name_view view;
    char text[128];
    uint8_t wire[1024];
    uint8_t short_output[1024];
    size_t required = 0u;
    size_t written = 0u;
    uint32_t update_id;
    uint32_t frame_id;
    uint32_t draw_id;
    uint32_t previous = 0u;
    uint32_t seen = 0u;
    uint32_t index;
    int result;

    build_test_dictionary(&dictionary, entries, text, &update_id, &frame_id,
                          &draw_id);
    CHECK(vp_name_dictionary_get_stats(&dictionary, &stats) == VP_RESULT_OK &&
              stats.user_entries == 3u && stats.sealed == 1u,
          "sealed dictionary snapshot stats");
    for (index = 0u; index < stats.total_entries; ++index) {
        CHECK(vp_name_dictionary_entry_at(&dictionary, index, &view) ==
                  VP_RESULT_OK,
              "snapshot entry is readable");
        CHECK(index == 0u || view.name_id > previous,
              "snapshot entries are sorted and unique");
        previous = view.name_id;
    }
    CHECK(vp_name_dictionary_entry_at(&dictionary, stats.total_entries,
                                       &view) == VP_ERROR_NOT_FOUND,
          "snapshot bounds are enforced");
    CHECK(vp_name_dictionary_lookup(&dictionary, update_id, &view) ==
                  VP_RESULT_OK &&
              view_equals(&view, "update"),
          "sealed user name lookup");
    CHECK(vp_name_dictionary_lookup(&dictionary, UINT32_C(0x12345678),
                                     &view) == VP_ERROR_NOT_FOUND &&
              view.name == NULL,
          "unknown in-memory name remains unresolved");

    CHECK(vp_name_dictionary_wire_size(&dictionary, &required) ==
                  VP_RESULT_OK &&
              required <= sizeof(wire),
          "compute bounded dictionary wire size");
    memset(short_output, 0xa5, sizeof(short_output));
    CHECK(vp_encode_name_dictionary_le(&dictionary, short_output,
                                        required - 1u, &written) ==
                  VP_ERROR_BUFFER_TOO_SMALL &&
              written == required,
          "one-byte-short output reports required size");
    for (index = 0u; index < sizeof(short_output); ++index)
        CHECK(short_output[index] == 0xa5,
              "short encoding leaves output untouched");

    memset(wire, 0xcc, sizeof(wire));
    CHECK(vp_encode_name_dictionary_le(&dictionary, wire, required,
                                        &written) == VP_RESULT_OK &&
              written == required,
          "encode exact-capacity name block");
    CHECK(read_u32_le(wire) == VP_NAME_WIRE_MAGIC &&
              read_u16_le(wire + 4u) == VP_NAME_WIRE_VERSION &&
              read_u16_le(wire + 6u) == VP_NAME_WIRE_HEADER_SIZE &&
              read_u16_le(wire + 8u) == VP_NAME_WIRE_ENTRY_HEADER_SIZE &&
              read_u16_le(wire + 10u) ==
                  VP_NAME_WIRE_FLAG_LITTLE_ENDIAN &&
              read_u32_le(wire + 12u) == stats.total_entries &&
              read_u32_le(wire + 16u) == required,
          "name block header is explicit little endian");

    /* Extra trailing bytes may hold the VPRF event stream; total_size locates
       its start. */
    CHECK(vp_name_wire_validate_le(wire, sizeof(wire), &info) ==
                  VP_RESULT_OK &&
              info.entry_count == stats.total_entries &&
              info.total_size == required &&
              info.version == VP_NAME_WIRE_VERSION,
          "receiver validates a prefixed name block");
    CHECK(vp_name_wire_cursor_init(&cursor, wire, sizeof(wire), NULL) ==
              VP_RESULT_OK,
          "initialize allocation-free receiver cursor");
    previous = 0u;
    while ((result = vp_name_wire_cursor_next(&cursor, &view)) ==
           VP_RESULT_OK) {
        CHECK(seen == 0u || view.name_id > previous,
              "wire entries are sorted and unique");
        previous = view.name_id;
        ++seen;
    }
    CHECK(result == VP_RESULT_END && seen == stats.total_entries,
          "receiver cursor visits every entry and terminates");
    CHECK(vp_name_wire_cursor_next(&cursor, &view) == VP_RESULT_END,
          "receiver cursor end is stable");

    CHECK(vp_name_wire_lookup_le(wire, required, frame_id, &view) ==
                  VP_RESULT_OK &&
              view_equals(&view, "main frame"),
          "wire receiver resolves a zone ID");
    CHECK(vp_name_wire_lookup_le(wire, required, draw_id, &view) ==
                  VP_RESULT_OK &&
              view_equals(&view, "draw calls"),
          "wire receiver resolves a counter ID");
    CHECK(vp_name_wire_lookup_le(wire, required,
                                 VP_METRIC_THREAD_RUN_CLOCKS, &view) ==
                  VP_RESULT_OK &&
              view.flags == VP_NAME_FLAG_BUILTIN &&
              view_equals(&view, "vita.thread.run_clocks"),
          "wire receiver resolves built-in sample IDs");
    CHECK(vp_name_wire_lookup_le(wire, required, UINT32_C(0x12345678),
                                 &view) == VP_ERROR_NOT_FOUND &&
              view.name == NULL,
          "wire receiver reports an unknown ID");
}

static void test_wire_rejects_corruption_and_truncation(void)
{
    struct vp_name_dictionary dictionary;
    struct vp_name_entry entries[8];
    char text[128];
    uint8_t wire[1024];
    uint8_t damaged[1024];
    size_t required;
    size_t written;
    size_t length;
    uint32_t update_id;
    uint32_t frame_id;
    uint32_t draw_id;
    uint32_t offset;
    uint32_t index;
    uint32_t padded;

    build_test_dictionary(&dictionary, entries, text, &update_id, &frame_id,
                          &draw_id);
    CHECK(vp_name_dictionary_wire_size(&dictionary, &required) ==
              VP_RESULT_OK,
          "compute corruption fixture size");
    CHECK(vp_encode_name_dictionary_le(&dictionary, wire, sizeof(wire),
                                        &written) == VP_RESULT_OK &&
              written == required,
          "encode corruption fixture");

    for (length = 0u; length < required; ++length) {
        CHECK(vp_name_wire_validate_le(wire, length, NULL) != VP_RESULT_OK,
              "every truncated prefix is rejected");
    }
    memcpy(damaged, wire, required);
    damaged[0] ^= 1u;
    CHECK(vp_name_wire_validate_le(damaged, required, NULL) ==
              VP_ERROR_MALFORMED,
          "bad dictionary magic is rejected");
    memcpy(damaged, wire, required);
    damaged[4] = (uint8_t)(VP_NAME_WIRE_VERSION + 1u);
    CHECK(vp_name_wire_validate_le(damaged, required, NULL) ==
              VP_ERROR_UNSUPPORTED,
          "unknown dictionary version is rejected");
    memcpy(damaged, wire, required);
    damaged[20] = 1u;
    CHECK(vp_name_wire_validate_le(damaged, required, NULL) ==
              VP_ERROR_MALFORMED,
          "nonzero reserved header bytes are rejected");

    /* Find a padded entry, then verify both hash protection and deterministic
       zero padding. */
    offset = VP_NAME_WIRE_HEADER_SIZE;
    for (index = 0u; index < read_u32_le(wire + 12u); ++index) {
        uint16_t name_length = read_u16_le(wire + offset + 4u);
        padded = ((uint32_t)name_length + 3u) & ~UINT32_C(3);
        if (index == 0u) {
            memcpy(damaged, wire, required);
            damaged[offset + VP_NAME_WIRE_ENTRY_HEADER_SIZE] ^= 1u;
            CHECK(vp_name_wire_validate_le(damaged, required, NULL) ==
                      VP_ERROR_MALFORMED,
                  "name bytes are protected by their stable ID");
        }
        if (padded != name_length) {
            memcpy(damaged, wire, required);
            damaged[offset + VP_NAME_WIRE_ENTRY_HEADER_SIZE + name_length] =
                1u;
            CHECK(vp_name_wire_validate_le(damaged, required, NULL) ==
                      VP_ERROR_MALFORMED,
                  "nonzero wire padding is rejected");
            break;
        }
        offset += VP_NAME_WIRE_ENTRY_HEADER_SIZE + padded;
    }
    CHECK(index < read_u32_le(wire + 12u),
          "corruption fixture includes a padded name");

    /* Locate a built-in record and strip its flag. */
    offset = VP_NAME_WIRE_HEADER_SIZE;
    for (index = 0u; index < read_u32_le(wire + 12u); ++index) {
        uint16_t name_length = read_u16_le(wire + offset + 4u);
        padded = ((uint32_t)name_length + 3u) & ~UINT32_C(3);
        if (read_u32_le(wire + offset) == VP_METRIC_FREE_USER_BYTES) {
            memcpy(damaged, wire, required);
            damaged[offset + 6u] = 0u;
            damaged[offset + 7u] = 0u;
            CHECK(vp_name_wire_validate_le(damaged, required, NULL) ==
                      VP_ERROR_MALFORMED,
                  "built-in identity flag is required");
            break;
        }
        offset += VP_NAME_WIRE_ENTRY_HEADER_SIZE + padded;
    }
    CHECK(index < read_u32_le(wire + 12u),
          "corruption fixture contains built-in names");
}

#define LOOKUP_THREADS 4u
#define LOOKUP_ITERATIONS 10000u

struct lookup_args {
    const struct vp_name_dictionary* dictionary;
    const uint8_t* wire;
    size_t wire_size;
    uint32_t name_id;
    const char* expected;
    int result;
};

static void lookup_body(struct lookup_args* args)
{
    uint32_t i;
    args->result = 0;
    for (i = 0u; i < LOOKUP_ITERATIONS; ++i) {
        struct vp_name_view memory_view;
        struct vp_name_view wire_view;
        if (vp_name_dictionary_lookup(args->dictionary, args->name_id,
                                      &memory_view) != VP_RESULT_OK ||
            !view_equals(&memory_view, args->expected) ||
            vp_name_wire_lookup_le(args->wire, args->wire_size, args->name_id,
                                   &wire_view) != VP_RESULT_OK ||
            !view_equals(&wire_view, args->expected)) {
            args->result = -1;
            return;
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI lookup_thread(LPVOID user)
{
    lookup_body((struct lookup_args*)user);
    return 0;
}
#else
static void* lookup_thread(void* user)
{
    lookup_body((struct lookup_args*)user);
    return NULL;
}
#endif

static void test_concurrent_sealed_reads(void)
{
    struct vp_name_dictionary dictionary;
    struct vp_name_entry entries[8];
    struct lookup_args args[LOOKUP_THREADS];
    char text[128];
    uint8_t wire[1024];
    size_t written;
    uint32_t update_id;
    uint32_t frame_id;
    uint32_t draw_id;
    uint32_t i;
#if defined(_WIN32)
    HANDLE threads[LOOKUP_THREADS];
#else
    pthread_t threads[LOOKUP_THREADS];
#endif

    build_test_dictionary(&dictionary, entries, text, &update_id, &frame_id,
                          &draw_id);
    CHECK(vp_encode_name_dictionary_le(&dictionary, wire, sizeof(wire),
                                        &written) == VP_RESULT_OK,
          "encode concurrent lookup fixture");
    for (i = 0u; i < LOOKUP_THREADS; ++i) {
        args[i].dictionary = &dictionary;
        args[i].wire = wire;
        args[i].wire_size = written;
        args[i].name_id = (i & 1u) != 0u ? update_id : frame_id;
        args[i].expected = (i & 1u) != 0u ? "update" : "main frame";
        args[i].result = -1;
#if defined(_WIN32)
        threads[i] = CreateThread(NULL, 0, lookup_thread, &args[i], 0, NULL);
        CHECK(threads[i] != NULL, "create dictionary lookup thread");
#else
        CHECK(pthread_create(&threads[i], NULL, lookup_thread, &args[i]) == 0,
              "create dictionary lookup thread");
#endif
    }
    for (i = 0u; i < LOOKUP_THREADS; ++i) {
#if defined(_WIN32)
        if (threads[i] != NULL) {
            CHECK(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0,
                  "join dictionary lookup thread");
            CloseHandle(threads[i]);
        }
#else
        CHECK(pthread_join(threads[i], NULL) == 0,
              "join dictionary lookup thread");
#endif
        CHECK(args[i].result == 0, "sealed concurrent lookup stays stable");
    }
    vp_name_dictionary_deinit(&dictionary);
}

int main(void)
{
    test_validation_and_lifecycle();
    test_registration_bounds_and_collisions();
    test_snapshot_and_wire_round_trip();
    test_wire_rejects_corruption_and_truncation();
    test_concurrent_sealed_reads();

    if (failures != 0) {
        fprintf(stderr, "%d profiler name test(s) failed\n", failures);
        return 1;
    }
    puts("vitaprofiler names: all native tests passed");
    return 0;
}

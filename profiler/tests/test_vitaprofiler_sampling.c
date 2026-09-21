#include "vitaprofiler_sampling.h"

#include <stdio.h>
#include <string.h>

static int failures;

struct legacy_sampler_layout {
    const struct vp_sample_provider* provider;
    struct vp_sample_frame* frames;
    uint64_t active_token;
    int32_t last_provider_error;
    uint32_t frame_capacity;
    uint32_t max_depth;
    uint32_t capabilities;
    uint32_t active;
    uint32_t release_pending;
    uint32_t initialized;
};

_Static_assert(sizeof(struct vp_sampler_config) ==
                   sizeof(void*) + sizeof(uint32_t) * 4u,
               "legacy sampler config layout changed");
_Static_assert(sizeof(struct vp_sample_provider) ==
                   sizeof(uint32_t) * 2u + sizeof(void*) * 4u,
               "legacy sample provider layout changed");
_Static_assert(sizeof(struct vp_sampler) ==
                   sizeof(struct legacy_sampler_layout),
               "legacy sampler layout changed");
_Static_assert(offsetof(struct vp_sampler, initialized) ==
                   offsetof(struct legacy_sampler_layout, initialized),
               "legacy sampler field offsets changed");
_Static_assert(sizeof(struct vp_sampler_status) == sizeof(uint32_t) * 5u,
               "legacy sampler status layout changed");
_Static_assert(VP_SAMPLE_CAP_ALL == ((UINT32_C(1) << 7) - 1u),
               "legacy sampler capability mask changed");

#define CHECK(condition, message)                                           \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__);   \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

static int legacy_begin(void* user,
                        const struct vp_sample_target* target,
                        uint32_t max_depth, uint64_t* lease_token,
                        struct vp_sample_capture* capture)
{
    (void)user;
    (void)max_depth;
    *lease_token = 1u;
    capture->cursor.pc = UINT32_C(0x81000000);
    capture->thread_id = target->kind == VP_SAMPLE_TARGET_CURRENT ? 1 : 0;
    return VP_RESULT_OK;
}

static int legacy_end(void* user, uint64_t lease_token)
{
    (void)user;
    return lease_token == 1u ? VP_RESULT_OK : VP_ERROR_STATE;
}

static void test_legacy_abi_layout_and_canaries(void)
{
    struct guarded_sampler {
        uint8_t before[16];
        struct vp_sampler value;
        uint8_t after[16];
    } sampler;
    struct guarded_status {
        uint8_t before[16];
        struct vp_sampler_status value;
        uint8_t after[16];
    } status;
    struct vp_sample_frame frame;
    struct vp_sample sample;
    const struct vp_sample_provider provider = {
        VP_SAMPLE_PROVIDER_ABI_VERSION,
        VP_SAMPLE_CAP_CURRENT_THREAD_PC |
            VP_SAMPLE_CAP_FOREIGN_THREAD_PC |
            VP_SAMPLE_CAP_STABLE_IDENTITY |
            VP_SAMPLE_CAP_FOREIGN_CONTEXT_CONFIDENCE,
        legacy_begin, NULL, legacy_end, NULL};
    const struct vp_sampler_config config = {
        &frame, 1u, 1u, VP_SAMPLE_CAP_CURRENT_THREAD_PC, 0u};
    const struct vp_sampler_config foreign_config = {
        &frame, 1u, 1u,
        VP_SAMPLE_CAP_FOREIGN_THREAD_PC |
            VP_SAMPLE_CAP_STABLE_IDENTITY |
            VP_SAMPLE_CAP_FOREIGN_CONTEXT_CONFIDENCE,
        0u};
    uint8_t canary[16];
    memset(&sampler, 0, sizeof(sampler));
    memset(&status, 0, sizeof(status));
    memset(canary, 0xa5, sizeof(canary));
    memset(sampler.before, 0xa5, sizeof(sampler.before));
    memset(sampler.after, 0xa5, sizeof(sampler.after));
    memset(status.before, 0xa5, sizeof(status.before));
    memset(status.after, 0xa5, sizeof(status.after));

    CHECK(vp_sampler_init(&sampler.value, &provider, &config) ==
              VP_RESULT_OK &&
              vp_sampler_sample_current(&sampler.value, &sample) ==
                  VP_RESULT_OK &&
              vp_sampler_get_status(&sampler.value, &status.value) ==
                  VP_RESULT_OK &&
              vp_sampler_deinit(&sampler.value) == VP_RESULT_OK,
          "legacy ABI entry points remain functional");
    CHECK(vp_sampler_init(
              &sampler.value, &provider, &foreign_config) ==
              VP_ERROR_UNSUPPORTED,
          "legacy provider may advertise foreign support but cannot request it");
    CHECK(memcmp(sampler.before, canary, sizeof(canary)) == 0 &&
              memcmp(sampler.after, canary, sizeof(canary)) == 0 &&
              memcmp(status.before, canary, sizeof(canary)) == 0 &&
              memcmp(status.after, canary, sizeof(canary)) == 0,
          "legacy sampler and status writes preserve caller canaries");
}

#define vp_sampler vp_sampler_v2
#define vp_sampler_config vp_sampler_config_v2
#define vp_sampler_status vp_sampler_status_v2
#define vp_sampler_init vp_sampler_init_v2
#define vp_sampler_deinit vp_sampler_deinit_v2
#define vp_sampler_sample_current vp_sampler_sample_current_v2
#define vp_sampler_sample_foreign vp_sampler_sample_foreign_v2
#define vp_sampler_retry_release vp_sampler_retry_release_v2
#define vp_sampler_get_status vp_sampler_get_status_v2

struct fake_provider {
    struct vp_sample_provider_v2 provider;
    struct vp_sample_cursor cursors[VP_SAMPLE_MAX_FRAMES + 1u];
    uint32_t cursor_count;
    uint32_t next_index;
    uint32_t begin_calls;
    uint32_t next_calls;
    uint32_t end_calls;
    uint32_t validate_calls;
    uint32_t fail_validate_call;
    uint32_t fail_next_call;
    int begin_result;
    int next_result;
    int end_result;
    int validate_result;
    int32_t current_thread_id;
    uint64_t observed_identity;
};

static int fake_validate(void* user, uint64_t lease_token,
                         const struct vp_sample_target* target)
{
    struct fake_provider* fake = (struct fake_provider*)user;
    if (lease_token != UINT64_C(0x123456789) ||
        target->kind != VP_SAMPLE_TARGET_FOREIGN)
        return VP_ERROR_STATE;
    ++fake->validate_calls;
    if (fake->validate_calls == fake->fail_validate_call)
        return fake->validate_result;
    return VP_RESULT_OK;
}

static int fake_begin(void* user, const struct vp_sample_target* target,
                      uint32_t max_depth, uint64_t* lease_token,
                      struct vp_sample_capture* capture)
{
    struct fake_provider* fake = (struct fake_provider*)user;
    (void)max_depth;
    ++fake->begin_calls;
    fake->next_index = 1u;
    if (fake->begin_result != VP_RESULT_OK)
        return fake->begin_result;
    *lease_token = UINT64_C(0x123456789);
    capture->cursor = fake->cursors[0];
    capture->thread_id = target->kind == VP_SAMPLE_TARGET_CURRENT
                             ? fake->current_thread_id
                             : target->thread_id;
    capture->identity = target->kind == VP_SAMPLE_TARGET_CURRENT
                            ? 0u
                            : (fake->observed_identity != 0u
                                   ? fake->observed_identity
                                   : target->identity);
    return VP_RESULT_OK;
}

static int fake_next(void* user, uint64_t lease_token,
                     const struct vp_sample_cursor* current,
                     struct vp_sample_cursor* next)
{
    struct fake_provider* fake = (struct fake_provider*)user;
    (void)current;
    if (lease_token != UINT64_C(0x123456789))
        return VP_ERROR_STATE;
    ++fake->next_calls;
    if (fake->fail_next_call == fake->next_calls)
        return fake->next_result;
    if (fake->next_index >= fake->cursor_count)
        return VP_RESULT_END;
    *next = fake->cursors[fake->next_index++];
    return VP_RESULT_OK;
}

static int fake_end(void* user, uint64_t lease_token)
{
    struct fake_provider* fake = (struct fake_provider*)user;
    ++fake->end_calls;
    if (lease_token != UINT64_C(0x123456789))
        return VP_ERROR_STATE;
    return fake->end_result;
}

static struct fake_provider make_fake(uint32_t capabilities,
                                      uint32_t cursor_count)
{
    struct fake_provider fake;
    memset(&fake, 0, sizeof(fake));
    fake.provider.base.abi_version = VP_SAMPLE_PROVIDER_ABI_VERSION_V2;
    fake.provider.base.capabilities = capabilities;
    fake.provider.base.begin_sample = fake_begin;
    fake.provider.base.next_frame = fake_next;
    fake.provider.base.end_sample = fake_end;
    fake.provider.base.user = &fake;
    fake.provider.validate_sample = fake_validate;
    fake.provider.max_callback_us = 500u;
    fake.current_thread_id = 17;
    fake.cursor_count = cursor_count;
    for (uint32_t i = 0u; i < cursor_count; ++i) {
        fake.cursors[i].pc = UINT32_C(0x81001000) + i * 4u;
        fake.cursors[i].sp = UINT32_C(0x82002000) + i * 16u;
        fake.cursors[i].frame_pointer =
            UINT32_C(0x82002008) + i * 16u;
        fake.cursors[i].flags =
            i == 0u ? VP_SAMPLE_FRAME_FLAG_THUMB : 0u;
    }
    return fake;
}

static void fix_fake_user(struct fake_provider* fake)
{
    fake->provider.base.user = fake;
}

static struct vp_sampler_config make_config(
    struct vp_sample_frame* frames, uint32_t capacity, uint32_t depth,
    uint32_t capabilities)
{
    const struct vp_sampler_config config = {
        frames, capacity, depth, capabilities, 1000u, 0u};
    return config;
}

static void test_validation_and_capabilities(void)
{
    struct vp_sampler sampler;
    struct vp_sample_frame frames[4];
    struct fake_provider fake =
        make_fake(VP_SAMPLE_CAP_CURRENT_THREAD_PC, 1u);
    struct vp_sampler_config config = make_config(
        frames, 4u, 4u, VP_SAMPLE_CAP_CURRENT_THREAD_PC);
    struct vp_sample sample;
    memset(&sampler, 0, sizeof(sampler));
    fix_fake_user(&fake);

    CHECK(vp_sampler_init(&sampler, NULL, &config) ==
              VP_ERROR_UNSUPPORTED,
          "missing provider fails closed");
    fake.provider.base.abi_version = 99u;
    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_ERROR_UNSUPPORTED,
          "provider ABI mismatch is unsupported");
    fake.provider.base.abi_version = VP_SAMPLE_PROVIDER_ABI_VERSION_V2;
    config.required_capabilities =
        VP_SAMPLE_CAP_FOREIGN_THREAD_PC |
        VP_SAMPLE_CAP_STABLE_IDENTITY |
        VP_SAMPLE_CAP_FOREIGN_CONTEXT_CONFIDENCE |
        VP_SAMPLE_CAP_EXIT_AWARE_IDENTITY |
        VP_SAMPLE_CAP_BOUNDED_CALLBACKS |
        VP_SAMPLE_CAP_RELEASE_ROLLBACK;
    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_ERROR_UNSUPPORTED,
          "unadvertised foreign sampling is unsupported");
    fake.provider.base.capabilities =
        VP_SAMPLE_CAP_FOREIGN_THREAD_PC |
        VP_SAMPLE_CAP_STABLE_IDENTITY;
    config.required_capabilities = fake.provider.base.capabilities;
    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "foreign provider must advertise context confidence");
    fake.provider.base.capabilities =
        VP_SAMPLE_CAP_CURRENT_THREAD_PC |
        VP_SAMPLE_CAP_CURRENT_THREAD_STACK;
    config.required_capabilities = fake.provider.base.capabilities;
    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "stack provider must advertise bounded reads");
    fake.provider.base.capabilities = VP_SAMPLE_CAP_CURRENT_THREAD_PC;
    config = make_config(frames, 4u, VP_SAMPLE_MAX_FRAMES + 1u,
                         VP_SAMPLE_CAP_CURRENT_THREAD_PC);
    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "hard frame-depth bound is enforced");
    config = make_config(frames, VP_SAMPLE_MAX_FRAMES + 1u, 4u,
                         VP_SAMPLE_CAP_CURRENT_THREAD_PC);
    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_ERROR_INVALID_ARGUMENT,
          "hard frame-storage bound is enforced");

    config = make_config(frames, 4u, 4u,
                         VP_SAMPLE_CAP_CURRENT_THREAD_PC);
    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_RESULT_OK,
          "current-PC provider initializes");
    fake.cursors[0].sp = 0u;
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_ERROR_UNSUPPORTED &&
              fake.begin_calls == 0u,
          "current provider never widens to a foreign target");
    CHECK(vp_sampler_sample_current(&sampler, &sample) == VP_RESULT_OK &&
              sample.frame_count == 1u &&
              sample.stop_reason == VP_SAMPLE_STOP_PC_ONLY &&
              sample.thread_id == 17 &&
              sample.frames[0].pc == UINT32_C(0x81001000) &&
              sample.frames[0].sp == 0u,
          "current-PC sample needs no unwind stack pointer");
    CHECK(vp_sampler_deinit(&sampler) == VP_RESULT_OK,
          "clean sampler deinitializes");
}

static void test_depth_bound_and_canaries(void)
{
    struct {
        struct vp_sample_frame before;
        struct vp_sample_frame frames[3];
        struct vp_sample_frame after;
    } storage;
    struct vp_sampler sampler;
    struct vp_sample sample;
    struct fake_provider fake = make_fake(
        VP_SAMPLE_CAP_CURRENT_THREAD_PC |
            VP_SAMPLE_CAP_CURRENT_THREAD_STACK |
            VP_SAMPLE_CAP_BOUNDED_STACK_READ,
        8u);
    struct vp_sampler_config config = make_config(
        storage.frames, 3u, 3u,
        VP_SAMPLE_CAP_CURRENT_THREAD_PC |
            VP_SAMPLE_CAP_CURRENT_THREAD_STACK |
            VP_SAMPLE_CAP_BOUNDED_STACK_READ);
    memset(&sampler, 0, sizeof(sampler));
    memset(&storage, 0xa5, sizeof(storage));
    fix_fake_user(&fake);

    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_RESULT_OK,
          "bounded stack sampler initializes");
    memset(&storage.before, 0x5a, sizeof(storage.before));
    memset(&storage.after, 0x5a, sizeof(storage.after));
    CHECK(vp_sampler_sample_current(&sampler, &sample) ==
              VP_RESULT_PARTIAL &&
              sample.frame_count == 3u &&
              sample.stop_reason == VP_SAMPLE_STOP_DEPTH_LIMIT &&
              fake.next_calls == 2u,
          "deep unwind stops exactly at configured depth");
    {
        struct vp_sample_frame canary;
        memset(&canary, 0x5a, sizeof(canary));
        CHECK(memcmp(&storage.before, &canary, sizeof(canary)) == 0 &&
                  memcmp(&storage.after, &canary, sizeof(canary)) == 0,
              "bounded unwind preserves adjacent storage");
    }
}

static void test_partial_unwind_and_output_stability(void)
{
    struct vp_sampler sampler;
    struct vp_sample_frame frames[4];
    struct vp_sample_frame saved_frames[4];
    struct vp_sample sample;
    struct vp_sample saved_sample;
    struct fake_provider fake = make_fake(
        VP_SAMPLE_CAP_CURRENT_THREAD_PC |
            VP_SAMPLE_CAP_CURRENT_THREAD_STACK |
            VP_SAMPLE_CAP_BOUNDED_STACK_READ,
        3u);
    struct vp_sampler_config config = make_config(
        frames, 4u, 4u,
        VP_SAMPLE_CAP_CURRENT_THREAD_PC |
            VP_SAMPLE_CAP_CURRENT_THREAD_STACK |
            VP_SAMPLE_CAP_BOUNDED_STACK_READ);
    memset(&sampler, 0, sizeof(sampler));
    fix_fake_user(&fake);
    fake.fail_next_call = 2u;
    fake.next_result = VP_ERROR_IO;

    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_RESULT_OK,
          "partial-unwind sampler initializes");
    CHECK(vp_sampler_sample_current(&sampler, &sample) ==
              VP_RESULT_PARTIAL &&
              sample.frame_count == 2u &&
              sample.stop_reason == VP_SAMPLE_STOP_PROVIDER_ERROR &&
              sample.provider_error == VP_ERROR_IO &&
              frames[2].pc == 0u && frames[3].pc == 0u,
          "partial read preserves only the validated prefix");

    fake.fail_next_call = 0u;
    fake.next_calls = 0u;
    CHECK(vp_sampler_sample_current(&sampler, &sample) == VP_RESULT_OK &&
              sample.frame_count == 3u &&
              sample.stop_reason == VP_SAMPLE_STOP_COMPLETE,
          "complete unwind succeeds after partial provider result");
    saved_sample = sample;
    memcpy(saved_frames, frames, sizeof(frames));
    fake.next_calls = 0u;
    CHECK(vp_sampler_sample_current(&sampler, &sample) == VP_RESULT_OK &&
              memcmp(&sample, &saved_sample, sizeof(sample)) == 0 &&
              memcmp(frames, saved_frames, sizeof(frames)) == 0,
          "identical provider input has stable bounded output");

    fake.begin_result = VP_ERROR_PLATFORM;
    memset(frames, 0xa5, sizeof(frames));
    CHECK(vp_sampler_sample_current(&sampler, &sample) ==
              VP_ERROR_PLATFORM &&
              sample.frame_count == 0u && sample.thread_id == 0 &&
              frames[0].pc == 0u && frames[3].pc == 0u,
          "failed begin clears prior output instead of returning stale data");
}

static void test_foreign_identity_and_release_quarantine(void)
{
    const uint32_t capabilities =
        VP_SAMPLE_CAP_FOREIGN_THREAD_PC |
        VP_SAMPLE_CAP_FOREIGN_THREAD_STACK |
        VP_SAMPLE_CAP_STABLE_IDENTITY |
        VP_SAMPLE_CAP_BOUNDED_STACK_READ |
        VP_SAMPLE_CAP_FOREIGN_CONTEXT_CONFIDENCE |
        VP_SAMPLE_CAP_EXIT_AWARE_IDENTITY |
        VP_SAMPLE_CAP_FAULT_CONTAINED_READ |
        VP_SAMPLE_CAP_BOUNDED_CALLBACKS |
        VP_SAMPLE_CAP_RELEASE_ROLLBACK;
    struct vp_sampler sampler;
    struct vp_sample_frame frames[4];
    struct vp_sample sample;
    struct vp_sampler_status status;
    struct fake_provider fake = make_fake(capabilities, 2u);
    struct vp_sampler_config config =
        make_config(frames, 4u, 4u, capabilities);
    memset(&sampler, 0, sizeof(sampler));
    fix_fake_user(&fake);

    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_RESULT_OK,
          "foreign sampler requires stable identity capability");
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_ERROR_UNSUPPORTED &&
              sample.frame_count == 0u && fake.end_calls == 1u,
          "foreign sample requires confident register context");
    fake.cursors[0].flags |=
        VP_SAMPLE_FRAME_FLAG_CONTEXT_CONFIDENT;
    fake.observed_identity = UINT64_C(0x2222);
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_ERROR_STALE_IDENTITY &&
              sample.frame_count == 0u && fake.end_calls == 2u,
          "stale identity is rejected after releasing provider lease");

    fake.observed_identity = 0u;
    fake.fail_next_call = 1u;
    fake.next_result = VP_ERROR_STALE_IDENTITY;
    fake.next_calls = 0u;
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_RESULT_PARTIAL &&
              sample.frame_count == 1u &&
              sample.stop_reason == VP_SAMPLE_STOP_STALE_IDENTITY &&
              sample.provider_error == VP_ERROR_STALE_IDENTITY,
          "mid-unwind identity loss preserves only validated PC");

    fake.fail_next_call = 0u;
    fake.next_calls = 0u;
    fake.end_result = VP_ERROR_IO;
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_ERROR_RELEASE_REQUIRED &&
              sample.stop_reason == VP_SAMPLE_STOP_RELEASE_ERROR,
          "release failure quarantines sampler");
    CHECK(vp_sampler_get_status(&sampler, &status) == VP_RESULT_OK &&
              status.active == 1u && status.release_pending == 1u &&
              status.last_provider_error == VP_ERROR_IO,
          "release obligation is externally visible");
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_ERROR_RELEASE_REQUIRED &&
              vp_sampler_deinit(&sampler) == VP_ERROR_RELEASE_REQUIRED,
          "quarantined sampler rejects reuse and teardown");
    fake.end_result = VP_RESULT_OK;
    CHECK(vp_sampler_retry_release(&sampler) == VP_RESULT_OK &&
              vp_sampler_deinit(&sampler) == VP_RESULT_OK,
          "successful retry discharges ownership before teardown");
}

static void test_foreign_exit_fault_timeout_and_uid_reuse(void)
{
    const uint32_t capabilities =
        VP_SAMPLE_CAP_FOREIGN_THREAD_PC |
        VP_SAMPLE_CAP_FOREIGN_THREAD_STACK |
        VP_SAMPLE_CAP_STABLE_IDENTITY |
        VP_SAMPLE_CAP_BOUNDED_STACK_READ |
        VP_SAMPLE_CAP_FOREIGN_CONTEXT_CONFIDENCE |
        VP_SAMPLE_CAP_EXIT_AWARE_IDENTITY |
        VP_SAMPLE_CAP_FAULT_CONTAINED_READ |
        VP_SAMPLE_CAP_BOUNDED_CALLBACKS |
        VP_SAMPLE_CAP_RELEASE_ROLLBACK;
    struct vp_sampler sampler;
    struct vp_sample_frame frames[4];
    struct vp_sample sample;
    struct fake_provider fake = make_fake(capabilities, 2u);
    struct vp_sampler_config config =
        make_config(frames, 4u, 4u, capabilities);
    memset(&sampler, 0, sizeof(sampler));
    fix_fake_user(&fake);
    fake.cursors[0].flags |= VP_SAMPLE_FRAME_FLAG_CONTEXT_CONFIDENT;

    fake.provider.max_callback_us = 2000u;
    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_ERROR_UNSUPPORTED,
          "foreign provider callback bound must fit caller watchdog");
    fake.provider.max_callback_us = 500u;
    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_RESULT_OK,
          "fully guarded foreign provider initializes");

    fake.observed_identity = UINT64_C(0x2222);
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_ERROR_STALE_IDENTITY &&
              sample.stop_reason == VP_SAMPLE_STOP_STALE_IDENTITY,
          "UID reuse with a different generation fails closed");
    fake.observed_identity = 0u;

    fake.validate_calls = 0u;
    fake.fail_validate_call = 1u;
    fake.validate_result = VP_RESULT_END;
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_ERROR_MALFORMED &&
              sample.frame_count == 0u &&
              sample.stop_reason == VP_SAMPLE_STOP_MALFORMED,
          "unexpected positive identity status fails closed");

    fake.validate_calls = 0u;
    fake.fail_validate_call = 1u;
    fake.validate_result = VP_ERROR_THREAD_EXITED;
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_ERROR_THREAD_EXITED &&
              sample.frame_count == 0u &&
              sample.stop_reason == VP_SAMPLE_STOP_THREAD_EXITED,
          "thread exit before first frame produces no stale sample");

    fake.validate_calls = 0u;
    fake.fail_validate_call = 2u;
    fake.validate_result = VP_ERROR_TIMEOUT;
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_RESULT_PARTIAL &&
              sample.frame_count == 1u &&
              sample.stop_reason == VP_SAMPLE_STOP_TIMEOUT &&
              sample.provider_error == VP_ERROR_TIMEOUT,
          "watchdog timeout preserves only the validated prefix");

    fake.validate_calls = 0u;
    fake.fail_validate_call = 0u;
    fake.fail_next_call = 1u;
    fake.next_calls = 0u;
    fake.next_result = VP_ERROR_READ_FAULT;
    CHECK(vp_sampler_sample_foreign(
              &sampler, 9, UINT64_C(0x1111), &sample) ==
              VP_RESULT_PARTIAL &&
              sample.frame_count == 1u &&
              sample.stop_reason == VP_SAMPLE_STOP_READ_FAULT &&
              sample.provider_error == VP_ERROR_READ_FAULT,
          "fault-contained read failure is explicit and bounded");
    CHECK(vp_sampler_deinit(&sampler) == VP_RESULT_OK,
          "guarded foreign sampler releases every failed attempt");
}

static void test_malformed_unwind_progress(void)
{
    struct vp_sampler sampler;
    struct vp_sample_frame frames[3];
    struct vp_sample sample;
    struct fake_provider fake = make_fake(
        VP_SAMPLE_CAP_CURRENT_THREAD_PC |
            VP_SAMPLE_CAP_CURRENT_THREAD_STACK |
            VP_SAMPLE_CAP_BOUNDED_STACK_READ,
        2u);
    struct vp_sampler_config config = make_config(
        frames, 3u, 3u,
        VP_SAMPLE_CAP_CURRENT_THREAD_PC |
            VP_SAMPLE_CAP_CURRENT_THREAD_STACK |
            VP_SAMPLE_CAP_BOUNDED_STACK_READ);
    memset(&sampler, 0, sizeof(sampler));
    fix_fake_user(&fake);
    fake.cursors[1].sp = fake.cursors[0].sp;

    CHECK(vp_sampler_init(&sampler, &fake.provider, &config) ==
              VP_RESULT_OK,
          "malformed-progress sampler initializes");
    CHECK(vp_sampler_sample_current(&sampler, &sample) ==
              VP_RESULT_PARTIAL &&
              sample.frame_count == 1u &&
              sample.stop_reason == VP_SAMPLE_STOP_MALFORMED &&
              sample.provider_error == VP_ERROR_MALFORMED,
          "non-progressing unwind fails closed with stable prefix");
}

int main(void)
{
    test_legacy_abi_layout_and_canaries();
    test_validation_and_capabilities();
    test_depth_bound_and_canaries();
    test_partial_unwind_and_output_stability();
    test_foreign_identity_and_release_quarantine();
    test_foreign_exit_fault_timeout_and_uid_reuse();
    test_malformed_unwind_progress();

    if (failures != 0) {
        fprintf(stderr, "%d sampling test(s) failed\n", failures);
        return 1;
    }
    puts("vitaprofiler sampling: all native tests passed");
    return 0;
}

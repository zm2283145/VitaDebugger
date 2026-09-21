#include "vitaprofiler_sampling.h"

#include <string.h>

#define VP_SAMPLER_MAGIC UINT32_C(0x5650534d)

static int vp_sampler_is_valid(const struct vp_sampler* sampler)
{
    return sampler != NULL && sampler->initialized == VP_SAMPLER_MAGIC &&
           sampler->provider != NULL && sampler->frames != NULL &&
           sampler->frame_capacity >= sampler->max_depth &&
           sampler->frame_capacity <= VP_SAMPLE_MAX_FRAMES &&
           sampler->max_depth != 0u;
}

static int vp_capabilities_valid(uint32_t capabilities,
                                 uint32_t provider_abi)
{
    uint32_t foreign;
    if (capabilities == 0u || (capabilities & ~VP_SAMPLE_CAP_ALL) != 0u)
        return 0;
    if ((capabilities & (VP_SAMPLE_CAP_CURRENT_THREAD_PC |
                         VP_SAMPLE_CAP_FOREIGN_THREAD_PC)) == 0u)
        return 0;
    if ((capabilities & VP_SAMPLE_CAP_CURRENT_THREAD_STACK) != 0u &&
        (capabilities & VP_SAMPLE_CAP_CURRENT_THREAD_PC) == 0u)
        return 0;
    if ((capabilities & VP_SAMPLE_CAP_FOREIGN_THREAD_STACK) != 0u &&
        (capabilities & VP_SAMPLE_CAP_FOREIGN_THREAD_PC) == 0u)
        return 0;
    if ((capabilities & (VP_SAMPLE_CAP_CURRENT_THREAD_STACK |
                         VP_SAMPLE_CAP_FOREIGN_THREAD_STACK)) != 0u &&
        (capabilities & VP_SAMPLE_CAP_BOUNDED_STACK_READ) == 0u)
        return 0;
    foreign = capabilities & (VP_SAMPLE_CAP_FOREIGN_THREAD_PC |
                              VP_SAMPLE_CAP_FOREIGN_THREAD_STACK);
    if (foreign != 0u) {
        const uint32_t required =
            VP_SAMPLE_CAP_STABLE_IDENTITY |
            VP_SAMPLE_CAP_FOREIGN_CONTEXT_CONFIDENCE |
            VP_SAMPLE_CAP_EXIT_AWARE_IDENTITY |
            VP_SAMPLE_CAP_BOUNDED_CALLBACKS |
            VP_SAMPLE_CAP_RELEASE_ROLLBACK;
        if (provider_abi != VP_SAMPLE_PROVIDER_ABI_VERSION_V2 ||
            (capabilities & required) != required)
            return 0;
        if ((capabilities & VP_SAMPLE_CAP_FOREIGN_THREAD_STACK) != 0u &&
            (capabilities & VP_SAMPLE_CAP_FAULT_CONTAINED_READ) == 0u)
            return 0;
    }
    return 1;
}

static uint32_t vp_sampler_stop_reason(int result)
{
    switch (result) {
    case VP_ERROR_STALE_IDENTITY:
        return VP_SAMPLE_STOP_STALE_IDENTITY;
    case VP_ERROR_THREAD_EXITED:
        return VP_SAMPLE_STOP_THREAD_EXITED;
    case VP_ERROR_TIMEOUT:
        return VP_SAMPLE_STOP_TIMEOUT;
    case VP_ERROR_READ_FAULT:
        return VP_SAMPLE_STOP_READ_FAULT;
    case VP_ERROR_MALFORMED:
        return VP_SAMPLE_STOP_MALFORMED;
    default:
        return VP_SAMPLE_STOP_PROVIDER_ERROR;
    }
}

static const struct vp_sample_provider_v2* vp_provider_v2(
    const struct vp_sample_provider* provider)
{
    return (const struct vp_sample_provider_v2*)provider;
}

static void vp_sampler_v2_load(const struct vp_sampler_v2* source,
                               struct vp_sampler* target)
{
    memset(target, 0, sizeof(*target));
    if (source == NULL)
        return;
    target->provider =
        source->provider != NULL ? &source->provider->base : NULL;
    target->frames = source->frames;
    target->active_token = source->active_token;
    target->last_provider_error = source->last_provider_error;
    target->frame_capacity = source->frame_capacity;
    target->max_depth = source->max_depth;
    target->capabilities = source->capabilities;
    target->active = source->active;
    target->release_pending = source->release_pending;
    target->initialized = source->initialized;
}

static void vp_sampler_v2_store(struct vp_sampler_v2* target,
                                const struct vp_sampler* source)
{
    if (target == NULL || source == NULL)
        return;
    target->active_token = source->active_token;
    target->last_provider_error = source->last_provider_error;
    target->active = source->active;
    target->release_pending = source->release_pending;
    target->initialized = source->initialized;
}

static int vp_cursor_valid(const struct vp_sample_cursor* cursor,
                           int require_stack_pointer)
{
    return cursor != NULL && cursor->pc != 0u &&
           (!require_stack_pointer || cursor->sp != 0u) &&
           (cursor->flags & ~VP_SAMPLE_FRAME_FLAG_ALL) == 0u;
}

static void vp_sample_reset(struct vp_sampler* sampler,
                            struct vp_sample* sample)
{
    memset(sample, 0, sizeof(*sample));
    if (!vp_sampler_is_valid(sampler))
        return;
    memset(sampler->frames, 0,
           sizeof(sampler->frames[0]) * sampler->frame_capacity);
    sample->frames = sampler->frames;
    sample->frame_capacity = sampler->frame_capacity;
}

static int vp_sampler_release_active(struct vp_sampler* sampler)
{
    int result;
    if (!sampler->active || sampler->active_token == 0u)
        return VP_ERROR_STATE;
    result = sampler->provider->end_sample(sampler->provider->user,
                                            sampler->active_token);
    if (result != VP_RESULT_OK) {
        sampler->last_provider_error =
            result < 0 ? result : VP_ERROR_MALFORMED;
        sampler->release_pending = 1u;
        return VP_ERROR_RELEASE_REQUIRED;
    }
    sampler->active_token = 0u;
    sampler->active = 0u;
    sampler->release_pending = 0u;
    return VP_RESULT_OK;
}

static int vp_sampler_finish(struct vp_sampler* sampler,
                             struct vp_sample* sample, int sample_result)
{
    int release_result = vp_sampler_release_active(sampler);
    if (release_result != VP_RESULT_OK) {
        sample->stop_reason = VP_SAMPLE_STOP_RELEASE_ERROR;
        sample->provider_error = sampler->last_provider_error;
        return release_result;
    }
    return sample_result;
}

static int vp_sampler_sample(struct vp_sampler* sampler,
                             const struct vp_sample_target* target,
                             uint32_t pc_capability,
                             uint32_t stack_capability,
                             struct vp_sample* sample)
{
    struct vp_sample_capture capture;
    struct vp_sample_cursor current;
    uint64_t lease_token = 0u;
    uint32_t depth;
    int stack_requested;
    int result;

    if (sample == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    vp_sample_reset(sampler, sample);
    if (!vp_sampler_is_valid(sampler))
        return VP_ERROR_NOT_INITIALIZED;
    if (target == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (sampler->release_pending != 0u || sampler->active != 0u)
        return VP_ERROR_RELEASE_REQUIRED;
    if ((sampler->capabilities & pc_capability) == 0u)
        return VP_ERROR_UNSUPPORTED;

    stack_requested =
        (sampler->capabilities & stack_capability) != 0u;
    depth = stack_requested ? sampler->max_depth : 1u;
    memset(&capture, 0, sizeof(capture));
    sampler->last_provider_error = VP_RESULT_OK;
    result = sampler->provider->begin_sample(
        sampler->provider->user, target, depth, &lease_token, &capture);
    if (result != VP_RESULT_OK) {
        sampler->last_provider_error =
            result < 0 ? result : VP_ERROR_MALFORMED;
        sample->provider_error = sampler->last_provider_error;
        sample->stop_reason =
            vp_sampler_stop_reason(sampler->last_provider_error);
        if (lease_token == 0u)
            return sampler->last_provider_error;
        sampler->active_token = lease_token;
        sampler->active = 1u;
        result = vp_sampler_release_active(sampler);
        return result == VP_RESULT_OK ? VP_ERROR_MALFORMED : result;
    }
    if (lease_token == 0u)
        return VP_ERROR_MALFORMED;

    sampler->active_token = lease_token;
    sampler->active = 1u;
    result = VP_RESULT_OK;
    if (!vp_cursor_valid(&capture.cursor, stack_requested) ||
        capture.thread_id <= 0 || capture.reserved != 0u ||
        (target->kind == VP_SAMPLE_TARGET_CURRENT &&
         target->thread_id != 0))
        result = VP_ERROR_MALFORMED;
    else if (target->kind == VP_SAMPLE_TARGET_FOREIGN &&
             (capture.thread_id != target->thread_id ||
              capture.identity != target->identity))
        result = VP_ERROR_STALE_IDENTITY;
    else if (target->kind == VP_SAMPLE_TARGET_FOREIGN &&
             (capture.cursor.flags &
              VP_SAMPLE_FRAME_FLAG_CONTEXT_CONFIDENT) == 0u)
        result = VP_ERROR_UNSUPPORTED;
    else if (target->kind == VP_SAMPLE_TARGET_FOREIGN)
        result = vp_provider_v2(sampler->provider)->validate_sample(
            sampler->provider->user, sampler->active_token, target);
    if (result != VP_RESULT_OK) {
        if (result >= 0)
            result = VP_ERROR_MALFORMED;
        vp_sample_reset(sampler, sample);
        if (vp_sampler_release_active(sampler) != VP_RESULT_OK) {
            sample->stop_reason = VP_SAMPLE_STOP_RELEASE_ERROR;
            sample->provider_error = sampler->last_provider_error;
            return VP_ERROR_RELEASE_REQUIRED;
        }
        sampler->last_provider_error = result;
        sample->provider_error = result;
        sample->stop_reason = vp_sampler_stop_reason(result);
        return result;
    }

    sample->thread_id = capture.thread_id;
    sample->identity = capture.identity;
    current = capture.cursor;
    for (;;) {
        struct vp_sample_frame* frame =
            &sampler->frames[sample->frame_count++];
        frame->pc = current.pc;
        frame->sp = current.sp;
        frame->flags = current.flags;

        if (!stack_requested) {
            sample->stop_reason = VP_SAMPLE_STOP_PC_ONLY;
            return vp_sampler_finish(sampler, sample, VP_RESULT_OK);
        }
        if (sample->frame_count >= depth) {
            sample->stop_reason = VP_SAMPLE_STOP_DEPTH_LIMIT;
            return vp_sampler_finish(sampler, sample, VP_RESULT_PARTIAL);
        }

        struct vp_sample_cursor next;
        if (target->kind == VP_SAMPLE_TARGET_FOREIGN) {
            result = vp_provider_v2(sampler->provider)->validate_sample(
                sampler->provider->user, sampler->active_token, target);
            if (result != VP_RESULT_OK) {
                sampler->last_provider_error =
                    result < 0 ? result : VP_ERROR_MALFORMED;
                sample->provider_error = sampler->last_provider_error;
                sample->stop_reason =
                    vp_sampler_stop_reason(sampler->last_provider_error);
                return vp_sampler_finish(
                    sampler, sample, VP_RESULT_PARTIAL);
            }
        }
        memset(&next, 0, sizeof(next));
        result = sampler->provider->next_frame(
            sampler->provider->user, sampler->active_token, &current, &next);
        if (result == VP_RESULT_END) {
            sample->stop_reason = VP_SAMPLE_STOP_COMPLETE;
            return vp_sampler_finish(sampler, sample, VP_RESULT_OK);
        }
        if (result != VP_RESULT_OK) {
            sampler->last_provider_error =
                result < 0 ? result : VP_ERROR_MALFORMED;
            sample->provider_error = sampler->last_provider_error;
            sample->stop_reason =
                vp_sampler_stop_reason(sampler->last_provider_error);
            return vp_sampler_finish(sampler, sample, VP_RESULT_PARTIAL);
        }
        if (!vp_cursor_valid(&next, 1) || next.sp <= current.sp) {
            sampler->last_provider_error = VP_ERROR_MALFORMED;
            sample->provider_error = VP_ERROR_MALFORMED;
            sample->stop_reason = VP_SAMPLE_STOP_MALFORMED;
            return vp_sampler_finish(sampler, sample, VP_RESULT_PARTIAL);
        }
        current = next;
    }
}

int vp_sampler_init(struct vp_sampler* sampler,
                    const struct vp_sample_provider* provider,
                    const struct vp_sampler_config* config)
{
    if (sampler == NULL || config == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (provider == NULL)
        return VP_ERROR_UNSUPPORTED;
    if (sampler->initialized == VP_SAMPLER_MAGIC)
        return sampler->release_pending != 0u
                   ? VP_ERROR_RELEASE_REQUIRED
                   : VP_ERROR_BUSY;
    if (provider->abi_version != VP_SAMPLE_PROVIDER_ABI_VERSION)
        return VP_ERROR_UNSUPPORTED;
    if (!vp_capabilities_valid(provider->capabilities,
                               provider->abi_version) ||
        provider->begin_sample == NULL || provider->end_sample == NULL ||
        ((provider->capabilities &
          (VP_SAMPLE_CAP_CURRENT_THREAD_STACK |
           VP_SAMPLE_CAP_FOREIGN_THREAD_STACK)) != 0u &&
         provider->next_frame == NULL) ||
        (provider->capabilities &
         (VP_SAMPLE_CAP_FOREIGN_THREAD_PC |
          VP_SAMPLE_CAP_FOREIGN_THREAD_STACK)) != 0u)
        return VP_ERROR_INVALID_ARGUMENT;
    if (config->frames == NULL || config->frame_capacity == 0u ||
        config->frame_capacity > VP_SAMPLE_MAX_FRAMES ||
        config->max_depth == 0u ||
        config->max_depth > VP_SAMPLE_MAX_FRAMES ||
        config->frame_capacity < config->max_depth ||
        config->required_capabilities == 0u ||
        !vp_capabilities_valid(config->required_capabilities,
                               provider->abi_version) ||
        config->reserved != 0u)
        return VP_ERROR_INVALID_ARGUMENT;
    if ((config->required_capabilities & ~provider->capabilities) != 0u)
        return VP_ERROR_UNSUPPORTED;
    memset(sampler, 0, sizeof(*sampler));
    memset(config->frames, 0,
           sizeof(config->frames[0]) * config->frame_capacity);
    sampler->provider = provider;
    sampler->frames = config->frames;
    sampler->frame_capacity = config->frame_capacity;
    sampler->max_depth = config->max_depth;
    sampler->capabilities = config->required_capabilities;
    sampler->initialized = VP_SAMPLER_MAGIC;
    return VP_RESULT_OK;
}

int vp_sampler_init_v2(
    struct vp_sampler_v2* sampler,
    const struct vp_sample_provider_v2* provider,
    const struct vp_sampler_config_v2* config)
{
    const struct vp_sample_provider* base_provider;
    uint32_t foreign;
    if (sampler == NULL || config == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (provider == NULL)
        return VP_ERROR_UNSUPPORTED;
    base_provider = &provider->base;
    if (sampler->initialized == VP_SAMPLER_MAGIC)
        return sampler->release_pending != 0u
                   ? VP_ERROR_RELEASE_REQUIRED
                   : VP_ERROR_BUSY;
    if (base_provider->abi_version != VP_SAMPLE_PROVIDER_ABI_VERSION_V2)
        return VP_ERROR_UNSUPPORTED;
    foreign = base_provider->capabilities &
              (VP_SAMPLE_CAP_FOREIGN_THREAD_PC |
               VP_SAMPLE_CAP_FOREIGN_THREAD_STACK);
    if (!vp_capabilities_valid(base_provider->capabilities,
                               base_provider->abi_version) ||
        base_provider->begin_sample == NULL ||
        base_provider->end_sample == NULL ||
        ((base_provider->capabilities &
          (VP_SAMPLE_CAP_CURRENT_THREAD_STACK |
           VP_SAMPLE_CAP_FOREIGN_THREAD_STACK)) != 0u &&
         base_provider->next_frame == NULL) ||
        (foreign != 0u &&
         (provider->validate_sample == NULL ||
          provider->max_callback_us == 0u ||
          provider->reserved != 0u)))
        return VP_ERROR_INVALID_ARGUMENT;
    if (config->frames == NULL ||
        config->frame_capacity == 0u ||
        config->frame_capacity > VP_SAMPLE_MAX_FRAMES ||
        config->max_depth == 0u ||
        config->max_depth > VP_SAMPLE_MAX_FRAMES ||
        config->frame_capacity < config->max_depth ||
        config->required_capabilities == 0u ||
        !vp_capabilities_valid(config->required_capabilities,
                               base_provider->abi_version) ||
        config->reserved != 0u)
        return VP_ERROR_INVALID_ARGUMENT;
    if ((config->required_capabilities &
         ~base_provider->capabilities) != 0u)
        return VP_ERROR_UNSUPPORTED;
    if ((config->required_capabilities &
         (VP_SAMPLE_CAP_FOREIGN_THREAD_PC |
          VP_SAMPLE_CAP_FOREIGN_THREAD_STACK)) != 0u &&
        (config->callback_timeout_us == 0u ||
         provider->max_callback_us > config->callback_timeout_us))
        return VP_ERROR_UNSUPPORTED;

    memset(sampler, 0, sizeof(*sampler));
    memset(config->frames, 0,
           sizeof(config->frames[0]) * config->frame_capacity);
    sampler->provider = provider;
    sampler->frames = config->frames;
    sampler->frame_capacity = config->frame_capacity;
    sampler->max_depth = config->max_depth;
    sampler->capabilities = config->required_capabilities;
    sampler->initialized = VP_SAMPLER_MAGIC;
    sampler->callback_timeout_us = config->callback_timeout_us;
    return VP_RESULT_OK;
}

int vp_sampler_deinit(struct vp_sampler* sampler)
{
    if (!vp_sampler_is_valid(sampler))
        return VP_ERROR_NOT_INITIALIZED;
    if (sampler->active != 0u || sampler->release_pending != 0u)
        return VP_ERROR_RELEASE_REQUIRED;
    memset(sampler, 0, sizeof(*sampler));
    return VP_RESULT_OK;
}

int vp_sampler_deinit_v2(struct vp_sampler_v2* sampler)
{
    struct vp_sampler base;
    int result;
    if (sampler == NULL)
        return VP_ERROR_NOT_INITIALIZED;
    vp_sampler_v2_load(sampler, &base);
    result = vp_sampler_deinit(&base);
    if (result == VP_RESULT_OK)
        memset(sampler, 0, sizeof(*sampler));
    return result;
}

int vp_sampler_sample_current(struct vp_sampler* sampler,
                              struct vp_sample* sample)
{
    const struct vp_sample_target target = {
        VP_SAMPLE_TARGET_CURRENT, 0, 0u};
    return vp_sampler_sample(
        sampler, &target, VP_SAMPLE_CAP_CURRENT_THREAD_PC,
        VP_SAMPLE_CAP_CURRENT_THREAD_STACK, sample);
}

int vp_sampler_sample_foreign(struct vp_sampler* sampler,
                              int32_t thread_id, uint64_t identity,
                              struct vp_sample* sample)
{
    struct vp_sample_target target;
    if (sample == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (thread_id <= 0 || identity == 0u) {
        vp_sample_reset(sampler, sample);
        return VP_ERROR_INVALID_ARGUMENT;
    }
    target.kind = VP_SAMPLE_TARGET_FOREIGN;
    target.thread_id = thread_id;
    target.identity = identity;
    return vp_sampler_sample(
        sampler, &target, VP_SAMPLE_CAP_FOREIGN_THREAD_PC,
        VP_SAMPLE_CAP_FOREIGN_THREAD_STACK, sample);
}

int vp_sampler_sample_current_v2(struct vp_sampler_v2* sampler,
                                 struct vp_sample* sample)
{
    struct vp_sampler base;
    int result;
    vp_sampler_v2_load(sampler, &base);
    result = vp_sampler_sample_current(
        sampler != NULL ? &base : NULL, sample);
    vp_sampler_v2_store(sampler, &base);
    return result;
}

int vp_sampler_sample_foreign_v2(struct vp_sampler_v2* sampler,
                                 int32_t thread_id, uint64_t identity,
                                 struct vp_sample* sample)
{
    struct vp_sampler base;
    int result;
    vp_sampler_v2_load(sampler, &base);
    result = vp_sampler_sample_foreign(
        sampler != NULL ? &base : NULL, thread_id, identity, sample);
    vp_sampler_v2_store(sampler, &base);
    return result;
}

int vp_sampler_retry_release(struct vp_sampler* sampler)
{
    if (!vp_sampler_is_valid(sampler))
        return VP_ERROR_NOT_INITIALIZED;
    if (sampler->release_pending == 0u || sampler->active == 0u)
        return VP_ERROR_STATE;
    if (vp_sampler_release_active(sampler) != VP_RESULT_OK)
        return VP_ERROR_RELEASE_REQUIRED;
    sampler->last_provider_error = VP_RESULT_OK;
    return VP_RESULT_OK;
}

int vp_sampler_retry_release_v2(struct vp_sampler_v2* sampler)
{
    struct vp_sampler base;
    int result;
    vp_sampler_v2_load(sampler, &base);
    result = vp_sampler_retry_release(
        sampler != NULL ? &base : NULL);
    vp_sampler_v2_store(sampler, &base);
    return result;
}

int vp_sampler_get_status(const struct vp_sampler* sampler,
                          struct vp_sampler_status* status)
{
    if (!vp_sampler_is_valid(sampler))
        return VP_ERROR_NOT_INITIALIZED;
    if (status == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    status->capabilities = sampler->capabilities;
    status->max_depth = sampler->max_depth;
    status->active = sampler->active;
    status->release_pending = sampler->release_pending;
    status->last_provider_error = sampler->last_provider_error;
    return VP_RESULT_OK;
}

int vp_sampler_get_status_v2(
    const struct vp_sampler_v2* sampler,
    struct vp_sampler_status_v2* status)
{
    if (sampler == NULL || sampler->initialized != VP_SAMPLER_MAGIC)
        return VP_ERROR_NOT_INITIALIZED;
    if (status == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    status->capabilities = sampler->capabilities;
    status->max_depth = sampler->max_depth;
    status->active = sampler->active;
    status->release_pending = sampler->release_pending;
    status->last_provider_error = sampler->last_provider_error;
    status->callback_timeout_us = sampler->callback_timeout_us;
    return VP_RESULT_OK;
}

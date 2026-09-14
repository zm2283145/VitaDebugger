#include "vitaprofiler_gpu.h"

#include <string.h>

#define VP_GRAPHICS_HOOKS_MAGIC UINT32_C(0x56504748)

static const char* const vp_graphics_zone_names[VP_GRAPHICS_ZONE_COUNT] = {
    "vitagl.frame.cpu",
    "vitagl.swap_buffers.cpu",
    "scegxm.scene.cpu",
    "scegxm.finish.cpu_wait",
    "scegxm.display_queue.cpu_submit",
};

static const char* const
    vp_graphics_counter_names[VP_GRAPHICS_COUNTER_COUNT] = {
        "vitagl.draw_calls",
        "scegxm.draw_calls",
    };

int vp_graphics_register_names(struct vp_name_dictionary* dictionary,
                               struct vp_graphics_name_ids* names)
{
    struct vp_graphics_name_ids registered;
    uint32_t i;
    int result;
    if (dictionary == NULL || names == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    memset(&registered, 0, sizeof(registered));
    for (i = 0u; i < VP_GRAPHICS_ZONE_COUNT; ++i) {
        result = vp_name_dictionary_register(dictionary,
                                             vp_graphics_zone_names[i],
                                             &registered.zones[i]);
        if (result != VP_RESULT_OK)
            return result;
    }
    for (i = 0u; i < VP_GRAPHICS_COUNTER_COUNT; ++i) {
        result = vp_name_dictionary_register(dictionary,
                                             vp_graphics_counter_names[i],
                                             &registered.counters[i]);
        if (result != VP_RESULT_OK)
            return result;
    }
    *names = registered;
    return VP_RESULT_OK;
}

int vp_graphics_hooks_init(struct vp_graphics_hooks* hooks,
                           struct vp_context* context,
                           const struct vp_graphics_name_ids* names)
{
    struct vp_stats stats;
    uint32_t i;
    if (hooks == NULL || context == NULL || names == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    memset(hooks, 0, sizeof(*hooks));
    if (vp_get_stats(context, &stats) != VP_RESULT_OK)
        return VP_ERROR_NOT_INITIALIZED;
    for (i = 0u; i < VP_GRAPHICS_ZONE_COUNT; ++i) {
        if (names->zones[i] == 0u)
            return VP_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < VP_GRAPHICS_COUNTER_COUNT; ++i) {
        if (names->counters[i] == 0u)
            return VP_ERROR_INVALID_ARGUMENT;
    }
    hooks->context = context;
    hooks->names = *names;
    hooks->initialized = VP_GRAPHICS_HOOKS_MAGIC;
    return VP_RESULT_OK;
}

int vp_graphics_zone_begin(struct vp_graphics_hooks* hooks,
                           enum vp_graphics_zone zone,
                           struct vp_zone_scope* scope)
{
    if (hooks == NULL || hooks->initialized != VP_GRAPHICS_HOOKS_MAGIC ||
        hooks->context == NULL)
        return VP_ERROR_NOT_INITIALIZED;
    if ((uint32_t)zone >= VP_GRAPHICS_ZONE_COUNT)
        return VP_ERROR_INVALID_ARGUMENT;
    return vp_zone_begin(hooks->context, hooks->names.zones[(uint32_t)zone],
                         scope);
}

int vp_graphics_zone_end(struct vp_graphics_hooks* hooks,
                         struct vp_zone_scope* scope)
{
    if (hooks == NULL || hooks->initialized != VP_GRAPHICS_HOOKS_MAGIC ||
        hooks->context == NULL)
        return VP_ERROR_NOT_INITIALIZED;
    return vp_zone_end(hooks->context, scope);
}

int vp_graphics_counter(struct vp_graphics_hooks* hooks,
                        enum vp_graphics_counter counter, int64_t value)
{
    if (hooks == NULL || hooks->initialized != VP_GRAPHICS_HOOKS_MAGIC ||
        hooks->context == NULL)
        return VP_ERROR_NOT_INITIALIZED;
    if ((uint32_t)counter >= VP_GRAPHICS_COUNTER_COUNT)
        return VP_ERROR_INVALID_ARGUMENT;
    return vp_counter(hooks->context,
                      hooks->names.counters[(uint32_t)counter], value);
}

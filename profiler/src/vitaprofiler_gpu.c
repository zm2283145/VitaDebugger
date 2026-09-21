#include "vitaprofiler_gpu.h"

#include <string.h>

#define VP_GRAPHICS_HOOKS_MAGIC UINT32_C(0x56504748)
#define VP_GRAPHICS_LEGACY_ZONE_COUNT 5u
#define VP_GRAPHICS_LEGACY_COUNTER_COUNT 2u

static const char* const vp_graphics_zone_names[VP_GRAPHICS_ZONE_COUNT] = {
    "vitagl.frame.cpu",
    "vitagl.swap_buffers.cpu",
    "scegxm.scene.cpu",
    "scegxm.finish.cpu_wait",
    "scegxm.display_queue.cpu_submit",
    "vitagl.draw.cpu_call",
    "vitagl.shader.cpu",
    "vitagl.state.cpu",
    "vitagl.alloc.cpu",
    "scegxm.draw.cpu_submit",
    "scegxm.shader.cpu",
    "scegxm.state.cpu",
    "scegxm.alloc.cpu",
    "scegxm.scene_begin.cpu",
    "scegxm.scene_end.cpu",
    "scegxm.scene_reset.cpu",
    "scegxm.display_queue_add.cpu_wait",
    "scegxm.display_callback.cpu",
    "display.vblank.cpu_wait",
    "vitagl.command.cpu_submit",
    "vitagl.clear.cpu_call",
    "vitagl.fence.cpu_wait",
    "vitagl.program.cpu",
    "vitagl.render_target.cpu_transition",
    "vitagl.buffer.cpu_transition",
    "vitagl.upload.cpu",
    "scegxm.command.cpu_submit",
    "scegxm.clear.cpu_submit",
    "scegxm.fence.cpu_wait",
    "scegxm.program.cpu",
    "scegxm.render_target.cpu_transition",
    "scegxm.buffer.cpu_transition",
    "scegxm.upload.cpu",
};

static const char* const
    vp_graphics_counter_names[VP_GRAPHICS_COUNTER_COUNT] = {
        "vitagl.draw_calls",
        "scegxm.draw_calls",
        "vitagl.shader_changes",
        "scegxm.shader_changes",
        "vitagl.state_changes",
        "scegxm.state_changes",
        "vitagl.allocation_bytes",
        "scegxm.allocation_bytes",
        "vitagl.clear_calls",
        "scegxm.clear_calls",
        "vitagl.program_changes",
        "scegxm.program_changes",
        "vitagl.upload_bytes",
        "scegxm.upload_bytes",
    };

static const char* const vp_graphics_frame_names[VP_GRAPHICS_FRAME_COUNT] = {
    "graphics.frame.cpu",
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
    for (i = 0u; i < VP_GRAPHICS_LEGACY_ZONE_COUNT; ++i) {
        result = vp_name_dictionary_register(dictionary,
                                             vp_graphics_zone_names[i],
                                             &registered.zones[i]);
        if (result != VP_RESULT_OK)
            return result;
    }
    for (i = 0u; i < VP_GRAPHICS_LEGACY_COUNTER_COUNT; ++i) {
        result = vp_name_dictionary_register(dictionary,
                                             vp_graphics_counter_names[i],
                                             &registered.counters[i]);
        if (result != VP_RESULT_OK)
            return result;
    }
    *names = registered;
    return VP_RESULT_OK;
}

int vp_graphics_register_extended_names(
    struct vp_name_dictionary* dictionary,
    struct vp_graphics_name_ids* names)
{
    struct vp_graphics_name_ids registered;
    uint32_t i;
    int result;
    if (dictionary == NULL || names == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    result = vp_graphics_register_names(dictionary, &registered);
    if (result != VP_RESULT_OK)
        return result;
    for (i = VP_GRAPHICS_LEGACY_ZONE_COUNT;
         i < VP_GRAPHICS_ZONE_COUNT; ++i) {
        result = vp_name_dictionary_register(dictionary,
                                             vp_graphics_zone_names[i],
                                             &registered.zones[i]);
        if (result != VP_RESULT_OK)
            return result;
    }
    for (i = VP_GRAPHICS_LEGACY_COUNTER_COUNT;
         i < VP_GRAPHICS_COUNTER_COUNT; ++i) {
        result = vp_name_dictionary_register(dictionary,
                                             vp_graphics_counter_names[i],
                                             &registered.counters[i]);
        if (result != VP_RESULT_OK)
            return result;
    }
    for (i = 0u; i < VP_GRAPHICS_FRAME_COUNT; ++i) {
        result = vp_name_dictionary_register(dictionary,
                                             vp_graphics_frame_names[i],
                                             &registered.frames[i]);
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
    for (i = 0u; i < VP_GRAPHICS_LEGACY_ZONE_COUNT; ++i) {
        if (names->zones[i] == 0u)
            return VP_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < VP_GRAPHICS_LEGACY_COUNTER_COUNT; ++i) {
        if (names->counters[i] == 0u)
            return VP_ERROR_INVALID_ARGUMENT;
    }
    hooks->context = context;
    hooks->names = *names;
    hooks->enabled = 1u;
    hooks->initialized = VP_GRAPHICS_HOOKS_MAGIC;
    return VP_RESULT_OK;
}

int vp_graphics_hooks_set_enabled(struct vp_graphics_hooks* hooks,
                                  int enabled)
{
    if (hooks == NULL || hooks->initialized != VP_GRAPHICS_HOOKS_MAGIC ||
        hooks->context == NULL)
        return VP_ERROR_NOT_INITIALIZED;
    hooks->enabled = enabled != 0 ? 1u : 0u;
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
    if (scope == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (hooks->names.zones[(uint32_t)zone] == 0u)
        return VP_ERROR_UNSUPPORTED;
    if (hooks->enabled == 0u) {
        memset(scope, 0, sizeof(*scope));
        return VP_RESULT_OK;
    }
    return vp_zone_begin(hooks->context, hooks->names.zones[(uint32_t)zone],
                         scope);
}

int vp_graphics_zone_end(struct vp_graphics_hooks* hooks,
                         struct vp_zone_scope* scope)
{
    if (hooks == NULL || hooks->initialized != VP_GRAPHICS_HOOKS_MAGIC ||
        hooks->context == NULL)
        return VP_ERROR_NOT_INITIALIZED;
    if (scope == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (scope->active == 0u && hooks->enabled == 0u)
        return VP_RESULT_OK;
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
    if (hooks->names.counters[(uint32_t)counter] == 0u)
        return VP_ERROR_UNSUPPORTED;
    if (hooks->enabled == 0u)
        return VP_RESULT_OK;
    return vp_counter(hooks->context,
                      hooks->names.counters[(uint32_t)counter], value);
}

int vp_graphics_frame_mark(struct vp_graphics_hooks* hooks,
                           enum vp_graphics_frame frame)
{
    if (hooks == NULL || hooks->initialized != VP_GRAPHICS_HOOKS_MAGIC ||
        hooks->context == NULL)
        return VP_ERROR_NOT_INITIALIZED;
    if ((uint32_t)frame >= VP_GRAPHICS_FRAME_COUNT)
        return VP_ERROR_INVALID_ARGUMENT;
    if (hooks->names.frames[(uint32_t)frame] == 0u)
        return VP_ERROR_UNSUPPORTED;
    if (hooks->enabled == 0u)
        return VP_RESULT_OK;
    return vp_frame_mark(hooks->context, hooks->names.frames[(uint32_t)frame]);
}

void vp_graphics_scope_stack_init(struct vp_graphics_scope_stack* stack)
{
    if (stack != NULL)
        memset(stack, 0, sizeof(*stack));
}

int vp_graphics_scope_push(struct vp_graphics_hooks* hooks,
                           struct vp_graphics_scope_stack* stack,
                           enum vp_graphics_zone zone)
{
    int result;
    if (stack == NULL)
        return VP_ERROR_INVALID_ARGUMENT;
    if (stack->depth >= VP_GRAPHICS_SCOPE_STACK_CAPACITY)
        return VP_ERROR_CAPACITY;
    result = vp_graphics_zone_begin(hooks, zone,
                                    &stack->scopes[stack->depth]);
    if (result == VP_RESULT_OK || result == VP_RESULT_DROPPED)
        ++stack->depth;
    return result;
}

int vp_graphics_scope_pop(struct vp_graphics_hooks* hooks,
                          struct vp_graphics_scope_stack* stack)
{
    struct vp_zone_scope* scope;
    if (stack == NULL || stack->depth == 0u)
        return VP_ERROR_INVALID_ARGUMENT;
    --stack->depth;
    scope = &stack->scopes[stack->depth];
    if (scope->active == 0u)
        return VP_RESULT_OK;
    return vp_graphics_zone_end(hooks, scope);
}

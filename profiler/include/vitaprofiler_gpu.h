#ifndef VITAPROFILER_GPU_H
#define VITAPROFILER_GPU_H

#include "vitaprofiler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* These are cooperative CPU wall-time hooks. They intentionally do not include
 * VitaGL or SceGxm headers and do not claim GPU execution timestamps. */
#define VP_GRAPHICS_SCOPE_STACK_CAPACITY 8u

/* Stable FNV-1a IDs for every built-in graphics hook name. Keep these values
 * and their corresponding strings stable so captures remain comparable. */
#define VP_GRAPHICS_NAME_ID_VITAGL_FRAME UINT32_C(0x0673be8f)
#define VP_GRAPHICS_NAME_ID_VITAGL_SWAP_BUFFERS UINT32_C(0xce43600d)
#define VP_GRAPHICS_NAME_ID_SCEGXM_SCENE UINT32_C(0x50f1791e)
#define VP_GRAPHICS_NAME_ID_SCEGXM_FINISH_WAIT UINT32_C(0xdc7573cb)
#define VP_GRAPHICS_NAME_ID_SCEGXM_DISPLAY_QUEUE_SUBMIT UINT32_C(0xfb338ba9)
#define VP_GRAPHICS_NAME_ID_VITAGL_DRAW_CALL UINT32_C(0x2ba242b5)
#define VP_GRAPHICS_NAME_ID_VITAGL_SHADER UINT32_C(0x44cbf5c7)
#define VP_GRAPHICS_NAME_ID_VITAGL_STATE UINT32_C(0x0fa93a83)
#define VP_GRAPHICS_NAME_ID_VITAGL_ALLOCATION UINT32_C(0x50ba0cbf)
#define VP_GRAPHICS_NAME_ID_SCEGXM_DRAW_SUBMIT UINT32_C(0x40032165)
#define VP_GRAPHICS_NAME_ID_SCEGXM_SHADER UINT32_C(0x718c7ebb)
#define VP_GRAPHICS_NAME_ID_SCEGXM_STATE UINT32_C(0xfdc03f57)
#define VP_GRAPHICS_NAME_ID_SCEGXM_ALLOCATION UINT32_C(0x810fb933)
#define VP_GRAPHICS_NAME_ID_SCEGXM_SCENE_BEGIN UINT32_C(0xdd7d7c34)
#define VP_GRAPHICS_NAME_ID_SCEGXM_SCENE_END UINT32_C(0xbaca47c0)
#define VP_GRAPHICS_NAME_ID_SCEGXM_SCENE_RESET UINT32_C(0x4657fdd2)
#define VP_GRAPHICS_NAME_ID_SCEGXM_DISPLAY_QUEUE_ADD_WAIT UINT32_C(0xbca22f18)
#define VP_GRAPHICS_NAME_ID_SCEGXM_DISPLAY_CALLBACK UINT32_C(0x1752f812)
#define VP_GRAPHICS_NAME_ID_DISPLAY_VBLANK_WAIT UINT32_C(0x661b035f)
#define VP_GRAPHICS_NAME_ID_VITAGL_COMMAND_SUBMIT UINT32_C(0xd494bac8)
#define VP_GRAPHICS_NAME_ID_VITAGL_CLEAR UINT32_C(0xd677a8b0)
#define VP_GRAPHICS_NAME_ID_VITAGL_FENCE_WAIT UINT32_C(0x74fbd8b9)
#define VP_GRAPHICS_NAME_ID_VITAGL_PROGRAM UINT32_C(0xfacb36aa)
#define VP_GRAPHICS_NAME_ID_VITAGL_RENDER_TARGET_TRANSITION \
    UINT32_C(0x395c8da4)
#define VP_GRAPHICS_NAME_ID_VITAGL_BUFFER_TRANSITION UINT32_C(0x28ec2f66)
#define VP_GRAPHICS_NAME_ID_VITAGL_UPLOAD UINT32_C(0x922de153)
#define VP_GRAPHICS_NAME_ID_SCEGXM_COMMAND_SUBMIT UINT32_C(0x562cb864)
#define VP_GRAPHICS_NAME_ID_SCEGXM_CLEAR UINT32_C(0x108e8f7c)
#define VP_GRAPHICS_NAME_ID_SCEGXM_FENCE_WAIT UINT32_C(0x233f4c05)
#define VP_GRAPHICS_NAME_ID_SCEGXM_PROGRAM UINT32_C(0x5b0cc58e)
#define VP_GRAPHICS_NAME_ID_SCEGXM_RENDER_TARGET_TRANSITION \
    UINT32_C(0x92c79c60)
#define VP_GRAPHICS_NAME_ID_SCEGXM_BUFFER_TRANSITION UINT32_C(0x1688d482)
#define VP_GRAPHICS_NAME_ID_SCEGXM_UPLOAD UINT32_C(0x96624247)
#define VP_GRAPHICS_NAME_ID_VITAGL_DRAW_CALLS UINT32_C(0x63fe2b3e)
#define VP_GRAPHICS_NAME_ID_SCEGXM_DRAW_CALLS UINT32_C(0x6d220c7a)
#define VP_GRAPHICS_NAME_ID_VITAGL_SHADER_CHANGES UINT32_C(0x0294c9d9)
#define VP_GRAPHICS_NAME_ID_SCEGXM_SHADER_CHANGES UINT32_C(0x699b536d)
#define VP_GRAPHICS_NAME_ID_VITAGL_STATE_CHANGES UINT32_C(0x8ffc54d5)
#define VP_GRAPHICS_NAME_ID_SCEGXM_STATE_CHANGES UINT32_C(0x4a857ca9)
#define VP_GRAPHICS_NAME_ID_VITAGL_ALLOCATION_BYTES UINT32_C(0x52221118)
#define VP_GRAPHICS_NAME_ID_SCEGXM_ALLOCATION_BYTES UINT32_C(0x4aa72834)
#define VP_GRAPHICS_NAME_ID_VITAGL_CLEAR_CALLS UINT32_C(0xcdf93d59)
#define VP_GRAPHICS_NAME_ID_SCEGXM_CLEAR_CALLS UINT32_C(0x1dfd691d)
#define VP_GRAPHICS_NAME_ID_VITAGL_PROGRAM_CHANGES UINT32_C(0xa848812c)
#define VP_GRAPHICS_NAME_ID_SCEGXM_PROGRAM_CHANGES UINT32_C(0x4cd1b2d0)
#define VP_GRAPHICS_NAME_ID_VITAGL_UPLOAD_BYTES UINT32_C(0x5216a187)
#define VP_GRAPHICS_NAME_ID_SCEGXM_UPLOAD_BYTES UINT32_C(0xaad535a3)
#define VP_GRAPHICS_NAME_ID_FRAME UINT32_C(0x48fe74e3)

enum vp_graphics_zone {
    VP_GRAPHICS_ZONE_VITAGL_FRAME = 0,
    VP_GRAPHICS_ZONE_VITAGL_SWAP_BUFFERS = 1,
    VP_GRAPHICS_ZONE_SCEGXM_SCENE = 2,
    VP_GRAPHICS_ZONE_SCEGXM_FINISH_WAIT = 3,
    VP_GRAPHICS_ZONE_SCEGXM_DISPLAY_QUEUE_SUBMIT = 4,
    VP_GRAPHICS_ZONE_VITAGL_DRAW_CALL = 5,
    VP_GRAPHICS_ZONE_VITAGL_SHADER = 6,
    VP_GRAPHICS_ZONE_VITAGL_STATE = 7,
    VP_GRAPHICS_ZONE_VITAGL_ALLOCATION = 8,
    VP_GRAPHICS_ZONE_SCEGXM_DRAW_SUBMIT = 9,
    VP_GRAPHICS_ZONE_SCEGXM_SHADER = 10,
    VP_GRAPHICS_ZONE_SCEGXM_STATE = 11,
    VP_GRAPHICS_ZONE_SCEGXM_ALLOCATION = 12,
    VP_GRAPHICS_ZONE_SCEGXM_SCENE_BEGIN = 13,
    VP_GRAPHICS_ZONE_SCEGXM_SCENE_END = 14,
    VP_GRAPHICS_ZONE_SCEGXM_SCENE_RESET = 15,
    VP_GRAPHICS_ZONE_SCEGXM_DISPLAY_QUEUE_ADD_WAIT = 16,
    VP_GRAPHICS_ZONE_SCEGXM_DISPLAY_CALLBACK = 17,
    VP_GRAPHICS_ZONE_DISPLAY_VBLANK_WAIT = 18,
    VP_GRAPHICS_ZONE_VITAGL_COMMAND_SUBMIT = 19,
    VP_GRAPHICS_ZONE_VITAGL_CLEAR = 20,
    VP_GRAPHICS_ZONE_VITAGL_FENCE_WAIT = 21,
    VP_GRAPHICS_ZONE_VITAGL_PROGRAM = 22,
    VP_GRAPHICS_ZONE_VITAGL_RENDER_TARGET_TRANSITION = 23,
    VP_GRAPHICS_ZONE_VITAGL_BUFFER_TRANSITION = 24,
    VP_GRAPHICS_ZONE_VITAGL_UPLOAD = 25,
    VP_GRAPHICS_ZONE_SCEGXM_COMMAND_SUBMIT = 26,
    VP_GRAPHICS_ZONE_SCEGXM_CLEAR = 27,
    VP_GRAPHICS_ZONE_SCEGXM_FENCE_WAIT = 28,
    VP_GRAPHICS_ZONE_SCEGXM_PROGRAM = 29,
    VP_GRAPHICS_ZONE_SCEGXM_RENDER_TARGET_TRANSITION = 30,
    VP_GRAPHICS_ZONE_SCEGXM_BUFFER_TRANSITION = 31,
    VP_GRAPHICS_ZONE_SCEGXM_UPLOAD = 32,
    VP_GRAPHICS_ZONE_COUNT = 33,
};

enum vp_graphics_counter {
    VP_GRAPHICS_COUNTER_VITAGL_DRAW_CALLS = 0,
    VP_GRAPHICS_COUNTER_SCEGXM_DRAW_CALLS = 1,
    VP_GRAPHICS_COUNTER_VITAGL_SHADER_CHANGES = 2,
    VP_GRAPHICS_COUNTER_SCEGXM_SHADER_CHANGES = 3,
    VP_GRAPHICS_COUNTER_VITAGL_STATE_CHANGES = 4,
    VP_GRAPHICS_COUNTER_SCEGXM_STATE_CHANGES = 5,
    VP_GRAPHICS_COUNTER_VITAGL_ALLOCATION_BYTES = 6,
    VP_GRAPHICS_COUNTER_SCEGXM_ALLOCATION_BYTES = 7,
    VP_GRAPHICS_COUNTER_VITAGL_CLEAR_CALLS = 8,
    VP_GRAPHICS_COUNTER_SCEGXM_CLEAR_CALLS = 9,
    VP_GRAPHICS_COUNTER_VITAGL_PROGRAM_CHANGES = 10,
    VP_GRAPHICS_COUNTER_SCEGXM_PROGRAM_CHANGES = 11,
    VP_GRAPHICS_COUNTER_VITAGL_UPLOAD_BYTES = 12,
    VP_GRAPHICS_COUNTER_SCEGXM_UPLOAD_BYTES = 13,
    VP_GRAPHICS_COUNTER_COUNT = 14,
};

enum vp_graphics_frame {
    VP_GRAPHICS_FRAME = 0,
    VP_GRAPHICS_FRAME_COUNT = 1,
};

struct vp_graphics_name_ids {
    uint32_t zones[VP_GRAPHICS_ZONE_COUNT];
    uint32_t counters[VP_GRAPHICS_COUNTER_COUNT];
    uint32_t frames[VP_GRAPHICS_FRAME_COUNT];
};

struct vp_graphics_hooks {
    struct vp_context* context;
    struct vp_graphics_name_ids names;
    uint32_t enabled;
    uint32_t initialized;
};

/* A stack is owned by one instrumented call flow, normally one render thread.
 * It adds no global renderer state and bounds nesting without allocation. */
struct vp_graphics_scope_stack {
    struct vp_zone_scope scopes[VP_GRAPHICS_SCOPE_STACK_CAPACITY];
    uint32_t depth;
};

/* Register the fixed hook names before sealing the dictionary. The operation
 * can partially succeed on capacity failure; registrations are idempotent, so
 * initialization may be retried with a larger quiescent dictionary. */
int vp_graphics_register_names(struct vp_name_dictionary* dictionary,
                               struct vp_graphics_name_ids* names);
int vp_graphics_register_extended_names(
    struct vp_name_dictionary* dictionary,
    struct vp_graphics_name_ids* names);
int vp_graphics_hooks_init(struct vp_graphics_hooks* hooks,
                           struct vp_context* context,
                           const struct vp_graphics_name_ids* names);
/* Toggle only while graphics-helper producers are quiescent and no
 * instrumented scope is active. Disabled calls do not read the clock or touch
 * the event ring. */
int vp_graphics_hooks_set_enabled(struct vp_graphics_hooks* hooks,
                                  int enabled);
int vp_graphics_zone_begin(struct vp_graphics_hooks* hooks,
                           enum vp_graphics_zone zone,
                           struct vp_zone_scope* scope);
int vp_graphics_zone_end(struct vp_graphics_hooks* hooks,
                         struct vp_zone_scope* scope);
int vp_graphics_counter(struct vp_graphics_hooks* hooks,
                        enum vp_graphics_counter counter, int64_t value);
int vp_graphics_frame_mark(struct vp_graphics_hooks* hooks,
                           enum vp_graphics_frame frame);
void vp_graphics_scope_stack_init(struct vp_graphics_scope_stack* stack);
int vp_graphics_scope_push(struct vp_graphics_hooks* hooks,
                           struct vp_graphics_scope_stack* stack,
                           enum vp_graphics_zone zone);
int vp_graphics_scope_pop(struct vp_graphics_hooks* hooks,
                          struct vp_graphics_scope_stack* stack);

#ifdef __cplusplus
}
#endif

/* Define VITAPROFILER_GRAPHICS_ENABLED for instrumented builds. With it
 * undefined, arguments are referenced only through sizeof and are not
 * evaluated, so disabled call sites add no calls, clock reads, or ring work. */
#if defined(VITAPROFILER_GRAPHICS_ENABLED)
#define VP_GRAPHICS_ZONE_BEGIN(hooks, zone, scope) \
    vp_graphics_zone_begin((hooks), (zone), (scope))
#define VP_GRAPHICS_ZONE_END(hooks, scope) \
    vp_graphics_zone_end((hooks), (scope))
#define VP_GRAPHICS_COUNTER(hooks, counter, value) \
    vp_graphics_counter((hooks), (counter), (value))
#define VP_GRAPHICS_FRAME_MARK(hooks, frame) \
    vp_graphics_frame_mark((hooks), (frame))
#define VP_GRAPHICS_SCOPE_PUSH(hooks, stack, zone) \
    vp_graphics_scope_push((hooks), (stack), (zone))
#define VP_GRAPHICS_SCOPE_POP(hooks, stack) \
    vp_graphics_scope_pop((hooks), (stack))
#else
#define VP_GRAPHICS_ZONE_BEGIN(hooks, zone, scope) \
    ((void)sizeof(hooks), (void)sizeof(zone), (void)sizeof(scope), \
     VP_RESULT_OK)
#define VP_GRAPHICS_ZONE_END(hooks, scope) \
    ((void)sizeof(hooks), (void)sizeof(scope), VP_RESULT_OK)
#define VP_GRAPHICS_COUNTER(hooks, counter, value) \
    ((void)sizeof(hooks), (void)sizeof(counter), (void)sizeof(value), \
     VP_RESULT_OK)
#define VP_GRAPHICS_FRAME_MARK(hooks, frame) \
    ((void)sizeof(hooks), (void)sizeof(frame), VP_RESULT_OK)
#define VP_GRAPHICS_SCOPE_PUSH(hooks, stack, zone) \
    ((void)sizeof(hooks), (void)sizeof(stack), (void)sizeof(zone), \
     VP_RESULT_OK)
#define VP_GRAPHICS_SCOPE_POP(hooks, stack) \
    ((void)sizeof(hooks), (void)sizeof(stack), VP_RESULT_OK)
#endif

#endif

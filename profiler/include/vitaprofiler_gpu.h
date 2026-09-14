#ifndef VITAPROFILER_GPU_H
#define VITAPROFILER_GPU_H

#include "vitaprofiler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* These are cooperative CPU wall-time hooks. They intentionally do not include
 * VitaGL or SceGxm headers and do not claim GPU execution timestamps. */
enum vp_graphics_zone {
    VP_GRAPHICS_ZONE_VITAGL_FRAME = 0,
    VP_GRAPHICS_ZONE_VITAGL_SWAP_BUFFERS = 1,
    VP_GRAPHICS_ZONE_SCEGXM_SCENE = 2,
    VP_GRAPHICS_ZONE_SCEGXM_FINISH_WAIT = 3,
    VP_GRAPHICS_ZONE_SCEGXM_DISPLAY_QUEUE_SUBMIT = 4,
    VP_GRAPHICS_ZONE_COUNT = 5,
};

enum vp_graphics_counter {
    VP_GRAPHICS_COUNTER_VITAGL_DRAW_CALLS = 0,
    VP_GRAPHICS_COUNTER_SCEGXM_DRAW_CALLS = 1,
    VP_GRAPHICS_COUNTER_COUNT = 2,
};

struct vp_graphics_name_ids {
    uint32_t zones[VP_GRAPHICS_ZONE_COUNT];
    uint32_t counters[VP_GRAPHICS_COUNTER_COUNT];
};

struct vp_graphics_hooks {
    struct vp_context* context;
    struct vp_graphics_name_ids names;
    uint32_t initialized;
};

/* Register the fixed hook names before sealing the dictionary. The operation
 * can partially succeed on capacity failure; registrations are idempotent, so
 * initialization may be retried with a larger quiescent dictionary. */
int vp_graphics_register_names(struct vp_name_dictionary* dictionary,
                               struct vp_graphics_name_ids* names);
int vp_graphics_hooks_init(struct vp_graphics_hooks* hooks,
                           struct vp_context* context,
                           const struct vp_graphics_name_ids* names);
int vp_graphics_zone_begin(struct vp_graphics_hooks* hooks,
                           enum vp_graphics_zone zone,
                           struct vp_zone_scope* scope);
int vp_graphics_zone_end(struct vp_graphics_hooks* hooks,
                         struct vp_zone_scope* scope);
int vp_graphics_counter(struct vp_graphics_hooks* hooks,
                        enum vp_graphics_counter counter, int64_t value);

#ifdef __cplusplus
}
#endif

#endif

#include "vitaprofiler_gpu.h"

#include <stdio.h>

int main(void)
{
    struct vp_graphics_hooks* hooks = NULL;
    struct vp_graphics_hooks_v2* hooks_v2 = NULL;
    struct vp_zone_scope* scope = NULL;
    struct vp_graphics_scope_stack* stack = NULL;
    int touched = 0;

    if (VP_GRAPHICS_ZONE_BEGIN(hooks + touched++,
                               VP_GRAPHICS_ZONE_VITAGL_FRAME,
                               scope + touched++) != VP_RESULT_OK ||
        VP_GRAPHICS_ZONE_END(hooks + touched++, scope + touched++) !=
            VP_RESULT_OK ||
        VP_GRAPHICS_COUNTER(hooks + touched++,
                            VP_GRAPHICS_COUNTER_VITAGL_DRAW_CALLS,
                            touched++) != VP_RESULT_OK ||
        VP_GRAPHICS_FRAME_MARK(hooks + touched++, VP_GRAPHICS_FRAME) !=
            VP_RESULT_OK ||
        VP_GRAPHICS_SCOPE_PUSH(hooks + touched++, stack + touched++,
                               VP_GRAPHICS_ZONE_VITAGL_DRAW_CALL) !=
            VP_RESULT_OK ||
        VP_GRAPHICS_SCOPE_POP(hooks + touched++, stack + touched++) !=
            VP_RESULT_OK ||
        VP_GRAPHICS_V2_ZONE_BEGIN(
            hooks_v2 + touched++, VP_GRAPHICS_ZONE_SCEGXM_FENCE_WAIT,
            scope + touched++) != VP_RESULT_OK ||
        VP_GRAPHICS_V2_COUNTER(
            hooks_v2 + touched++, VP_GRAPHICS_COUNTER_VITAGL_UPLOAD_BYTES,
            touched++) != VP_RESULT_OK ||
        touched != 0) {
        fputs("disabled graphics instrumentation evaluated an argument\n",
              stderr);
        return 1;
    }

    puts("vitaprofiler graphics disabled: no arguments evaluated");
    return 0;
}

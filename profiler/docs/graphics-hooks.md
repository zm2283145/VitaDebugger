# Cooperative VitaGL and SceGxm hooks

`vitaprofiler_gpu.h` provides header-independent instrumentation points for
code that already owns VitaGL or SceGxm calls. It includes neither SDK and does
not interpose, patch, or automatically wrap a graphics library.

`vp_graphics_register_names()` registers these stable dictionary labels before
the dictionary is sealed:

- `vitagl.frame.cpu`
- `vitagl.swap_buffers.cpu`
- `scegxm.scene.cpu`
- `scegxm.finish.cpu_wait`
- `scegxm.display_queue.cpu_submit`
- `vitagl.draw_calls`
- `scegxm.draw_calls`

The zone helpers record CPU wall time around the call sites. They do **not**
measure execution on the GPU.

## Suggested hook points

- Wrap a VitaGL application's complete frame in
  `VP_GRAPHICS_ZONE_VITAGL_FRAME`.
- Wrap `vglSwapBuffers()` in `VP_GRAPHICS_ZONE_VITAGL_SWAP_BUFFERS`.
- Begin `VP_GRAPHICS_ZONE_SCEGXM_SCENE` immediately before
  `sceGxmBeginScene()` and end it immediately after the matching
  `sceGxmEndScene()` path has returned.
- Wrap `sceGxmFinish()` in `VP_GRAPHICS_ZONE_SCEGXM_FINISH_WAIT`. Its duration
  is CPU-observed wait/call time, not a GPU command duration.
- Wrap `sceGxmDisplayQueueAddEntry()` in
  `VP_GRAPHICS_ZONE_SCEGXM_DISPLAY_QUEUE_SUBMIT`.
- Publish draw-call totals once per frame with the matching counter hook.

Example:

```c
struct vp_zone_scope finish;

if (vp_graphics_zone_begin(&graphics,
        VP_GRAPHICS_ZONE_SCEGXM_FINISH_WAIT, &finish) == VP_RESULT_OK) {
    int result = sceGxmFinish(context);
    vp_graphics_zone_end(&graphics, &finish);
    return result;
}
return sceGxmFinish(context);
```

Always execute the real graphics call if the profiler ring is full. A dropped
zone begin leaves the scope inactive, so omit the end call in that path, as in
the example. Production wrappers should preserve the application's existing
error and cleanup flow.

The interfaces and name integration compile for both host and Vita targets and
have native event/dictionary tests. No automatic VitaGL/SceGxm integration and
no hardware timing claim are part of this increment.

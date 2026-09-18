# Cooperative VitaGL and SceGxm instrumentation

`vitaprofiler_gpu.h` is an opt-in, header-independent helper layer for code
that already owns VitaGL or SceGxm calls. It includes neither SDK, does not
discover renderer state, and does not interpose, patch, or automatically wrap
a graphics library. The application or pinned graphics-library source chooses
each call site and retains all call, error, thread, and shutdown ownership.

Every duration is CPU wall time observed around a call. The API provides no
GPU timestamps and cannot distinguish GPU execution from command construction,
driver work, queueing, or CPU waits.

## Stable operations

`vp_graphics_register_extended_names()` registers all labels before the
dictionary is sealed. Their FNV-1a IDs are public `VP_GRAPHICS_NAME_ID_*`
constants and are tested so captures remain comparable. The original
`vp_graphics_register_names()` remains the compatibility path for the five
original zones and two draw counters; deeper helpers return
`VP_ERROR_UNSUPPORTED` when initialized from that smaller name set.

| Kind | Stable name | Intended owned boundary |
| --- | --- | --- |
| Zone | `vitagl.frame.cpu` | Complete application frame around VitaGL work |
| Zone | `vitagl.swap_buffers.cpu` | `vglSwapBuffers()` |
| Zone | `vitagl.draw.cpu_submit` | One VitaGL draw submission |
| Zone | `vitagl.shader.cpu` | Shader create, compile, link, or bind operation |
| Zone | `vitagl.state.cpu` | Material state change or a bounded state batch |
| Zone | `vitagl.alloc.cpu` | Graphics allocation or release call |
| Zone | `scegxm.scene.cpu` | Matching `sceGxmBeginScene()`/`sceGxmEndScene()` flow |
| Zone | `scegxm.draw.cpu_submit` | One `sceGxmDraw()` submission |
| Zone | `scegxm.finish.cpu_wait` | `sceGxmFinish()` CPU-observed wait |
| Zone | `scegxm.display_queue.cpu_submit` | `sceGxmDisplayQueueAddEntry()` |
| Zone | `scegxm.shader.cpu` | Program registration, binding, or release |
| Zone | `scegxm.state.cpu` | State setter or a bounded state batch |
| Zone | `scegxm.alloc.cpu` | SceGxm-related allocation or release call |
| Counter | `vitagl.draw_calls`, `scegxm.draw_calls` | Per-frame submitted draw totals |
| Counter | `vitagl.shader_changes`, `scegxm.shader_changes` | Per-frame shader changes |
| Counter | `vitagl.state_changes`, `scegxm.state_changes` | Per-frame state changes |
| Counter | `vitagl.allocation_bytes`, `scegxm.allocation_bytes` | Caller-defined live or allocated byte total |
| Frame | `graphics.frame.cpu` | One designated frame stream per profiler context |

Allocation counters intentionally do not impose live-versus-cumulative
semantics. Pick one interpretation for an integration and document it beside
the call site. Prefer per-frame counters over a zone around every inexpensive
state setter; per-draw and per-state zones can create more telemetry traffic
than the render work warrants.

## Opt-in lifecycle and disabled cost

An enabled integration:

1. initializes the caller-owned profiler context and name dictionary;
2. calls `vp_graphics_register_extended_names()` while the dictionary is
   mutable;
3. seals the dictionary and calls `vp_graphics_hooks_init()`;
4. emits from application-owned call sites; and
5. stops producers and drains before deinitializing caller-owned storage.

Define `VITAPROFILER_GRAPHICS_ENABLED` only for instrumented builds and use the
uppercase `VP_GRAPHICS_*` call-site macros. When the definition is absent,
those macros do not evaluate their arguments and compile to no profiler calls.
The graphics operation itself must always remain outside the macro.

`vp_graphics_hooks_set_enabled()` additionally provides a runtime switch. A
runtime-disabled hook checks one flag but does not read the clock or touch the
ring. Toggle only while graphics-helper producers are quiescent and no
graphics scope is active. Initialization, runtime toggle, dictionary storage,
drain ownership, and shutdown remain the application's responsibility.

## VitaGL application example

This pattern marks one application-owned frame, times nested work with the
bounded caller-owned stack, and preserves rendering if telemetry is disabled,
dropped, or full:

```c
void render_one_frame(struct renderer* renderer)
{
    struct vp_graphics_scope_stack scopes;

    vp_graphics_scope_stack_init(&scopes);
    (void)VP_GRAPHICS_FRAME_MARK(&renderer->graphics, VP_GRAPHICS_FRAME);
    (void)VP_GRAPHICS_SCOPE_PUSH(
        &renderer->graphics, &scopes, VP_GRAPHICS_ZONE_VITAGL_FRAME);

    if (renderer->shader_dirty) {
        (void)VP_GRAPHICS_SCOPE_PUSH(
            &renderer->graphics, &scopes, VP_GRAPHICS_ZONE_VITAGL_SHADER);
        bind_current_shader(renderer);
        (void)VP_GRAPHICS_SCOPE_POP(&renderer->graphics, &scopes);
        ++renderer->shader_changes;
    }

    draw_scene(renderer);

    (void)VP_GRAPHICS_SCOPE_PUSH(
        &renderer->graphics, &scopes,
        VP_GRAPHICS_ZONE_VITAGL_SWAP_BUFFERS);
    vglSwapBuffers(GL_FALSE);
    (void)VP_GRAPHICS_SCOPE_POP(&renderer->graphics, &scopes);

    (void)VP_GRAPHICS_COUNTER(
        &renderer->graphics, VP_GRAPHICS_COUNTER_VITAGL_DRAW_CALLS,
        renderer->draw_calls);
    (void)VP_GRAPHICS_COUNTER(
        &renderer->graphics, VP_GRAPHICS_COUNTER_VITAGL_SHADER_CHANGES,
        renderer->shader_changes);
    (void)VP_GRAPHICS_SCOPE_POP(&renderer->graphics, &scopes);
}
```

The fixed stack permits eight nested scopes, allocates nothing, and returns
`VP_ERROR_CAPACITY` without emitting an event on overflow. Pushes whose begin
event is dropped still reserve one logical stack level, so the matching pop is
safe and emits no unmatched end. Pop is LIFO and rejects underflow. A stack
belongs to one call flow; do not share it between render threads.

For complex functions with early returns, use one cleanup label and pop every
successfully pushed logical level there. Never skip or repeat the real graphics
operation based on a profiler result.

## Pinned VitaGL source / direct SceGxm example

Only source that directly owns an SceGxm call can place the corresponding
hook. For example, inside a reviewed VitaGL source build:

```c
int submit_display_entry(struct renderer* renderer)
{
    struct vp_zone_scope submit;
    int profile_result;
    int result;

    profile_result = VP_GRAPHICS_ZONE_BEGIN(
        &renderer->graphics,
        VP_GRAPHICS_ZONE_SCEGXM_DISPLAY_QUEUE_SUBMIT, &submit);
    result = sceGxmDisplayQueueAddEntry(
        renderer->old_sync, renderer->new_sync,
        renderer->display_data);
    if (profile_result == VP_RESULT_OK)
        (void)VP_GRAPHICS_ZONE_END(&renderer->graphics, &submit);
    return result;
}
```

The same structure applies to `sceGxmDraw()`, `sceGxmFinish()`, shader/state
operations, and allocation calls. `sceGxmFinish()` specifically measures CPU
time spent in the call, which may include a wait; it is not GPU command timing.
Application code outside VitaGL must not claim to wrap SceGxm calls that VitaGL
owns internally.

## Ordering, overload, and export

The helpers emit ordinary `VP_EVENT_ZONE_BEGIN`, `VP_EVENT_ZONE_END`,
`VP_EVENT_COUNTER`, and `VP_EVENT_FRAME` records. Existing VPRF wire encoding,
decoded JSON, and Chrome Trace/Perfetto export therefore need no graphics-only
format or receiver changes. Host tests exercise nested ordering, stable IDs,
stack overflow/underflow, compile-time and runtime disabled paths, and the
complete C writer to Python JSON/Perfetto pipeline.

The core ring remains the final bound. A full ring drops without waiting and
increments normal profiler drop statistics. Do not synchronously drain or
perform network sends from render call sites. Use a dedicated cooperative
drain thread or an existing non-render control thread with explicit
shutdown/join ownership.

The [Render96EX VitaGL integration record](render96ex-vitagl-integration.md)
identifies concrete application-owned frame, draw, and swap boundaries. The
deeper operation set above has host coverage only in this change. It still
requires an opt-in Vita/Render96EX capture to measure overhead, event volume,
balance, loss, and shutdown behavior on hardware.

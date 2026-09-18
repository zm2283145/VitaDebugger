# Render96EX VitaGL integration and hardware baseline

This records the cooperative call-site audit and subsequent default-off
integration in the `Render96Ex-Vita` checkout. It deliberately does not patch
imports, interpose VitaGL/SceGxm symbols, or modify a graphics library binary. The `vitaprofiler_gpu.h` hooks record CPU-observed time only; they are not GPU
timestamps. The library now exposes deeper draw submission, shader/state,
allocation, frame-marker, and bounded nesting helpers, but the captured
Render96EX baseline below predates those additions.

## Audit identity

The source audit was performed on 2026-09-15 against
`https://github.com/Render96/Render96ex.git` at base commit
`5c4eb73881b4a1dec2fdfdb16071112d5ff3070c`. The working checkout was dirty:
`src/pc/gfx/gfx_vitagl.c` differed from that commit by five added lines and one
removed line. These are the Git blob IDs of the exact working-copy inputs:

- `src/pc/pc_main.c`: `071b06028639fdc8027901ae0884458fee71fc12`
- `src/pc/gfx/gfx_vita.c`: `4a81869ce1cb62e022b5fcf113f6e41b7f921371`
- `src/pc/gfx/gfx_vitagl.c`: `29edfc3e1f0369f0b28d3509c2598bfcb84a3096`

Those hashes identify the inputs before instrumentation. The implemented
working-copy blobs reviewed on 2026-09-15 are:

- `src/pc/pc_main.c`: `3467649e5abe064ff2d548b2e17df7c981511ad2`
- `src/pc/gfx/gfx_vita.c`: `1199c29a7e896f525680ad478caa864f6aebac23`
- `src/pc/gfx/gfx_vitagl.c`: `391f51e3ea928515f9dd25e7e872cac4dfdc3479`
- `src/pc/vita_profiler.c`: `91a5eeb25052ee2996c6021aa9212bc5f53055bf`
- `src/pc/vita_profiler.h`: `4a1b274d352476623693228daf91c988846f0ee6`
- `src/goddard/draw_objects.c`: `6683c43f5bcd29fc4aa1095eb0b15cec95caced7`
- `src/goddard/renderer.c`: `59732f655bd137d00619bd2b1cfaef30d8be0426`

Re-run the call-site audit if any of those contents change. The native
VitaProfiler integration test preserves the intended five-event ordering and
balance contract; it is deliberately synthetic and does not scan or compile a
Render96EX checkout.

## Audited ownership points

- `src/pc/pc_main.c::produce_one_frame()` owns one complete game frame. It
  calls `gfx_start_frame()` twice for the game's two rendering passes, so
  `gfx_vitagl_start_frame()` is not a reliable whole-frame boundary.
- `src/pc/gfx/gfx_vita.c::gfx_vita_swap_buffers_end()` directly owns the only
  audited `vglSwapBuffers(GL_FALSE)` call.
- `src/pc/gfx/gfx_vitagl.c::gfx_vitagl_draw_triangles()` directly owns the
  audited `vglDrawObjects(GL_TRIANGLES, count)` call.
- No direct `sceGxmBeginScene()`, `sceGxmEndScene()`, `sceGxmFinish()`, or
  `sceGxmDisplayQueueAddEntry()` call is present under `Render96Ex-Vita/src`.
  VitaGL owns those calls internally, so Render96EX must not pretend to wrap
  them from outside the library.

## Implemented opt-in integration

The normal build stays unchanged. `VITA_PROFILER=0` is the default; only its
`1` branch adds the profiler include path, archive, network libraries, and
`VITA_PROFILER_ENABLED`. State and lifecycle live in the single
Render96EX-owned `src/pc/vita_profiler.c` translation unit rather than exposing
profiler globals across the renderer.

That owner now:

1. initialize one caller-owned event ring and name dictionary;
2. call `vp_graphics_register_names()` with the dictionary still unsealed;
3. seal the dictionary, initialize `vp_graphics_hooks`, and start any drain
   owner before publishing the enabled flag;
4. begin `VP_GRAPHICS_ZONE_VITAGL_FRAME` at entry to
   `produce_one_frame()` and end it after the second `gfx_end_frame()`;
5. count successful calls through `gfx_vitagl_draw_triangles()` and publish
   `VP_GRAPHICS_COUNTER_VITAGL_DRAW_CALLS` once at the same frame boundary;
6. wrap the real `vglSwapBuffers()` call with
   `VP_GRAPHICS_ZONE_VITAGL_SWAP_BUFFERS`; and
7. records coarse Mario-head/Goddard phase zones and per-view shape/command
   counters; and
8. stops producers, drains completely, closes the stream/sink with retries,
   and only then deinitializes the dictionary and ring.

The real draw/swap/frame operation must execute exactly once regardless of a
profiler return value. If a zone begin is dropped, leave its inactive scope
alone and continue the game. Do not synchronously drain or perform a bounded
TCP send from the render call site: the sink may wait until its configured
deadline and would turn receiver backpressure into a frame hitch. Use a
dedicated cooperative drain thread or an existing non-render control thread,
with an explicit shutdown/join owner.

An optional second phase may patch the project's pinned VitaGL **source** at
the exact SceGxm call sites it owns and use the deeper SceGxm draw, wait,
display-queue, shader/state, and allocation hook IDs. The application-owned
Render96EX layer can also adopt `graphics.frame.cpu`, per-frame shader/state
and allocation counters, and the compile-time no-op macros. That must be a
reviewed source build with balanced cleanup on every return path. Runtime
symbol replacement, import hooks, and binary interposition remain out of
scope.

## Bounded integration gate

The first enabled build captured 300 frames through startup and the fixed
Mario-head scene and passed the frame/draw/swap, balance, transport, and clean
EOF requirements below. Its exact results are in the
[hardware record](../../docs/hardware/profiler-render96ex-head-baseline-2026-09-15.md).
That capture predates the Goddard and PMU additions. A second 300-frame capture
then passed the combined integration with 5,423 events, 25 resolved names,
2,321 balanced scope pairs, zero loss, 203 complete Goddard samples, and 75
bounded event-`0x01` PMU reads followed by a clean exact-restoring close.
It met the following requirements:

- 300 balanced `vitagl.frame.cpu` zones;
- one `vitagl.draw_calls` sample per complete frame;
- balanced `vitagl.swap_buffers.cpu` zones for every actual swap;
- zero unmatched scopes after receiver validation;
- explicit ring-drop and TCP transport statistics; and
- clean drain, EOF, thread join, and repeated application relaunch.

The second capture measured a 3.917 ms median `goddard.head.cpu` callback inside
an 83.373 ms median head frame, with 828 median VitaGL draws and 0.379 ms median
total time in the two CPU-observed swaps. The next hardware capture should place the new hooks below Goddard list
construction at source-owned VitaGL/SceGxm submission or synchronization
points. No such deeper Render96EX hardware capture is claimed by the current
library-only change.

Compare frame time with telemetry disabled, enabled without a receiver, and
enabled with a receiver. A receiver outage must fail/disable telemetry without
skipping rendering or leaving the game stuck in shutdown. This gate can
characterize CPU-side submission and wait cost; it cannot label any duration
as GPU execution time.

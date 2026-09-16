# Real-world VitaGL profiler integration

This record summarizes two bounded 300-frame captures from a graphics-heavy,
VitaGL-based 3D homebrew application on retail 3.65 hardware. The application
was chosen because it contains a complex interactive model scene, asset-heavy
transitions, and an in-game demo workload; it is a practical integration test,
not a dependency of VitaDebugger.

Both captures completed with zero profiler-ring loss and zero TCP transport
loss. The follow-up capture:

- resolved 25 stable event names;
- balanced 2,321 begin/end scope pairs;
- recorded 75 bounded PMU event-`0x01` samples;
- closed the PMU lease with exact restoration; and
- produced data usable by the text, decoded-JSON, and Perfetto paths.

In the tested complex model scene, the high-level scene callback accounted for
about 3.9 ms of an 83.4 ms median frame. That result directed the next
instrumentation split toward graphics submission, synchronization, and true
GPU work rather than the already-measured high-level callback.

The captures demonstrate that the profiler can run in a substantial real
application and preserve useful timing/counter data without dropping records.
They do not claim that the profiler automatically identifies every bottleneck,
nor do CPU-observed VitaGL/SceGxm call-site timings equal hardware GPU
timestamps.

See also:

- [binary trace pipeline](../../profiler/docs/binary-trace.md)
- [cooperative graphics hooks](../../profiler/docs/graphics-hooks.md)
- [retail PMU evidence](profiler-pmu-retail-3.65.md)
- [dormant-thread PMU re-arm result](../../kernel/pmu-profiler-thread-exit-gate/hardware-results/2026-09-15-first-attempt/README.md)

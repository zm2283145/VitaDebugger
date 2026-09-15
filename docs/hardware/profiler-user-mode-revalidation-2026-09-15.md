# VitaProfiler user-mode revalidation on retail 3.65

Date: 2026-09-15

The ordinary user-mode probe (`VDPR00001`) was rebuilt from the current
integration worktree, installed through the already hardware-tested signed
VitaDevDeploy FTP path, and launched through Vita Companion 1.06. No kernel
plugin was changed for this run.

The VitaDevDeploy result was `success` at stage `complete` for signed job
`9e56b0fde28072078fb673ccad2ce998`. The displayed probe result was manually
confirmed as **PASS: 13 checks, 0 failures**.

This revalidates:

- the sealed name dictionary and wire encoding;
- timing zones, frame markers, counters, and live memory/thread samples;
- FIFO event shape and ID-to-name resolution;
- four concurrent producer threads;
- bounded pressure/drop accounting, complete-record draining, and slot reuse.

Artifacts built and tested:

- `vitaprofiler-probe.vpk`: 78,411 bytes, SHA-256
  `F7F39388A74C2D42B69A0AE7CCF2C1EB37FDB5EEFA70A8B023D24F3D228D2A64`
- `vitaprofiler-probe.elf`: 129,336 bytes, SHA-256
  `7BF1E17BC482F7AD536F0FD2A171ADDE4EAB4FB7D4207828A6ACF96B9AD4C36E`

This gate validates the profiler library's user-mode core after adding the
Vita TCP sink to the archive. It does **not** by itself validate a live TCP
capture, production PMU events, or VitaGL/SceGxm interception; those remain
separate gates.

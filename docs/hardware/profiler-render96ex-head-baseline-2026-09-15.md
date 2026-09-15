# Render96EX Mario-head profiler baseline on retail 3.65

Date: 2026-09-15

The default-off Render96EX profiler integration completed its first bounded
application capture on a retail PS Vita running system software 3.65. The
separate `R96PRF001` build streamed exactly 300 complete frames to
VitaProfiler's PC receiver while the game advanced from startup into the
interactive Mario-head press-start scene.

## Integrity

The Vita status record reported `stage=complete`, `result=2`, 300 frames, 2,100
accepted and written events, 67,748 transmitted bytes, zero ring drops, zero
writer loss, zero TCP failures, an empty pending ring, and closed writer/TCP
state 3. All 15 dictionary entries decoded and no referenced name ID was
unresolved.

- Profiler VPK SHA-256:
  `9C8847D74084EC93200E8082A8963B67C3F877C9DB6F73485C3FCC7C67D013B0`
- Raw capture SHA-256:
  `EFEFD98FCB12B1929DD1E8F290673ABD48E9426727F004B11C61695831BAD793`
- Decoded JSON SHA-256:
  `724032907AC077175C4D5617CDB75F77889C6FFEEA40B0D830FD69EF1E7D7879`
- Perfetto JSON SHA-256:
  `933482BD63A9B60A80A49AA27A219F008DE522FADC917476BD48EFF47AB7C5E4`
- Vita status SHA-256:
  `59DE2FDD443047F3F19A3524F39A0FA09EC069DA2E0DEEDAC598FD5221238FB7`

The capture, decoded/viewer outputs, and Vita status were reviewed and hashed
during the run but are not duplicated in this repository; the hashes above
identify those exact files.

## First result

Excluding the first startup frame, the initial low-complexity screens held near
36.8 ms/frame with 52 VitaGL draw calls. Frames 96 and 98 contained one-time
loading stalls of approximately 1.01 and 6.09 seconds. Frames 126 through 300
formed the stable Mario-head workload:

- frame time was approximately 82--84 ms, or about 12 FPS;
- draw calls ranged from approximately 814 to 852 per frame and averaged about
  829; and
- the two `vitagl.swap_buffers.cpu` zones together averaged about 0.38 ms per
  frame.

This rules out the instrumented buffer-swap calls as the dominant CPU-observed
cost and shows an approximately sixteen-fold draw-call increase relative to
the 52-draw screens. It does not yet distinguish animation/skinning, scene
traversal, display-list construction, VitaGL state/command submission, an
earlier synchronization point, or true GPU execution.

## Coverage boundary

This baseline emitted `vitagl.frame.cpu`, `vitagl.swap_buffers.cpu`, and
`vitagl.draw_calls`. It did not contain the subsequently added Goddard phase
zones or PMU samples and explicitly recorded `cpu_observed_only=1`. The next
Render96EX capture had to validate those coarse Goddard zones and the bounded
PMU path without weakening the one-real-event-per-boot gate.

## Follow-up Goddard + PMU capture: passed

The follow-up 300-frame capture completed on the same date with 5,423 events,
zero ring/transport loss, closed writer/TCP state 3, 25 resolved names, and
2,321 exactly balanced zone pairs. It contained 203 complete instances of
every Goddard zone and both Goddard workload counters.

- Profiler VPK SHA-256:
  `723F0742E77E17C71816682B57F59CA4C3F07380471B21F1411E3AAAFB41D5ED`
- Installed `eboot.bin` SHA-256:
  `B6481399CBBC9310BB0D5FB7FEF7D533CF61BD3AF6E82812C2D46B6294E53095`
- Raw capture SHA-256:
  `AC8DE11074EF7BC894CC2E36D2BCF6E6516A6B188680557A2C7682E07FB3F383`
- Decoded JSON SHA-256:
  `0B2DB9988A5804D97EDD5BAF2BC044AFA84EC6A3CE995436E0E6DEE998161485`
- Perfetto JSON SHA-256:
  `3D906F6A1149D653BA47FEE391AA973C15DC0AD67D119DBE61760DBEB9E58DCB`
- Vita status SHA-256:
  `849B6009BC83B4507DC3492A2C51A3866F1C044DC27C0B41CBCBFA5319412D5D`

The complete `goddard.head.cpu` callback had a 3.917 ms median and 4.157 ms
95th percentile, while the corresponding final 203 complete frames had an
83.373 ms median. Movement/skinning was the largest measured Goddard phase at
a 3.337 ms median; deferred vertex conversion was 2.522 ms and scene traversal
was 0.431 ms. The two swap calls together had a 0.379 ms median and VitaGL draw
count had a median of 828.

The PMU transport recorded 75 raw core-wide event-`0x01` samples, ending at
6,822, then returned zero from close with `pmu_active=0` and
`pmu_window_complete=1`. Discovery, validation, open, read, and event
publication all returned zero. Affinity restoration returned positive
`0x00010000`, the expected previous forced-core-0 mask and therefore a success.

This second capture validates the current Goddard instrumentation and one
bounded multi-sample PMU lease. It shifts the next performance split below the
coarse Goddard phases, toward VitaGL command/state submission, an unwrapped
synchronization point, or true GPU execution. The raw capture and status are
retained with the Render96EX integration; generated decoded and Perfetto JSON
are identified above without being duplicated here.

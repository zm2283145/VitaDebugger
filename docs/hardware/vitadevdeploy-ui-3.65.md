# VitaDevDeploy graphical UI lifecycle gate

Status: the opt-in native vita2d interface passed its supervised lifecycle gate
on a retail Vita running system software 3.65 on 2026-09-14. After the initial
checks and memory-card database update, it completed six consecutive
launch/exit cycles:
three SceShell peel closures and three Circle cleanup exits. None produced a GPU
fault or LiveArea hang during that sequence. Two intermittent launch-time GPU
faults have now been observed outside the sequence, so the UI gate no longer
supports unattended use. The headless agent remains the production default
until graphics startup/teardown and SceShell transition stress pass.

## Artifact under test

- Agent title ID: `VDEVDEP01`
- Variant: install-enabled permanent agent with
  `VDEV_ENABLE_DISPLAY_UI=ON`
- EBOOT SHA-256:
  `0d5fc0db0eea70533c086b81a533170e7e2c4808066bbc2b122b67625f7e9250`
- VPK SHA-256:
  `1e2741abe14f9af5658208a707a394a65ef3799202187ae93908ae49cf3501bf`

The build helper verified all five LiveArea entries byte-for-byte against the
reviewed source tree before publishing the VPK. The installed `bg.png`,
`startup.png`, and `template.xml` were then read back over FTP and matched the
same local files.

## Observed result

The first launch reached the waiting state and rendered the complete 960x544
interface, including its readiness state, connection message, animated
activity indicator, deployment milestones, and idle-exit instruction:

![VitaDevDeploy waiting interface](vitadevdeploy-ui-waiting-3.65.jpg)

A short Circle tap was intentionally not counted as a failure because the
idle loop samples input at 250 ms intervals. Holding Circle produced the normal
cleanup path and startup diagnostic stage 10 with result zero. The agent was
then launched a second time, reached startup stage 8 with result zero, and
again exited normally through Circle with stage 10 and result zero. SceShell
remained responsive across both launch/exit cycles.

Following the isolated report, three direct-launch/SceShell-peel-close cycles
and three direct-launch/Circle-cleanup cycles all completed normally. This
validates both the intended cleanup path and the ordinary user-facing SceShell
closure path under those repetitions. It does not rule out an intermittent
startup race. Vita Companion's unauthenticated `destroy` path was not part of
this gate and should not be used on the display build.

## Later intermittent launch incident

Later on 2026-09-14, Vita Companion's title-scoped kill of `SLRS00001` returned
`Killed.` and the host immediately requested launch of `VDEVDEP01`. This abrupt
target-to-deployer transition is outside the supported normal deployment path,
which starts with the Vita already at LiveArea. The host timed out before it
could read a fresh `challenge.v1`, FTP became unreachable, and the device
displayed another GPU fault. No signed job had been uploaded or installed. This
was the second observed launch-time GPU fault and means the earlier event can
no longer be classified as non-repeating.

After recovery, the graphical agent was opened again and remained stable at
its waiting screen. The host reused that exact live challenge with
`--reuse-running-agent`; job `1f67454d9d53e729acffaf161d9cf1cb` completed
verification and installation with result zero, and the target launched. This
separates the intermittent pre-challenge/display-startup failure from the
signed package verification and promotion path, but it does not yet identify
which vita2d/GXM or SceShell transition call failed. Do not automate another
graphical launch/close stress gate until that lifecycle is audited and a
recovery plan is ready.

The leading hypothesis is a display-resource handoff race: the killed target
owned a debug-screen framebuffer and did not run an orderly display shutdown,
while the deployer entered vita2d/GXM immediately on launch. This remains an
inference, not a confirmed root cause. The next gate should preserve the crash
dump and startup trace, add markers around vita2d initialization/first draw/
first swap, wait for the old target and transports to quiesce, and A/B the same
transition with the headless agent.

## LiveArea presentation follow-up

The custom bubble icon appeared, but SceShell initially continued to show its
generic background and gate after the in-place package update:

![LiveArea before cache refresh](vitadevdeploy-livearea-3.65.jpg)

Because the installed artwork and template bytes matched the VPK, this was
classified as a SceShell presentation-cache refresh check rather than an asset
integrity or deployment failure. The normal system database update triggered
by removing and reinserting the memory card subsequently made the custom
background visible. This was a media/content rescan, not deletion and rebuild
of `app.db`. It confirms the asset package while remaining separate from the
in-app lifecycle gate.

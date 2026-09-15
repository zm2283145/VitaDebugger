# VitaDebugger

VitaDebugger is an experimental development toolkit for PlayStation Vita
homebrew. The repository combines an application-linked GDB server
(`libuvdb.a`), an optional narrow kernel companion, bounded network logging, a
low-overhead profiler foundation, and the signed VitaDevDeploy remote
installation helper.

The project is based on
[sleirsgoevy/vita-uvdb](https://github.com/sleirsgoevy/vita-uvdb) and is being
hardened, documented, tested on hardware, and expanded into a general VitaSDK
development tool. It is designed to be embedded in ordinary VitaSDK C or C++
homebrew rather than tied to one game or engine.

## Why this project exists

Vita homebrew development is unusually difficult to diagnose. Community
developers generally use the unofficial VitaSDK toolchain and ordinary retail
Vita hardware, without access to Sony's official SDK, official debugging tools,
or scarce development kits. A crash may provide only a dump or crash screen;
timing problems often require manually added file logs; and repeatedly building,
installing, launching, reproducing, and retrieving logs makes iteration slow.

This project aims to give the homebrew community a practical workflow using
hardware and software developers can actually obtain:

- Stop an application before the standard crash screen takes over.
- See the precise source line, call stack, registers, and relevant memory.
- Set breakpoints and inspect or change state while the program is running.
- Stream logs and diagnostic events over the network.
- Profile CPU time, frame pacing, memory use, and eventually GPU work.
- Build, install, launch, debug, and profile from normal VitaSDK projects and
  familiar IDEs with as little manual device interaction as practical.

It is intended for legitimate homebrew development and debugging on systems the
developer controls. It does not include or require Sony's proprietary SDK.

## Current status

The tested compatibility target is homebrew-enabled retail Vita hardware
running system software 3.65, including handheld PS Vita models and Vita TV.
The application-side library and optional kernel companion have run on both
device classes. That compatibility result does not mean every experimental
feature gate was repeated on both classes. Each "hardware-tested" claim below
is scoped to the exact feature, device class, configuration, and artifact named
by its linked evidence. Firmware 3.60 and every other firmware release,
development hardware, and other plugin combinations remain untested. Current
status includes:

- GDB connections over TCP.
- Source and function breakpoints.
- ARM and Thumb software breakpoints (`Z0` and `z0`).
- Register, stack, and arbitrary application-memory inspection.
- Live variable modification.
- Source-level backtraces.
- Basic instruction and source stepping.
- Taken and non-taken 16-bit Thumb conditional branches.
- Selected Thumb-2 `B.W`, conditional `B.W`, `BL`, and `BLX` instructions.
- Thumb `POP {..., PC}` returns and high-register `MOV PC, Rm` branches.
- Thumb-2 `TBB` and `TBH` table branches with protected table-entry reads.
- Thumb-2 `LDMIA/POP.W` returns that restore `PC` from a register list.
- Thumb `IT` blocks using saved condition flags and width-aware skipping.
- Thumb-2 `LDMDB` returns that restore `PC` from below the base address.
- ARM-state branches, `BX`/`BLX`, data-processing writes to `PC`, and
  increment/decrement load-multiple PC restores. The data-processing planner
  covers immediate and immediate-shifted register operands for the practical
  `AND`/`EOR`/`SUB`/`RSB`/`ADD`/`ADC`/`SBC`/`RSC`/`ORR`/`MOV`/`BIC`/`MVN`
  forms.
- Thumb high-register `ADD`/`MOV`/`BX`/`BLX`, `CBZ`/`CBNZ`, and Thumb-2/ARM
  immediate, negative, pre/post-indexed, and register-offset load-to-PC forms
  with protected target reads and correct ARM/Thumb interworking.
- Catching data aborts before the standard Vita crash screen.
- Structured fault information: exception type, signal, FSR, FAR, PC, LR, SP.
- Reconnection at a later `uvdb_enter()` after a disconnected session.
- Opt-in persistent server thread for clean reattachment and experimental
  Ctrl-C interruption while the application is running.
- Hardware-tested cooperative registration and GDB discovery of named
  application threads.
- Hardware-tested persistent attachment, Ctrl-C interruption, clean detach,
  immediate reattachment, and recovery after abrupt client termination.
- A hardware-tested kernel companion providing ABI discovery,
  caller-process-only thread enumeration, tokenized stop/resume sessions, and
  watchdog recovery from an abandoned session.
- Fail-closed library startup validation for the exact kernel ABI, required
  thread-control capabilities, and 64-entry inventory contract.
- Hardware-tested GDB all-stop integration: initial attach, live Ctrl-C,
  lease renewal during long stops, continue, detach, reconnect, and recovery
  after forced client termination.
- A bounded unified thread inventory with hardware-tested `Hg`/`Hc` and
  `vCont;c;s` behavior. Deterministic live tests stepped two distinct foreign
  worker paths and attributed each stop to the requested thread. GDB's normal
  positive-`Hc` breakpoint step-over now retains the healthy all-stop token,
  keeps peer application threads suspended while the exact stopped exception
  thread executes one instruction, and passes detach/reconnect testing. The
  internal lease keeper remains runnable to maintain the stop. True scheduler-locked
  execution of an arbitrary foreign thread remains pending.
- Hardware-tested practical GDB stepping for the exact positive-`Hc` hidden-
  breakpoint step-over without `E16`, representative ARM-state `MOV PC, Rm`,
  decrementing load-multiple, and `LDR PC, [Rn]` writers, plus bounded Thumb-2
  and A32 `LDREX`/`STREX` sequences. Both exclusive sequences completed one
  successful store and stopped after `STREX`; abrupt disconnect recovery
  removed both armed traps and restored their exact original instruction bytes.
- Hardware-tested read-only GDB register integration for main and worker
  threads owned by an active stop session, including state-dependent selection
  of runnable/current and syscall-return ARM banks plus symbolized stack frames.
- Strict individual-register `p` reads for ARM core, CPSR, legacy unavailable
  FPA slots, and opt-in D0-D31/FPSCR. Selected exception-thread R0/CPSR `P`
  writes pass live mutation, read-back, exact restoration, clean detach, and
  reconnect on retail 3.65. Foreign-thread core and floating-point writes are
  hardware-confirmed fail-closed and remain disabled pending a separately
  restorable kernel mutation path.
- Hardware-tested stop-session reconciliation: threads created after the
  initial snapshot are discovered and suspended by the next lease renewal.
- Hardware-tested read-only ARM debug-resource discovery reporting six
  breakpoint, four watchpoint, and two context-aware breakpoint comparators.
- A default-off, token-protected VFP snapshot boundary for suspended
  session-owned threads, plus an opt-in D0-D31/FPSCR GDB packet path. The
  known-pattern hardware gate passes every D-register, FPSCR, ownership,
  session-end, resume, and worker-restoration check. The automated live-GDB
  gate also passes foreign-thread reads through continue, clean detach,
  transport loss, and two reconnect paths.
- GDB loaded-module discovery through chunk-safe `qXfer:libraries:read` and
  main-module `qOffsets` derived from the same live segment metadata. The
  bounded host workflow now passes installed main/SUPRX build-identity checks,
  deliberate mismatch rejection before RSP connection, an idempotent same-
  process refresh, and five main-plus-user-SUPRX source-breakpoint sessions
  across detach/reconnect and two fresh-ASLR relaunches. ASLR correctness and
  the verified-build milestone are complete.
- An initial Windows VS Code workflow and isolated sample application now turn
  F5 into build, signed deployment, launch, installed-build verification,
  live-ASLR symbol loading, GDB attachment, and exact-title cleanup. The Vita
  IPv4 address, ports, SDK paths, and private-key path live only in ignored
  per-user files; the demo exposes a visible variable for breakpoint-time
  mutation. Its build/toolchain gates and the exact GDB/MI attach, Watch-style
  assignment, continue, interrupt, detach, and reconnect sequence pass on
  retail 3.65 hardware; see the
  [validation record](docs/hardware/vscode-debug-demo-gdb-mi-3.65.json). The
  actual VS Code UI-driven F5 session also passes build/deploy/attach, source
  breakpoint, Watch-value mutation, resume, pause, and source stepping. Its
  direct-TCP revalidation additionally passes installed-EBOOT identity,
  live-ASLR symbols, detach, and exact-title cleanup. The first post-fix run
  exposed a recoverable SceShell launch race before the immediate full retry
  passed; see the
  [direct-TCP validation record](docs/hardware/vscode-debug-demo-direct-tcp-3.65.json).
- A completed bounded `stdout`/`stderr` bridge that emits GDB `O` packets only
  after no-ack negotiation, never gives application threads ownership of the
  RSP socket, and isolates output between reconnect generations. Native tests
  cover queue pressure, framing, failures, commit retry, and reconnect
  behavior. On retail Vita hardware running system software 3.65, the
  live raw-RSP gate passed two consecutive sessions: each negotiated no-ack
  mode, received an initial `T05`, decoded six `O` packets carrying both
  streams, stopped with `T02` on Ctrl-C, remained quiet while stopped, and
  detached cleanly before the fresh reconnect.
- A fixed read-only GDB `qRcmd` registry for `monitor help`, `status`,
  `threads`, `modules`, `console`, and `display`. The latter two expose bounded
  console transport/loss statistics and framebuffer/display metadata without
  consuming the log queue or reading pixels. Exact command parsing, bounded
  snapshots, safe name rendering, and line-safe response truncation pass native
  host tests. On a retail Vita running system software 3.65, two raw-RSP
  sessions and GDB 15.2 passed all six commands, state preservation, clean
  detach, and reconnect with six stopped threads, 15 loaded modules, a healthy
  console transport, and coherent 960x544 display state at 59.940 Hz. A later
  two-session raw-RSP follow-up on the rebuilt artifact also passed the
  generation-scoped post-accept display sampler with vcounts 2050 and 2135,
  clean reconnect, and no console loss or transport errors. The GDB front-end
  portion remains evidence from the original full functional gate.
- Hardware-tested DebugNet-compatible UDP logging with bounded messages,
  concurrent producers, stop/restart under load, and GDB attach/detach
  coexistence.
- A debugger-enabled larger application build as a real-world integration test.
- A separately versioned kernel thread-mutation transaction ABI with bounded
  snapshot, provisional stage, exact read-back verification, commit, explicit
  restore, retained-target lifetime checking, and stop-lease cleanup. Each
  transaction covers exactly one register bank and requires a fully stopped
  process with no extra exempt thread. Its lifecycle and rollback behavior pass
  native tests. The current Vita backend intentionally advertises zero writable
  register banks because no documented, typed, hardware-validated foreign-
  thread core/VFP setter and durable target-object provider are available; no
  new on-device write capability is claimed yet. VitaSDK's NID database contains
  an untyped VM-context setter lead whose 3.65 ABI still requires static
  verification.
- A standalone allocation-free profiler foundation for named zones, counters,
  frame markers, Vita memory snapshots, and known-thread statistics.
- A hardware-tested user-mode Vita profiler self-test covering event order,
  timing zones, counters, memory/thread snapshots, bounded multithreaded
  pressure, wire encoding, exact drop accounting, queue-slot reuse, and the
  bounded name dictionary. All 13 on-device checks pass, including live
  resolution of every captured custom and built-in event ID, without calling
  the kernel plugin.
- A hardware-tested profiler capture pipeline: an allocation-free callback
  drain, caller-owned Vita TCP sink, bounded PC receiver, named text summaries,
  complete decoded JSON, and Chrome Trace/Perfetto export. Two real retail-3.65
  captures delivered and independently decoded all 13 expected events with
  clean EOF; pre-connect cancellation, forced peer disconnect, and a successful
  recovery relaunch also passed. Cooperative CPU-side VitaGL/SceGxm hook points and a
  fail-closed PMU provider/lease boundary are implemented. Retail 3.65 hardware
  rejected both user-mode `ScePerf` loading paths, so the direct adapter now
  returns unsupported instead of calling an unresolved stub. Optional kernel
  ABI v1.13 safely reads a same-core Cortex-A9 PMU inventory, reports six event
  counters, and leaves all observed control state unchanged. A separate
  disposable kernel gate then passed the first bounded counter mutation on all
  three application cores: fixed lane 5, software-increment event `0x00`,
  `17/17` read-back, and byte-exact gate-snapshot restoration with no retained
  obligation. A host-tested bridge and separately versioned, default-off kernel
  transport now connect that session to VitaProfiler's exact-restore provider
  ABI with fixed core 0/lane 5, an owner-bound lease, watchdog, and orphan
  cleanup. Its ordinary experimental build remains event `0x00`; a second
  compile gate plus request acknowledgement admits only `0x01`, `0x03`, and
  `0x10`. Separate durable-journal hardware gates then passed all three events
  on retail 3.65: fixed core-0/lane-5 samples returned 56, 23, and 97
  respectively, and every close proved exact restoration. Timeout,
  disconnect, process-exit, ownership-conflict, and safe re-arm gates remain,
  so this does not enable unrestricted production PMU sampling.
  A first 300-frame Render96EX TCP capture also completed with zero ring or
  transport loss. It measured the stable Mario-head workload at roughly
  82--84 ms/frame and 814--852 draw calls/frame while the two CPU-observed
  swaps totaled only about 0.38 ms/frame, localizing the main cost ahead of the
  instrumented swap calls. A follow-up combined capture then passed the coarse
  Goddard zones and 75 bounded `0x01` PMU reads with balanced scopes, zero
  transport loss, and a clean exact-restoring close. Its 3.917 ms median
  Goddard callback inside an 83.373 ms median head frame moves the next split
  into VitaGL submission/synchronization or true GPU work. Binary graphics
  interposition is intentionally not planned.
- A read-only external-attach protocol and allocation-free broker core with
  exact-title discovery, kernel-ABI/capability negotiation, short-lived identity
  tickets, bounded host tooling, native tests, and a passing Vita cross-build.
  Its Vita identity adapter fails closed: no resident listener, trusted foreign-
  target identity provider, module injection, process mutation, or live GDB
  attach is implemented yet.
- A bundled, separately licensed VitaDevDeploy subproject. Its signed normal
  install-and-launch path has passed on retail hardware and its host-side
  validation, signing, startup, transport, and recovery logic has 73 automated
  tests. Interrupted-install fault injection and bootstrap recovery validation
  are still in progress.

It is already useful for controlled application debugging. It is not yet a
complete system-wide debugger; see [Current limitations](#current-limitations).
The [`cerwym/kvdb` feature-parity checklist](docs/kvdb-feature-parity.md) tracks
equivalent debugger capabilities and the hardware gates required before each
one is advertised. VitaDebugger also targets guarded hardware execution
breakpoints (`Z1`), which are beyond KVDB's documented `Z2`-`Z4` watchpoint
handlers.

## Hardware validation

The overall runtime-compatibility baseline includes retail handheld PS Vita and
Vita TV hardware running system software 3.65. Individual milestone reports
remain narrower: they document the device class and configuration actually used
for that gate, and must not be read as proof that every gate ran on both device
classes. They also make no claim about other firmware, newlib revisions,
development hardware, or plugin combinations.

The kernel boundary is being introduced in deliberately small stages. These
Vita screenshots and structured records capture the completed probe results:

| Milestone | Hardware evidence | What it validates |
| --- | --- | --- |
| v1 | [Thread enumeration](docs/hardware/kernel-probe-v1-enumeration.jpg) | ABI discovery, caller-process thread IDs, bounds and NULL rejection |
| v2 | [Single-thread suspend](docs/hardware/kernel-probe-v2-single-thread-suspend.jpg) | `0x1002` suspended state, normal resume to state 0, disposable worker behavior |
| v3 | [Stop session and watchdog](docs/hardware/kernel-probe-v3-stop-session-watchdog.jpg) | Tokenized process stop/end and automatic recovery of an abandoned lease |
| v4 | [Lease-keeper exemption](docs/hardware/kernel-probe-v4-lease-exemption.jpg) | A validated exempt thread remains active to renew long GDB stop sessions |
| v5 | [Saved register banks](docs/hardware/kernel-probe-v5-register-banks.jpg) | Session ownership checks and both raw ARM banks; the sleeping syscall-bound worker's resumable user state was observed in entry 1, while later runnable-worker tests established that selection is state-dependent |
| v6 | [Late-thread reconciliation](docs/hardware/kernel-probe-v6-late-thread-reconcile.jpg) | A thread created after stop begins is discovered on renewal, suspended, tracked, and resumed with the session |
| v7 | [Hardware-debug discovery](docs/hardware/kernel-probe-v7-hw-debug-discovery.jpg) | Read-only CP14 identification and the Vita's six breakpoint, four watchpoint, and two context-aware comparator counts |
| VFP v8 | [Corrected D32/FPSCR probe](docs/hardware/kernel-vfp-probe-v8-bank0-pass.jpg) | Guarded D0-D31 capture, raw FPSCR entry-0 mapping, ownership rejection, two-thread stop/resume, and clean worker restoration |
| VFP live GDB | [Lifecycle evidence](docs/hardware/gdb-vfp-live-3.65.json) | Foreign-thread D0/D31/FPSCR reads across continue/interrupt, detach/reconnect, transport loss without `D`, recovery, and final detach |
| Individual `p`/`P` | [Transactional evidence](docs/hardware/gdb-register-pp-3.65.json) | Direct R0/CPSR packet mutation, byte-exact read-back and restoration in two sessions, clean detach/reconnect, and unchanged foreign core/VFP state after rejected writes |
| ARM/Thumb step | [Live GDB evidence](docs/hardware/gdb-arm-thumb-step-3.65.json) | GDB's exact positive-`Hc` hidden-breakpoint step-over without `E16`, three representative ARM-state PC writers with exact register effects, clean detach, and reconnect |
| Practical step/register gate | [Exclusive-step lifecycle evidence](docs/hardware/gdb-step-register-exclusive-3.65.json) | Hidden-breakpoint step-over without `E16`, bounded Thumb-2 and A32 `LDREX`/`STREX`, restored R0/CPSR `p`/`P` transactions, exact trap-byte restoration after abrupt disconnect, and clean reconnect |
| ASLR + verified build | [Build-identity lifecycle evidence](docs/hardware/gdb-aslr-build-identity-3.65.json) | Installed main/SUPRX identity verification, deliberate mismatch rejection with the last good view preserved, idempotent same-process refresh, and five source-breakpoint sessions across reconnect and two ASLR relaunches |
| GDB monitor registry | [Console/display lifecycle evidence](docs/hardware/gdb-monitor-console-display-3.65.json) | Two raw-RSP sessions and GDB 15.2 passed `help`, `status`, `threads`, `modules`, `console`, and `display` with state preservation, clean detach/reconnect, non-consuming console statistics, and read-only display metadata |
| DebugNet v24 | [Sustained UDP stream](docs/hardware/debugnet-v24-sustained-stream.jpg) | Logging remains live after a loaded stop/restart cycle, with the displayed queue draining and no packet drops or send errors |
| Profiler + names | [13-check probe](docs/hardware/profiler-name-dictionary-3.65.json) | Thirteen passing user-mode checks, including the original timing/counter/snapshot/ring-pressure coverage plus bounded dictionary encoding and live resolution of every captured custom and built-in event ID; no kernel calls |
| Profiler live TCP | [Two validated captures and recovery evidence](docs/hardware/profiler-tcp-stream-retail-3.65.md) | Caller-owned SceNet connection, sealed dictionary plus 13-event stream, PC-side validation/decoding/Perfetto export, clean EOF and teardown, pre-connect cancellation, forced peer reset, and successful recovery relaunch; no kernel calls |
| PMU session gate | [Per-core write/restore evidence](docs/hardware/profiler-pmu-session-gate-3.65.md) | The isolated lane-5 software-increment transaction passed application cores 0, 1, and 2 with `17/17` read-back, zero operation/restore errors, exact gate-snapshot restoration, and no retained obligation; arbitrary real-event continuous profiling remains disabled |
| PMU real event `0x01` | [Durable journal and screenshots](kernel/pmu-profiler-gate/hardware-results/2026-09-15-event-01/README.md) | One owner-attended core-0/lane-5 L1 instruction-cache miss/refill sample returned 56; all transport calls succeeded and the completion journal proves exact restoration |
| Render96EX profiler | [Two 300-frame Mario-head captures](docs/hardware/profiler-render96ex-head-baseline-2026-09-15.md) | The CPU/VitaGL baseline and follow-up Goddard+PMU capture both closed with zero loss; the latter resolved 25 names, balanced 2,321 scope pairs, sampled `0x01` 75 times, and localized only about 3.9 ms of an 83.4 ms median head frame inside Goddard |
| VitaDevDeploy direct TCP | [Signed transfer/install evidence](docs/hardware/vitadevdeploy-direct-tcp-retail-3.65.md) | Verification-only and disposable install-and-launch jobs returned durable success, installed-EBOOT readback matched, recovery status was clean, and the guarded agent completed twelve launch/Circle-exit cycles without a new GPU dump |
| VS Code direct-TCP F5 | [Build/deploy/debug evidence](docs/hardware/vscode-debug-demo-direct-tcp-3.65.json) | The immediate full retry passed build, signed install, launch, EBOOT identity, ASLR symbols, source breakpoint, live mutation, resume, detach, and exact-title cleanup; the first attempt's recoverable SceShell launch race remains open |
| VitaDevDeploy UI | [Retail 3.65 lifecycle evidence](docs/hardware/vitadevdeploy-ui-3.65.md) | Two old launch-time GPU faults led to a wait-before-pool-reuse guard; its replacement passed twelve automated launch/Circle-exit cycles without a new dump, while broader lifecycle and interruption stress keeps the interface opt-in |

Every listed probe check passed. These records document controlled test
coverage; they do not claim that arbitrary applications or every firmware and
plugin combination are already supported.

The earlier screenshots and journals do not all embed their system-software
version, but those recorded runs used 3.65. Future hardware gates must record
the firmware contemporaneously.

The first disposable disabled-comparator round-trip attempt on 2026-09-12
[rebooted during its kernel critical section](docs/hardware/hw-disabled-probe-attempt-1.md).
Its valid pre-probe journal proves entry but not the exact failing instruction or
any comparator write. The separate, strictly sequential
[staged read-only ladder](docs/hardware/hw-read-ladder-attempt-2.md) then passed
its lifecycle, MIDR, DIDR, and DSCRint gates before rebooting at its first
`DBGVCR` read on core 1. Comparator rungs were never run.

A subsequent KBL source audit identified DIP switch 228
(`SYSTEM_FLAG_ENABLE_HW_BREAKPOINTS`) as the strongest Vita-specific control
lead. It is bit 4 (`0x10`) of the system-control word at KBL offset `0x5C`, not
the unrelated KBL field at offset `0xE4`. On 2026-09-13, the isolated read-only
`VDCP00005` [inventory](kernel/dipsw-read-probe/README.md) found bits 203 and
228 clear and consistent. The separately audited `VDCP00006` rung proved that
the installed kernel API changes only system-control mask `0x10`, observes bit
228 high, and restores the exact original state; a clean reboot and read-only
sample confirmed restoration.

The final late-runtime A/B used `VDCP00007`: after a durable `READ_PENDING`
record, it performed the already-proven Set/readback operation, a same-core IRQ
guard, and exactly one DBGVCR read before its unconditional Clear path. The Vita
rebooted and never committed `COMPLETE`; both journals remained valid and
unchanged, and the post-reboot inventory again found both bits and words clear.
Because the design intentionally performs no I/O while bit 228 is high, the
journal cannot identify one exact instruction inside that short critical path.
Paired with the prior API result and audited single-MRC binary, however, this is
consistent with the same DBGVCR access boundary and shows that late runtime bit
228 did not produce a safely returning path in the tested retail 3.65
environment. The
[bit-228 report](docs/hardware/dipsw-228-hw-debug.md) preserves the exact
evidence and timing limitations. Hardware `Z1`-`Z4` remains disabled; no
comparator was accessed or modified.

A later [hardware-debug and foreign-VFP research review](docs/hardware/research-review-2026-09-14.md)
retains the VM-context setter, cooperative target-thread VFP mutation, and
mismatch breakpoints as useful leads while correcting the proposed safety
order. The Cortex-A9 TRM marks its OS Lock registers unimplemented, and erratum
764319 means `DBGPRSR` and `DBGOSLSR` may themselves raise Undefined Instruction
when `DBGSWENABLE` is low. VitaSDK's public SceExcpmgr context is only documented
for 3.60 and its installed headers have no matching release API. No new CP14 or
exception-handler hardware test is enabled by that research.

The preceding [FPSCR discovery run](docs/hardware/kernel-vfp-probe-v8-fpscr-bank-discovery.jpg)
is retained separately because its one failed expectation established that the
saved worker FPSCR is in raw entry 0 rather than entry 1. The corrected v8 row
above is the subsequent all-pass gate.

## End goals

The intended final design is a hybrid toolkit:

```text
GDB / IDE / profile viewer / log console / build runner
          | debug, logs, profiles       | signed deployment
     application libraries          VitaDevDeploy agent
          |
    optional narrow kernel companion
```

Planned components are:

### `libvitadebug`

The primary application-side library. Developers will compile it into debug
builds to provide source-aware GDB debugging, cooperative application control,
symbols, intentional breakpoints, exception reporting, and custom integrations.
The current `libuvdb.a` will evolve into this component.

### `vitadebug.skprx`

An optional, tightly scoped kernel plugin providing operations that cannot be
implemented reliably from a user process:

- Enumerating all application threads.
- Coherently suspending and resuming them.
- Reading and, after additional validation, writing registers belonging to
  another thread.
- Hardware breakpoints and watchpoints.
- Process and module discovery.
- Eventually attaching to an application not built with `libvitadebug`.

The plugin will expose a small validated interface rather than a general
arbitrary kernel-access service. Library-only operation will remain supported.
The current implementation derives process ownership in kernel context,
excludes the calling debugger thread, records only threads it successfully
suspends, rolls back partial failures, requires a process-owned session token,
and automatically resumes an abandoned session when its short lease expires.
Foreign-thread register reads dynamically select the hardware-validated
  current/resumable user-mode state from the two raw kernel banks in experimental
  kernel-integrated builds. A restorable mutation transaction boundary now
  exists and is host-tested, but its default Vita backend exposes zero writable
  banks until a supported setter and durable exact-target lifetime provider can
  be implemented and independently validated. Transactions reject an additional
  runnable exempt thread and mix neither core nor VFP state in one transaction.

### `libvitaprofiler`

A separate low-overhead profiler intended for optimized builds:

- Named CPU timing zones and counters.
- Frame-time and frame-pacing histories.
- Per-thread CPU sampling when the kernel companion is present.
- Vita system-memory statistics.
- Optional VitaGL RAM/CDRAM pool statistics.
- Draw, texture, shader, allocation, and audio-underflow counters supplied by
  the host application or graphics/audio integrations.
- CPU-observed timing around cooperative VitaGL/SceGxm call sites. True GPU
  submission, wait, and completion timestamps remain planned where the APIs
  permit them.
- An in-memory event ring buffer with network and file export options.

Profiling remains separate from GDB because debugger stops and debug compiler
settings distort real-time performance measurements.

### DebugNet-compatible log streaming

The library includes an optional UDP log transport wire-compatible with the
simple text receivers commonly used with
[psxdev/debugnet](https://github.com/psxdev/debugnet). The original project is
used as a protocol and workflow reference; VitaDebugger's sender is a separate
hardened implementation and is not API/ABI-compatible with `libdebugnet`.
DebugNet-style logging and GDB complement one another:

- GDB handles breakpoints, faults, registers, stacks, memory, and control.
- DebugNet carries non-stopping logs, profiler events, frame timing, audio
  status, and other continuous diagnostics.

Log calls enqueue bounded datagrams and never perform network I/O on the calling
thread. A dedicated worker sleeps on a semaphore and owns the UDP socket. Its
64-message queue is allocated only while logging is active; contention and
overflow drop messages instead of blocking gameplay. Statistics expose current
queue depth, datagrams accepted by the Vita network stack, dropped and truncated
messages, failed sends, and the most recent send result. Initialization and
bounded shutdown are explicit, invalid
configuration fails cleanly, and applications are not required to enable logs.

Networking must already be initialized. Start the stream after network startup:

```c
struct uvdb_debugnet_config logs = {
    .server_ip = "10.1.1.10", /* development computer */
    .port = 18194,
    .level = UVDB_LOG_DEBUG,
};

if (uvdb_debugnet_start(&logs) == 0) {
    uvdb_debugnet_printf(UVDB_LOG_INFO, "scene=%s frame=%u\n", scene, frame);
}
```

`uvdb_debugnet_write()` and `uvdb_debugnet_printf()` return `0` when queued,
`1` when filtered by the selected level, `2` when queued with truncation, and
`-1` when logging is unavailable or the bounded queue is busy/full. Messages,
including their level prefix, are limited to 1023 bytes. Query
`uvdb_debugnet_get_stats()` for local loss and truncation; its `queued` field is
the current queue depth rather than a cumulative total. Call
`uvdb_debugnet_stop()` after joining application log producers during normal
shutdown; `uvdb_shutdown()` also requests a bounded stop. Serialize start/stop
calls from one application control thread, and do not call shutdown from an
exception handler. UDP delivery is intentionally best-effort: a `sent` count
means the Vita network stack accepted the datagram, not that the PC received it.

Run the included cross-platform receiver on the development computer:

```sh
python tools/debugnet_listener.py --port 18194 --source VITA_IP
```

On Windows, allow inbound UDP port 18194 when prompted and use a Private network
profile for the trusted LAN shared with the Vita. Keep any manual firewall rule
limited to the selected UDP port and local subnet; never expose the receiver to
the internet.

### VitaDevDeploy

The [`deploy/`](deploy/) subproject supplies a signed PC-to-Vita deployment
path for the normal edit/build/test loop. Its host command validates a VPK,
signs a one-use job, transfers the package through the authenticated direct-TCP
carrier or the FTP fallback, waits for a durable result from the user-mode Vita
agent, and can launch the installed title. Direct mode still uses Vita
Companion for small result/recovery reads and title lifecycle commands. Only
the public signing key is embedded in the agent; the private key remains on the
development computer.

VitaDevDeploy is currently an experimental, one-shot service intended for a
trusted private LAN. It does not add another kernel plugin. It has its own
[setup and recovery guide](deploy/README.md), [security boundary](deploy/SECURITY.md),
[third-party notices](deploy/THIRD_PARTY.md), and GPL-3.0-only
[license](deploy/LICENSE).

### Desktop and IDE tools

The repository now includes an initial
[VS Code build/deploy/debug sample](examples/vscode-debug-demo/README.md). Each
developer enters their own Vita IPv4 address once; ignored local configuration
then drives VitaDevDeploy, installed-build verification, ASLR symbol capture,
and the single GDB connection used by F5. The end goal additionally includes
ready-to-use LLDB command files, a Debug Adapter Protocol bridge, a live log
console, a trace viewer, and documented APIs that other IDE extensions can
consume. A later authenticated Vita service may consolidate the narrowly
required file transfer, title launch/stop, screenshot, wake/no-sleep, debugger,
log, and profiler operations that currently depend on separate tools. That
service should keep privileged kernel operations isolated behind the small
validated companion ABI rather than moving the whole toolchain into kernel
space.

## Requirements

### Development computer

- A working [VitaSDK](https://vitasdk.org/) installation.
- `VITASDK` set to the SDK directory and `$VITASDK/bin` on `PATH`.
- `arm-vita-eabi-gcc`, `arm-vita-eabi-ar`, and `arm-vita-eabi-gdb`.
- GNU Make for the included build.
- Python 3.10 or newer for the host-side symbol/lifecycle tools and Python
  regression tests.
- A Vita C or C++ homebrew project capable of linking a static library.
- Local-network connectivity to the Vita.

Remote deployment additionally requires PowerShell, CMake, an Ed25519
implementation, Vita Companion 1.06, and one-time installation of the
VitaDevDeploy agent. See
[the deployment requirements](deploy/README.md#requirements) for the exact
setup and safety boundary.

All application objects, the debugger library, Kubridge imports, and other
libraries must use compatible VitaSDK ABIs.

### Vita

- A homebrew-capable retail PS Vita or Vita TV running system software 3.65.
  Both device classes are tested runtime-compatibility targets; consult each
  linked milestone for its narrower feature-validation scope. Firmware 3.60
  and all other firmware releases remain untested.
- TaiHEN/Ensō or an equivalent kernel-plugin environment. The documented
  kernel-companion gates currently cover retail 3.65 environments.
- [Kubridge](https://github.com/bythos14/kubridge) with exception and memory-
  protection support. The tested version is the official `v0.3.1_hotfix`
  `exceptions_mprotect` release.
- `kubridge.skprx` loaded from `ur0:tai/config.txt`, normally under `*KERNEL`.
- The Vita and development computer connected to the same trusted network.

Restart the Vita after changing TaiHEN configuration. A matching Kubridge
header and import stub are also required when linking the application.

### Network initialization and security

The host application must initialize Vita networking before its first
`uvdb_enter()`. Projects already using Vita socket APIs or an appropriate SDL
network path may have done this. The included test performs a UDP socket
operation first; otherwise follow the SceNet/SceNetCtl VitaSDK samples.

The default endpoint is TCP port `1234`. The current protocol is unauthenticated
and unencrypted. Never expose it to the internet or forward the port through a
router. Use it only on a trusted development network.

## Building

### Windows build environment

On Windows, use `tools/invoke-vita-env.ps1` for direct compiler, Make, and
CMake invocations. An absolute path to MSYS2's `gcc.exe` is not sufficient by
itself: its `cc1.exe` child also needs the matching MinGW64 runtime directory
on `PATH`. The wrapper validates VitaSDK's `bin` directory, both required MSYS2
directories, and every non-system DLL imported directly by the installed
MinGW64 `cc1.exe`. It changes only the current process environment while the
child runs and restores it afterward.

Validate the environment without launching a compiler:

```powershell
.\tools\invoke-vita-env.ps1 -ValidateOnly
```

Place any wrapper options before the executable. Every remaining token is
forwarded to that executable unchanged, including short native options such as
GCC's `-o`. For example, the hardware-debug register encoder has a host-only
safety test that does not build or install a Vita kernel plugin:

```powershell
.\tools\invoke-vita-env.ps1 C:\msys64\mingw64\bin\gcc.exe `
  -std=c11 -Wall -Wextra -Werror -Ikernel/include `
  kernel/src/armv7_debug_codec.c tests/host/test_armv7_debug_codec.c `
  -o test-armv7-debug-codec.exe
.\test-armv7-debug-codec.exe
```

For nonstandard installations, pass `-VitaSdkPath`, `-Msys2RuntimePath`, or
`-Msys2UsrBinPath` before the executable. Do not copy MinGW DLLs into VitaSDK
or Windows system directories; keeping the matching runtime together avoids
silent version skew.

Provide the Kubridge source and import-library locations:

```sh
export VITASDK=/path/to/vitasdk
export PATH="$VITASDK/bin:$PATH"

make \
  KUBRIDGE_DIR=../kubridge \
  KUBRIDGE_LIB_DIR=../kubridge/build-local
```

This produces `libuvdb.a`. The public header is `uvdb.h`.

Applications must link `SceNet_stub` for the console bridge's running-state
socket readiness and nonblocking send path, in addition to `SceNetPs_stub`.
Applications using module discovery must also link
`SceKernelModulemgr_stub`; the included test Makefile supplies all three
dependencies automatically.

The read-only `monitor display` sampler is opt-in so the base archive does not
force a display-library dependency on every application. Build with
`UVDB_MONITOR_DISPLAY=1` and add `SceDisplay_stub` to the final application link
when that command is needed. For a server-owned connection, the initial
synthetic stop is deferred until after `accept`; sampling then occurs on the
ordinary server thread outside the global debugger lock and exception context.
The sample is published only for that same live socket generation, and the
stopped exception path reads only the complete current-generation cache. A real
fault cancels the deferred handoff and its later queued synthetic trap is
ignored. A direct `uvdb_enter()` can therefore report display data as
unavailable until the current generation receives an ordinary-context sample.

Build the included test VPK with:

```sh
make package \
  KUBRIDGE_DIR=../kubridge \
  KUBRIDGE_LIB_DIR=../kubridge/build-local
```

This creates `uvdb-test.vpk`. Retain `test.elf` for GDB.

Enable the deterministic direct-write `stdout` and `stderr` markers used by the
GDB console smoke test with:

```sh
make package UVDB_GDB_CONSOLE_TEST=1
```

The same flag may be combined with `UVDB_KERNEL_THREAD_CONTROL=1` when testing
the matching all-stop kernel companion.

After building and installing the matching kernel companion, enable integrated
all-stop in the test build with:

```sh
make package UVDB_KERNEL_THREAD_CONTROL=1
```

To compile the hardware test with UDP logging enabled, supply the development
computer's LAN address (not the Vita address):

```sh
make package UVDB_KERNEL_THREAD_CONTROL=1 \
  UVDB_DEBUGNET_HOST=10.1.1.10 \
  UVDB_DEBUGNET_PORT=18194
```

The repository's on-device stress package also passes
`UVDB_DEBUGNET_LIFECYCLE_TEST=1`. That option deliberately floods the bounded
queue from two producers and stops/restarts the sender while they are active;
do not enable it in a normal application build.

Application builds must add `-DUVDB_KERNEL_THREAD_CONTROL`, include
`kernel/include`, and link the generated
`libvitadebug_kernel_stub.a`. Keep the application library and plugin ABI from
the same VitaDebugger revision. The library itself validates the loaded
companion's exact ABI, required thread-control capability mask, and thread
capacity before direct entry or server startup; a mismatch fails closed before
opening a debugger socket or creating helper threads.

### Building the experimental kernel companion

The kernel companion is an independent CMake project. Build it from a path
without spaces because current VitaSDK SELF-generation tools may split paths:

```sh
cmake -S kernel -B kernel/build -G "Unix Makefiles"
cmake --build kernel/build
```

This produces `vitadebug.skprx` plus strong and weak user import libraries.
The current companion provides ABI/capability queries, caller-process thread
enumeration, lease-protected stop sessions, renewal-time reconciliation of new
threads, and token-protected reads of both raw, state-dependent ARM register
banks for a session-owned suspended thread. ABI v1.11 also reserves a read-only
candidate VFP snapshot call. A normal build compiles that undocumented path
out, omits
its capability bit, and returns `VD_KERNEL_ERROR_VFP_DISABLED` if called. Keep
a known-good taiHEN configuration backup while testing kernel builds.

The normal build produces `vitadebug-kernel-probe.vpk`. That stable probe
checks the ABI, capability bits, main/worker thread visibility, invalid
argument rejection, a normal tokenized stop/end sequence, and automatic
watchdog resumption after an intentionally abandoned lease.

The candidate VFP implementation must be built explicitly in a separate
directory:

```sh
cmake -S kernel -B kernel/build-vfp -G "Unix Makefiles" \
  -DVITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT=ON
cmake --build kernel/build-vfp
```

This build advertises the VFP capability and uses an aligned global scratch
area with prefix canaries and a full page of trailing canaries. Any changed
canary rejects the snapshot rather than copying it to user mode. It also emits
the separate `vitadebug-vfp-probe.vpk` (`VDCP00002`), which loads distinct bit
patterns into D0-D31 on a disposable worker, changes only that worker's FPSCR,
captures it while session-owned and suspended, and checks every value before
restoring the worker state. Keeping this gate separate leaves the stable stop,
lease, and watchdog probe unchanged. Any failed boundary check is shown as a
`FAIL` line on screen. Do not enable GDB VFP reads until this probe passes on
the target firmware. See
[VFP register layout validation gate](docs/vfp-layout-gate.md) for the evidence
boundary, protections, and promotion checklist.

After the separate VFP hardware probe passes, the experimental application
build is:

```sh
make UVDB_KERNEL_THREAD_CONTROL=1 UVDB_KERNEL_VFP_READS=1 \
  VITADEBUG_KERNEL_DIR=kernel \
  VITADEBUG_KERNEL_BUILD_DIR=kernel/build-vfp
```

This feature is read-only. Full `G` and individual `P` writes to VFP registers
are rejected in that build so GDB cannot silently claim that it changed VFP
state. Individual `p` reads preserve the negotiated register numbering and
return correctly sized unavailable markers when a stopped thread has no saved
VFP bank. At each stop, the stub also verifies the exact kernel ABI and
capability bit before negotiating the extended packet; a normal fail-closed
plugin retains the legacy core-only contract. See
[GDB individual register access](docs/gdb-register-access.md) for the exact
read/write matrix and completed retail 3.65 transactional gate.

The repository test application can additionally compile a diagnostic-only
registered worker with deterministic D0, D31, and FPSCR values. Add
`UVDB_GDB_VFP_FIXTURE=1` to the command above and follow the
[live GDB VFP validation gate](docs/gdb-vfp-validation.md). The flag is rejected
unless the thread-control and VFP-read gates are also enabled. The fixture uses
a call-free busy loop and must not be enabled in a production application.

To build the separate user-module symbol fixture and pack it into the test VPK,
enable the ASLR gate alongside kernel thread control:

```sh
make package UVDB_KERNEL_THREAD_CONTROL=1 UVDB_GDB_ASLR_FIXTURE=1 \
  VITADEBUG_KERNEL_DIR=kernel \
  VITADEBUG_KERNEL_BUILD_DIR=kernel/build
```

This retains `build-aslr-fixture/uvdb_aslr_fixture.elf` for GDB, converts its
same-stem `.velf` metadata sidecar, and packages only the `.suprx`. The test app
loads it before opening the debugger port. See
[ASLR-aware GDB symbols](docs/gdb-symbol-loading.md#automated-main-plus-user-suprx-gate)
for the live lifecycle command and evidence contract. The fixture is diagnostic
only and the build flag is rejected without kernel-assisted thread control.

### Offline protocol checks

The ARM register serializer is platform-independent and has a native unit test.
The target-description test parses the exact XML bytes embedded in the stub and
checks both the legacy core-register gap and the explicit D0-D31/FPSCR
656-character packet contract:

```sh
cc -std=c11 -Wall -Wextra -Werror -Isrc -I. \
  src/uvdb_rsp.c tests/host/test_rsp_registers.c -o test-rsp
./test-rsp
python -m unittest discover -s tests/host -p "test_*.py" -v
```

## Remote deployment

After completing the one-time agent and signing-key setup in the
[VitaDevDeploy guide](deploy/README.md), verify the host tools from the
repository root:

```powershell
Set-Location .\deploy
py -3 -m unittest discover -s tests -t . -p "test_*.py" -v
py -3 -m host.vitadevdeploy --help
```

With the Vita awake at LiveArea and Vita Companion active, a normal signed
install-and-launch operation is:

```powershell
py -3 -m host.vitadevdeploy deploy "C:\path\to\MyHomebrew.vpk" `
  --vita 192.168.1.42 `
  --private-key .\local\deploy_private.pem `
  --action install_launch
```

Keep the private key under `deploy/local/` or another access-controlled path;
never copy it to the Vita or commit it. VitaDevDeploy deliberately refuses to
force-close a running application, so begin deployment from LiveArea. See the
subproject guide for verification-only operation, dry runs, agent builds,
bootstrap recovery, and failure handling.

## Makefile integration

Put the archive after application objects and its dependencies after the
archive; static-library link order matters.

```make
VITA_DEBUGGER ?= 0
VITADEBUGGER_DIR ?= ../VitaDebugger
KUBRIDGE_DIR ?= ../kubridge
KUBRIDGE_LIB_DIR ?= $(KUBRIDGE_DIR)/build-local

ifeq ($(VITA_DEBUGGER),1)
  CFLAGS += -Og -g3 -DVITA_GDB_DEBUGGER
  CFLAGS += -I$(VITADEBUGGER_DIR) -I$(KUBRIDGE_DIR)

  LDFLAGS += $(VITADEBUGGER_DIR)/libuvdb.a
  LDFLAGS += -L$(KUBRIDGE_LIB_DIR) -lkubridge_stub
  LDFLAGS += -lSceNet_stub -lSceNetPs_stub -pthread
endif
```

For C++ applications, retain the normal Vita C++ runtime link option, commonly
`-lstdc++`, after libraries that require it.

## Temporary CMake integration

An exported CMake package is planned. Until then, import the archive:

```cmake
set(VITADEBUGGER_DIR "${CMAKE_SOURCE_DIR}/../VitaDebugger")
set(KUBRIDGE_DIR "${CMAKE_SOURCE_DIR}/../kubridge")

add_library(vitadebugger STATIC IMPORTED GLOBAL)
set_target_properties(vitadebugger PROPERTIES
    IMPORTED_LOCATION "${VITADEBUGGER_DIR}/libuvdb.a"
    INTERFACE_INCLUDE_DIRECTORIES "${VITADEBUGGER_DIR};${KUBRIDGE_DIR}"
)

target_compile_options(my_app PRIVATE -Og -g3)
target_compile_definitions(my_app PRIVATE VITA_GDB_DEBUGGER=1)
target_link_directories(my_app PRIVATE "${KUBRIDGE_DIR}/build-local")
target_link_libraries(my_app PRIVATE
    vitadebugger
    kubridge_stub
    SceNet_stub
    SceNetPs_stub
    pthread
)
```

## Application integration

Call `uvdb_enter()` after networking is ready:

```c
#ifdef VITA_GDB_DEBUGGER
#include <uvdb.h>
#endif

int main(int argc, char **argv) {
#ifdef VITA_GDB_DEBUGGER
    const struct uvdb_config config = {
        .port = 1234,
        .max_packet_buffer = 256 * 1024,
    };

    if (uvdb_configure(&config) == 0) {
        uvdb_enter();
    }
#endif

    return application_main(argc, argv);
}
```

On its first successful call, `uvdb_enter()` opens the server and waits for GDB.

### Persistent server and Ctrl-C

Applications that need reattachment without returning to a manual debugger
entry point can start the opt-in service after networking is initialized:

```c
if (uvdb_start_server() < 0) {
    /* report or handle startup failure */
}
```

The service owns a small 64 KiB Vita thread. It listens on the configured port,
accepts a new client after a clean GDB `detach`, and watches an attached session
for GDB's Ctrl-C interrupt byte while the application is running. Call
`uvdb_stop_server()` during orderly teardown, or let `uvdb_shutdown()` stop it
and release all debugger resources.

Without kernel integration, this remains an application-side stop: Ctrl-C stops
the debugger service thread while other application threads continue. Builds
compiled with `UVDB_KERNEL_THREAD_CONTROL=1` and linked to the matching kernel
stub use lease-protected process all-stop. A dedicated exempt keeper renews the
lease while GDB is stopped; continue, detach, shutdown, and I/O failure end the
session, while the kernel watchdog recovers an abandoned client. Experimental
builds can also return the dynamically selected current/resumable user-mode
register bank for a suspended thread. A VFP D32 target description and packet
serializer are available only with `UVDB_KERNEL_VFP_READS=1`; leave that flag
off in normal builds because the kernel snapshot ABI is undocumented and must
be revalidated for each firmware baseline. Its retail 3.65 live-GDB lifecycle
gate passes. Foreign register writes, including VFP writes, remain disabled.
Later calls while connected act as intentional software breakpoints. Calling it
before graphics initialization normally leaves a black screen while waiting;
this is expected.

Passing `NULL` to `uvdb_configure()` restores TCP port 1234 and a 256 KiB packet
limit. The allowed buffer range is currently 4 KiB through 16 MiB. Configuration
must occur while the debugger is idle.

### State, fault information, and shutdown

```c
enum uvdb_state state = uvdb_get_state();

struct uvdb_fault_info fault;
if (uvdb_get_last_fault(&fault) == 1) {
    /* exception_type, signal, fault_status, fault_address, pc, lr, sp */
}

uvdb_shutdown();
```

Call `uvdb_shutdown()` only from normal application code, never from an
exception callback. It removes debugger breakpoints and handlers, closes
sockets, restores redirected `stdout` and `stderr`, and releases debugger-owned
buffers and the safe-memory message pipe. Full shutdown is deliberately
terminal: use `uvdb_stop_server()` followed by `uvdb_start_server()` for an
ordinary disconnect/reconnect. KuBridge can copy a user-handler pointer before
slot replacement without exposing when that pending dispatch has retired, so a
completed `uvdb_shutdown()` cannot safely authorize restarting its handler
state or unloading a dynamically injected debugger module.

Orderly teardown first joins the server, obtains or recovers a coherent stop,
verifies restoration of every debugger-owned breakpoint byte, ends the stop
session, restores handlers, closes new handler admission, and only then joins
the lease keeper and other helpers. Captured predecessor pointers and the
closed callback gate remain immutable for the lifetime of the linked image so
an already-dispatched late callback has a safe chain target. `uvdb_stop_server()`
may retain the lease keeper when a breakpoint
restoration obligation is still outstanding; this prevents the watchdog from
resuming application threads into an uncertain trap. This shutdown ordering is
host-reviewed and host-tested, but the restoration-failure path has not yet
received a fault-injected live-hardware proof.

### Optional stdout/stderr forwarding

Start the persistent debugger service, then install the bridge after Vita
networking is ready:

```c
#include <unistd.h>

int console_capture_enabled =
    uvdb_start_server() == 0 && uvdb_redirect_stdio() == 0;

/* Later, while the target is running. Pre-negotiation output is dropped. */
if (console_capture_enabled) {
    static const char message[] = "forwarded through GDB O packets\n";
    write(STDOUT_FILENO, message, sizeof(message) - 1u);
}
```

`uvdb_redirect_stdio()` saves the original Vita newlib mappings for descriptors
1 and 2, redirects both through a nonblocking socket pair, and starts one
joinable capture helper. The helper only copies into a fixed 64-by-128-byte
queue. The single RSP owner emits those records as standard hex-encoded GDB `O`
packets only after the client completes `QStartNoAckMode` negotiation. Bytes are
bounded by both the nonblocking socket buffer and fixed queue; they may be
dropped when disconnected, contended, full, or stale instead of blocking the
application. Output from an old connection is purged rather than replayed after
reconnect.

The bridge preserves the application's existing newlib buffering policy. Use
`fflush()`, configure `setvbuf()` before first use, or issue checked direct
`write()` calls when delivery timing matters. A short write or `EAGAIN` at the
application-facing socket happens before the queue and therefore is not present
in the queue's loss counters.

For explicit teardown, call `uvdb_restore_stdio()` only after
`uvdb_stop_server()` returns success. This prevents an asynchronous all-stop
from suspending the capture helper while restoration waits to join it.
`uvdb_shutdown()` performs this stop-before-restore order automatically.
Redirect and restore are idempotent/retryable, but the application must
serialize descriptor changes with its own concurrent stdio writers.

Build the test app with `UVDB_GDB_CONSOLE_TEST=1`, launch it, and run:

```powershell
py -3 tools/gdb_console_smoke.py --host VITA_IP --reconnect
```

The script checks no-ack negotiation, both stream markers, Ctrl-C, the rule that
no console packet follows a stop reply, clean detach, and a fresh reconnect.
On retail Vita hardware running system software 3.65, both consecutive
sessions passed: each reported an initial `T05`, delivered six `O` packets
containing both `stdout` and `stderr`, returned `T02` for Ctrl-C, emitted
nothing during the stopped-boundary check, and detached cleanly. This result
does not claim support for another firmware, newlib revision, development
hardware, or plugin combination.
See the [bounded GDB console transport guide](docs/gdb-console-transport.md) for
the complete behavior and validation procedure. DebugNet remains the better
path for sustained logging and profiler output, including periods when GDB is
stopped or disconnected.

### Cooperative thread registration

Application threads can register a stable name for GDB discovery:

```c
static void *worker(void *argument) {
    uvdb_register_thread("asset worker");
    run_worker(argument);
    uvdb_unregister_thread();
    return NULL;
}
```

The implementation supports GDB thread listing, names, liveness checks,
selection, and identification of the thread that entered the exception
handler. Kernel-integrated builds suspend the other process threads coherently
and can report a selected suspended thread's dynamically selected current or
syscall-return general registers, PC, SP, LR, and CPSR. The new kernel boundary
can capture a selected thread's candidate D0-D31 state and both raw FPSCR bank
values without changing them. GDB exposure remains compile-time opt-in; its
retail 3.65 live lifecycle gate passed, while foreign register writes remain
rejected. Strict `p` reads can select any readable stopped thread. Strict `P`
writes are limited to ARM core/CPSR state in the selected exception thread and
run inside a renewed stop-operation boundary; VFP/FPA and foreign-thread writes
return an error.

## Connecting with GDB

Keep the exact unstripped ELF produced alongside the VPK. The packaged Vita
executable may be stripped, but GDB must load the unstripped file from the same
build:

```sh
arm-vita-eabi-gdb path/to/my_app.unstripped.elf
```

Then connect at the GDB prompt:

```gdb
target remote VITA_IP:1234
break my_function
continue
```

Replace the address with the Vita's IP. Connect directly: probing port 1234
first can consume the current single-client connection.

Useful commands include:

```gdb
break source_file.c:120
info breakpoints
backtrace
info registers
frame 2
info locals
print variable_name
set variable variable_name = 42
x/16wx $sp
step
next
stepi
continue
detach
```

VitaDebugger also exposes a small read-only diagnostic registry:

```gdb
monitor help
monitor status
monitor threads
monitor modules
monitor console
monitor display
```

The commands report debugger/stop/fault state, the stopped thread inventory,
loaded-module segments, bounded console transport/loss counters, and read-only
display metadata. They do not consume the console queue, read framebuffer
pixels, or execute arbitrary command text. Output is returned as one bounded
final hex-encoded `qRcmd` reply and ends with an explicit truncation marker if
the full report cannot fit. See the
[GDB monitor-command guide](docs/gdb-monitor-commands.md) for the exact
read-only boundary and report fields. The retail 3.65 evidence preserves the
original complete GDB 15.2 gate and a distinct rebuilt-artifact raw-RSP
follow-up for current-generation, post-accept display sampling in the
[console/display lifecycle record](docs/hardware/gdb-monitor-console-display-3.65.json).
The automated two-session gate is:

```powershell
py -3 tools/gdb_monitor_smoke.py --host VITA_IP --reconnect
```

The stub implements executable offsets for Vita runtime placement. Symbols will
still be incorrect if the ELF and installed VPK came from different builds.

## Exception behavior

The library currently registers Kubridge handlers for data aborts, prefetch
aborts, and undefined instructions. Software breakpoints and temporary stepping
traps use undefined instructions and are reported to GDB as `SIGTRAP`. Genuine
undefined instructions report `SIGILL`; memory aborts report `SIGSEGV`.

GDB does not automatically repair a fault. Continuing without changing the bad
register, memory, or control flow usually triggers the same fault again.
Applications installing their own exception handlers may conflict with the
stub. VitaDebugger now captures one predecessor per exception type, chains
unclaimed or nested faults through a serialized path, and restores the exact
captured slot at terminal shutdown. KuBridge has no compare-and-restore or
dispatcher-quiescence API, so exclusive ownership of those user slots and a
non-unloading linked image remain requirements.

## Project integration example

The [Makefile integration](#makefile-integration), [temporary CMake
integration](#temporary-cmake-integration), and [application
integration](#application-integration) examples above are intentionally
project-neutral. Build the debugger archive with the same VitaSDK ABI as the
application, link it after the application's objects, initialize networking,
then call `uvdb_enter()` or `uvdb_start_server()`. Use a distinct diagnostic
title ID when retaining an optimized build beside the debug build, and keep the
exact matching unstripped ELF on the development computer.

## Current limitations

- The library must currently be compiled into the application; it cannot attach
  to an arbitrary unmodified process. The new `attach/` subtree defines and
  host-tests the bounded, read-only discovery/identity boundary and an
  allocation-free broker state machine. Its Vita identity provider deliberately
  reports unavailable; no resident listener, injection loader, injected
  debugger module, or live external attach exists yet.
- Kernel all-stop is opt-in and requires the matching `vitadebug.skprx` ABI;
  library-only builds continue to provide application-side stopping.
- Kernel-assisted thread discovery now treats the complete 64-entry
  caller-process inventory as authoritative, uses the 32-entry cooperative
  registry only for names, deduplicates IDs, and filters debugger helpers.
  Library-only builds retain cooperative discovery. A newly created thread is
  folded into all-stop by background renewal or the synchronous renewal before
  the next thread-sensitive RSP operation.
- `Hg`, `Hc`, and the advertised `vCont;c;s` subset share one bounded selection
  model and fail closed on uncovered or multi-step action sets. Selected foreign
  Thumb-thread decoding and stop attribution pass on two deterministic,
  non-overlapping worker paths. An exact positive `Hc` for the current stopped
  exception thread has a hardware-tested one-instruction path that retains the
  kernel stop token and holds its peers. The `vCont;s:T;c` foreign-thread path
  still uses a process-wide temporary breakpoint and resumes peer threads;
  scheduler-locked execution of an arbitrary foreign thread remains
  unimplemented.
- Positive `Hc` followed by legacy `c` remains rejected. Legacy `s` is accepted
  only when the positive ID exactly names the stopped exception thread, the
  kernel stop token is active and healthy, and no PC override was requested.
  Different, stale, library-only, multi-thread, and address-override forms fail
  closed. The explicit foreign-thread form remains `vCont;s:T;c`.
- The retained-token step-over has no independent target-miss timeout yet. It
  accepts only bounded, recognized Thumb-2 or A32 `LDREX`-through-`STREX`
  sequences at an exclusive-load start; unmatched, nested, overlong, or otherwise
  unsupported exclusive sequences fail closed. Waits, syscalls, unsupported
  execution states, and uncertain PC writers are also rejected. A production
  resume-one path still needs transactional cancellation and trap rollback if
  the predicted instruction target is not reached.
- Blocking accept/receive/send now run outside the global debugger lock. A
  descriptor generation, packet-I/O lifetime, and whole-protocol owner keep
  buffers stable across those lock drops; shutdown cancels the socket and waits
  up to five seconds before retaining resources in the error state. Kernel stop
  acquisition/recovery, cache maintenance, and exception-slot replacement
  remain under the global lock because their coupled ownership contracts have
  not yet been split into retryable operations.
- Packet, memory, register, breakpoint, qXfer, thread-ID, and File-I/O parsing
  now use strict bounded fields and transactional outputs, with deterministic
  fuzz coverage. Live multi-chunk `M` writes check every kernel copy and never
  report success after failure, but remain potentially partially mutated until
  durable full-span rollback storage is integrated.
- Standard File-I/O literal `C` and optional bounded attachments parse safely.
  Exactly one `T02` is permitted only from a real saved all-stop context. The
  legacy `uvdb_remote_syscall()` path owns neither, so `C` fails closed by
  severing that protocol generation and returning `-1` rather than exposing
  synthetic zero registers while peer threads run.
- Full `uvdb_shutdown()` is terminal. KuBridge may have copied a callback
  pointer before handler restoration and offers no dispatch-lifetime fence;
  therefore dynamic debugger-module unload remains unsafe/unproven even after
  visible callbacks drain. stop/start of the persistent service remains the
  supported reconnect lifecycle.
- Foreign-thread register writes are not enabled. A separately versioned
  snapshot/stage/verify/commit-or-restore kernel transaction ABI now passes
  native failure and rollback tests. It requires a retained exact target object,
  a fully stopped process with no additional exempt thread, and one register
  bank per transaction so the overlapping FPSCR views cannot conflict. The
  default Vita backend advertises zero writable banks because no documented,
  typed, hardware-validated foreign-thread core/VFP setter or durable lifetime
  provider is available. The untyped VM-context NID is a research lead, not an
  enabled backend. Foreign-thread general
  register reads are hardware tested in both runnable/current and
  sleeping/syscall-return states; guarded foreign-thread VFP reads passed both
  the known-pattern kernel gate and the live GDB lifecycle gate. Individual
  core/CPSR writes are implemented only for the selected exception thread and
  pass the retail 3.65 mutation/read-back/exact-restoration gate across clean
  detach/reconnect. The same gate confirms that foreign core and VFP writes
  fail closed without changing state. VFP writes remain deliberately
  unsupported, and the read path remains opt-in because its kernel snapshot
  ABI is undocumented.
- Hardware breakpoint/watchpoint encoding and a guarded kernel session engine
  are implemented experimentally, but the staged retail probe rebooted at the
  first DSE-dependent `DBGVCR` read. A separate API test proved cached DIP 228
  set/readback/restore, yet the audited late-runtime DIP 228 + DBGVCR A/B also
  rebooted without returning to its final journal write. No comparator access
  or enabled comparator has passed a hardware gate, so GDB does not advertise
  `Z1`-`Z4`.
- Software stepping now chooses one condition-correct target for practical
  ARM/Thumb branches, interworking returns, immediate/immediate-shifted
  data-processing writes to `PC`, register/immediate PC loads, load-multiple
  returns, and in-flight IT blocks. It rejects self-targets, misaligned ARM
  targets and word sources, unsupported CPSR execution states, and selected-
  thread operations that could block. The former positive-`Hc` `E16` step-over
  and representative ARM-state `MOV`/`LDMDB`/`LDR` fixtures pass live GDB on
  retail 3.65. The bounded Thumb-2 and A32 `LDREX`/`STREX` fixtures also pass
  with a successful single store and exact abrupt-disconnect trap restoration.
  Privileged exception returns (`RFE`, `ERET`, and S-form ALU/LDM returns),
  `BXJ`, legacy Thumb-2 PC moves, register-controlled ARM shifts, and
  unsupported exclusive forms remain deliberately fail-closed. The broader
  decoder matrix has pure host-planner coverage; additional injected-memory
  and trap-rollback integration fixtures remain.
- The ASLR-aware host workflow requires the exact VPK, identity receipt, and
  retained unstripped ELFs. Its retail 3.65 gate passed read-only verification
  of the installed main and SUPRX bytes, deliberate mismatch rejection before
  RSP connection with the last good view preserved, idempotent same-process
  refresh, and the five-session main/SUPRX relaunch lifecycle. This completes
  the ASLR and verified-build milestone. The receipt proves equality to the
  retained artifacts, not publisher authenticity, and Sony system modules
  remain unmatched unless suitable symbols are supplied. Live same-process
  hot module churn and automatic IDE-triggered refresh are later convenience
  integration, not unfinished ASLR correctness.
- Remote syscall catching is not implemented.
- Kernel plugins cannot be debugged with the current application-side stub.
- The practical gate proves exact cleanup of two armed exclusive-step traps
  after an abrupt socket loss. Broader disconnect/error fault injection is still
  needed for bounded socket shutdown, arbitrary breakpoint sets, all-stop
  release, and recovery from a client or lease-keeper failure.
- Shutdown now retains the lease keeper until every outstanding breakpoint
  restoration obligation is resolved, then ends the stop session and releases
  handlers and helpers in order. That safety change passes host review and
  tests, but forced restoration failure has not yet been exercised live on a
  Vita.
- GDB console forwarding is intentionally bounded and lossy. It begins only
  after `QStartNoAckMode`, emits no unsolicited `O` packets after a stop reply,
  discards an old connection's bytes on reconnect, and can drop under socket or
  queue pressure. Newlib buffering is unchanged, and pre-capture short writes
  or `EAGAIN` cannot be included in the queue counters. Stop the debugger server
  before calling `uvdb_restore_stdio()`; `uvdb_shutdown()` does this
  automatically. Use DebugNet for sustained logging.
- GDB monitor commands are limited to the exact read-only `help`, `status`,
  `threads`, `modules`, `console`, and `display` registry. Arguments and
  unregistered commands are
  rejected; decoded command text is never executed or forwarded. Reports use
  fixed snapshot and packet bounds and visibly truncate at a complete line when
  possible. The retail 3.65 live gate passed state preservation, two clean raw-
  RSP detach/reconnect sessions, and display through GDB 15.2. A separate
  rebuilt-artifact raw-RSP follow-up passed generation-scoped sampling after
  `accept`; the front-end portion was not rerun in that follow-up. `display`
  reports buffer metadata only; it does not copy pixels or produce screenshots.
  Other firmware versions remain untested.
- The optional stdio bridge uses Vita newlib's private descriptor map because
  Vita newlib does not export `dup2`. Its current close/retry behavior was
  audited against newlib commit
  `64aa7aa33d4f380451a1f100d19589226cdad334`; other newlib revisions need a
  fresh lifecycle review even when the debugger core itself builds unchanged.
- The debugger and Vita Companion transports have no authentication or
  encryption. DebugNet uses best-effort UDP. Use the tools only on a private,
  trusted LAN; pairing, peer allowlists, and a secured control plane remain
  release blockers.
- `libvitaprofiler` records explicit zones, counters, frame markers, memory
  snapshots, supplied known-thread statistics, and a bounded name dictionary;
  its 13-check retail 3.65 probe resolved every captured custom and built-in
  event ID. A callback binary drain and PC-side bounded TCP receiver/viewer now
  pass host tests and can emit text, decoded JSON, and Chrome Trace/Perfetto
  output. A read-only kernel PMU inventory and the separate isolated lane-5
  software-increment write/read/exact-gate-restore test pass on retail 3.65
  across all three application cores. A reviewed bridge and versioned,
  default-off kernel transport now adapt that session to the profiler provider
  ABI and retain restoration obligations across release, timeout, and failed
  acquire. The normal kernel build still omits the backend/transport; the
  exported disabled stubs fail closed. Three real events and their stable names
  pass the double-gated fake-PMU build, and the matching disposable Vita client
  cross-builds with a durable attempted/completion journal. All three
  allowlisted events (`0x01`, `0x03`, and `0x10`) have passed separate bounded
  retail-3.65 normal-close samples with exact restoration. Vita
  timeout/disconnect/process-exit recovery and ownership-conflict gates remain
  pending. Two Render96EX 300-frame TCP
  captures pass with zero loss; the second validates the Goddard zones and 75
  bounded `0x01` PMU samples with clean close/restoration. Arbitrary thread PC/call-stack
  sampling, true GPU timestamps, and a bespoke desktop GUI also remain pending.
  Real-event admission is still latched to one attempt per boot. Any future
  re-arm must require no active lease, independently verified exact restore,
  matching owner/generation state, and either explicit close or proof that the
  owner process/thread is gone; that re-arm is not implemented.
  Graphics integration uses explicit application/library source call sites
  rather than binary interposition.
  The public ScePerf imports are unresolved in the tested retail runtime and the
  direct adapter fails closed.
- VitaDevDeploy now has an authenticated, size-bounded direct-TCP package
  carrier, and its basic signed verification plus disposable install-and-launch
  gates pass on retail 3.65. The carrier is not encrypted and v1 does not
  authenticate the Vita or its acknowledgements. Direct mode also still depends
  on Vita Companion's unauthenticated FTP for small result/recovery reads and
  its command transport for title lifecycle operations; use it only on a
  private LAN. The complete verified package must still be staged to a local
  Vita path before the system installer call.
- Deployment is serialized and one-shot, starts from LiveArea, does not provide
  general target-app rollback, and still needs full bootstrap recovery and
  interrupted-install fault-injection testing before it is production-ready.
- The optional vita2d deployment UI produced two intermittent launch-time GPU
  faults on the retail 3.65 test configuration after its first six-cycle gate.
  Both dumps resolve to the third rapid startup frame in
  `vita2d_swap_buffers`; the UI now waits for prior GPU work before libvita2d
  resets its shared transient pool. The guarded replacement completed twelve
  consecutive automated launch/Circle-exit cycles without a new dump, plus
  direct verification and install-and-launch. Broader lifecycle/interruption
  stress remains, so the interface stays opt-in.
- The tested compatibility target is retail handheld Vita and Vita TV hardware
  running system software 3.65 with the tested Kubridge release. Individual
  experimental feature gates may cover only the device class named in their
  evidence. Firmware 3.60 and all other firmware releases, development
  hardware, and other plugin combinations have not completed the same gates.
- Build-directory isolation, exported CMake/VitaSDK packages, CI, and a
  project-wide license are not finished.
- The initial IDE workflow targets Windows, VS Code, and Microsoft's C/C++ GDB
  adapter. It requires a paired VitaDevDeploy agent and Vita Companion on the
  same trusted LAN. It safely stops only its fixed sample title; hot-loaded
  module refresh, other IDEs, and a native DAP adapter remain later work.
- Long-running reconnect, shutdown, multithread, and fault stress testing is
  still in progress.

## Roadmap

1. Finish controlled thread renew/end failure injection and long multithreaded
   hardware soaks, then add scheduler-locked arbitrary-foreign-thread execution
   or displaced stepping so another running thread cannot win a temporary trap.
2. Refactor blocking debugger work outside the global lock, contain nested
   faults and chain prior handlers, harden remaining packet and memory/register
   parsers, and expand fake-transport, fake-kernel, and fuzz coverage.
3. Run long on-device console pressure, abrupt-disconnect, restore/retry, and
   shutdown soaks while retaining DebugNet for sustained logging.
4. Connect the host-tested restorable kernel mutation ABI to a supported Vita
   core/VFP setter and a retained exact process/thread-object lifetime provider.
   Keep bank capabilities disabled until that backend passes process exit,
   thread exit, UID churn/reuse, forced inventory failure, exact rollback, and
   lease-cleanup hardware gates, then route foreign-thread GDB writes through
   it. Add injected-memory/trap-rollback coverage before enabling
   privileged exception returns, unsupported exclusive forms, or other unsafe
   instruction families.
5. Keep hardware `Z1`-`Z4` fail-closed while researching a supported debug-
   register access path. Independently gate a page-protection/data-abort software
   watchpoint design with strict ownership, access decoding, single-step/rearm,
   cleanup, and false-positive tests.
6. Stress the hardware-validated VS Code F5 workflow across repeated direct-TCP
   deploy/launch cycles, then stress same-process hot module load/unload churn
   and add automatic in-session symbol refresh. The TCP-default host path, its
   combined 51-test symbol/IDE gate, and the end-to-end direct-TCP Vita run pass.
   A recoverable initial SceShell launch race remains a hardening item. This is
   dynamic-module and IDE convenience work; the ASLR, verified-build, live edit,
   detach, and source-step milestones are complete.
7. Hardware-gate the newly versioned, default-off PMU/provider transport. The
   fixed lane-5 software-increment transaction already proves exact
   gate-snapshot restoration on application cores 0 through 2; the narrow
   transport, fake-kernel matrix, and durable-journal disposable client now
   build with real events limited to `0x01`, `0x03`, and `0x10`. All three
   events have passed their separate core-0 normal-close samples with exact
   restoration on retail 3.65. Next test timeout/error/disconnect/process-exit
   cleanup and ownership conflicts.
   Bounded repeated `0x01` sampling has passed in Render96EX. The caller-owned Vita TCP sink and PC receiver pass
   live retail capture, disconnect, and recovery gates. The first 300-frame
   Render96EX CPU/VitaGL baseline and combined Goddard+PMU capture also pass;
   next add deeper VitaGL-owned/SceGxm timing where source hooks can do so
   safely, then build a dedicated desktop GUI on top of the working receiver,
   analyzer, and Perfetto export.
   Only after those recovery gates pass, design a safe post-restore real-event
   re-arm which requires an idle lease, independently verified exact restore,
   matching owner/generation, and explicit close or confirmed owner exit.
8. Complete protocol authentication/pairing and peer allowlists, isolated build
   directories, exported VitaSDK/CMake packages, CI and firmware coverage, and
   the project-wide licensing/release work.
9. Continue hardening VitaDevDeploy's authenticated, bounded direct-TCP package
   ingress. Basic signed verification, disposable install-and-launch, installed
   EBOOT readback, clean recovery status, and a twelve-cycle guarded-UI lifecycle
   run now pass on retail 3.65. Finish interruption/security, cold-start,
   Wi-Fi-reconnect, bootstrap-recovery, and device-authenticated protocol gates.
10. Extend the read-only external-attach scaffold with authenticated pairing,
    a shell-resident listener around the broker core, a trusted Vita target-
    identity provider, a fixed identity-checked per-process debugger-module
    loader, complete rollback/unload leases, and disposable-target hardware
    gates before enabling attachment to applications not compiled with the
    library.
11. Validate LLDB remote-protocol behavior and add a Debug Adapter Protocol
    bridge without regressing GDB.

## Repository layout

- `src/uvdb.c`: protocol server, safe memory access, breakpoints, stepping, and
  exception handling.
- `src/uvdb_registers.c` / `src/uvdb_registers.h`: host-testable selection of the valid
  user context from the Vita kernel's two raw ARM register banks.
- `src/uvdb_thread_control.c` / `src/uvdb_thread_control.h`: bounded thread inventory,
  selector parsing, and fail-closed resume/step planning.
- `src/uvdb_rsp.c` / `src/uvdb_rsp.h`: host-testable ARM and VFP register-packet
  serialization.
- `src/uvdb_vfp_policy.c` / `src/uvdb_vfp_policy.h`: fail-closed classification of
  normalized kernel VFP snapshot results.
- `src/uvdb_console.c` / `src/uvdb_console.h`: internal fixed-memory, generation-scoped
  GDB console queue and loss accounting.
- `src/uvdb_console_transport.c` / `src/uvdb_console_transport.h`: single-owner no-ack
  session state, `O`-packet framing, bounded pumping, and transport statistics.
- `src/uvdb_monitor.c` / `src/uvdb_monitor.h`: host-testable exact `qRcmd` registry and
  bounded read-only status, thread, and module report renderer.
- `uvdb.h`: public application API.
- `protocol/arm_vfp_target_xml.inc`: exact opt-in GDB D32 target description.
- `docs/gdb-register-access.md`: `p`/`P` numbering, thread-scope, mutation
  policy, transactional validation gate, and retail 3.65 evidence.
- `docs/gdb-monitor-commands.md`: monitor command reference, security boundary,
  response limits, native tests, and live-GDB validation evidence.
- `src/stdio_redirect.c` / `src/stdio_redirect.h`: restorable nonblocking Vita newlib
  `stdout`/`stderr` capture and internal-helper inventory filtering.
- `src/uvdb_debugnet.c`: bounded asynchronous UDP logs for DebugNet-style receivers.
- `tests/vita/test.c`: Vita hardware test program.
- `tests/vita/thumb_step_returns.S` / `tests/vita/thumb2_control_flow.S`:
  Vita instruction-stepping fixtures.
- `tools/gdb_console_smoke.py`: raw-RSP no-ack, stream, stop, detach, and
  reconnect validation for the Vita console fixture.
- `tools/gdb_monitor_smoke.py`: automated two-session live-RSP validation for
  the fixed read-only monitor registry, bounded errors, and state preservation.
- `tools/gdb_symbols.py`: bounded live module reconciliation and ASLR-aware GDB
  symbol-script generation.
- `tools/gdb_aslr_lifecycle.py`: automated five-session main/user-SUPRX source-
  breakpoint, reconnect, relaunch, and symbol-regeneration gate.
- `tests/aslr_fixture/`: diagnostic resident user module and shared gate control
  ABI used only by the ASLR hardware test.
- `tools/debugnet_listener.py`: cross-platform development-computer log receiver.
- `profiler/`: standalone bounded user-mode profiler library, Vita adapter,
  binary capture writer, PC-side receiver/viewer and Perfetto exporter,
  cooperative graphics hooks, guarded PMU provider boundary, tests, and
  integration documentation.
- `attach/`: read-only external-application discovery/identity protocol,
  allocation-free broker core and fail-closed Vita adapter, bounded host client,
  security and API audits, and native tests; it contains no resident listener,
  target identity provider, injection, or process-control implementation yet.
- `deploy/`: self-contained signed host/Vita remote deployment subproject,
  including its agent, host CLI, tests, security documentation, and license.
- `examples/vscode-debug-demo/`: isolated sample app plus generated-local VS
  Code configuration for one-click build, deploy, ASLR-aware attach, live
  variable editing, and exact-title stop.
- `tests/`: Vita fixtures and offline host protocol tests.
- `kernel/`: narrow kernel companion, generated user stubs, the stable boundary
  probe, the fail-closed VFP layout probe, and the disposable staged hardware-
  register read ladder.
- `Makefile`: static library and test-package build.

## Attribution

VitaDebugger derives from
[sleirsgoevy/vita-uvdb](https://github.com/sleirsgoevy/vita-uvdb). Kubridge and
DebugNet are separate projects maintained by their respective authors.
`deploy/` is a self-contained GPL-3.0-only subproject governed by
[`deploy/LICENSE`](deploy/LICENSE) and its third-party notices. That license
does not select a license for files outside the deployment subproject. A
project-wide license has not been selected yet, so do not assume redistribution
rights merely because the source is public. Review each dependency's license as
well; maintainers should add an explicit project license before a formal release.
The VFP work used VitaSDK's public header/NID declarations and a pinned review of
`cerwym/kvdb` commit `88742b760afa18cd11e251160d4c3b85357c30f1` only as a
research pointer to the undocumented kernel call. No KVDB source was copied;
its root repository does not currently contain an explicit `LICENSE` file.

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
- Hardware-tested ARM-state software stepping for `MOV PC, Rm`, decrementing
  load-multiple with writeback, and `LDR PC, [Rn]`, including exact PC and
  register-effect checks plus clean detach and reconnect.
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
  main-module `qOffsets` derived from the same live segment metadata,
  hardware-tested with 14 executable and system modules. A bounded host tool
  matches compatible unstripped ELFs and generates ASLR-aware GDB setup. Its
  live main-plus-user-SUPRX gate passes five source-breakpoint sessions across
  same-process detach/reconnect, two fresh-ASLR relaunches, automatic solib
  reuse, and explicit-address regeneration.
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
  `threads`, and `modules`. Exact command parsing, bounded snapshots, safe name
  rendering, and line-safe response truncation pass native host tests. On a
  retail Vita running system software 3.65, the automated two-session gate and
  a real GDB 15.2 session passed all four commands, state preservation, clean
  detach, and reconnect with five stopped threads and 14 loaded modules.
- Hardware-tested DebugNet-compatible UDP logging with bounded messages,
  concurrent producers, stop/restart under load, and GDB attach/detach
  coexistence.
- A debugger-enabled larger application build as a real-world integration test.
- A standalone allocation-free profiler foundation for named zones, counters,
  frame markers, Vita memory snapshots, and known-thread statistics.
- A hardware-tested user-mode Vita profiler self-test covering event order,
  timing zones, counters, memory/thread snapshots, bounded multithreaded
  pressure, wire encoding, exact drop accounting, and queue-slot reuse. All 11
  on-device checks pass without calling the kernel plugin.
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
| ASLR + user SUPRX | [Lifecycle evidence](docs/hardware/gdb-aslr-suprx-3.65.json) | Main and loaded-user-module source breakpoints in five GDB sessions, stable same-process layout, automatic symbols after relaunch, explicit-symbol regeneration, and observed main/SUPRX address changes on both relaunches |
| DebugNet v24 | [Sustained UDP stream](docs/hardware/debugnet-v24-sustained-stream.jpg) | Logging remains live after a loaded stop/restart cycle, with the displayed queue draining and no packet drops or send errors |
| Profiler v1 | [User-mode profiler probe](docs/hardware/profiler-user-mode-probe-v1.jpg) | Eleven passing checks for live zones, frames, counters, memory/thread snapshots, wire encoding, four-producer pressure/drop accounting, uniqueness, and ring reuse without kernel calls |
| VitaDevDeploy UI | [Retail 3.65 lifecycle evidence](docs/hardware/vitadevdeploy-ui-3.65.md) | Native vita2d waiting screen, six clean post-refresh launch/exit cycles through both SceShell peel-close and Circle cleanup, packaged LiveArea asset integrity, and successful artwork refresh; two intermittent launch-time GPU faults are now recorded and keep this mode experimental |

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
kernel-integrated builds. Foreign-thread writes remain disabled until mutation
and restoration are independently validated.

### `libvitaprofiler`

A separate low-overhead profiler intended for optimized builds:

- Named CPU timing zones and counters.
- Frame-time and frame-pacing histories.
- Per-thread CPU sampling when the kernel companion is present.
- Vita system-memory statistics.
- Optional VitaGL RAM/CDRAM pool statistics.
- Draw, texture, shader, allocation, and audio-underflow counters supplied by
  the host application or graphics/audio integrations.
- GPU submission, wait, and completion timing where VitaGL/SceGxm permit it.
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
signs a one-use job, transfers it through Vita Companion, waits for a durable
result from the user-mode Vita agent, and can launch the installed title. Only
the public signing key is embedded in the agent; the private key remains on the
development computer.

VitaDevDeploy is currently an experimental, one-shot service intended for a
trusted private LAN. It does not add another kernel plugin. It has its own
[setup and recovery guide](deploy/README.md), [security boundary](deploy/SECURITY.md),
[third-party notices](deploy/THIRD_PARTY.md), and GPL-3.0-only
[license](deploy/LICENSE).

### Desktop and IDE tools

The end goal also includes ready-to-use GDB and LLDB command files, VS Code
build/deploy/debug configurations, a Debug Adapter Protocol bridge, a live log
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
cc -std=c11 -Wall -Wextra -Werror -I. \
  uvdb_rsp.c tests/host/test_rsp_registers.c -o test-rsp
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
buffers and the safe-memory message pipe.

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
values without changing them. GDB
  exposure remains compile-time opt-in pending the live GDB lifecycle gate;
  foreign register writes are rejected. Strict `p` reads can select any
  readable stopped thread. Strict `P` writes are limited to ARM core/CPSR state
  in the selected exception thread and run inside a renewed stop-operation
  boundary; VFP/FPA and foreign-thread writes return an error.

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
```

The commands report debugger/stop/fault state, the stopped thread inventory,
and loaded-module segments. They do not execute arbitrary command text. Output
is returned as one bounded final hex-encoded `qRcmd` reply and ends with an
explicit truncation marker if the full report cannot fit. See the
[GDB monitor-command guide](docs/gdb-monitor-commands.md) for the exact
read-only boundary, report fields, host tests, and live-hardware evidence.
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
stub and must deliberately preserve and chain handlers.

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
  to an arbitrary unmodified process.
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
- The retained-token step-over has no independent target-miss timeout yet. Its
  decoder therefore rejects waits, syscalls, exclusive-load starts, unsupported
  execution states, and uncertain PC writers, but a production resume-one path
  still needs transactional cancellation and trap rollback if the predicted
  instruction target is not reached.
- The initial accept path and exception/RSP path still hold a global spin lock
  across blocking work. Shutdown, registration, nested faults, and handler
  chaining need a bounded state-machine refactor before this is suitable for
  hostile or failure-prone applications.
- RSP parsing is intended only for a trusted debugger today. Individual `p`/`P`
  register packets now have exact syntax, number, width, and hexadecimal-value
  validation. Memory and full-register `G` writes still need strict length,
  syntax, overflow, page-boundary, and breakpoint-overlap validation plus
  parser fuzzing.
- Foreign-thread register writes are not implemented. Foreign-thread general
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
  retail 3.65. Privileged exception returns (`RFE`, `ERET`, and S-form ALU/LDM
  returns), `BXJ`, legacy Thumb-2 PC moves, register-controlled ARM shifts, and
  exclusive-load sequences remain deliberately fail-closed. The broader
  decoder matrix has pure host-planner coverage; additional injected-memory
  and trap-rollback integration fixtures remain.
- The ASLR-aware host tool safely automates symbol loading only when matching
  unstripped ELFs are available. Main-module and loaded-user-SUPRX source
  breakpoints pass across detach/reconnect and two relaunches on retail 3.65.
  The runtime module list has names and segment addresses but no build digest,
  so the developer must retain symbols from the exact deployed build. Sony
  system modules remain unmatched unless suitable symbols are supplied.
- Remote syscall catching is not implemented.
- Kernel plugins cannot be debugged with the current application-side stub.
- Disconnect and error cleanup needs broader fault injection to prove bounded
  socket shutdown, removal of every software breakpoint, all-stop release, and
  recovery from a client or lease-keeper failure.
- GDB console forwarding is intentionally bounded and lossy. It begins only
  after `QStartNoAckMode`, emits no unsolicited `O` packets after a stop reply,
  discards an old connection's bytes on reconnect, and can drop under socket or
  queue pressure. Newlib buffering is unchanged, and pre-capture short writes
  or `EAGAIN` cannot be included in the queue counters. Stop the debugger server
  before calling `uvdb_restore_stdio()`; `uvdb_shutdown()` does this
  automatically. Use DebugNet for sustained logging.
- GDB monitor commands are limited to the exact read-only `help`, `status`,
  `threads`, and `modules` registry. Arguments and unregistered commands are
  rejected; decoded command text is never executed or forwarded. Reports use
  fixed snapshot and packet bounds and visibly truncate at a complete line when
  possible. The retail 3.65 live gate passed state preservation, two clean raw-
  RSP detach/reconnect sessions, and display through GDB 15.2. Other firmware
  versions remain untested.
- The optional stdio bridge uses Vita newlib's private descriptor map because
  Vita newlib does not export `dup2`. Its current close/retry behavior was
  audited against newlib commit
  `64aa7aa33d4f380451a1f100d19589226cdad334`; other newlib revisions need a
  fresh lifecycle review even when the debugger core itself builds unchanged.
- The debugger and Vita Companion transports have no authentication or
  encryption. DebugNet uses best-effort UDP. Use the tools only on a private,
  trusted LAN; pairing, peer allowlists, and a secured control plane remain
  release blockers.
- `libvitaprofiler` currently records explicit zones, counters, frame markers,
  memory snapshots, and supplied known-thread statistics. It does not yet have
  a name dictionary, binary network/file drain, desktop viewer, arbitrary
  thread PC/call-stack sampling, PMU ownership, or automatic VitaGL/SceGxm GPU
  instrumentation.
- VitaDevDeploy currently depends on Vita Companion's unauthenticated FTP and
  command transport. Signed one-use jobs protect the install decision, but do
  not authenticate or encrypt Companion itself; use it only on a private LAN.
- Deployment is serialized and one-shot, starts from LiveArea, does not provide
  general target-app rollback, and still needs full bootstrap recovery and
  interrupted-install fault-injection testing before it is production-ready.
- The optional vita2d deployment UI has now produced two intermittent
  launch-time GPU faults on the retail 3.65 test configuration despite six
  clean supervised launch/exit cycles. A later retry reached its waiting state
  and completed the signed install successfully. The exact graphics/SceShell
  transition cause is not isolated; keep unattended deployment headless until
  startup and teardown are hardened and stress-tested.
- The tested compatibility target is retail handheld Vita and Vita TV hardware
  running system software 3.65 with the tested Kubridge release. Individual
  experimental feature gates may cover only the device class named in their
  evidence. Firmware 3.60 and all other firmware releases, development
  hardware, and other plugin combinations have not completed the same gates.
- Build-directory isolation, exported CMake/VitaSDK packages, CI, and a
  project-wide license are not finished.
- Long-running reconnect, shutdown, multithread, and fault stress testing is
  still in progress.

## Roadmap

1. Keep the completed opt-in foreign-thread GDB D0/D31/FPSCR lifecycle gate and
   its 3.65 evidence reproducible. Revalidate both VFP gates before supporting
   another firmware baseline; keep writes as a separate restoration project.
2. Complete the remaining
   [thread-control validation gate](docs/gdb-thread-control-validation.md).
   Unified inventory, `Hg`/`Hc`, `vCont;c;s`, fail-closed selection,
   abandoned-client recovery, and deterministic selected foreign-Thumb-thread
   stepping are hardware tested. The exact stopped-thread positive-`Hc`
   step-over and representative ARM-state PC writers now pass the live gate.
   Controlled renew/end failures, cleanup fault injection, and longer stress
   runs remain. Then extend the retained-stop design into scheduler-locked
   arbitrary-foreign-thread resume or displaced stepping so a process-wide
   temporary breakpoint cannot be won by another running thread.
3. Move blocking accept/RSP work outside the global spin lock; add nested-fault
   handling, previous-handler chaining, strict packet parsing, fake-kernel host
   tests, fuzzing, and long reconnect/shutdown/multithread hardware soaks.
4. Extend the completed single-owner, no-ack GDB `O`-packet console bridge with
   longer on-device pressure, abrupt-disconnect, restore/retry, and shutdown
   soaks. Keep the completed read-only `qRcmd`
   `help`/`status`/`threads`/`modules` two-session hardware gate reproducible,
   and require the same gate before extending its command set. Decide whether console
   queue/transport loss counters need a stable public API; retain DebugNet as
   the sustained-log and profiler path.
5. Keep the completed strict `p` and selected exception-thread core/CPSR `P`
   retail 3.65 transactional gate reproducible. Keep the expanded practical
   ARM/Thumb-2 PC-writer decoder and its live ARM fixtures reproducible; add
   injected-memory/trap-rollback coverage and a bounded LDREX/STREX strategy
   before relaxing any remaining fail-closed instruction family. Design a new
   kernel ABI with snapshot, write, read-back, rollback, lease-expiry, and
   detach restoration before enabling any foreign-thread or VFP write.
6. Keep hardware `Z1`-`Z4` fail-closed. The late-runtime DIP 228 path has now
   been tested and did not yield a safely returning DBGVCR read. Research
   boot-time policy, DSE/authentication, OS Lock, and debug-power state offline;
   do not resume CP14 comparator, debug-status, identity-spoof, boot patch, or
   memory-mapped probes without a separately reviewed safe path. Only then
   restart the staged
   [KVDB feature-parity](docs/kvdb-feature-parity.md) gates for disabled-register
   restore, one context-scoped execution breakpoint, one data watchpoint,
   correct stop replies, and lease/detach cleanup. In parallel, evaluate a
   page-protection/data-abort software-watchpoint fallback with strict page
   ownership, access decoding, single-step/rearm, and false-positive tests.
7. Extend the completed main-plus-user-SUPRX ASLR lifecycle gate with deployed-
   artifact build identity, dynamic module load/unload refresh, and IDE-managed
   symbol regeneration. Continue leaving unmatched system modules explicit.
8. Extend the profiler foundation with a name dictionary, binary drain/receiver,
   desktop trace viewer, and host-application instrumentation; evaluate narrow
   kernel PC/PMU sampling and explicit VitaGL/SceGxm hooks separately.
9. Add protocol authentication/pairing and peer allowlists, isolated feature
   build directories, exported VitaSDK/CMake packages, CI, a firmware matrix,
   and a project-wide license.
10. Keep the completed VitaDevDeploy signed-install and optional graphical-UI
   lifecycle gates reproducible. Isolate the intermittent graphical launch
   fault, add a fail-safe graphics startup/teardown state machine, and stress
   transitions from recently closed applications. Finish interrupted-install
   and recovery fault injection. Research a versioned, fail-closed SceShell
   content/cache refresh for artwork updates; never make destructive `app.db`
   deletion an automatic deployment step. Then integrate it with VS Code
   build/deploy/stop, IntelliSense, GDB, logs, and profiles. Later replace the
   external Vita Companion dependency with a
   versioned authenticated device service if its implementation audit supports
   that design.
11. Add optional attachment to applications not compiled with the library.
12. As the final compatibility milestone after all debugger and profiler paths
    are confirmed, validate LLDB remote-protocol behavior and add a Debug
    Adapter Protocol bridge for IDEs without regressing GDB.

## Repository layout

- `uvdb.c`: protocol server, safe memory access, breakpoints, stepping, and
  exception handling.
- `uvdb_registers.c` / `uvdb_registers.h`: host-testable selection of the valid
  user context from the Vita kernel's two raw ARM register banks.
- `uvdb_thread_control.c` / `uvdb_thread_control.h`: bounded thread inventory,
  selector parsing, and fail-closed resume/step planning.
- `uvdb_rsp.c` / `uvdb_rsp.h`: host-testable ARM and VFP register-packet
  serialization.
- `uvdb_vfp_policy.c` / `uvdb_vfp_policy.h`: fail-closed classification of
  normalized kernel VFP snapshot results.
- `uvdb_console.c` / `uvdb_console.h`: internal fixed-memory, generation-scoped
  GDB console queue and loss accounting.
- `uvdb_console_transport.c` / `uvdb_console_transport.h`: single-owner no-ack
  session state, `O`-packet framing, bounded pumping, and transport statistics.
- `uvdb_monitor.c` / `uvdb_monitor.h`: host-testable exact `qRcmd` registry and
  bounded read-only status, thread, and module report renderer.
- `uvdb.h`: public application API.
- `protocol/arm_vfp_target_xml.inc`: exact opt-in GDB D32 target description.
- `docs/gdb-register-access.md`: `p`/`P` numbering, thread-scope, mutation
  policy, transactional validation gate, and retail 3.65 evidence.
- `docs/gdb-monitor-commands.md`: monitor command reference, security boundary,
  response limits, native tests, and live-GDB validation evidence.
- `stdio_redirect.c` / `stdio_redirect.h`: restorable nonblocking Vita newlib
  `stdout`/`stderr` capture and internal-helper inventory filtering.
- `uvdb_debugnet.c`: bounded asynchronous UDP logs for DebugNet-style receivers.
- `test.c`: Vita hardware test program.
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
  native tests, and integration documentation.
- `deploy/`: self-contained signed host/Vita remote deployment subproject,
  including its agent, host CLI, tests, security documentation, and license.
- `tests/`: focused instruction fixtures and offline protocol tests.
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

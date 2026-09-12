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

The current application-side library has been tested extensively on real Vita
hardware. Its current status includes:

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
- ARM-state `MOV PC, Rm` and increment/decrement load-multiple PC restores.
- Thumb-2 and ARM immediate load-to-PC forms with protected target reads and
  correct ARM/Thumb interworking.
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
- Hardware-tested GDB all-stop integration: initial attach, live Ctrl-C,
  lease renewal during long stops, continue, detach, reconnect, and recovery
  after forced client termination.
- Hardware-tested read-only GDB register integration for main and worker
  threads owned by an active stop session, including symbolized stack frames.
- Hardware-tested stop-session reconciliation: threads created after the
  initial snapshot are discovered and suspended by the next lease renewal.
- Hardware-tested read-only ARM debug-resource discovery reporting six
  breakpoint, four watchpoint, and two context-aware breakpoint comparators.
- A default-off, token-protected VFP snapshot boundary for suspended
  session-owned threads, plus an opt-in D0-D31/FPSCR GDB packet path. The
  known-pattern hardware gate now passes every D-register, FPSCR, ownership,
  session-end, resume, and worker-restoration check. Live GDB validation is the
  remaining promotion step.
- GDB loaded-module discovery through chunk-safe `qXfer:libraries:read`,
  hardware-tested with 14 executable and system modules.
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

The kernel boundary is being introduced in deliberately small stages on retail
hardware. These unedited Vita screenshots record the completed probe results:

| Milestone | Hardware evidence | What it validates |
| --- | --- | --- |
| v1 | [Thread enumeration](docs/hardware/kernel-probe-v1-enumeration.jpg) | ABI discovery, caller-process thread IDs, bounds and NULL rejection |
| v2 | [Single-thread suspend](docs/hardware/kernel-probe-v2-single-thread-suspend.jpg) | `0x1002` suspended state, normal resume to state 0, disposable worker behavior |
| v3 | [Stop session and watchdog](docs/hardware/kernel-probe-v3-stop-session-watchdog.jpg) | Tokenized process stop/end and automatic recovery of an abandoned lease |
| v4 | [Lease-keeper exemption](docs/hardware/kernel-probe-v4-lease-exemption.jpg) | A validated exempt thread remains active to renew long GDB stop sessions |
| v5 | [Saved register banks](docs/hardware/kernel-probe-v5-register-banks.jpg) | Session ownership checks and both raw ARM banks; bank 1 contains the saved user-mode PC, SP, CPSR, and general registers |
| v6 | [Late-thread reconciliation](docs/hardware/kernel-probe-v6-late-thread-reconcile.jpg) | A thread created after stop begins is discovered on renewal, suspended, tracked, and resumed with the session |
| v7 | [Hardware-debug discovery](docs/hardware/kernel-probe-v7-hw-debug-discovery.jpg) | Read-only CP14 identification and the Vita's six breakpoint, four watchpoint, and two context-aware comparator counts |
| VFP v8 | [Corrected D32/FPSCR probe](docs/hardware/kernel-vfp-probe-v8-bank0-pass.jpg) | Guarded D0-D31 capture, raw FPSCR entry-0 mapping, ownership rejection, two-thread stop/resume, and clean worker restoration |
| DebugNet v24 | [Sustained UDP stream](docs/hardware/debugnet-v24-sustained-stream.jpg) | Logging remains live after a loaded stop/restart cycle, with the displayed queue draining and no packet drops or send errors |
| Profiler v1 | [User-mode profiler probe](docs/hardware/profiler-user-mode-probe-v1.jpg) | Eleven passing checks for live zones, frames, counters, memory/thread snapshots, wire encoding, four-producer pressure/drop accounting, uniqueness, and ring reuse without kernel calls |

Every displayed probe check passed. These images document controlled test
coverage; they do not claim that arbitrary applications or every firmware and
plugin combination are already supported.

The first disposable disabled-comparator round-trip attempt on 2026-09-12
[rebooted during its kernel critical section](docs/hardware/hw-disabled-probe-attempt-1.md).
Its valid pre-probe journal proves entry but not the exact failing instruction or
any comparator write. The separate, strictly sequential
[staged read-only ladder](docs/hardware/hw-read-ladder-attempt-2.md) then passed
its lifecycle, MIDR, DIDR, and DSCRint gates before rebooting at the first
`DBGVCR` read on core 1. This matches an ARM boundary for denied extended CP14
debug access; the exact authentication, OS Lock, or debug-power cause is not yet
identified. Comparator rungs were not run, and enabled hardware debugging
remains blocked.

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
Foreign-thread register reads use the hardware-validated user-mode saved bank
in experimental kernel-integrated builds. Foreign-thread writes remain
disabled until mutation and restoration are independently validated.

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
python tools/debugnet_listener.py --port 18194 --source 10.1.1.93
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
- A Vita C or C++ homebrew project capable of linking a static library.
- Local-network connectivity to the Vita.

Remote deployment additionally requires Python 3.10 or newer, PowerShell,
CMake, an Ed25519 implementation, Vita Companion 1.06, and one-time installation
of the VitaDevDeploy agent. See [the deployment requirements](deploy/README.md#requirements)
for the exact setup and safety boundary.

All application objects, the debugger library, Kubridge imports, and other
libraries must use compatible VitaSDK ABIs.

### Vita

- A homebrew-capable PS Vita or Vita TV.
- TaiHEN/Ensō or an equivalent kernel-plugin environment.
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

Applications using module discovery must also link
`SceKernelModulemgr_stub`; the included test Makefile does this automatically.

Build the included test VPK with:

```sh
make package \
  KUBRIDGE_DIR=../kubridge \
  KUBRIDGE_LIB_DIR=../kubridge/build-local
```

This creates `uvdb-test.vpk`. Retain `test.elf` for GDB.

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
the same VitaDebugger revision.

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
threads, and token-protected reads of both saved ARM register banks for a
session-owned suspended thread. ABI v1.7 also reserves a read-only candidate
VFP snapshot call. A normal build compiles that undocumented path out, omits
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

This feature is read-only. Full `G` register writes are rejected in that build
so GDB cannot silently claim that it changed VFP state. At each stop, the stub
also verifies the exact kernel ABI and capability bit before negotiating the
extended packet; a normal fail-closed plugin retains the legacy core-only
contract.

The repository test application can additionally compile a diagnostic-only
registered worker with deterministic D0, D31, and FPSCR values. Add
`UVDB_GDB_VFP_FIXTURE=1` to the command above and follow the
[live GDB VFP validation gate](docs/gdb-vfp-validation.md). The flag is rejected
unless the thread-control and VFP-read gates are also enabled. The fixture uses
a call-free busy loop and must not be enabled in a production application.

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
  LDFLAGS += -lSceNetPs_stub -pthread
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
builds can also return the validated user-mode register bank for a selected
suspended thread. A VFP D32 target description and packet serializer are
available only with `UVDB_KERNEL_VFP_READS=1`; leave that flag off until the
separate known-pattern probe passes on the target firmware, and continue to
treat it as experimental until the live GDB lifecycle gate passes. Foreign
register writes remain disabled.
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
sockets, and releases debugger-owned buffers and the safe-memory message pipe.

### Optional stdout/stderr forwarding

After GDB connects:

```c
if (uvdb_redirect_stdio() == 0) {
    printf("Forwarded through GDB remote file I/O.\n");
}
```

This current implementation creates a helper thread and sends light diagnostic
output through GDB remote file I/O. It is an experimental compatibility path,
not the final console transport. The planned debugger transport will capture
both stdout and stderr into a bounded, thread-safe, nonblocking queue and emit
GDB remote-console `O` packets. Queue depth, truncation, and drop counts will be
observable; disconnect, target-stop, and shutdown paths must never leave an
application writer blocked. DebugNet remains the independent path for sustained
logs and profiler streaming, including periods when GDB is disconnected.

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
and can report a selected suspended thread's saved general registers, PC, SP,
LR, and CPSR. The new kernel boundary can capture a selected thread's candidate
D0-D31 state and both raw saved FPSCR bank values without changing them. GDB
exposure remains compile-time opt-in pending the live GDB lifecycle gate;
foreign register writes are rejected.

## Connecting with GDB

Keep the exact unstripped ELF produced alongside the VPK. The packaged Vita
executable may be stripped, but GDB must load the unstripped file from the same
build:

```sh
arm-vita-eabi-gdb path/to/my_app.unstripped.elf
```

Then connect at the GDB prompt:

```gdb
target remote 10.1.1.93:1234
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
- Thread discovery is not yet one coherent source of truth. The application
  registry is bounded to 32 cooperative entries while the kernel boundary can
  enumerate 64 threads; stale registrations and the debugger's own helper
  threads still need stricter filtering. A newly created thread is folded into
  all-stop only at the next lease renewal, leaving a brief interval before it
  is suspended.
- GDB's continue-thread selection is recorded, but per-thread stepping and the
  full `Hc`/`vCont` state model are not complete. Stop-session creation or lease
  renewal failure also needs to abort the client session consistently rather
  than allowing partially stopped debugging to continue.
- The initial accept path and exception/RSP path still hold a global spin lock
  across blocking work. Shutdown, registration, nested faults, and handler
  chaining need a bounded state-machine refactor before this is suitable for
  hostile or failure-prone applications.
- RSP parsing is intended only for a trusted debugger today. Memory and full-
  register write packets still need strict length, syntax, overflow, page-
  boundary, and breakpoint-overlap validation plus parser fuzzing.
- Foreign-thread register writes are not implemented. Foreign-thread general
  register reads are hardware tested; the guarded VFP snapshot passed its
  separate known-pattern hardware gate, but the opt-in GDB mapping still needs
  a live foreign-thread D0/D31/FPSCR read and lifecycle test before promotion.
- Hardware breakpoint/watchpoint encoding and a guarded kernel session engine
  are implemented experimentally, but the staged retail probe rebooted at the
  first DSE-dependent `DBGVCR` read. No comparator access or enabled comparator
  has passed a hardware gate, so GDB does not advertise `Z1`-`Z4`.
- Software stepping does not decode every instruction capable of writing PC.
  Important remaining cases are concentrated in shifted PC-writing data-
  processing forms, register-offset PC loads, and uncommon ARM/Thumb control
  flow.
- GDB receives module segment bases, but automatic symbol loading still
  requires matching unstripped module files and a configured solib search path.
- Remote syscall catching is not implemented.
- Kernel plugins cannot be debugged with the current application-side stub.
- Disconnect and error cleanup needs broader fault injection to prove bounded
  socket shutdown, removal of every software breakpoint, all-stop release, and
  recovery from a client or lease-keeper failure.
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
- Build-directory isolation, an exported CMake/VitaSDK package, CI and firmware
  compatibility matrices, and a project-wide license are not finished.
- Long-running reconnect, shutdown, multithread, and fault stress testing is
  still in progress.

## Roadmap

1. Hardware-test opt-in GDB D0/D31/FPSCR reads on a foreign stopped thread,
   including continue, detach, reconnect, and watchdog recovery, then promote
   the read-only mapping from experimental status.
2. Replace the split cooperative/kernel thread bookkeeping with a coherent
   stop-state machine, complete `Hc`/`vCont` selection, and make stop or lease
   failure fail closed with bounded resume and disconnect cleanup.
3. Move blocking accept/RSP work outside the global spin lock; add nested-fault
   handling, previous-handler chaining, strict packet parsing, fake-kernel host
   tests, fuzzing, and long reconnect/shutdown/multithread hardware soaks.
4. Replace the experimental remote-file-I/O stdio bridge with bounded stdout
   and stderr capture delivered as GDB remote-console `O` packets. Add explicit
   buffering, truncation/drop counters, thread-safety, reconnect behavior, and
   tests proving that target output cannot block while the debugger is stopped.
5. Complete ARM and Thumb-2 control-flow decoding, then add `p`/`P` register
   access and independently validate any foreign-thread mutation/restoration.
6. Keep hardware `Z1`-`Z4` fail-closed while researching offline whether Vita's
   DSE/authentication, OS Lock, or debug-power state has a documented safe
   control path. Do not resume CP14 comparator, debug-status, or memory-mapped
   debug probes without that evidence. Only then restart the staged
   [KVDB feature-parity](docs/kvdb-feature-parity.md) gates for disabled-register
   restore, one context-scoped execution breakpoint, one data watchpoint,
   correct stop replies, and lease/detach cleanup. In parallel, evaluate a
   page-protection/data-abort software-watchpoint fallback with strict page
   ownership, access decoding, single-step/rearm, and false-positive tests.
7. Complete ASLR-aware symbol relocation: reconcile `qOffsets` and
   `qXfer:libraries:read` with every loaded module segment, automate matching
   unstripped ELF loading, and verify relocated breakpoints across relaunches.
8. Extend the profiler foundation with a name dictionary, binary drain/receiver,
   desktop trace viewer, and host-application instrumentation; evaluate narrow
   kernel PC/PMU sampling and explicit VitaGL/SceGxm hooks separately.
9. Add protocol authentication/pairing and peer allowlists, isolated feature
   build directories, exported VitaSDK/CMake packages, CI, a firmware matrix,
   and a project-wide license.
10. Finish VitaDevDeploy bootstrap, interrupted-install, and recovery validation;
   then integrate it with VS Code build/deploy/stop, IntelliSense, GDB, logs,
   and profiles. Later replace the external Vita Companion dependency with a
   versioned authenticated device service if its implementation audit supports
   that design.
11. Add optional attachment to applications not compiled with the library.
12. As the final compatibility milestone after all debugger and profiler paths
    are confirmed, validate LLDB remote-protocol behavior and add a Debug
    Adapter Protocol bridge for IDEs without regressing GDB.

## Repository layout

- `uvdb.c`: protocol server, safe memory access, breakpoints, stepping, and
  exception handling.
- `uvdb_rsp.c` / `uvdb_rsp.h`: host-testable ARM and VFP register-packet
  serialization.
- `uvdb.h`: public application API.
- `protocol/arm_vfp_target_xml.inc`: exact opt-in GDB D32 target description.
- `stdio_redirect.c`: optional newlib stdout/stderr forwarding.
- `uvdb_debugnet.c`: bounded asynchronous UDP logs for DebugNet-style receivers.
- `test.c`: Vita hardware test program.
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

# VitaDebugger

VitaDebugger is an experimental, open-source remote debugging toolkit for
PlayStation Vita homebrew. Its current component is an application-linked GDB
server (`libuvdb.a`) that allows a matching GDB on a development computer to
debug a program running on real Vita hardware over a local network.

The project is based on
[sleirsgoevy/vita-uvdb](https://github.com/sleirsgoevy/vita-uvdb) and is being
hardened, documented, tested on hardware, and expanded into a general VitaSDK
development tool. Render96ex is one of its integration and stress-test targets,
but VitaDebugger is not a Render96-specific project.

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
- Integrate those capabilities into normal VitaSDK projects and familiar IDEs.

It is intended for legitimate homebrew development and debugging on systems the
developer controls. It does not include or require Sony's proprietary SDK.

## Current status

The current application-side library has been tested on real Vita hardware with:

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
- A debugger-enabled Render96ex build as a larger real-world test.

It is already useful for controlled application debugging. It is not yet a
complete system-wide debugger; see [Current limitations](#current-limitations).

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

Every displayed probe check passed. These images document controlled test
coverage; they do not claim that arbitrary applications or every firmware and
plugin combination are already supported.

## End goals

The intended final design is a hybrid toolkit:

```text
GDB / IDE / profile viewer / log console
                  |
          local network transport
                  |
      application libraries and APIs
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

### DebugNet log streaming

The toolkit will eventually integrate
[psxdev/debugnet](https://github.com/psxdev/debugnet) as an optional transport
for continuous logs and telemetry over the network. DebugNet and GDB complement
one another:

- GDB handles breakpoints, faults, registers, stacks, memory, and control.
- DebugNet carries non-stopping logs, profiler events, frame timing, audio
  status, and other continuous diagnostics.

The integration will be hardened for bounded messages, explicit initialization,
disconnect handling, thread safety, and failure without crashing the host
application. Applications will not be required to use DebugNet.

### Desktop and IDE tools

The end goal also includes ready-to-use GDB command files, VS Code build/deploy/
debug configurations, a live log console, a trace viewer, and documented APIs
that other IDE extensions can consume.

## Requirements

### Development computer

- A working [VitaSDK](https://vitasdk.org/) installation.
- `VITASDK` set to the SDK directory and `$VITASDK/bin` on `PATH`.
- `arm-vita-eabi-gcc`, `arm-vita-eabi-ar`, and `arm-vita-eabi-gdb`.
- GNU Make for the included build.
- A Vita C or C++ homebrew project capable of linking a static library.
- Local-network connectivity to the Vita.

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

Provide the Kubridge source and import-library locations:

```sh
export VITASDK=/path/to/vitasdk
export PATH="$VITASDK/bin:$PATH"

make \
  KUBRIDGE_DIR=../kubridge \
  KUBRIDGE_LIB_DIR=../kubridge/build-local
```

This produces `libuvdb.a`. The public header is `uvdb.h`.

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
session-owned suspended thread. Keep a known-good taiHEN configuration backup
while testing kernel builds.

The same build produces `vitadebug-kernel-probe.vpk`. The probe checks the ABI,
capability bits, main/worker thread visibility, invalid argument rejection, a
normal tokenized stop/end sequence, and automatic watchdog resumption after an
intentionally abandoned lease. Any failed boundary check is shown as a `FAIL`
line on screen.

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
suspended thread; register writes and VFP-register packet mapping remain
disabled.
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

This creates a helper thread and is suitable for light diagnostic output. Use
the planned DebugNet integration for sustained log or profiler streaming.

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
LR, and CPSR. VFP registers remain unavailable and foreign register writes are
rejected.

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

## Render96ex integration example

Render96ex is a test consumer, not a dependency. Its current local build option
uses this pattern:

```sh
make VERSION=us TARGET_VITA=1 VITA_DEBUGGER=1 \
  VITA_UVDB_DIR=../VitaDebugger \
  VITA_KUBRIDGE_DIR=../kubridge
```

Use a distinct Vita title ID for diagnostic packages so the optimized game can
remain installed. Preserve the matching unstripped ELF on the computer.

## Current limitations

- The library must currently be compiled into the application; it cannot attach
  to an arbitrary unmodified process.
- Kernel all-stop is opt-in and requires the matching `vitadebug.skprx` ABI;
  library-only builds continue to provide application-side stopping.
- A newly created thread is folded into all-stop at the next lease renewal, so
  there can be a brief interval before it is suspended.
- Foreign-thread register writes and VFP context mapping are not implemented;
  foreign-thread general-register reads are hardware tested.
- Hardware breakpoints and watchpoints are not implemented.
- Software stepping does not decode every instruction capable of writing PC.
  Important remaining cases include Thumb IT blocks, load-multiple into PC,
  and uncommon ARM control flow.
- Loaded-module base-address discovery is not implemented.
- Remote syscall catching is not implemented.
- Kernel plugins cannot be debugged with the current application-side stub.
- The network protocol has no authentication or encryption.
- Long-running reconnect, shutdown, multithread, and fault stress testing is
  still in progress.

## Roadmap

1. Validate foreign-thread VFP context mapping without enabling writes.
2. Complete ARM and Thumb-2 control-flow decoding.
3. Hardware breakpoints and watchpoints.
4. Reusable VitaSDK and exported CMake packages.
5. Hardened optional DebugNet log and telemetry streaming.
6. VS Code build, deployment, IntelliSense, and GDB configurations.
7. A separate optimized profiler library and desktop trace viewer.
8. Optional attachment to applications not compiled with the library.

## Repository layout

- `uvdb.c`: protocol server, safe memory access, breakpoints, stepping, and
  exception handling.
- `uvdb.h`: public application API.
- `stdio_redirect.c`: optional newlib stdout/stderr forwarding.
- `test.c`: Vita hardware test program.
- `tests/`: focused instruction fixtures.
- `kernel/`: narrow kernel companion, generated user stubs, and boundary probe.
- `Makefile`: static library and test-package build.

## Attribution

VitaDebugger derives from
[sleirsgoevy/vita-uvdb](https://github.com/sleirsgoevy/vita-uvdb). Kubridge and
DebugNet are separate projects maintained by their respective authors. Consult
this repository's license and each dependency's license before redistribution.

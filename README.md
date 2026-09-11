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
- Catching data aborts before the standard Vita crash screen.
- Structured fault information: exception type, signal, FSR, FAR, PC, LR, SP.
- Reconnection at a later `uvdb_enter()` after a disconnected session.
- Opt-in persistent server thread for clean reattachment and experimental
  Ctrl-C interruption while the application is running.
- Build-tested cooperative registration and GDB discovery of named application
  threads; hardware validation of this new path is in progress.
- A debugger-enabled Render96ex build as a larger real-world test.

It is already useful for controlled application debugging. It is not yet a
complete system-wide debugger; see [Current limitations](#current-limitations).

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
- Reading and writing registers belonging to another thread.
- Hardware breakpoints and watchpoints.
- Process and module discovery.
- Eventually attaching to an application not built with `libvitadebug`.

The plugin will expose a small validated interface rather than a general
arbitrary kernel-access service. Library-only operation will remain supported.

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

This is an experimental application-side stop rather than complete all-stop
debugging. Ctrl-C currently stops the debugger service thread; registered
application threads remain active and their registers remain unavailable.
Do not use it to inspect state that other threads are actively changing. The
planned kernel companion will provide validated process-wide suspension and
foreign-thread register capture.
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

The current user-mode implementation supports GDB thread listing, names,
liveness checks, selection, and identification of the thread that entered the
exception handler. It intentionally rejects register access for a selected
thread that is not stopped, because it does not yet possess a valid saved
context for that thread. Registration alone does not suspend the thread.

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
- Initial cooperative GDB thread discovery is implemented, but other threads
  are not yet coherently suspended and another thread can encounter a temporary
  stepping breakpoint first.
- The persistent server can receive Ctrl-C and reconnect after clean detach,
  but its application-side stop does not freeze other threads. Abrupt network
  loss recovery and repeated stop/resume stress testing remain experimental.
- Reliable enumeration, suspension, resumption, and foreign-thread register
  access require the planned kernel companion.
- Hardware breakpoints and watchpoints are not implemented.
- Software stepping does not decode every instruction capable of writing PC.
  Important remaining cases include Thumb IT blocks, `POP {..., pc}`, load-
  multiple into PC, `TBB`/`TBH`, `MOV pc`, and uncommon ARM control flow.
- Loaded-module base-address discovery is not implemented.
- Remote syscall catching is not implemented.
- Kernel plugins cannot be debugged with the current application-side stub.
- The network protocol has no authentication or encryption.
- Long-running reconnect, shutdown, multithread, and fault stress testing is
  still in progress.

## Roadmap

1. Persistent application-side connection, reattachment, and Ctrl-C handling.
2. Correct all-stop behavior backed by a minimal kernel companion.
3. Complete ARM and Thumb-2 control-flow decoding.
4. Hardware breakpoints and watchpoints.
5. Reusable VitaSDK and exported CMake packages.
6. Hardened optional DebugNet log and telemetry streaming.
7. VS Code build, deployment, IntelliSense, and GDB configurations.
8. A separate optimized profiler library and desktop trace viewer.
9. Optional attachment to applications not compiled with the library.

## Repository layout

- `uvdb.c`: protocol server, safe memory access, breakpoints, stepping, and
  exception handling.
- `uvdb.h`: public application API.
- `stdio_redirect.c`: optional newlib stdout/stderr forwarding.
- `test.c`: Vita hardware test program.
- `tests/`: focused instruction fixtures.
- `Makefile`: static library and test-package build.

## Attribution

VitaDebugger derives from
[sleirsgoevy/vita-uvdb](https://github.com/sleirsgoevy/vita-uvdb). Kubridge and
DebugNet are separate projects maintained by their respective authors. Consult
this repository's license and each dependency's license before redistribution.

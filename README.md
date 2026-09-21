# VitaDebugger

VitaDebugger is an experimental VitaSDK toolkit for debugging, logging, and
profiling PlayStation Vita homebrew. It currently provides:

- an application-linked GDB server (`libuvdb.a`);
- an optional, narrow kernel companion (`vitadebug.skprx`) for process all-stop
  and caller-process thread inspection;
- a standalone, allocation-free profiler (`libvitaprofiler.a`);
- bounded GDB-console, UDP-log, and profiler-trace transports; and
- the separately licensed [VitaDevDeploy](deploy/README.md) remote deployment
  helper and a [VS Code workflow](examples/vscode-debug-demo/README.md).

The debugger is embedded in a VitaSDK C or C++ application. It is not a
system-wide debugger and does not require Sony's proprietary SDK. Use it only
for legitimate homebrew development on systems you control.

> **Compatibility baseline:** retail PS Vita and Vita TV hardware running
> system software **3.65**. Individual validation records may cover a narrower
> device/configuration combination. Firmware 3.60 and every other firmware,
> development hardware, other newlib revisions, and other plugin combinations
> remain untested.

> **Network boundary:** the GDB and profiler transports are not authenticated
> or encrypted, and DebugNet-compatible logging uses best-effort UDP. Use them
> only on a trusted private LAN; never forward their ports to the internet.

## Start here

Choose the smallest mode that supplies the data you need:

| Mode | Add to the application | Kernel companion | Best for |
| --- | --- | --- | --- |
| [1. Debugger library only](#1-debugger-library-only) | `libuvdb.a` | No VitaDebugger companion; Kubridge is still required | Breakpoints, faults, registers, memory, source stepping, cooperative thread names |
| [2. Debugger + kernel companion](#2-debugger--kernel-companion) | Kernel-enabled `libuvdb.a` + import stub | Normal companion build | Coherent process all-stop, complete caller-process thread inventory, foreign-thread register reads |
| [3. Profiler library only](#3-profiler-library-only) | `libvitaprofiler.a` | No | Zones, counters, frame pacing, memory/known-thread snapshots, TCP traces |
| [4. Profiler + kernel PMU](#4-profiler--kernel-pmu-experimental) | Profiler + PMU import stub | Separately gated PMU build | Fixed-core, fixed-lane experimental PMU samples |

Debugger stops distort timing, so normal profiling should use mode 3 or 4
without an active GDB session.

## What is proven, and what is not

This distinction is important: “implemented” does not automatically mean
“hardware-validated” or “ready for unrestricted use.”

| Area | Hardware-validated on retail 3.65 | Experimental or unavailable |
| --- | --- | --- |
| Application debugger | GDB over TCP, up to 32 ARM/Thumb software breakpoints (`Z0`), source/function breakpoints, faults, registers, stack/memory access, live variable changes, practical stepping, detach/reconnect, Ctrl-C, bounded console output, read-only monitor commands; deterministic host stress covers 1,000 concurrent reconnect generations per run | Some uncommon PC-writing and privileged instructions fail closed; equivalent long-duration fault/reconnect stress on retail hardware is still pending |
| Kernel-assisted debugger | Caller-process thread inventory, lease-protected all-stop, late-thread reconciliation, watchdog recovery, selected foreign-thread core-register reads, exception-thread R0/CPSR writes, optional read-only VFP snapshots | Foreign-thread core/VFP writes are disabled; VFP reads use an undocumented, opt-in boundary; arbitrary scheduler-locked foreign-thread stepping is not complete |
| Breakpoint hardware | Read-only comparator inventory | **Hardware breakpoints and watchpoints are unavailable on the tested retail hardware.** GDB does not advertise `Z1`-`Z4`; guarded probes did not produce a safely returning comparator-access path |
| User-mode profiler | Named zones/counters, frames, memory and known-thread snapshots, bounded name dictionary, loss accounting, TCP capture, decoded JSON, and Chrome Trace/Perfetto export | Arbitrary thread PC/call-stack sampling, true GPU timestamps, and a dedicated desktop GUI remain pending |
| Kernel PMU | Fixed core 0/lane 5 normal-close gates for events `0x01`, `0x03`, and `0x10`, each with exact restoration; same-boot dormant-owner-thread safe re-arm also passed | Process exit, crash, receiver disconnect, timeout, and competing-owner recovery are still pending; cycles, arbitrary events/cores/lanes, and unrestricted production sampling are disabled |
| External application attach | Read-only protocol and broker boundaries have host tests and a Vita cross-build | No resident listener, trusted foreign-target identity provider, module injection, process mutation, or live GDB attach exists |

The application-linked stub must therefore be compiled into the program being
debugged. The [`attach/` scaffold](attach/README.md) does **not** attach to an
unmodified running application at this revision.

## Prerequisites

Development computer:

- [VitaSDK](https://vitasdk.org/) with `VITASDK` set and its `bin` directory on
  `PATH`;
- `arm-vita-eabi-gcc`, `arm-vita-eabi-ar`, `arm-vita-eabi-gdb`, GNU Make, and
  Python 3.10 or newer;
- CMake for the optional kernel companion; and
- local-network access to the Vita.

Vita:

- a homebrew-enabled retail Vita on system software 3.65;
- [Kubridge](https://github.com/bythos14/kubridge), tested with the official
  `v0.3.1_hotfix` `exceptions_mprotect` release, loaded from
  `ur0:tai/config.txt` under `*KERNEL`; and
- networking initialized by the application before the first debugger or
  profiler TCP call.

Keep the application, `libuvdb.a`, Kubridge imports, kernel import stub, and
boot-loaded companion on compatible VitaSDK/repository revisions. Restart the
Vita after changing TaiHEN configuration and keep a known-good configuration
backup before testing a kernel build.

On Windows, validate the repository's process-local VitaSDK/MSYS2 environment
wrapper first:

```powershell
.\tools\invoke-vita-env.ps1 -ValidateOnly
```

The examples below use a VitaSDK-capable shell. On Windows, commands can also
be passed through the wrapper, for example
`.\tools\invoke-vita-env.ps1 make host-tests`.

## 1. Debugger library only

Build the application-side archive from the repository root:

```sh
make \
  KUBRIDGE_DIR=../kubridge \
  KUBRIDGE_LIB_DIR=../kubridge/build-local
```

The output is `libuvdb.a`; the public header is `uvdb.h`. Put the archive after
application objects and its dependencies after the archive because static link
order matters:

```make
VITADEBUGGER_DIR ?= ../VitaDebugger
KUBRIDGE_DIR ?= ../kubridge
KUBRIDGE_LIB_DIR ?= $(KUBRIDGE_DIR)/build-local

CFLAGS += -Og -g3 -I$(VITADEBUGGER_DIR) -I$(KUBRIDGE_DIR)
LDFLAGS += $(VITADEBUGGER_DIR)/libuvdb.a
LDFLAGS += -L$(KUBRIDGE_LIB_DIR) -lkubridge_stub
LDFLAGS += -lSceNet_stub -lSceNetPs_stub
LDFLAGS += -lSceKernelModulemgr_stub -pthread
```

After the application has initialized Vita networking, use a deliberate entry
point that waits for GDB:

```c
#include <uvdb.h>

static void start_debugger(void)
{
    const struct uvdb_config config = {
        .port = 1234,
        .max_packet_buffer = 256 * 1024,
    };

    if (uvdb_configure(&config) == 0)
        uvdb_enter();
}
```

For a service that accepts clean reconnects and Ctrl-C while the application is
running, call `uvdb_start_server()` instead. Stop it with
`uvdb_stop_server()` during orderly teardown, or call terminal
`uvdb_shutdown()` from normal application code. Do not call shutdown from an
exception handler, and do not unload the linked debugger image after terminal
shutdown.

The listener does not treat a bare TCP connection as a debugger session. It
keeps the target running until the peer supplies a complete checksum-valid RSP
frame, then preserves that frame for the normal GDB handshake. Silent port
checks and non-RSP probes are closed after a bounded admission window without
using up the session.

Library-only Ctrl-C stops the debugger service thread; it does not coherently
suspend every application thread. Thread discovery is cooperative:

```c
static void *worker(void *argument)
{
    uvdb_register_thread("asset worker");
    run_worker(argument);
    uvdb_unregister_thread();
    return NULL;
}
```

Keep the exact unstripped ELF from the installed VPK build, then connect
directly without probing the single-client port first:

```sh
arm-vita-eabi-gdb build/my_app.elf
```

```gdb
target remote VITA_IP:1234
break source_file.c:120
continue
```

Useful commands include `backtrace`, `info registers`, `info locals`,
`print`, `set variable`, `x`, `step`, `next`, `stepi`, `continue`, and
`detach`. The fixed read-only monitor registry provides:

```gdb
monitor help
monitor status
monitor threads
monitor modules
monitor console
monitor display
```

`monitor display` reports metadata only, not framebuffer pixels. It requires a
library built with `UVDB_MONITOR_DISPLAY=1` and an application link against
`SceDisplay_stub`. See the [monitor command guide](docs/gdb-monitor-commands.md),
[console transport guide](docs/gdb-console-transport.md), and
[ASLR-aware symbol guide](docs/gdb-symbol-loading.md).

## 2. Debugger + kernel companion

Build the normal companion in a path without spaces:

```sh
cmake -S kernel -B kernel/build -G "Unix Makefiles"
cmake --build kernel/build
```

This produces `kernel/build/vitadebug.skprx`, the stable boundary probe VPK,
and user import libraries under `kernel/build/vitadebug_stubs/`. Install the
matching `.skprx` as a boot-loaded TaiHEN kernel plugin, reboot, and build the
debugger archive with kernel thread control enabled:

```sh
make \
  KUBRIDGE_DIR=../kubridge \
  KUBRIDGE_LIB_DIR=../kubridge/build-local \
  UVDB_KERNEL_THREAD_CONTROL=1 \
  VITADEBUG_KERNEL_DIR=kernel \
  VITADEBUG_KERNEL_BUILD_DIR=kernel/build
```

Compile the application with `-DUVDB_KERNEL_THREAD_CONTROL`, add
`kernel/include` to its include path, and link the generated stub after
`libuvdb.a`:

```make
CFLAGS += -DUVDB_KERNEL_THREAD_CONTROL
CFLAGS += -I$(VITADEBUGGER_DIR)/kernel/include
LDFLAGS += $(VITADEBUGGER_DIR)/kernel/build/vitadebug_stubs/libvitadebug_kernel_stub.a
```

Application code and GDB commands are otherwise the same as mode 1. Startup
validates the exact companion ABI, required thread-control capability mask,
and 64-entry inventory contract. A mismatch fails closed before the debugger
opens a socket or starts helper threads.

This mode adds coherent caller-process all-stop, complete process thread
enumeration, stop-lease renewal/recovery, and read-only register views for
session-owned suspended threads. Foreign-thread register writes remain
disabled. Optional D0-D31/FPSCR reads require the separate
`VITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT=ON` companion build plus
`UVDB_KERNEL_VFP_READS=1` in the library; they are read-only and should stay
off unless the [VFP validation gate](docs/vfp-layout-gate.md) has passed for the
target firmware.

The exact GDB register policy is documented in
[GDB register access](docs/gdb-register-access.md), and the implemented
step/fail-closed matrix is tracked in the
[feature-parity checklist](docs/kvdb-feature-parity.md).

## 3. Profiler library only

Build and link-check the standalone profiler:

```sh
make -C profiler vita-check
```

The archive is `profiler/build/vita/libvitaprofiler.a`. A typical application
link adds:

```make
CFLAGS += -I$(VITADEBUGGER_DIR)/profiler/include
LDFLAGS += $(VITADEBUGGER_DIR)/profiler/build/vita/libvitaprofiler.a
LDFLAGS += -lSceKernelThreadMgr_stub -lSceProcessmgr_stub
LDFLAGS += -lSceSysmem_stub -lSceLibKernel_stub
```

The application owns all storage. A minimal bounded capture looks like this:

```c
#include <vitaprofiler.h>

static struct vp_context profiler;
static struct vp_slot profiler_slots[1024]; /* 40 KiB */
static uint32_t frame_name;
static uint32_t update_name;

static int profiler_start(void)
{
    frame_name = vp_name_id("main frame");
    update_name = vp_name_id("update");
    return vp_vita_init(&profiler, profiler_slots, 1024);
}

static void run_one_frame(void)
{
    struct vp_zone_scope update;

    vp_frame_mark(&profiler, frame_name);
    if (vp_zone_begin(&profiler, update_name, &update) == VP_RESULT_OK) {
        update_application();
        vp_zone_end(&profiler, &update);
    } else {
        update_application();
    }
}

static size_t drain_profile(struct vp_event *events, size_t capacity)
{
    return vp_drain(&profiler, events, capacity);
}
```

Use the bounded name dictionary before producer threads start if exported
traces must resolve names. Exactly one consumer may drain a context; any number
of producers may record, and a full ring drops rather than blocks. Call memory
or known-thread snapshot helpers at a low frequency rather than in every draw.
The complete initialization, dictionary, TCP, and cleanup examples are in the
[profiler guide](profiler/README.md).

For a network trace, start the receiver first:

```sh
python profiler/tools/vitaprofiler_trace.py receive capture.vptrace \
  --bind 0.0.0.0 --port 18195 --source VITA_IP
```

Then inspect or export the finished capture:

```sh
python profiler/tools/vitaprofiler_trace.py view capture.vptrace
python profiler/tools/vitaprofiler_trace.py json capture.vptrace capture.json
python profiler/tools/vitaprofiler_trace.py chrome capture.vptrace capture.perfetto.json
```

The application remains responsible for the SceNet module/global lifetime;
add `SceNet_stub` when linking the TCP adapter. The adapter owns only its
socket/epoll resources. See the
[binary trace](profiler/docs/binary-trace.md) and
[Vita TCP sink](profiler/docs/vita-tcp-stream.md) guides.

The direct user-mode ScePerf initializer fails closed with
`VP_ERROR_UNSUPPORTED` on the tested retail runtime because its imports were
not safely bound. Mode 3 therefore makes no PMU claim.

## 4. Profiler + kernel PMU (experimental)

The normal companion build omits the PMU backend and its exported calls fail
closed. Build this mode only in a separate directory and follow the disposable
hardware-gate runbook:

```sh
cmake -S kernel -B kernel/build-pmu -G "Unix Makefiles" \
  -DVITADEBUG_EXPERIMENTAL_VFP_SNAPSHOT=ON \
  -DVITADEBUG_EXPERIMENTAL_HW_DEBUG=OFF \
  -DVITADEBUG_EXPERIMENTAL_PMU_PROFILER=ON \
  -DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=ON \
  -DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_SAFE_REARM=ON \
  -DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_PROCESS_EXIT_GATE=OFF \
  -DVITADEBUG_PMU_PROFILER_GATE_EVENT=0x01
cmake --build kernel/build-pmu
```

The only real-event values accepted by the build are:

| Value | Event name |
| --- | --- |
| `0x01` | L1 instruction-cache miss/refill |
| `0x03` | L1 data-cache miss/refill |
| `0x10` | Branch mispredict |

The public ABI fixes the sample to application core 0 and physical PMU lane 5,
accepts one event at a time, and requires an explicit real-event
acknowledgement. It exposes no arbitrary CP15 register, core, lane, cycle, or
raw-event selector. Counts are raw, core-wide events rather than per-thread
samples.

Link the application to both the profiler archive and the matching
`kernel/build-pmu/vitadebug_stubs/libvitadebug_kernel_stub.a`, include
`vitadebug_pmu_profiler.h`, and keep GetInfo/Open/Read/Close on the same
controller thread. That thread must run on application core 0 (affinity mask
`0x00010000`) for the validated path. The essential call order is:

```c
#include <vitaprofiler.h>
#include <vitadebug_pmu_profiler.h>

struct pmu_capture {
    struct vd_kernel_pmu_profiler_handle handle;
    struct vd_kernel_pmu_profiler_sample sample;
    int read_result;
    unsigned int open;
    unsigned int read_attempted;
};

/* Keep this object alive until pmu_capture_finish() closes successfully. */
static struct pmu_capture capture;

/* Call from a controller thread already pinned to core 0. */
static int pmu_capture_start(void)
{
    struct vd_kernel_pmu_profiler_info info = {
        .struct_size = sizeof(info),
        .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
    };
    struct vd_kernel_pmu_profiler_open_request request = {
        .struct_size = sizeof(request),
        .abi_version = VD_KERNEL_PMU_PROFILER_ABI_VERSION,
        .event_code = VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS,
        .lease_ms = VD_KERNEL_PMU_PROFILER_MIN_LEASE_MS,
        .flags = VD_KERNEL_PMU_PROFILER_OPEN_ACK_REAL_EVENT,
    };

    if (capture.open || vdKernelPmuProfilerGetInfo(&info) != 0 ||
        (info.capabilities & VD_KERNEL_PMU_PROFILER_CAP_REAL_EVENTS) == 0)
        return -1;

    capture.handle = (struct vd_kernel_pmu_profiler_handle){0};
    capture.sample = (struct vd_kernel_pmu_profiler_sample){0};
    capture.read_result = -1;
    capture.read_attempted = 0;
    if (vdKernelPmuProfilerOpen(&request, &capture.handle) != 0)
        return -1;
    capture.open = 1;
    return 0;
}

/* Retry after a retryable restoration result; this retains the handle. */
static int pmu_capture_finish(struct vp_context *profiler,
                              uint32_t counter_name)
{
    if (!capture.open)
        return -1;
    if (!capture.read_attempted) {
        capture.read_result = vdKernelPmuProfilerRead(&capture.handle,
                                                       &capture.sample);
        capture.read_attempted = 1;
    }

    int close_result = vdKernelPmuProfilerClose(&capture.handle);
    if (close_result != 0)
        return close_result;
    capture.open = 0;

    if (capture.read_result != 0)
        return capture.read_result;
    if (capture.sample.struct_size != sizeof(capture.sample) ||
        capture.sample.abi_version != VD_KERNEL_PMU_PROFILER_ABI_VERSION ||
        capture.sample.owner_token != capture.handle.owner_token ||
        capture.sample.generation != capture.handle.generation ||
        capture.sample.event_code !=
            VD_KERNEL_PMU_PROFILER_EVENT_ICACHE_MISS ||
        capture.sample.core_id != VD_KERNEL_PMU_PROFILER_FIXED_CORE ||
        capture.sample.physical_counter !=
            VD_KERNEL_PMU_PROFILER_FIXED_COUNTER)
        return -1;
    return vp_counter(profiler, counter_name,
                      (int64_t)capture.sample.value);
}
```

Register `counter_name` in the profiler dictionary before sealing it. Pin the
controller with `sceKernelChangeThreadCpuAffinityMask(..., 0x00010000)` before
Open, run the bounded workload between the two example calls, and restore the
old affinity only after `pmu_capture_finish()` succeeds. If Close reports a
restoration obligation, call the finish function again from the same thread;
do not discard the retained handle or treat a read as success. There is no
shipped application-side provider adapter from these kernel exports to
`vp_pmu_session_begin()`; the example records the validated raw sample as an
ordinary profiler counter. The
[PMU transport runbook](kernel/pmu-profiler-gate/README.md) is authoritative
for candidate builds and hardware runs.

This mode is not production-approved. Retail 3.65 gates passed normal close
and exact restoration for all three allowlisted events. The default-off
safe-rearm candidate also passed the narrower same-boot dormant-owner-thread
case: after the owning worker exited without Close, a fresh lease opened in the
same live process. The measured original-Open-to-replacement-Open upper bound
was 3,951 microseconds, before the five-second lease could expire; re-arm
polling itself took 251 microseconds, and the replacement closed with exact
restoration. Process exit, crash, forced termination, disconnect, timeout,
competing ownership, and UID-reuse/quarantine cases remain pending hardware
gates. The process-exit gate remains disabled.

## Optional logging and console output

For sustained non-stopping logs, start the DebugNet-compatible receiver:

```sh
python tools/debugnet_listener.py --port 18194 --source VITA_IP
```

Then start the bounded Vita sender after network initialization:

```c
const struct uvdb_debugnet_config logs = {
    .server_ip = "10.1.1.10", /* development computer */
    .port = 18194,
    .level = UVDB_LOG_DEBUG,
};

if (uvdb_debugnet_start(&logs) == 0)
    uvdb_debugnet_printf(UVDB_LOG_INFO, "frame=%u\n", frame_number);
```

The queue is bounded and lossy by design. The thread which calls
`uvdb_debugnet_start()` owns the stream, so start it from a thread which lives
for the whole logging session (normally the application's main thread). Query
`uvdb_debugnet_get_stats()` and call `uvdb_debugnet_stop()` after joining log
producers. Explicit stop drains queued messages on a bounded best-effort basis
and remains the deterministic choice before `sceNetTerm()` or module unload.
A single process-wide exit hook covers ordinary `return` from `main()` and
`exit()`: it closes the writer gate, drops queued work, avoids all socket calls,
and boundedly joins the sender before newlib releases the heap. If lock, writer,
or worker quiescence cannot be proven, it uses a narrowly scoped direct process
exit so heap teardown cannot race the sender. Direct `_Exit()`, `abort()`,
`sceKernelExitProcess()`, crashes, and SceShell termination can bypass the C
hook and are not guaranteed graceful-shutdown paths. The exact-owner thread
callback remains a nonblocking best-effort fallback for owner-thread and
SceShell close paths; call explicit stop whenever application code can do so.
Per-session generation and emergency-signal gates make delayed/racing callbacks
harmless after cleanup, restart, or thread-UID reuse. Socket and kernel-object
cleanup is retryable; if the initial start fails during resource setup, call
`uvdb_debugnet_stop()` once before retrying it. A successful send means the Vita
network stack accepted a UDP datagram, not that the PC received it.
The lifecycle design and hardware gate are documented in
[DebugNet owner lifecycle](docs/debugnet-owner-lifecycle.md).

For short debugger-console messages, start the persistent GDB server and then
call `uvdb_redirect_stdio()`. Forwarding begins only after GDB negotiates
no-ack mode, may drop under pressure, and must be restored only after the
server stops. `uvdb_shutdown()` performs the required order automatically.

## Build, test, and sample targets

Common repository targets:

```sh
make host-tests
make package KUBRIDGE_DIR=../kubridge KUBRIDGE_LIB_DIR=../kubridge/build-local
make -C profiler host-test
make -C profiler vita-probe
```

- `make package` produces `uvdb-test.vpk` and retains `test.elf` for GDB.
- `make -C profiler vita-probe` produces the user-mode profiler self-test at
  `profiler/build/vita-probe/vitaprofiler-probe.vpk`; it does not require the
  kernel companion.
- `make host-tests` covers the host-testable RSP, lifecycle, kernel-boundary,
  profiler, symbol, console, and attach scaffolds, including forced
  library-only and kernel-assisted production-translation-unit variants.

The isolated [VS Code sample](examples/vscode-debug-demo/README.md) turns F5
into build, signed deployment, launch, installed-build verification, live-ASLR
symbol loading, GDB connection, and exact-title cleanup. It uses the
application-linked debugger and does not require the optional companion.

## Remote deployment

VitaDevDeploy is a separate experimental subproject. Complete its one-time
agent and signing-key setup before using this command from `deploy/`:

```powershell
py -3 -m host.vitadevdeploy deploy "C:\path\to\MyHomebrew.vpk" `
  --vita 192.168.1.42 `
  --private-key .\local\deploy_private.pem `
  --action install_launch
```

Keep the private key off the Vita and out of version control. Begin deployment
from LiveArea; the tool deliberately refuses to force-close a running
application. The carrier is not encrypted and does not authenticate the Vita,
and direct mode still uses Vita Companion for limited lifecycle/result work.
Read the [deployment guide](deploy/README.md) and
[security boundary](deploy/SECURITY.md) before enabling installation.

## Hardware validation highlights

The linked records define the scope of each claim; none should be generalized
to another firmware or configuration.

| Milestone | Evidence | Result |
| --- | --- | --- |
| Kernel stop boundary | [Stop session/watchdog](docs/hardware/kernel-probe-v3-stop-session-watchdog.jpg), [late-thread reconciliation](docs/hardware/kernel-probe-v6-late-thread-reconcile.jpg) | Tokenized all-stop, abandoned-lease recovery, and newly created thread reconciliation passed |
| Practical stepping/registers | [Lifecycle record](docs/hardware/gdb-step-register-exclusive-3.65.json) | ARM/Thumb step cases, bounded exclusive sequences, R0/CPSR restoration, disconnect trap cleanup, and reconnect passed |
| VFP read path | [Kernel gate](docs/hardware/kernel-vfp-probe-v8-bank0-pass.jpg), [live GDB record](docs/hardware/gdb-vfp-live-3.65.json) | Guarded D0-D31/FPSCR reads and lifecycle checks passed; writes remain disabled |
| ASLR and build identity | [Five-session record](docs/hardware/gdb-aslr-build-identity-3.65.json) | Installed main/user-module identity, mismatch rejection, relaunch symbol refresh, and source breakpoints passed |
| GDB monitor/console | [Monitor record](docs/hardware/gdb-monitor-console-display-3.65.json) | Fixed read-only commands, state preservation, detach/reconnect, console statistics, and display metadata passed |
| GDB client admission | [Admission record](docs/hardware/debugger-client-admission-retail-3.65.json) | Silent, HTTP, partial, and bad-checksum probes did not consume the listener; GDB Ctrl-C, detach/reconnect, and pending-candidate shutdown passed |
| GDB RSP reset retry | [First attempt](docs/hardware/rsp-network-reset-retry-3.65.json), [second attempt](docs/hardware/rsp-network-reset-retry-2-3.65.json) | The second exact-candidate attempt deployed and matched installed identity, but the ACK sentinel peer closed after `qOffsets`, before reset injection; the no-ack sentinel and remaining matrix did not run |
| User-mode profiler | [13-check record](docs/hardware/profiler-name-dictionary-3.65.json) | Event order, zones, counters, snapshots, bounded pressure, encoding, names, and drop accounting passed without the kernel plugin |
| Profiler TCP | [Transport record](docs/hardware/profiler-tcp-stream-retail-3.65.md) | Named capture, clean EOF, cancellation, forced disconnect, and recovery relaunch passed |
| PMU mutation boundary | [Per-core record](docs/hardware/profiler-pmu-session-gate-3.65.md) | Fixed lane-5 software-increment transaction and exact snapshot restoration passed on application cores 0-2 |
| PMU real events | [`0x01`](kernel/pmu-profiler-gate/hardware-results/2026-09-15-event-01/README.md), [`0x03`](kernel/pmu-profiler-gate/hardware-results/2026-09-15-event-03/README.md), [`0x10`](kernel/pmu-profiler-gate/hardware-results/2026-09-15-event-10/README.md) | One bounded normal-close sample per event passed with exact restoration |
| PMU safe re-arm | [Dormant-thread record](kernel/pmu-profiler-thread-exit-gate/hardware-results/2026-09-15-first-attempt/README.md) | Same-process, same-boot re-arm after the original controller thread terminated passed; broader terminal paths remain pending |
| IDE workflow | [Direct-TCP F5 record](docs/hardware/vscode-debug-demo-direct-tcp-3.65.json) | Build, signed deploy, launch, build identity, ASLR symbols, source break, mutation, resume, detach, and cleanup passed after one recoverable launch retry |

Read-only debug-resource discovery reported breakpoint/watchpoint comparators,
but discovery is not usable breakpoint support. Guarded retail probes rebooted
before a safely returning `DBGVCR` path and never accessed or enabled a
comparator. The detailed record is
[Hardware breakpoint enablement research](docs/hardware/dipsw-228-hw-debug.md).

## Important lifecycle limits

- `uvdb_shutdown()` is terminal. Use
  `uvdb_stop_server()`/`uvdb_start_server()` for ordinary reconnect cycles.
- Kernel all-stop requires an exact matching companion; the user library fails
  closed on an ABI/capability mismatch.
- GDB software stepping intentionally rejects unsupported execution states,
  uncertain PC writers, waits/syscalls, and uncovered action sets.
- Foreign-thread core and VFP writes are disabled. Core/CPSR writes are limited
  to the selected exception thread; VFP writes are unsupported.
- GDB console forwarding and DebugNet logging are bounded and can lose data.
- Kernel plugins cannot be debugged with this application-side stub.
- The exact installed VPK and retained unstripped ELF must match or symbols may
  be wrong. The verified-build workflow checks equality, not publisher
  authenticity.
- Deterministic host stress exercises 1,000 reconnect/shutdown generations per
  run while three protocol/fault workers and a console-pressure producer race
  the production gates. A separate production-translation-unit regression
  repeats 100 ACK/no-ack stopped-peer resets and proves listener generation
  recovery. A 2026-09-20 retail run found that a raw blocking receive did not
  wake after `SO_LINGER` RST; packet I/O now polls nonblocking with exact socket-
  generation cancellation. A retry from exact commit `f6bacc8` stopped before
  package staging because VitaDevDeploy did not publish a fresh challenge; no
  RSP case ran. A second authorized attempt deployed the same frozen artifact
  and matched its installed eboot, but the ACK sentinel peer closed immediately
  after `qOffsets`, before fixture access or reset injection. Both attempts
  cleaned up to LiveArea with port 1234 closed. A diagnostic-only retail build
  then captured the first empty raw receive as `-35`
  (`-SCE_NET_EAGAIN`) during response-ACK polling, while the target was stopped
  and the connection was still owned. That diagnostic build accepted only the
  distinct encoded `SCE_NET_ERROR_EAGAIN`, proving the lifecycle failure was a
  would-block representation mismatch. The local follow-up accepts exactly
  encoded `SCE_NET_ERROR_EAGAIN` and raw negative `SCE_NET_EAGAIN`/
  `SCE_NET_EWOULDBLOCK`; generic `-1` and unrelated negatives remain fatal.
  The first exact-fix retail confirmation deployed and matched the installed
  eboot, but hard-stopped before exercising the fix: TCP accepted the ACK
  sentinel and the client fully sent valid `qSupported`, yet the target sent
  neither the RSP ACK nor a response within 15 seconds. The title cleaned up
  normally. Hardware confirmation of the fix and the stopped-RST plus long-
  duration-equivalent cancellation/soak matrix therefore remain pending. A
  compile-time-only admission diagnostic now records a fixed-size ledger from
  listener readiness through candidate accept, valid-frame peek, promotion,
  protocol ownership, stopped-state entry, and first packet receive. The Vita
  test title exposes the snapshot on a separate bounded UDP query port, so a
  wedged admitted RSP socket cannot hide the transition boundary. Production-
  TU tests cover immediate and delayed first packets plus disconnect during
  promotion without consuming the peeked frame or leaking ownership. Candidate
  withdrawal and connected descriptor/generation publication are mirrored in
  one snapshot transaction. The bounded host runner requires a numeric IPv4
  literal and tolerates individual UDP telemetry misses until the outer
  readiness or detach deadline; malformed snapshots still fail immediately.
  The corrected diagnostic then passed on retail 3.65: explicit readiness,
  `qSupported`, `qOffsets`, the complete ordered admission/ownership ledger,
  clean detach, and both ACK/no-ack stopped-reset recovery sentinels all passed.
  Listener replacement completed in 1.84 and 2.33 seconds. The subsequent
  lifecycle matrix passed the baseline plus `m` and `M` disconnect invariants,
  then stopped fail-closed at `g` because the frozen runner rejected the
  protocol's valid `xx` unavailable-register markers as nonhex. No later case
  ran. The target's 336-character reply had 136 hexadecimal characters and 200
  `x` markers with no other characters, so this is a runner validation defect,
  not evidence of target corruption. See
  [the retail record](docs/hardware/rsp-admission-confirmation-3.65.json).
  The local follow-up now validates the exact 336-character legacy `g` shape:
  core registers and CPSR must remain hexadecimal, while each byte in the
  legacy FPA region may be either two hexadecimal nibbles or one complete
  lowercase `xx` unavailable marker. Lone, mixed, misplaced, malformed, short,
  and long forms remain rejected. The uncompleted matrix is still
  hardware-unconfirmed. A bounded completion attempt reused the exact installed
  target and first passed explicit readiness, `qSupported`, `qOffsets`, the
  full admission ledger, and detach. The immediately following matrix baseline
  connected and completely sent another valid `qSupported`, but received no
  ACK or response for 15 seconds. It stopped before `disconnect-g`, so the
  corrected parser was not exercised on hardware and no later case ran. See
  [the completion retry record](docs/hardware/rsp-network-completion-retry-3.65.json).
  Diagnostic ABI v2 now tags every retained transition with its connection
  epoch and result, records exception/protocol rejection, protocol release,
  detach normalization, and listener reopen, and publishes the active owner's
  epoch. A nonblocking per-event writer claim prevents concurrent diagnostic
  writers from corrupting a retained epoch; any dropped contended write is
  explicit and fails the gate. The lifecycle runner accepts only a canonical
  numeric IPv4 target and captures the expected epoch over UDP from inside the
  failing RSP request, before caller cleanup can close the connected socket.
  Expected, stale, advanced, malformed, unavailable, and locally failed
  telemetry outcomes remain distinct. A dedicated `second-admission` runner
  phase performs exactly two `qSupported`/`qOffsets` sessions separated by a
  clean detach and cannot continue into disconnect, shutdown, or soak cases.
  Connect failures also trigger bounded telemetry retrieval. The first ABI v2
  retail attempt stopped before RSP negotiation when no UDP-ready snapshot
  arrived within 20 seconds. Its installed target identity was exact, but the
  post-launch identity check had already opened TCP 1234, violating the
  required UDP-ready-before-TCP ordering; the attempt is therefore a
  procedural diagnostic failure, not target evidence. Subsequent runs must
  verify the installed eboot without probing TCP 1234. The diagnostic CLI's
  `--ready-only` mode uses UDP exclusively and accepts only a pristine epoch-0
  snapshot with no candidate, connection, owner, stopped state, or dropped
  event writes. UDP transport records each timeout, transport error, or
  unexpected peer. See
  [the attempt record](docs/hardware/rsp-second-admission-attempt-1-3.65.json).
  A corrected, uncontaminated attempt then passed pristine epoch-0 readiness
  and two consecutive `qSupported`/`qOffsets`/detach sessions, ending at epoch
  2 with no rejected ownership, dropped event, stale socket, or listener-reopen
  failure. This rejects the suspected deterministic detach-to-immediate-
  reconnect ownership bug. The remaining matrix could not start: one reused
  launch and one fresh signed reinstall both produced 40 consecutive UDP-ready
  timeouts before any TCP contact. Further identical retries are therefore
  blocked pending separate startup/readiness telemetry. A compile-time-only,
  two-slot persistent journal retrieved through Companion FTP then proved that
  a fresh exact build reached `test_ready` and `main_loop` with no failed
  initialization stage, while UDP 1235 still returned no packet for 20
  seconds. TCP 1234 was not contacted and the run stopped cleanly. This narrows
  the unexplained boundary to the admission receive path. A follow-up retained
  100 live polls on descriptor 4, each returning `-1`/errno 11, while none of
  40 valid host datagrams reached parsing and no response was attempted.
  Effective-bind, route-address, and on-device self-datagram telemetry remain
  necessary before a local fix is justified. See
  [the confirmation record](docs/hardware/rsp-second-admission-confirmation-3.65.json)
  [the startup diagnostic record](docs/hardware/rsp-startup-diagnostic-3.65.json),
  and [the poll diagnostic record](docs/hardware/rsp-poll-diagnostic-3.65.json).

## Documentation map

- [Debugger core hardening](docs/debugger-core-hardening.md)
- [GDB thread-control validation](docs/gdb-thread-control-validation.md)
- [GDB register access](docs/gdb-register-access.md)
- [GDB stepping gate](docs/gdb-step-register-gate.md)
- [ASLR-aware symbols](docs/gdb-symbol-loading.md)
- [GDB monitor commands](docs/gdb-monitor-commands.md)
- [GDB console transport](docs/gdb-console-transport.md)
- [Profiler guide](profiler/README.md)
- [Profiler binary trace](profiler/docs/binary-trace.md)
- [Profiler TCP stream](profiler/docs/vita-tcp-stream.md)
- [PMU failure matrix](profiler/docs/pmu-failure-matrix.md)
- [External-attach scaffold](attach/README.md)
- [VitaDevDeploy guide](deploy/README.md)
- [VS Code demo](examples/vscode-debug-demo/README.md)

## Repository layout

- `uvdb.h`, `src/`: application debugger API and implementation.
- `kernel/`: narrow kernel companion, import stubs, probes, and PMU gates.
- `profiler/`: profiler library, Vita adapters, TCP receiver, and trace tools.
- `tools/`: GDB lifecycle, symbol, console, monitor, and log helpers.
- `tests/`: host regression suites and Vita fixtures.
- `examples/vscode-debug-demo/`: isolated build/deploy/debug sample.
- `attach/`: read-only external-attach protocol/broker scaffold; no live
  attach implementation.
- `deploy/`: signed deployment subproject with its own license and notices.

## Attribution and licensing

VitaDebugger derives from
[sleirsgoevy/vita-uvdb](https://github.com/sleirsgoevy/vita-uvdb). Kubridge and
[DebugNet](https://github.com/psxdev/debugnet) are separate projects maintained
by their respective authors. DebugNet is a protocol/workflow reference;
VitaDebugger's bounded sender is a separate implementation and is not
API/ABI-compatible with `libdebugnet`.

`deploy/` is a self-contained GPL-3.0-only subproject governed by
[`deploy/LICENSE`](deploy/LICENSE) and
[`deploy/THIRD_PARTY.md`](deploy/THIRD_PARTY.md). That license does not select
a license for files outside the deployment subproject. A project-wide license
has not been selected, so do not assume redistribution rights merely because
the source is public. Review every dependency's license before redistribution;
an explicit project license is still required before a formal release.

The VFP work used VitaSDK's public header/NID declarations and a pinned review
of `cerwym/kvdb` commit
`88742b760afa18cd11e251160d4c3b85357c30f1` only as a research pointer to an
undocumented kernel call. No KVDB source was copied; its root repository does
not currently contain an explicit `LICENSE` file.

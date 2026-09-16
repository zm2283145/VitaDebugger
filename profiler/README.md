# libvitaprofiler foundation

This directory contains the first standalone, user-mode-safe profiling layer for
VitaDebugger. It is deliberately separate from the GDB stub: stopping in a
debugger and compiling without optimization both distort the timing that a
profiler is meant to measure.

The current increment provides:

- Named CPU/wall-time zones using the Vita monotonic microsecond clock.
- Signed counters for application, renderer, audio, and asset-pipeline metrics.
- Frame markers with measured frame-to-frame duration and a monotonically
  increasing frame ID.
- User-mode snapshots of free USER, CDRAM, and PHYCONT memory.
- User-mode snapshots for a known application thread: cumulative `runClocks`,
  free stack, thread preemptions, and interrupt preemptions.
- A caller-owned, fixed-capacity event ring with multi-producer/single-consumer
  operation and explicit drop accounting.
- A versioned 32-byte stream header and 32-byte event encoding with fixed-width
  fields and explicit little-endian serialization.
- A bounded caller-owned name dictionary, automatic names for the eight Vita
  metrics, and a separately versioned little-endian dictionary block for trace
  receivers. The event wire ABI remains version 1.
- Allocation-free validation and decoding for complete `VPRF` event streams.
- A callback-based drain that writes one combined `VPNM` + `VPRF` binary
  capture and permanently fails on ambiguous sink errors.
- An opt-in, caller-owned Vita TCP sink with bounded nonblocking SceNet
  connect/send behavior, partial-send handling, explicit cleanup obligations,
  and transport statistics.
- A bounded TCP receiver plus text, complete decoded JSON, and Chrome Trace/Perfetto
  viewer output with named zone, counter, frame, and built-in metric handling.
- Cooperative, header-independent CPU wall-time hook interfaces for VitaGL and
  SceGxm call sites.
- A guarded PMU provider/lease abstraction plus an injectable owned-reset
  adapter for the public ScePerf semantics. The adapter retains failed cleanup
  obligations. Its direct Vita convenience initializer now fails closed
  because retail 3.65 hardware proved that the ScePerf imports remain unbound.
- A hardware-tested, read-only Cortex-A9 PMU inventory in optional
  VitaDebugger kernel ABI v1.13. A separate disposable session gate passed one
  fixed lane-5 software-increment transaction with exact gate-snapshot
  restoration on all
  three application cores of the tested retail 3.65 Vita. Production counter
  configuration remains disabled. A reviewed bridge and separately versioned,
  default-off user/kernel transport now adapt this kernel session to the
  exact-restore provider ABI with fixed core 0/lane 5, an owner-bound lease,
  watchdog, timeout recovery, and orphan cleanup. The transport and disposable
  Vita gate pass host/fake-kernel tests and Vita cross-builds. Its ordinary
  experimental build accepts only software increment; a second compile gate
  plus request acknowledgement admits only `0x01`, `0x03`, and `0x10`. Cycles
  remain rejected and the normal kernel build stays unchanged. The first
  durable hardware gates passed `0x01`, `0x03`, and `0x10` on retail 3.65 with
  valid samples and exact close/restore; recovery/lifecycle gates remain.

The portable core does not start threads, allocate memory, open files, use the
network, stop an application, or currently call the VitaDebugger kernel
companion. The optional TCP adapter creates a socket only when the application
calls it; it never initializes or terminates SceNet. The separate
development-computer tool owns its TCP listener and output files. PMU discovery
is an optional companion capability, not a hidden dependency of the portable
profiler core.

## Quick start

```c
#include <vitaprofiler.h>

static struct vp_context profiler;
static struct vp_slot profiler_slots[1024]; /* 40 KiB */
static struct vp_name_dictionary profiler_names;
static struct vp_name_entry profiler_name_entries[128];
static char profiler_name_text[4096];

static uint32_t frame_id;
static uint32_t update_id;
static uint32_t draw_calls_id;

void profiler_start(void)
{
    struct vp_name_dictionary_config names = {
        .entries = profiler_name_entries,
        .entry_capacity = 128,
        .text = profiler_name_text,
        .text_capacity = sizeof(profiler_name_text),
    };

    if (vp_vita_init(&profiler, profiler_slots, 1024) != VP_RESULT_OK)
        return;

    /* Registration copies text into bounded caller-owned storage. Perform it
       once before worker threads start, then seal the immutable dictionary. */
    if (vp_name_dictionary_init(&profiler_names, &names) != VP_RESULT_OK ||
        vp_name_dictionary_register(&profiler_names, "main frame",
                                    &frame_id) != VP_RESULT_OK ||
        vp_name_dictionary_register(&profiler_names, "update", &update_id) !=
            VP_RESULT_OK ||
        vp_name_dictionary_register(&profiler_names, "draw calls",
                                    &draw_calls_id) != VP_RESULT_OK ||
        vp_name_dictionary_seal(&profiler_names) != VP_RESULT_OK)
        return;
}

void run_one_frame(void)
{
    struct vp_zone_scope update;

    vp_frame_mark(&profiler, frame_id);
    if (vp_zone_begin(&profiler, update_id, &update) == VP_RESULT_OK) {
        update_game();
        vp_zone_end(&profiler, &update);
    } else {
        update_game();
    }

    vp_counter(&profiler, draw_calls_id, draw_call_count);
}
```

Register every name that a trace receiver should display; cached IDs avoid
hashing strings in hot paths.
See [Profiler name dictionary](docs/name-dictionary.md) for complete lifecycle,
wire-format, receiver, collision, and capacity details.

See [Binary trace pipeline](docs/binary-trace.md) for the stream writer, TCP
receiver, validation rules, summaries, and Perfetto export, and [Vita TCP
stream sink](docs/vita-tcp-stream.md) for the opt-in application-side SceNet
adapter. The separate [Vita TCP hardware gate](docs/vita-tcp-hardware-gate.md)
validates that path from an ordinary user-mode app without the kernel plugin.
See [Cooperative graphics hooks](docs/graphics-hooks.md) for safe
VitaGL/SceGxm call-site instrumentation, the [real-world VitaGL integration
record](../docs/hardware/profiler-real-world-vitagl-2026-09-15.md) for a
default-off application example, and [Guarded CPU/PMU provider
boundary](docs/pmu-provider.md) for the explicit exact-restore versus
application-owned counter lifecycle.

Call `vp_vita_record_memory()` at a low frequency, such as once per second, not
once per draw. Call `vp_vita_record_thread()` with `0` for the current thread or
with an application-owned thread ID that is already known. Those VitaSDK
queries are more expensive than an ordinary counter or zone event.

Each Vita record helper captures one snapshot and then publishes its metrics as
several independent ring records. That publication is non-transactional: if the
ring fills partway through, a prefix may already be queued when the helper
returns `VP_RESULT_DROPPED`. Do not blindly retry the helper, because that would
duplicate the accepted prefix. Atomic batch reservation and an explicit
snapshot correlation ID are future format work.

A drain thread or an existing application control thread can copy complete
events without stopping producers:

```c
struct vp_event batch[64];
size_t count = vp_drain(&profiler, batch, 64);
```

An exporter can encode each record with `vp_encode_event_le()` and send batches
through a binary telemetry channel, a file, or a desktop trace viewer without
changing instrumented game code. Export the sealed name dictionary with
`vp_encode_name_dictionary_le()` so that receiver can label each numeric ID.
`vitaprofiler_stream.h` packages that sequence into a checked single-consumer
writer when a complete-buffer sink callback is available.

## Concurrency and overload behavior

The event ring is bounded and never waits for space:

- Any number of producer threads may call `vp_record()`, `vp_emit()`,
  `vp_counter()`, `vp_zone_begin()`, and `vp_zone_end()` concurrently.
- Custom `vp_clock_fn` and `vp_thread_id_fn` callbacks must therefore be
  reentrant and concurrency-safe. The clock must return monotonic microseconds;
  the thread callback must identify whichever producer is calling it.
- Exactly one consumer may call `vp_drain()` for a context.
- `vp_frame_mark()` must be called by one designated frame thread because its
  previous-frame timestamp and frame sequence are intentionally non-atomic.
- `vp_init()` and `vp_deinit()` require all producers and the consumer to be
  stopped. A context must not be reinitialized while it is live.
- Do not asynchronously terminate, cancel, or suspend-and-destroy a producer
  inside a record call. A producer reserves its slot before publishing it, so
  killing it in that interval can leave an unpublished gap that prevents the
  single consumer from advancing. Let calls return before shutting down a
  producer.
- When the ring is full, the producer returns `VP_RESULT_DROPPED`, increments
  the drop counter, and continues immediately. Existing unread data is never
  overwritten.
- A dropped zone-begin event leaves its scope inactive, so the caller should
  omit `vp_zone_end()` for that scope as shown above. If a zone-end event is
  dropped, the exported trace can see the global drop count and treat the
  unmatched begin as incomplete.
- `accepted` and `dropped` are wrapping 32-bit lifetime counters. `pending` is a
  bounded instantaneous observation and may be briefly approximate while a
  producer is publishing a reserved slot.

The queue uses 32-bit atomic operations only. At capacity 1024, the context has
40 KiB of slot storage. There is no heap allocation during initialization or
recording.

## Event contract

Each `vp_event` is exactly 32 bytes:

| Field | Meaning |
| --- | --- |
| `timestamp_us` | Monotonic event timestamp in microseconds |
| `value` | Signed value; zone/frame end events use elapsed microseconds |
| `name_id` | Stable FNV-1a ID or a reserved built-in metric ID |
| `thread_id` | Producer or sampled Vita thread ID |
| `correlation_id` | Matching zone ID or frame sequence |
| `type` | Zone, counter, frame, memory, process, thread, or custom event |
| `flags` | First-frame, thread mismatch, clock regression, or raw-value flags |

Never write native structs directly to a stream. Prefix an export with the
header produced by `vp_wire_header_init()` and encode it and every event with
the little-endian encoder functions. This keeps padding and host byte order out
of the file/network ABI. The header records wire version 1, 32-byte header and
event sizes, and a 1 MHz timestamp frequency.

Names are not copied into the hot ring. `vp_name_id()` returns stable 32-bit
FNV-1a IDs; callers should cache them. The name dictionary now provides the
ID-to-name table for the viewer, rejects collisions during registration, and
validates IDs again when a receiver opens the encoded block. IDs from
`0xfff00001` through `0xfff00008` are reserved and automatically resolve to the
built-in Vita sample names.

## What works without the kernel companion

| Capability | User library | Optional kernel work |
| --- | --- | --- |
| Named timing zones and counters | Implemented | None |
| Frame time/FPS calculation | Implemented through frame markers | None |
| Vita free-memory snapshots | Implemented with public VitaSDK calls | None |
| Known-thread cumulative stats | Implemented with a supplied/current thread ID | None |
| Renderer/audio/allocator metrics | Generic counters and zones are ready | Integration hooks in each subsystem |
| All-process thread enumeration | Cooperative/known IDs only | Narrow process-owned enumeration |
| Statistical PC/call-stack sampling | Not safely available | Tokenized, read-only sampler boundary |
| PMU hardware counters | Provider boundary, named raw samples, and injected owned-reset adapter tests; direct ScePerf initialization fails closed on tested retail 3.65 | Read-only inventory and the isolated lane-5 software-increment transaction pass on application cores 0-2. The default-off transport passed bounded core-0 `0x01`, `0x03`, and `0x10` samples with exact restoration, plus same-boot re-arm after its owning worker exited. Process-exit/crash, disconnect, timeout, and ownership-conflict gates remain |
| GPU workload timing | Explicit CPU-side VitaGL/SceGxm call-site hooks | GPU timestamps or automatic interposition are not implemented |

`SceKernelThreadInfo.runClocks` is exported as a cumulative **raw** value because
the public header's wording and behavior need hardware characterization before
the viewer assigns a stronger unit or CPU-utilization meaning. Deltas are still
useful for controlled experiments. This library does not enumerate arbitrary
threads or sample their program counters.

There is no generic user-mode call that reveals every GPU command's execution
time. Useful GPU profiling will require narrow hooks around VitaGL/SceGxm
submission, waits, buffer swaps, allocations, shader changes, and draw calls.
Those hooks can emit into this same ring without changing its core.

## Build and verify

On Windows, run the Vita targets from an MSYS2 shell or put the MSYS2
`mingw64\bin` and `usr\bin` directories plus the selected VitaSDK `bin`
directory on `PATH`. Invoking `arm-vita-eabi-gcc.exe` by full path alone is not
sufficient because its `cc1.exe` child must also load the MinGW runtime DLLs.

From this directory on a normal VitaSDK shell:

```sh
make vita-check
```

This compiles both the portable core and the user-mode Vita adapter with
`-Wall -Wextra -Werror`, then creates `build/vita/libvitaprofiler.a`. Link that
archive into an application together with the VitaSDK stubs for KernelThreadMgr,
LibKernel, Processmgr, and Sysmem (ordinary VitaSDK application links normally
already include LibKernel). Applications opting into the TCP sink additionally
link `SceNet_stub` and keep the SceNet module/global lifetime caller-owned.
`vita-check` cross-builds the fail-closed adapter;
the separate zero-call `vita-pmu-discovery` target includes VitaSDK's
`psp2/perf.h` and links the six adapter-used imports as a loader diagnostic.
Neither build proves that those functions are bound at runtime: both documented
sysmodule loading and the normal `libperf.suprx` user-module path failed on the
tested retail 3.65 setup. `vp_vita_pmu_owned_init()` therefore returns
`VP_ERROR_UNSUPPORTED` instead of calling those unresolved stubs. See the
[user-mode PMU hardware gate](docs/pmu-hardware-gate.md), the
[kernel discovery record](../docs/hardware/profiler-pmu-retail-3.65.md), and the
[isolated PMU session result](../docs/hardware/profiler-pmu-session-gate-3.65.md).
The separately built real-event candidate is governed by the
[PMU profiler transport gate runbook](../kernel/pmu-profiler-gate/README.md).

On a Unix-like development host with a native C compiler:

```sh
make host-test
```

The native tests check validation, bounded drop/reuse behavior, FIFO ordering,
zone/frame semantics, exact event and dictionary wire bytes, dictionary
collisions/capacity/truncation, concurrent multi-producer delivery, concurrent
read-only name resolution after sealing, combined stream drain/decoding,
fail-closed sink behavior, bounded/partial Vita TCP transport behavior, PMU
ownership/restoration policy, public-ScePerf
adapter sequencing and cleanup, and graphics hook emission.
The Python suite adds corrupt/truncated capture rejection, fragmented loopback
TCP reception, byte bounds, named zone analysis, and JSON/Perfetto output.

From a Visual Studio Developer Command Prompt on Windows:

```bat
if not exist build\host mkdir build\host
cl /nologo /std:c11 /O2 /W4 /WX /Iinclude src\vitaprofiler.c tests\test_vitaprofiler.c /Febuild\host\test_vitaprofiler.exe
build\host\test_vitaprofiler.exe
cl /nologo /std:c11 /O2 /W4 /WX /Iinclude src\vitaprofiler.c src\vitaprofiler_names.c tests\test_vitaprofiler_names.c /Febuild\host\test_vitaprofiler_names.exe
build\host\test_vitaprofiler_names.exe
```

That abbreviated MSVC example covers only the portable core and name table.
Run `make host-test` in the validated VitaSDK/MSYS2 environment for the complete
gate: all five native suites, the C-writer-to-Python-reader wire fixture, and
the Python receiver/viewer tests.

The native suite tests the ScePerf adapter's sequencing through injected calls;
actual Vita system calls exist only on hardware. `make vita-check` cross-builds
the implementation and links a small import check against `ScePerf_stub` and
`SceKernelThreadMgr_stub`.

## Vita user-mode self-test

Build the separate diagnostic application with:

```sh
make vita-probe
```

The resulting `build/vita-probe/vitaprofiler-probe.vpk` is an ordinary
user-mode application with title ID `VDPR00001`. It does not call or require
the VitaDebugger kernel plugin. On real hardware it displays PASS/FAIL checks
for timing zones, frame markers, counters, memory and current-thread snapshots,
exact event and name-dictionary encoding, live ID-to-name resolution,
concurrent multi-producer pressure/drop accounting, and ring reuse. The result
remains on screen for five minutes before the diagnostic exits normally.

The original retail 3.65 run passed all 11 core checks, including four
concurrent producers accepting the 64-slot capacity, accounting for all 448
excess events as drops, draining unique complete records, and reusing a
drained slot. See the [original unedited result
screenshot](../docs/hardware/profiler-user-mode-probe-v1.jpg).

The dictionary-enabled probe subsequently passed all 13 checks on retail 3.65.
The two added gates prove bounded dictionary encoding and live resolution of
every captured event ID, including custom and built-in names. See the [unedited
13-check result
screenshot](../docs/hardware/profiler-name-dictionary-3.65.jpg).

The same 13-check probe was rebuilt from the TCP-sink integration worktree and
passed again on retail 3.65 on 2026-09-15, without changing or invoking the
kernel plugin. The artifact hashes and signed-deployment result are recorded in
the [revalidation journal](../docs/hardware/profiler-user-mode-revalidation-2026-09-15.md).

The production Vita TCP sink and PC receiver subsequently passed a live
retail-3.65 gate, including named-event decoding, clean EOF, pre-connect cancel,
forced peer disconnect, and a successful recovery relaunch. See the
[TCP hardware journal](../docs/hardware/profiler-tcp-stream-retail-3.65.md) and
its retained raw captures for exact results.

The same transport then completed two clean 300-frame captures from a
graphics-heavy VitaGL-based 3D homebrew application. Both recorded zero
ring/transport loss. The follow-up resolved 25 stable names, balanced 2,321
scope pairs, and recorded 75 bounded event-`0x01` samples before a clean
exact-restoring close. See the
[real-world integration record](../docs/hardware/profiler-real-world-vitagl-2026-09-15.md).

The probe link reserves `__sce_headroom=0x1000`. This uses the VitaSDK linker
script's supported SCE-metadata headroom mechanism and avoids a Windows
`vita-elf-create` heap-corruption bug when an optimized read-only segment ends
too close to the next 64 KiB boundary. It does not change the profiler library
or require an unoptimized build.

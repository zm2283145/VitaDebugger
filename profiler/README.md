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

It does not start threads, allocate memory, open files, use the network, stop an
application, or call the VitaDebugger kernel companion. No kernel plugin change
is part of this foundation.

## Quick start

```c
#include <vitaprofiler.h>

static struct vp_context profiler;
static struct vp_slot profiler_slots[1024]; /* 40 KiB */

static uint32_t frame_id;
static uint32_t update_id;

void profiler_start(void)
{
    if (vp_vita_init(&profiler, profiler_slots, 1024) != VP_RESULT_OK)
        return;

    /* Hash names once, outside hot paths. The future viewer will use the same
       stable IDs and a name table supplied by the application/build. */
    frame_id = vp_name_id("main frame");
    update_id = vp_name_id("update");
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

    vp_counter(&profiler, vp_name_id("draw calls"), draw_call_count);
}
```

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

The current foundation intentionally stops at this boundary. A later transport
can encode each record with `vp_encode_event_le()` and send batches through a
binary telemetry channel, a file, or a desktop trace viewer without changing
instrumented game code.

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
FNV-1a IDs; callers should cache them and eventually provide an ID-to-name table
to the viewer. Hash collisions are possible and must be rejected when such a
table is built. IDs from `0xfff00001` through `0xfff00008` are reserved for the
built-in Vita samples.

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
| PMU hardware counters | Not used | Per-core save/restore and ownership design |
| GPU workload timing | App submission/wait zones only | VitaGL/SceGxm hooks; not inherently kernel work |

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
already include LibKernel).

On a Unix-like development host with a native C compiler:

```sh
make host-test
```

The native test checks validation, bounded drop/reuse behavior, FIFO ordering,
zone/frame semantics, exact wire bytes, and concurrent multi-producer delivery.

From a Visual Studio Developer Command Prompt on Windows:

```bat
if not exist build\host mkdir build\host
cl /nologo /std:c11 /O2 /W4 /WX /Iinclude src\vitaprofiler.c tests\test_vitaprofiler.c /Febuild\host\test_vitaprofiler.exe
build\host\test_vitaprofiler.exe
```

The Vita adapter is intentionally excluded from the native executable because
its public system calls exist only on Vita. The Vita cross-build is the compile
gate for that file.

## Vita user-mode self-test

Build the separate diagnostic application with:

```sh
make vita-probe
```

The resulting `build/vita-probe/vitaprofiler-probe.vpk` is an ordinary
user-mode application with title ID `VDPR00001`. It does not call or require
the VitaDebugger kernel plugin. On real hardware it displays PASS/FAIL checks
for timing zones, frame markers, counters, memory and current-thread snapshots,
exact wire encoding, concurrent multi-producer pressure/drop accounting, and
ring reuse. The result remains on screen for five minutes before the diagnostic
exits normally.

The first run on the project's retail Vita running system software 3.65 passed
all 11 checks, including four concurrent producers accepting the 64-slot
capacity, accounting for all 448 excess events as drops, draining unique
complete records, and reusing a drained slot. The app
then exited normally after its five-minute result display. See the [unedited
result screenshot](../docs/hardware/profiler-user-mode-probe-v1.jpg).

The probe link reserves `__sce_headroom=0x1000`. This uses the VitaSDK linker
script's supported SCE-metadata headroom mechanism and avoids a Windows
`vita-elf-create` heap-corruption bug when an optimized read-only segment ends
too close to the next 64 KiB boundary. It does not change the profiler library
or require an unoptimized build.

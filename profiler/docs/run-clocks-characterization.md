# `SceKernelThreadInfo.runClocks` characterization

## Current conclusion

The VitaSDK headers establish that `SceKernelThreadInfo.runClocks` uses
`SceKernelSysClock`, whose storage type is `SceUInt64` and therefore
`uint64_t`. The field comment says only "Number of clock cycles run." Those
headers do not establish the increment unit, effective counter/wrap width,
update rules, core affinity behavior, suspend behavior, reset behavior, or a
conversion to CPU utilization.

The exact retained samples and the limits of what they prove are recorded in
the [hardware evidence baseline](../../docs/hardware/profiler-runclocks-baseline-2026-09-18.md).

Pinned public sources:

- [VitaSDK `SceKernelSysClock` definition (`SceUInt64`)](https://github.com/vitasdk/vita-headers/blob/a4e9692fb7b4de1e8d0bb5609af63c01be0b7396/include/psp2common/types.h#L96-L97)
- [VitaSDK `runClocks` field and its complete comment](https://github.com/vitasdk/vita-headers/blob/38938d4018da820ba3e7207383bf7027ec547b0a/include/psp2common/kernel/threadmgr.h#L123-L124)
- [Vita newlib's incomplete `getrusage()` implementation](https://github.com/vitasdk/newlib/blob/6ddc88b2ca316e43830fe59ea5efdceea39f8f47/newlib/libc/sys/vita/resource.c#L105-L125)

The newlib code divides `runClocks` by 1,000,000 while filling a `timeval`, but
it is implementation evidence, not a platform contract: it uses assignment in
the `who` condition, computes `tv_usec` without a modulo, then unconditionally
sets `EINVAL` and returns `-1`. VitaProfiler therefore does not adopt that
conversion.

VitaProfiler therefore preserves the field as a cumulative raw unsigned value.
The binary record carries the original 64 bits in the signed wire `value`
field, sets `VP_EVENT_FLAG_RAW_VALUE`, and uses the built-in name
`vita.thread.run_clocks`. The host tool reconstructs those bits as
`raw_value_u64`. It reports deltas only in `raw` units and never divides them by
elapsed time to produce a percentage or known time unit.

## Bounded capture

Use a dedicated application-owned measurement thread. Record its thread ID at
creation and assign a generation number that increments every time the
application creates a new measurement thread, even if Vita later reuses the
same numeric ID. Do not infer object identity from `SceUID` reuse.

For one trial:

1. Fix the Vita model, firmware, title build, clock profile, power state, and
   workload. Record all of them in the metadata file below.
2. Choose a positive sample interval, a sample-count limit, and a total
   duration limit before starting. Use at least two samples. Keep the interval
   low-frequency enough that `sceKernelGetThreadInfo()` does not materially
   change the workload under test.
3. Start one fresh `.vptrace`. On a control thread, call
   `vp_vita_record_thread()` for the known measurement-thread ID at the chosen
   interval. Drain concurrently so the bounded ring does not fill. Stop when
   either predeclared bound is reached; record producer drops and reject a
   trial with loss.
4. End the stream cleanly. Preserve the raw `.vptrace`; do not concatenate
   launches or reconnect into an existing stream.
5. Identify each measurement-thread generation by inclusive event-index range
   in the metadata. A generation boundary deliberately suppresses a delta,
   including when the next generation has the same numeric thread ID.
6. Generate the report:

   ```sh
   python tools/vitaprofiler_trace.py runclocks \
     trial.vptrace trial.runclocks-experiment.json trial.runclocks.json
   ```

The receiver retains its 16-MiB and 262,144-event defaults. The characterization
command additionally rejects fewer than two raw samples, samples outside the
declared thread generations, sample counts above the declared limit, capture
spans above the declared duration, timestamp regressions, and missing
`raw_value` flags. The report includes the source capture's SHA-256 digest,
wire version, actual sample span and interval range, normalized metadata, every
raw value, and every accepted raw delta. Raw values and deltas include decimal
and hexadecimal strings so values above JavaScript's exact integer range remain
lossless in JSON consumers.

## Experiment metadata

Metadata is UTF-8 JSON, limited to 64 KiB. All fields shown below are required
except `notes`; use an explicit `"unknown"` string rather than omitting a
condition that was not measured.

```json
{
  "format": "vitaprofiler-runclocks-experiment-v1",
  "experiment_id": "pch2000-365-idle-001",
  "captured_at_utc": "2026-09-18T05:00:00Z",
  "device_model": "PCH-2000",
  "firmware": "3.65",
  "title_id": "VDPR00001",
  "build_id": "git-ee6c799+instrumentation-build",
  "workload": "measurement worker blocked in sceKernelDelayThread",
  "clock_profile": "application default; unchanged",
  "power_state": "AC attached; battery 100%",
  "sample_interval_us": 10000,
  "sample_count_limit": 1000,
  "capture_duration_limit_us": 12000000,
  "producer_dropped_events": 0,
  "sink_lost_events": 0,
  "threads": [
    {
      "thread_id": "0x40010003",
      "generation": 0,
      "label": "measurement-worker-0",
      "first_event_index": 120,
      "last_event_index": 4116
    }
  ],
  "counter_bits": null,
  "max_wrap_delta_raw": null,
  "notes": "cold launch; radios unchanged"
}
```

The 64-bit source storage does not prove that all 64 bits participate in the
hardware/software counter. `counter_bits` describes the experimentally
established effective wrap width and must remain `null` until hardware evidence
establishes it. With unknown effective width, every decrease is
`reset_or_reuse`, produces no delta, starts a new inferred generation when no
explicit generation covers it, and increments the counter epoch. This prevents
a reset or reused thread ID from becoming a huge synthetic delta.
Wire-v2 thread-generation declarations provide these explicit boundaries when
no run-clocks experiment supplies an overriding thread-range map. The first
sample in each supplied generation intentionally has no cross-generation
delta.

The strict report rejects nonzero `producer_dropped_events` or
`sink_lost_events`. Read those values from `vp_get_stats()` and
`vp_stream_writer_get_stats()` after the final drain; a lossy trial cannot
establish counter continuity.

If a controlled wrap experiment establishes a width, set `counter_bits` to
`32` or `64` and set `max_wrap_delta_raw` to a predeclared upper bound for one
sampling interval. A decrease is classified as `wrap` only when its modular
delta is within that bound. Otherwise it remains `reset_or_reuse`. The report
records this policy; a wrap delta is still raw and unitless.

## Required hardware measurements

Run multiple trials for each condition and retain the raw capture, metadata,
report, exact application artifact hash, and any producer/transport loss
counts.

| Question | Controlled comparison |
| --- | --- |
| Does the value advance only while the target runs? | Same thread busy-looping, sleeping, semaphore-blocked, and suspended, with equal sample schedules |
| Is the scale stable? | Fixed-duration busy loops at each supported application clock profile and on each available core affinity |
| Is it per-thread and migration-safe? | Two independent workers, then one worker allowed to migrate versus pinned affinity |
| What is the effective width? | Long-running/high-rate accumulation plus values captured immediately before and after an observed decrease |
| What resets it? | Dormant-thread restart, thread exit/recreate, title suspend/resume, process relaunch, and device reboot, each in a separate capture |
| Can a numeric thread ID be reused? | Repeated bounded create/join cycles with application-recorded generation labels |
| What is sampling overhead? | Identical workload with no sampling and with several fixed sampling intervals |
| Does it correlate with process time? | Sample `sceKernelGetProcessTimeWide()` at the same monotonic boundaries and compare trial-to-trial deltas without assuming equal units |

A candidate unit requires repeatable ratios across idle/busy state, clock
profiles, affinities, and devices, plus an independently measured reference
interval. Until those measurements agree and explain wraps/resets, keep
`unit = "unknown"` and do not present the value or its delta as time, cycles,
CPU load, or percent utilization.

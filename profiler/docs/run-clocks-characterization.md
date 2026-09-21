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

## Source-owned bounded experiment

`runclocks-experiment/main.c` is a separate, file-only application package for
the existing user-owned disposable title `VDPR00001`. It has no network code or
network libraries. It is omitted from `all`, `check`, and `vita-check`, and
compiles with `VP_RUNCLOCKS_EXPERIMENT_ENABLED=0` by default. A disabled build
only displays that state and exits; it does not create evidence. An enabled
build still requires an on-device Cross press within 30 seconds, otherwise it
aborts without creating evidence.

The fixed plan is deliberately small: one sleeping worker and one bounded
integer worker at 200 ms, fresh generations of both at 400 ms, then two fresh
400-ms integer workers. The control thread samples every 25 ms, drains after
each round, permits at most two workers, uses a 64-slot ring, and declares a
five-second capture bound. Every phase and every application-created thread
generation receives an inclusive event-index range. A generation boundary or
phase boundary deliberately suppresses a delta, including when Vita reuses the
same numeric thread ID.

The app writes exactly one wire-v2 `.vptrace` and one experiment JSON under
`ux0:data/VitaDebugger`. Both names contain a required operator-supplied,
path-safe experiment ID. Existing paths cause an abort; files are opened with
`SCE_O_EXCL`. A failed or lossy capture never gets a metadata file and must not
be treated as evidence. The capture identifies
`sceKernelGetProcessTimeWide` as a monotonic microsecond reference timer. The
metadata repeats that contract and records the RTC-derived UTC capture time,
device alias/model, firmware, title, build, clock profile, power state, phase
plan, generation map, and all loss counters.

After an authorized trial, preserve both source files and generate the report:

   ```sh
   python tools/vitaprofiler_trace.py runclocks \
     trial.vptrace trial.runclocks-experiment.json trial.runclocks.json
   ```

The receiver retains its 16-MiB and 262,144-event defaults. The characterization
command additionally rejects incomplete/truncated captures, fewer than two raw
samples, samples outside declared thread generations or phases, mismatched
phase worker counts, samples above declared count/duration bounds, timer
metadata that disagrees with the capture, timestamp regressions, missing
`raw_value` flags, nonzero loss, and metadata loss counts that disagree with
wire-v2 final statistics. Metadata is capped at 64 KiB, phases and generations
at 64, workers per phase at 16, and total declared duration at ten minutes.

The report includes the source capture's SHA-256 digest, wire version, actual
sample span and interval range, normalized metadata, every raw value, every
accepted raw delta, phase summaries, raw-delta/reference-timer rates, and
Pearson correlations between raw deltas and reference elapsed microseconds.
All rates are labeled `unknown_raw_units_per_reference_second`; correlations
are descriptive only. They are not a unit conversion, utilization, percent,
time, or cycle-rate claim. Raw values and deltas include decimal and
hexadecimal strings so values above JavaScript's exact integer range remain
lossless in JSON consumers.

## Experiment metadata

Metadata is UTF-8 JSON, limited to 64 KiB. All fields shown below are required
except `notes`; use an explicit `"unknown"` string rather than omitting a
condition that was not measured.

```json
{
  "format": "vitaprofiler-runclocks-experiment-v2",
  "experiment_id": "pch2000-365-idle-001",
  "captured_at_utc": "2026-09-18T05:00:00Z",
  "device_model": "PCH-2000",
  "device_id": "lab-vita-slim-a",
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
  "transport_lost_events": 0,
  "sink_lost_events": 0,
  "reference_timer": {
    "source": "sceKernelGetProcessTimeWide",
    "unit": "microseconds",
    "frequency_hz": 1000000,
    "monotonic": true
  },
  "threads": [
    {
      "thread_id": "0x40010003",
      "generation": 0,
      "label": "measurement-worker-0",
      "first_event_index": 120,
      "last_event_index": 4116
    }
  ],
  "phases": [
    {
      "phase_id": "idle-200ms-r1",
      "condition": "worker repeatedly calls sceKernelDelayThread",
      "repeat": 1,
      "target_duration_us": 200000,
      "worker_count": 1,
      "first_event_index": 0,
      "last_event_index": 31
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

The strict report rejects nonzero producer, transport, or sink loss. The
source-owned harness reads those values from `vp_get_stats()` and
`vp_stream_writer_get_stats_v2()` after the final drain and binds them to the
wire-v2 final statistics; a lossy trial cannot establish counter continuity.

If a controlled wrap experiment establishes a width, set `counter_bits` to
`32` or `64` and set `max_wrap_delta_raw` to a predeclared upper bound for one
sampling interval. A decrease is classified as `wrap` only when its modular
delta is within that bound. Otherwise it remains `reset_or_reuse`. The report
records this policy; a wrap delta is still raw and unitless.

## Required hardware measurements

No hardware run is authorized by this document. In particular, do not contact
`10.1.1.217` or any other device until the coordinator explicitly releases the
device after the PMU cleanup session.

After that explicit release, use this exact procedure:

1. Update to the reviewed characterization commit in an isolated worktree.
   Record its full Git commit and tree IDs.
2. Choose a new path-safe experiment ID, stable device alias, exact
   model/firmware, clock profile, and power state. Confirm the Vita RTC is
   correct, then build only the disposable package from `profiler`:

   ```sh
   make vita-runclocks-experiment \
     VITA_RUNCLOCKS_ENABLE=1 \
     VITA_RUNCLOCKS_EXPERIMENT_ID=pch2000-365-default-r01 \
     VITA_RUNCLOCKS_DEVICE_MODEL=PCH-2000 \
     VITA_RUNCLOCKS_DEVICE_ID=lab-vita-slim-a \
     VITA_RUNCLOCKS_FIRMWARE=3.65 \
     VITA_RUNCLOCKS_BUILD_ID=git-FULL40HEX-tree-FULL40HEX \
     VITA_RUNCLOCKS_CLOCK_PROFILE=application-default-unchanged \
     VITA_RUNCLOCKS_POWER_STATE=AC-attached-battery-full
   ```

3. Record SHA-256 hashes of the VPK, `eboot.bin`, and ELF before installation.
   Install only to `VDPR00001`. Do not start a receiver and do not enable any
   network path.
4. Launch the title, verify the displayed experiment ID and bounds, then press
   Cross once. Circle or a 30-second wait aborts. Do not relaunch with the same
   ID because evidence paths are non-overwriting.
5. Copy the matching `.vptrace` and JSON from
   `ux0:data/VitaDebugger` without modifying them. Hash both files.
6. Run the `runclocks` command above, without `--force`, then archive the raw
   files, report, application hashes, commit/tree IDs, and operator log
   together. Reject the trial on any tool error.
7. Repeat with a new experiment ID. Clock-profile, affinity, suspend, and
   sampling-overhead variants require separately reviewed source/config
   changes; do not improvise them in the baseline package.

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

## Acceptance criteria for any stronger interpretation

A single successful capture is only harness validation. A normalization or
unit hypothesis may be proposed for review only when all of these are true:

1. Every retained capture is complete, wire-v2, hash-bound to metadata, within
   all declared bounds, and has zero producer/transport/sink loss.
2. At least five accepted repetitions exist for every duration, workload,
   worker-count, clock-profile, affinity, and device cell under comparison.
3. Every decrease is explained by a declared generation/reset boundary or by
   at least three observed, bounded repeats of the same candidate modulus. No
   unexplained discontinuity is discarded.
4. Busy-phase raw delta versus reference duration has Pearson correlation at
   least 0.99, and the candidate normalized rate has coefficient of variation
   at most 2% within each cell. Sleep, blocked, and suspended controls are
   reported separately rather than folded into the fit.
5. The proposed relationship predicts held-out durations within 2% and either
   remains within 2% across clock profiles, affinities, worker counts, and
   devices or names and validates every required conditioning variable.
6. Sampling overhead is independently bounded to at most 1% against a
   no-sampling control, and conclusions remain stable across at least three
   sampling intervals.
7. A trusted independent reference establishes dimensional meaning. A strong
   correlation with `sceKernelGetProcessTimeWide()` alone is insufficient to
   rename the numerator.

Failure of any criterion preserves the current raw counter, unknown unit, and
no-normalization presentation.

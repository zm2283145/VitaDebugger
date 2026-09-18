# VitaProfiler `runClocks` evidence baseline

Date: 2026-09-18

## Repository evidence reviewed

The two retained retail-3.65 TCP gate captures each contain one
`vita.thread.run_clocks` sample for thread ID `0x40010003`:

| Capture | Event timestamp | Raw value |
| --- | ---: | ---: |
| `profiler-tcp-gate-pass-2026-09-15.vptrace` | 44,761,148,989 us | 419,975 |
| `profiler-tcp-gate-recovery-pass-2026-09-15.vptrace` | 45,127,974,102 us | 391,475 |

Each event has `VP_EVENT_FLAG_RAW_VALUE`. The captures are from separate title
launches, so matching numeric thread IDs do not prove that the samples describe
one thread object or one uninterrupted counter lifetime. One sample per launch
cannot establish a delta, counter width, reset rule, frequency, time unit, or
CPU-utilization relationship. The lower value in the later launch is consistent
with at least a per-thread or per-launch reset, but does not prove either.

The source adapter copies the field returned by
`sceKernelGetThreadInfo(target, &info)` without conversion. Repository hardware
records contain no controlled reference workload that establishes what one raw
increment represents.

## Safe conclusion

`SceKernelThreadInfo.runClocks` remains a cumulative raw counter with
`unit = "unknown"`. No CPU percentage, elapsed-time conversion, cycle count, or
cross-launch delta is supported by current evidence.

The host analyzer now preserves the unsigned 64-bit pattern, derives raw deltas
only within one thread generation, and rejects ambiguous decreases unless an
explicit counter width plus bounded modular delta supports a wrap. The strict
characterization report binds normalized experiment metadata to the raw
capture's SHA-256 digest.

## Hardware follow-up

Run the bounded
[`runClocks` characterization protocol](../../profiler/docs/run-clocks-characterization.md)
on Vita. The minimum useful dataset is:

1. repeated idle, busy-loop, sleeping, blocked, and suspended trials;
2. each supported application clock profile and controlled core affinity;
3. explicit thread creation generations, exit/recreate, suspend/resume,
   process relaunch, and reboot boundaries;
4. a long/high-rate trial capable of observing a decrease near a candidate
   modulus; and
5. no-sampling versus several fixed-interval trials to quantify probe
   overhead.

Retain the VPK/ELF hashes, `.vptrace`, experiment JSON, characterization JSON,
and loss counters for every trial. Do not update the unit or enable a wrap
model until repeated results explain all observed scaling and discontinuities.

# Cooperative input trace format version 1

Input traces support reproducible tests inside the current source-owned
application. They cannot capture or inject VitaShell/SceShell, system UI,
another title, PS/power/volume controls, or global controller/touch state.
The application must explicitly register its physical input samples for
recording and an application-local callback/queue for playback. This layer
contains no `SceCtrl`, `SceTouch`, shell hook, or system input API.

Recording and playback are separate negotiated capabilities with separate
consents. Recording requires `VD_INPUT_TRACE_EXPLICIT_RECORD_CONSENT`.
Playback additionally requires `VD_INPUT_TRACE_EXPLICIT_PLAYBACK_CONSENT`,
the companion mutation consent, and the application callback. They are
mutually exclusive. Reconnect never starts playback: disconnect terminally
closes the companion session, and a fresh session must explicitly import and
start a trace again.

## Header

All integers use network byte order. A trace starts with this 96-byte header:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII `VDTR` |
| 4 | 2 | version, exactly `1` |
| 6 | 2 | header size, exactly `96` |
| 8 | 2 | event size, exactly `64` |
| 10 | 2 | explicit end/abort reason |
| 12 | 4 | flags, exactly `1` (complete bounded header) |
| 16 | 9 | uppercase source-owned title ID |
| 25 | 3 | reserved, zero |
| 28 | 4 | nonzero PID |
| 32 | 8 | nonzero process generation |
| 40 | 8 | nonzero companion session ID |
| 48 | 8 | nonzero application/host-selected trace ID |
| 56 | 4 | event count, at most 4096 |
| 60 | 4 | exact total byte size |
| 64 | 8 | duration in microseconds, at most one hour |
| 72 | 4 | IEEE CRC-32 integrity checksum |
| 76 | 20 | reserved, zero |

The checksum covers header bytes 0-71, header bytes 76-95, and every event.
It detects accidental/malformed changes but is not a signature. A trace is
accepted for playback only after exact title/PID/generation/session matching
and only when its end reason is `COMPLETE`.

End reasons are `1 COMPLETE`, `2 ABORTED`, `3 OVERFLOW`, `4 DISCONNECT`,
`5 TIMEOUT`, `6 CALLBACK_FAILURE`, `7 SHUTDOWN`, `8 IDENTITY_CHANGE`, and
`9 CANCELLED`. Aborted traces remain inspectable but are never replayed.

## Events

Exactly `event_count` fixed 64-byte events follow:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | sequence, exactly index + 1 |
| 4 | 2 | kind: `1 INPUT`, `2 CHECKPOINT` |
| 6 | 2 | event size, exactly `64` |
| 8 | 8 | monotonic relative timestamp in microseconds |
| 16 | 8 | monotonic application frame index, or `UINT64_MAX` if absent |
| 24 | 4 | application buttons |
| 28 | 2 | signed left X |
| 30 | 2 | signed left Y |
| 32 | 2 | signed right X |
| 34 | 2 | signed right Y |
| 36 | 1 | touch count, 0-2 |
| 37 | 1 | checkpoint marker, zero for input |
| 38 | 2 | reserved, zero |
| 40 | 8 | touch 0: ID, X, Y, force as four `u16` |
| 48 | 4 | reserved, zero |
| 52 | 8 | touch 1: ID, X, Y, force as four `u16` |
| 60 | 4 | reserved, zero |

The final event timestamp equals the header duration. Checkpoint markers are
`1 FRAME`, `2 ASSERTION`, and `3 USER`; every input field in a checkpoint is
zero. Unused touch slots are zero.

The allowed application button mask is `0x0000f3f9`: select, start, D-pad,
left/right trigger, triangle, circle, cross, and square. No PS, power, volume,
shell, or undocumented bit is accepted. Axis and touch values are preserved
exactly as supplied by the source-owned application.

## Bounds and lifecycle

The caller supplies fixed storage and chooses a lower event/duration limit if
desired. When capacity, event count, or duration would be exceeded, recording
terminates with `OVERFLOW` or `TIMEOUT`; no event is silently dropped.
Nonmonotonic timestamps/frame indices and malformed enums fail closed.

Playback is real-time 1x and tick-driven. It never sleeps inside the C library,
dispatches at most 32 due events per tick, measures maximum scheduling drift,
and fails when configured drift is exceeded. Pause is explicitly unsupported.
Completion, cancel, error, timeout, disconnect, identity change, callback
failure, and shutdown force a neutral application callback.

## Host tooling

`vdscreen.trace` provides `verify_trace`, `save_trace`, `trace_listing`, and
`replay_trace`. Replay requires a caller-registered cooperative callback,
limits each host sleep to 50 ms, reports drift, supports cancellation, and
forces neutral in `finally`.

The file CLI verifies, lists, or atomically copies already-received traces:

```powershell
py -3 .\screen\tools\vdtrace.py verify .\input.vdtrace
py -3 .\screen\tools\vdtrace.py list .\input.vdtrace
py -3 .\screen\tools\vdtrace.py save .\input.vdtrace .\saved\input.vdtrace
```

There is deliberately no CLI that chooses a device, title, PID, address, or
system input backend. A future transport adapter must preserve the companion's
pairing, capability, identity, deadline, and per-session playback-consent
checks.

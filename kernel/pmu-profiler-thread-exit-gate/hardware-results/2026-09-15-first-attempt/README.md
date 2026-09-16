# Retail 3.65 PMU dormant-thread re-arm result

This directory preserves the first completed `VDCP00012` hardware gate for the
default-off PMU safe-rearm candidate.

## Result

**PASS** on a retail handheld PS Vita running system software 3.65.

The gate used a dedicated worker on application core 0 and physical PMU lane 5.
The worker opened real event `0x10` with a 5,000 ms lease, produced a valid
sample, and returned without calling `Close`. After the application positively
observed that worker terminate, its still-live main thread opened event `0x01`,
read a second valid sample, and closed the replacement lease with exact PMU
restoration.

The fresh lease was admitted in the same process and boot. No process-exit
stage was compiled or armed.

## Decoded journal

Both 256-byte records pass the production checksum and state validator.

| Field | Attempt record | Completion record |
| --- | ---: | ---: |
| Revision / state | `1 / ATTEMPTED` | `2 / COMPLETE` |
| Flags | `0x00000000` | `0x00000007` (`OWNER_SAMPLE`, `REARM_SAMPLE`, `PASS`) |
| Capabilities | `0x0000002F` | `0x0000002F` |
| Owner token / generation | `0 / 0` | `1 / 1` |
| Replacement token / generation | `0 / 0` | `2 / 2` |
| Owner sample | `0` | `98` |
| Replacement sample | `0` | `23` |
| Re-arm polling time | `0 us` | `251 us` |
| Owner-open to replacement-open | `0 us` | `3,951 us` |

The 3,951 us upper-bound delta is strictly below the abandoned 5,000,000 us
lease, so ordinary lease expiry cannot explain the successful second Open.
Every owner/replacement open, read, wait, close, delete, journal, and affinity
restoration result passed. The replacement identity is distinct, and the
reserved record tail is zero.

## Evidence hashes

| Artifact | SHA-256 |
| --- | --- |
| `pmu-thread-exit-v1-a.bin` | `E2F57A7D054B92791E9B09C9E7F0DC89E68DB716BB96379CD83EC1E47533361E` |
| `pmu-thread-exit-v1-b.bin` | `9A2ED7D7168F5BB8E8AF246519517B21EF7096BC568D9DD16882E589D82DB8E9` |
| `pmu-thread-exit-pass.jpg` | `33E3C1D05011CD5CBCC22EF653992D1FEAB11021C260F5F0C98FAA6D517ABCA7` |
| Kernel candidate | `A6A7FB6058F9F07EBDD401D5ECFEFECD2C0D5EB4F7E95943CB0ACA88DF995EFD` |
| Gate VPK | `F5E3A04A1099691D994851C1685F9E1A910698C8542BB3D87E269C210E049AD8` |

The active plugin replaced a known-good companion only after upload
round-trip verification; `ur0:tai/config.txt` was not changed.

## Proven boundary

This result proves controlled recovery after the **owning thread** terminates
while its application remains alive. It does not prove recovery after process
exit, forced termination, a crash, receiver disconnect, send timeout, retained
UID reuse, competing ownership, other PMU lanes/cores, or other firmware.
Those remain separate fail-closed hardware gates.

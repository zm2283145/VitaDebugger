# Isolated PMU session gate on retail 3.65

Date: 2026-09-14

The disposable `VDCP00009` probe completed its deliberately narrow first PMU
write gate on all three Vita application cores of the tested retail Vita
running system software 3.65. This was not the production profiler and did not
use the public user-mode `ScePerf` path.

## Exact artifacts

- `vitadebug-pmu-session-probe.vpk` SHA-256:
  `0A4D60E056418A8BABBD9266DB50D318D3960DBF76BB4932A67555A25EABB80E`
- `vitadebug-pmu-session-probe.skprx` SHA-256:
  `1B170B336D3339FB699A7C7CEEA6683A78650DE01963A035AD5F9BE46279D850`

## Decoded durable results

The probe ran cores in the required 0, 1, 2 order. Both 512-byte journal slots
were present and checksum-valid after every run. The newest record for each
run was a completed state (`state=4`) with a zero syscall result, zero journal
result, zero operation result, zero restore result, backend ready, and no
retained restoration obligation.

The [unedited core-0 PASS screenshot](profiler-pmu-session-core0-pass-3.65.jpg)
has SHA-256
`55CFA45E825CFB63301372251E60CB874BEE11F176882E2D5B0A3192B0740226`.
Core 1 and core 2 are represented by their two-slot durable journals rather
than screenshots.

| Core | Newest slot | Revision | Sequence | Passed-core mask | MIDR | MPIDR | Stage | Count | Exact restore |
| --- | --- | ---: | ---: | ---: | --- | --- | ---: | ---: | --- |
| 0 | A | 3 | 1 | `0x1` | `0x412FC09A` | `0x80000000` | 7 | 17/17 | yes |
| 1 | B | 6 | 2 | `0x3` | `0x412FC09A` | `0x80000001` | 7 | 17/17 | yes |
| 2 | A | 9 | 3 | `0x7` | `0x412FC09A` | `0x80000002` | 7 | 17/17 | yes |

Stage 7 is the backend's completed stage. Each run observed the expected
Cortex-A9 identity, selected only programmable lane 5, configured architectural
event `0x00`, issued exactly 17 `PMSWINC` writes, read back exactly 17, and
verified the complete gate snapshot byte-for-byte against the original. That
snapshot covers shared controls, the selector, and selected lane 5; lanes 0
through 4 are deliberately not read. The final status was `ready=1`,
`obligation=0` on every core.

The archived raw-record hashes are:

| Run and slot | SHA-256 |
| --- | --- |
| core 0, A | `1CAC97B6AF61D20A8EB7A1C6F5C4DB55AAA73BDB548CD86C292B527275B6F5EE` |
| core 0, B | `EA6C10976ED298B4B19A36991B2382D4FD355CAA0D23687B137818F3769CE792` |
| core 1, A | `C7E722F2A9AC76E7B179BBCF00F7341A07386557D7365D42B849FD1F92201CC9` |
| core 1, B | `FEF87E4F91AAA28DBEBBE4C6F606E1506DF0C61AA95CA6CE7B79987ECAA46AA5` |
| core 2, A | `FCB8AC811C0734994AC6269B9D3BBD1E8F72BD47882ECD30C5C64948224F4BF3` |
| core 2, B | `0F7939116C24A21841D002B049AD4BA0D4456DDE0DA8B2E76B0B8DCD663B46F2` |

The raw records remain under the ignored local hardware-evidence directory;
they are not committed to the repository.

## Scope of the result

This proves that the audited kernel backend can perform one deterministic
software-increment transaction on lane 5 and exactly restore every shared
control and selected-lane field observed or touched by the gate on application
cores 0 through 2 of this retail 3.65 Vita. It advances the PMU work beyond
read-only discovery.

It does **not** yet prove arbitrary hardware-event selection, cycle-counter
ownership, continuous or periodic sampling, coexistence with another PMU user,
the host-tested profiler bridge running through a Vita kernel export,
timeout/disconnect/process-exit cleanup, the Vita-side TCP/file drain owner, or
behavior on core 3, Vita TV, or another firmware. Those remain separate gates
before live PMU counters are advertised.

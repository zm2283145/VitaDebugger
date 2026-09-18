# Practical stepping/register live gate

`tools/gdb_step_register_gate.py` is the bounded hardware gate for the
practical ARM/Thumb stepping and individual-register milestone. It does not
install or launch an application and does not change the kernel plugin. Start
the matching `SLRS00001` diagnostic build first, then run:

```powershell
py -3 tools\gdb_step_register_gate.py `
  --host 192.0.2.10 `
  --main-elf test.elf `
  --vpk uvdb-test.vpk `
  --kernel-plugin kernel\build-vfp-live3\vitadebug.skprx `
  --firmware 3.65 `
  --device-class retail-handheld `
  --kernel-abi 0x0001000b `
  --evidence local\gdb-step-register-gate.json
```

`--elf` is an alias for `--main-elf`. A freshly generated ASLR symbol script
may be supplied instead with `--symbol-script`; the gate safely extracts its
single `file` command and requires any recorded remote endpoint to match the
requested `--host` and `--port`. It does not source arbitrary commands from
that file. Main-image relocation continues to come from the target's live
`qOffsets` response when GDB attaches.

## Pass conditions

The command prints `PASS: practical GDB stepping/register hardware gate` and
returns zero only after all of these checks succeed:

1. VitaSDK `arm-vita-eabi-gdb` hits `step_target` twice with the same persistent
   software breakpoint. The second continue exercises GDB's hidden
   breakpoint step-over and must not expose `E16`.
2. A GDB instruction step from each Thumb-2 and A32 `LDREX` fixture reaches the
   instruction immediately after its matching `STREX`. `R2` must report a
   successful store and the fixture word must increment exactly once.
3. The monitor inventory proves GDB selected the stopped exception thread.
   Individual `p` and `P` packets are forced, then R0 is mutated, read back,
   restored, and read back again. CPSR repeats the transaction while changing
   only the V flag. Neither temporary value is ever resumed or detached.
4. Monitor status reports zero software breakpoints, the application advances
   through a clean detach, and a second GDB session reconnects and detaches.
5. A raw-RSP failure-injection session reads the exact original bytes at both
   exclusive-step trap sites, installs two software breakpoints, verifies each
   target site physically contains the correct Thumb or A32 UDF encoding, and
   closes its socket without `z0` or `D`. After reconnect, monitor status must
   report zero breakpoints and both sites must exactly match their original
   fixture encodings before the final clean detach.

Before the first breakpoint write, the gate also reads and matches the exact
current instruction bytes at all five addresses it may patch: `step_target`,
both exclusive-load sites, and both post-`STREX` trap sites. A stale or
mismatched main ELF therefore fails before `Z0` is allowed to modify target
code. This is a narrow safety identity check for the diagnostic fixture, not a
replacement for the full VPK/SUPRX build-identity workflow.

The optional evidence file is written atomically. It contains artifact hashes,
portable observations, runtime fixture addresses, and hashes of the GDB/MI
transcripts rather than the raw transcript, local filesystem paths, or network
endpoint. A failure returns nonzero, prints the exact assertion locally, and
records a sanitized failure category in portable evidence.
Register restoration is retried and verified before the gate issues any resume
or detach after a mutation. The current GDB protocol has no atomic remote
register-write/rollback operation; a physical network loss in the interval
between a `P` write and its restoring `P` write cannot be made transactional by
a host-only gate. The gate keeps that interval to one immediate read-back and
restore and never deliberately resumes a temporary value.

## Retail 3.65 result

The complete gate passed on a retail handheld Vita running system software
3.65 with kernel ABI v1.11 and VitaSDK GDB 15.2. The persistent hidden-
breakpoint step-over repeated without `E16`; both the Thumb-2 and A32 bounded
`LDREX`/`STREX` fixtures reported a successful store, incremented their word
exactly once, and stopped after `STREX`. Individual R0 and CPSR `p`/`P`
transactions were read back and restored before any resume or detach. The
failure-injection session then disconnected with two traps armed; reconnect
found zero software breakpoints and the exact original bytes at both sites
before a clean detach.

The portable artifact records hashes, observations, and the two clean GDB/MI
session transcript hashes without the Vita endpoint or workstation paths:
[retail 3.65 practical stepping/register evidence](hardware/gdb-step-register-exclusive-3.65.json).

This gate closes the currently implemented practical milestone. It does not
claim scheduler-locked arbitrary foreign-thread execution, displaced stepping,
foreign-thread/VFP writes, privileged exception-return decoding, syscall
catching, or support for unmatched and otherwise unsafe exclusive sequences.

## Current roadmap increment

The follow-on safety increment is host-modeled and Vita cross-built, but has no
new hardware result. Host tests now cover trap partial writes, sync/read-back
failures, verification corruption, disconnect, competing ownership, exact
original-byte retention, restoration retry, and stale retained target/module
identity. Production still uses the unbound trap helper because a durable Vita
target/module object provider has not been proven.

The Vita fixture ELF now exports non-executed exact encoding tables for
representative accepted ARM/Thumb branches, interworking, PC loads, and
multi-register PC loads. Rejected tables cover PC-writing privileged exception
returns, `BXJ`, register-controlled A32 shifts, SVC, WFE/WFI, nested or
mismatched exclusive sequences, and a non-final IT-block PC write. These
symbols prove that the encodings cross-assemble and allow a future gate to
inspect their bytes; they do not claim that any rejected instruction was
executed on hardware.

Arbitrary foreign-thread isolation remains disabled. A host capability model
requires scheduler ownership, context identity, trap ownership, rollback
readiness, and matching nonzero generations before a resume-one or displaced
step provider may run. Current VitaSDK declarations do not document a
scheduler lock or atomic resume-one/resuspend contract, so no production
provider is wired. Unsupported cases must continue to fail before changing
memory/registers or acquiring trap/stop ownership.

### ABI v1.14 rerun checkpoint

The first retail 3.65 rerun of the ABI `0x0001000e` candidate on 2026-09-18
passed plugin/config migration, exact artifact hashing, fixture-byte preflight,
kernel compatibility, healthy stop-session, zero-breakpoint, and four-thread
inventory checks. It then failed before the first breakpoint stop on three
attempts: MI `-break-insert *step_target` completed at `0x81039ab0`,
`-exec-continue` reported running, and GDB logged
`warning: Exception condition detected on fd 380`; no `*stopped` arrived within
15, 60, or diagnostic 30 seconds. The first two abandoned clients recovered
with zero breakpoints and a healthy stop session. After the third attempt the
TCP listener accepted but did not answer `qSupported`, so the title was killed.
No renew/`EndStop` injection was attempted, and the verified prior config/plugin
were restored.

The first diagnosis identified a real host-reproducible resume-tail ownership
race. Process `EndStop` can schedule the main thread into `step_target` before
the server controller's previous exception callback releases the protocol and
exception gates. The bounded owner-qualified handoff closes that modeled race,
but it is not hardware-validated.

A second focused retail run of commit `c43c6c5` used VPK
`27F66B561BD94C2BD9DA291C4C29F36969CF44C8B84C8792AAFB1C977AAB6EC6`,
ELF
`8129D74D27324A5C12349BBF22865C1E81B68EE22887F9AAC1EEAE0B68636EE7`,
and unchanged SKPRX
`D7553A52A458B38CAA9B0B8074028F6150AE1DB2DD223C3414E610F2B8C7F942`.
ABI/status and exact fixture-byte preflight passed, but one verified
`step_target` breakpoint plus one `-exec-continue` again produced no
`*stopped` within 30 seconds. Killing only host GDB allowed abandoned-client
cleanup: fresh raw RSP was responsive, reported zero software breakpoints and
a healthy active stop session, and detached cleanly. The retained last fault
was undefined instruction at `0x81029162`, but that run did not persist
`qOffsets`, module ranges, or the runtime breakpoint address. The value cannot
be mapped to the fixed ELF or reused as evidence; a reconnect synthetic stop
may also have overwritten the single last-fault slot. The handoff-only root
cause is therefore insufficient.

The next run is diagnostic-only and must use
`tools/gdb_step_register_gate.py --first-breakpoint-only --evidence <path>`.
Before the one continue, schema v2 persists the complete MI records, raw
`qOffsets` result, `monitor modules`, full `monitor status`, resolved live
fixture addresses, breakpoint number/address, and a phase checkpoint
atomically to the evidence file before execution. On a missing `*stopped`, it
does not issue an MI breakpoint deletion or detach: it preserves the timeout,
kills only the host GDB transport, reconnects through bounded raw RSP, and
checkpoints each successfully collected `qOffsets`, modules, status, threads,
and stop reply before attempting the next query. It detaches only when zero
software breakpoints are proven. `monitor status` retains eight newest-first
callback traces; their slash-separated values are:

- the header reports callbacks omitted because all eight trace slots were
  still active; each record reports whether its callback is still active;
- `handoff=wait-seq/done-seq/result/attempts`, where result is `1` clear,
  `2` same owner, `3` released after waiting, or `4` timeout;
- `guard`, `session`, `protocol`, and `lock` are
  `sequence/result`;
- `predecessor=sequence/reason/invoked`, where reasons `1..5` are closed
  guard, nested guard, unclaimable session, protocol contention, and state-lock
  contention;
- `publish=sequence/pc/signal/synthetic/breakpoint-match`;
- `stop=begin-sequence/result/stopped-operation-sequence/result`;
- `packet=wait-sequence/socket-poll-sequence/socket-wake-sequence/wake-result/`
  `ready-sequence/status-query-sequence`;
- `reply=attempt-sequence/socket-poll-sequence/socket-wake-sequence/`
  `wake-result/result-sequence/result`, followed by callback `exit`.

Stop after this focused diagnostic regardless of result. Do not run broader
stepping or renew/`EndStop` injection until the retained trace proves which
stage the application UDF reached and a separately reviewed fix passes this
same first-stop gate.

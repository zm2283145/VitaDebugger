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

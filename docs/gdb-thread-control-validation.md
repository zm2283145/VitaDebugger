# GDB thread-control validation gate

The application stub now uses one bounded protocol inventory. In a
kernel-assisted build, the complete caller-process kernel list is authoritative
and cooperative registrations annotate names only; a library-only build uses
the cooperative registry as its fallback inventory. The application thread
that caused the stop is first, duplicates are removed, and the debugger's
server and lease-keeper threads are always hidden. Initial attach and Ctrl-C
choose a suspended application thread instead of exposing the server thread's
exception context. `qfThreadInfo`, `qThreadExtraInfo`, `T`, `Hg`, `Hc`, register
reads, and resume planning all use this same snapshot.

The first seven live-hardware phases have validated inventory, selection,
abandoned-client recovery, current-main-thread Thumb stepping, fail-closed
foreign/stale positive-`Hc` behavior, exact-stopped-thread positive-`Hc`
step-over, dynamic raw register-bank selection, deterministic foreign-worker
Thumb stepping, and representative ARM-state PC writers. Phase 5 safely
exposed and recovered from the register-bank bug before any foreign thread was
stepped; Phase 6 verified its user-library fix without changing the kernel
companion. Phase 7 fixed GDB's ordinary hidden-breakpoint step-over `E16`, then
verified ARM-state `MOV PC`, `LDMDB {..., PC}`, and `LDR PC` execution, clean
detach, and reconnect. Controlled failure fixtures and longer stress runs
now have deterministic host coverage but remain unrun on hardware. The larger
decoder matrix has pure host coverage; only the
representative forms named in Phase 7 inherit that hardware result. Do not
describe the entire thread-control work as fully promoted until every check
below passes.

## Current evidence boundary

This roadmap increment has three deliberately separate evidence levels:

- **Host-modeled:** the production `uvdb.c` translation unit passes 2,048
  logical-tick fake-kernel cycles covering stop ownership, late-thread
  reconciliation, controller/lease exclusion, renewal and `EndStop` failure,
  socket shutdown, watchdog expiry, fresh-stop recovery, stale generation
  completion, disconnect, restart, trap cleanup, and zero leaked ownership.
  No wall-clock sleep determines the result.
- **Vita cross-built:** library-only, kernel-thread-control, and opt-in VFP
  configurations compile and package with the current VitaSDK. The new
  encoding tables are linked as data; dangerous forms are not executed.
- **Hardware-proven:** only the Phase 1-7 results recorded below. This
  increment has not contacted a Vita and adds no Phase 8 hardware claim.

Renewal failure is now published only when both the observed stop token and
its monotonic generation still match. A delayed failure from a retired session
therefore cannot poison a replacement stop. Controlled renew/`EndStop`
injection is currently a host fake-kernel facility, not a remotely exposed
production or kernel test hook.

## Selection contract

- `Hg0` and `Hg-1` resolve to the current stopped thread, then to the first
  visible thread only if no stopped thread exists. A positive `Hg` ID must be
  present in the current inventory.
- `Hc0` uses the deterministic stopped-thread fallback for a legacy `s`.
  `Hc-1` continues the process, but `Hc-1` plus legacy `s` is rejected when
  multiple threads are visible.
  Positive `Hc` followed by legacy `c` is rejected. A following legacy `s` is
  accepted only if that ID exactly names the current stopped exception thread,
  the kernel stop token is active and healthy, and there is no address
  override. That narrow path retains the stop token while the exception thread
  executes one instruction. Use `vCont;s:T;c` for a foreign thread while
  explicitly continuing its peers.
- Stale positive selectors are retained and their next operation fails. They
  are never silently widened to `Hg0`/`Hc-1` or process-wide execution.
- VitaDebugger does not advertise the multiprocess extension, so `pPID.TID`
  selectors are rejected rather than partially parsed.
- `vCont?` reports only `c` and `s`. Action lists use the protocol's leftmost
  match. Every visible thread must receive an action, and at most one thread may
  be selected to step. Every explicit positive selector must still name a live
  inventoried thread, even when a later default action would cover the process;
  unsupported signals, range steps, stale selectors, partial resume sets,
  malformed IDs, and multi-thread step requests fail without resuming anything.

For the exact stopped exception thread, the kernel stop token already excludes
the controller whose handler must return. The positive-`Hc` step-over path keeps
that token active, installs one validated temporary trap, and returns only that
controller while peer application threads remain suspended. Blocking
SVC/WFE/WFI operations, exclusive-load starts, PC overrides, unsupported CPSR
execution states, and unsafe decoder targets fail before resume. This bounded
path fixed GDB's automatic persistent-breakpoint step-over without adding a
kernel ABI. The internal lease keeper also remains runnable to renew the stop.
This path does not yet have an independent target-miss timeout; a future
resume-one transaction must add cancellation and trap rollback if the
predicted address is not reached.

For a selected foreign thread, the kernel stop token supplies both raw ARM
register banks. The user stub selects a valid entry-0 current-user context when
present, otherwise a valid entry-1 syscall-return context, and fails closed if
neither is usable. That selected state feeds the software-step decoder. The
temporary instruction breakpoint remains process-wide and ending the current
stop lease resumes the whole process. This provides correct selected-thread
target calculation, but not scheduler-locked foreign-thread isolation: another
running thread can reach the same temporary address first. A future design
needs a validated resume-one/resuspend kernel transaction or displaced
stepping. The deterministic worker fixture avoids this ambiguity by parking
each worker in a different exported function; it does not remove the general
limitation for application threads that share code.

The host model now names the minimum proof boundary for any future isolated
foreign step: scheduler ownership, durable context identity, exclusive trap
ownership, and rollback readiness must all refer to the same nonzero stop
generation and a live selected thread distinct from the exception thread.
Current VitaSDK headers document debug suspend/resume and read-only suspended
thread snapshots, but not scheduler locking, nesting/ownership semantics, or
an atomic resume-one/resuspend transaction. No production provider satisfies
the model, so arbitrary foreign-thread execution remains disabled. The
existing `vCont;s:T;c` behavior remains process-resume stepping with its
documented distinct-path limitation; it must not be described as isolated.

## Stop/resume failure contract

- A failed kernel all-stop acquisition closes the client instead of serving a
  partially stopped process.
- A lease-renewal failure retains the session token, marks the stop unusable,
  and shuts down the RSP socket to wake a blocked receive. The controller first
  tries to renew/reconcile the same stop, then tries a fresh all-stop if the old
  lease has expired.
- A failed explicit resume similarly re-reconciles to all-stop before restoring
  temporary and persistent software breakpoints and retrying resume. If neither
  stop can be proven coherent while UDF patches are active, the application
  layer does not call `EndStop`: it closes the client but preserves the token,
  handlers, breakpoint table, and stopped-state marker for a later safe recovery
  attempt. The kernel watchdog remains the final target-resume backstop.
- Clean detach and a peer disconnect while running remove every software
  breakpoint and reset selectors before accepting another client.

## Host gate

Run:

```sh
make host-test-register-bank
make host-test-thread-control
make host-tests
```

The raw-bank test covers entry-0 current-user selection, entry-1 syscall-return
selection, entry-0 precedence, zero PC/SP rejection and fallback, privileged
mode rejection, and neither-valid failure. The thread-control test covers exact
thread-ID and resume-address parsing, `0`/`-1`, fail-closed stale selection,
inventory bounds and deduplication, legacy-resume scope, leftmost `vCont`
matching, explicit/default coverage, stale `vCont` selectors, selected
stepping, exact-stopped-thread retained-token validation, blocking/exclusive
instruction rejection, ARM/Thumb direct and memory-address planners, IT
placement, target/source alignment, conservative PC-writer classification, the
no-EndStop-with-live-UDF cleanup policy, unsupported actions, and malformed
packets. It also removes each foreign-step ownership proof independently and
rejects stale context, trap, and rollback generations. The kernel-control
integration target exercises the real lease lifecycle for 2,048 deterministic
cycles, including renew/`EndStop` injection and cleanup. Breakpoint-patch tests
cover partial writes, verification corruption, disconnect, competing ownership,
exact original-byte retention, stale target/module identity, and restoration
retry. These are host tests. Phase 7 additionally exercises
successful target-memory reads, temporary-trap insertion, and retained-token
integration live; the expanded injected rollback and identity matrix remains
unrun on hardware. Also cross-build library-only, kernel-thread-control, and
opt-in VFP variants so none of the feature combinations drift.

The focused commands for this increment are:

```sh
make host-test-thread-control
make host-test-breakpoint-patch
make host-test-exclusive-step
make host-test-uvdb-core-integration
make host-test-uvdb-kernel-control-integration
```

## Recorded live-hardware results

Phases 1-6 used kernel companion ABI `0x00010008` (capabilities
`0x8000001f`) and kernel SHA-256
`01F4A833C548ED71675D73D4C4FB5634B9496D159F952B68244CBC2BC2A884FC`.
Phases 1-4 used installed `eboot.bin` SHA-256
`DE9E81D4C30B0220075CEA18700D2B605153CF2A0A837BB0F36B0DFD6FE48583`
and matching symbol ELF SHA-256
`E3F5456C3E922D20FAC051343940E65E139A8AB135CB2FED1E8171B85367A31A`.
Phase 5 used the first worker-fixture build: VPK SHA-256
`081759A04FC4BD9A574F1CD462B126D560EF4579F951750798133EC596700543`,
ELF SHA-256
`BFFC676B4E4A0F6CC92266926DBE5A60B6304B08B8022671E24EF7CD86695629`,
and installed `eboot.bin` SHA-256
`C50D486CA88DFEDB6EC77CB75046A067F24BD032842AACB6B6110DB15CE0B3A1`.
Phase 6 used the corrected selector build: VPK SHA-256
`D069D40F0E959CF461D3A10B9299DE167BEC960D77122308A6010A94D3B5AEB9`,
ELF SHA-256
`CC066774055264B93807A1196A80C2B5574BADA65036B09CF314C149E59A6ADF`,
and installed `eboot.bin` SHA-256
`FA6F6595910A1C944CBA95FE1469D2E1EB6B3EC2F48F18838B51AA302895769D`.
The Phase 6 installed file was pulled back over FTP and matched exactly. The
Phase 1-6 test configuration was a retail handheld running system software
3.65, although the version was not captured contemporaneously in those
transcripts or screenshots. Phase 7 used kernel ABI `0x0001000b`, kernel
SHA-256
`B4BF79A53D88EF34C0F1AC00D29557F8B709E60ACB35E14222E292D1F47792BF`,
VPK SHA-256
`E406254E79D81D690656E76BF1C04CBD5E8C758E16F62194C7B997BDF7426AC0`,
ELF SHA-256
`25E32D107B466DE43E881A643A09B89ADEC9D8AC5C71AB7CC7D3BC4EB8FD791C`,
and local packaged `eboot.bin` SHA-256
`A20EC805D84B5016B28C26FD800AC03C24FC0DAD400DB9DF34FF8F8971E51833`.
This was source commit `bd8a323`. Its signed VitaDevDeploy job installed and
launched successfully. The structured
Phase 7 evidence is
[`hardware/gdb-arm-thumb-step-3.65.json`](hardware/gdb-arm-thumb-step-3.65.json).
These results do not establish behavior on any other firmware or device class.

### Phase 1: inventory and fail-closed selection

Passed in the interactive session. `vCont?` replied `vCont;c;s`,
`qfThreadInfo` replied
`m40010003,400100c3,40010121,40010127`, and `qC` replied
`QC40010003`. GDB showed main thread `40010003`, one otherwise unregistered
process thread `400100c3`, worker 0 `40010121`, and worker 1 `40010127`, with
all four register sets readable. The debugger server and lease-keeper threads
were absent from the inventory. Each of `Hg7fffffff`, `Hc7fffffff`,
`vCont;s:7fffffff;c`, and incomplete `vCont;s` received `E16`; registers
remained readable and the process remained stopped. A clean detach resumed the
application counter.

This phase was not written to a file under `local/thread-control-live`; the raw
replies above come from the observed interactive transcript. Repeat it with GDB
logging enabled before treating the evidence archive as complete.

### Phase 2: thread selection and abandoned-client recovery

Passed. `phase2-selection.log` records the same four-thread inventory and
successful register/backtrace reads from both workers. Worker 0 and worker 1
had distinct stack pointers, `0x81168fc8` and `0x81178fc8`, while both were in
`sceKernelDelayThread` through `usleep`. Before forced client termination the
counters were main `0x7e5`, worker 0 `0x2729`, and worker 1 `0x1a2f`.

After GDB was terminated without detaching, a new client reattached and
`phase2-reconnect.log` recorded the same four IDs. The counters had advanced to
main `0x9ef`, worker 0 `0x3146`, and worker 1 `0x20f2`, demonstrating watchdog
resume and successful reconnect after an abandoned client. The replacement
client then detached cleanly.

### Phase 3: current-main-thread Thumb stepping

The tested Thumb path passed. `phase3-main-step.log` records main thread
`40010003` stopping at `step_target+8`, PC `0x810619a4`, on
`ldr r3, [r7, #4]`. The persistent breakpoint was deleted before stepping.
The raw legacy exchange was `Hc0` -> `OK`, followed by `s` ->
`T05thread:40010003;`. A direct register read initially still displayed the
cached pre-step PC; after `maintenance flush register-cache`, PC was
`0x810619a6`, the expected next Thumb instruction `adds r3, #3`.

The explicit exchange `vCont;s:40010003;c` then replied
`T05thread:40010003;`. After the same register-cache flush, PC was
`0x810619a8`, the expected `str r3, [r7, #4]`. Thus the exact observed PC
progression was `0x810619a4` -> `0x810619a6` -> `0x810619a8`, and both stop
replies named the selected main thread. Raw `maintenance packet` commands do
not make GDB invalidate its register cache automatically, so every such live
step check must flush the register cache before judging PC progression.

This phase validates legacy and explicit-`vCont` stepping only for the current
main thread in Thumb state. Phase 6 subsequently satisfied the deterministic
foreign-worker Thumb portion, and Phase 7 subsequently satisfied the named
ARM-state fixtures. The saved Phase 3 log ends while stopped after a final
`qC` -> `QC40010003`; it does not itself contain the subsequent detach/resume
evidence.

### Phase 4: foreign positive-`Hc` fail-closed behavior

Passed. `phase4-positive-hc.log` records `Hc40010121` selecting worker 0 and
returning `OK`. The following raw legacy `s` and `c` packets each returned
`E16`. Across both rejected operations, PC `0x81078bf8`, SP `0x811c0b28`, LR
`0x8106cc15`, and CPSR `0x20010010` were identical, demonstrating that neither
request resumed or otherwise changed the selected execution context. `Hc0`
then returned `OK`, and `qC` still reported the original stopped main thread
`QC40010003`. The operator detached cleanly afterward, although the saved
transcript ends at that final `qC` and does not contain the detach exchange.

### Phase 5: runnable-worker register-bank failure and safe recovery

The first foreign-worker attempt stopped at its read-only gate and did not send
any step packet. `phase5-worker-step.log` records both fixture workers as valid
in `sceKernelDelayThread` before arming. After both arm values were set, the
application continued, Ctrl-C stopped it with arm and ready both `{1, 1}`, and
the distinct path symbols resolved to `0x81058902` and `0x81058956`. However,
both named workers then reported PC `0`, CPSR `0`, and no matching symbol while
the sleeping main thread retained a valid context. Both releases were safely
set to one and the client detached without attempting `s` or `vCont`.

`phase5-recovery.log` records the post-release A/B result. Arm and ready were
both `{0, 0}`, release remained `{1, 1}`, path-specific results were nonzero,
and both named workers had returned to their normal sleep loop with valid,
distinct stacks and user-mode CPSR `0x10`. This proves the fixture recovered;
it does not count as a foreign-step pass.

The failure was in the user stub, not the stop session or RSP serializer.
VitaSDK describes the two `ksceKernelGetThreadCpuRegisters` entries as raw,
state-dependent current/exception contexts. A thread blocked in a syscall has
its resumable user state in entry 1, whereas a thread stopped while executing
user code has its current state in entry 0 and may have an empty entry 1. The
stub had unconditionally selected entry 1 because the earlier register probe
covered only a sleeping worker.

The source now centralizes a fail-closed selection rule: a bank is usable only
when its CPSR mode is user (`(cpsr & 0x1f) == 0x10`) and its PC and SP are
nonzero; valid entry 0 wins, otherwise valid entry 1 is used, and neither-valid
snapshots are rejected. Inventory validation, register replies, and the
software-step decoder all use this rule. This is a user-library correction;
the raw kernel ABI and experimental VFP mapping remain unchanged. Only the user
test application was rebuilt for Phase 6. Its read-only arm/Ctrl-C gate placed
both worker PCs inside their respective distinct function ranges before any
step packet was sent; no kernel change was required.

### Phase 6: corrected bank selection and foreign-worker Thumb stepping

Passed on 2026-09-13. The corrected selector build and exact hashes are listed
above; the installed `eboot.bin` was pulled back from the Vita and matched the
local artifact. Runtime relocation placed `uvdb_worker_step_path_0` at
`0x8103c902` and `uvdb_worker_step_path_1` at `0x8103c956`.

The read-only safety gate first reported ready `{1, 1}`. Worker 0 was at PC
`0x8103c926`, SP `0x810f0fb8`, CPSR `0x60000030`, inside path 0. Worker 1 was at
PC `0x8103c972`, SP `0x81150fb8`, CPSR `0x60000030`, inside path 1. Both PCs and
stacks were nonzero, both CPSRs described Thumb user mode, and the paths were
distinct, so the step gate was allowed to proceed.

For worker 0, `vCont;s:40010121;c` replied
`T05thread:40010121;`. After flushing GDB's register cache, the PC had advanced
exactly one Thumb instruction from `0x8103c926` (`ldr r3, [r3, #0]`) to
`0x8103c928` (`cmp r3, #0`); `qC` replied `QC40010121`. `Hc0` then replied `OK`
and its following legacy `s` again replied `T05thread:40010121;`. The flushed PC
advanced exactly to `0x8103c92a` (`beq.n`), and `qC` again named worker 0.

The peer worker remained in its own loop while worker 0 stepped. Immediately
before worker 1's selected step it was at PC `0x8103c980`, SP `0x81150fb8`, and
CPSR `0x60000030`, on a `beq.n 0x8103c970` with the zero flag set.
`vCont;s:40010127;c` replied `T05thread:40010127;`; after the required cache
flush the PC was exactly the taken target `0x8103c970`, and `qC` replied
`QC40010127`.

Both release values were then set to one and the process continued briefly.
After Ctrl-C, arm and ready were both `{0, 0}`, release was `{1, 1}`, and the
path results were nonzero (`0x13578d4a`, `0x2468a490`). Both workers had returned
to `sceKernelDelayThread` with valid user-mode contexts, and a clean detach
resumed the application. No persistent GDB breakpoint was installed during the
fixture.

This validates state-dependent entry-0 current/runnable and entry-1
syscall-return selection on hardware, exact foreign-Thumb target decoding, and
stop attribution for two distinct workers. It does not prove scheduler-locked
single-thread execution because the current all-stop boundary resumes peer
threads during the temporary-breakpoint step. Controlled renew/end failure
fixtures also remain pending. The interactive
transcript is retained locally as `phase6-bank-selector-read.log` under the
ignored `local/thread-control-live` directory; it is not presented as a
repository-hosted evidence file.

### Phase 7: GDB breakpoint step-over and ARM-state stepping

Passed on 2026-09-14 on a retail handheld running system software 3.65. GNU GDB
15.2 first installed a Thumb software breakpoint at `0x81040b52`, continued to
it, removed it, selected stopped thread `40010003` with `Hc40010003`, and sent
legacy `s`. The stopped instruction was `0x687b` (`ldr r3, [r7, #4]`). The
stub replied `OK` to `Hc`, then returned `T05thread:40010003;` from the step;
the earlier `E16` was absent. GDB reinserted the persistent breakpoint and the
next continue hit it normally. This is the exact packet sequence used by GDB's
automatic hidden-breakpoint step-over, not a hand-crafted substitute.

The same connection then stepped three A32 fixtures with CPSR T clear before
and after each operation:

- `MOV PC, R0`: PC `0x810411f0` -> expected `0x810411f8`.
- `LDMDB R0!, {R4, PC}`: PC `0x81041204` -> expected `0x8104120c`, R0
  `0x81041218` -> `0x81041210`, and R4 `0x8899aabb` -> `0xccddeeff`.
- `LDR PC, [R0]`: PC `0x81041220` -> expected `0x81041228`.

Every step returned `T05thread:40010003;`. GDB detached cleanly, immediately
reattached with all five application threads visible and PC in
`sceKernelDelayThread`, then detached cleanly again. No kernel companion change
was needed: the exact exception thread is already the controller excluded from
its active all-stop token, so retaining that healthy token holds its peers
while the handler returns for one instruction.

This phase hardware-validates only the exact positive-`Hc` stopped-thread path
and the three named A32 forms. The extended ARM/Thumb decoder matrix has host
coverage but not a hardware fixture for every encoding. Arbitrary foreign-
thread scheduler locking, controlled token-renew/end failures, injected memory
faults, temporary-trap rollback, bounded exclusive-sequence stepping, and long
stress runs remain separate gates.

## Deterministic foreign-worker step fixture

The two existing named workers keep their original counter, logging, and sleep
behavior until explicitly armed. The test ELF exports these controls so GDB can
operate the fixture without adding a breakpoint or changing the test binary:

- `uvdb_worker_step_arm[2]` asks a worker to enter its path.
- `uvdb_worker_step_ready[2]` is one only while that worker is parked.
- `uvdb_worker_step_release[2]` lets the parked worker return to its normal
  loop.
- `uvdb_worker_step_result[2]` records a path-specific result after release.
- `uvdb_worker_step_path_0` and `uvdb_worker_step_path_1` are externally
  visible, non-inlined functions with distinct loop addresses. No other test
  thread calls either path.

Start with the target stopped and with no ordinary GDB breakpoints installed.
Reset, then arm both workers:

```gdb
set var uvdb_worker_step_release[0] = 0
set var uvdb_worker_step_release[1] = 0
set var uvdb_worker_step_ready[0] = 0
set var uvdb_worker_step_ready[1] = 0
set var uvdb_worker_step_result[0] = 0
set var uvdb_worker_step_result[1] = 0
set var uvdb_worker_step_arm[0] = 1
set var uvdb_worker_step_arm[1] = 1
continue
```

Wait at least 100 ms, interrupt with Ctrl-C, then confirm that both ready values
are one. Also record the worker IDs from `info threads`; use the raw hexadecimal
SceUID without a `0x` prefix in packets below. Do not proceed if either worker
is outside its corresponding path.

```gdb
print uvdb_worker_step_ready
info address uvdb_worker_step_path_0
info address uvdb_worker_step_path_1
disassemble /r uvdb_worker_step_path_0
disassemble /r uvdb_worker_step_path_1
info threads
thread <GDB-NUMBER-FOR-WORKER-0>
maintenance flush register-cache
p/x $pc
x/i $pc
info symbol $pc
thread <GDB-NUMBER-FOR-WORKER-1>
maintenance flush register-cache
p/x $pc
x/i $pc
info symbol $pc
```

The two disassemblies must have non-overlapping address ranges. Worker 0's PC
must fall within `uvdb_worker_step_path_0`'s displayed range, and worker 1's PC
must fall within `uvdb_worker_step_path_1`'s range. `info symbol` alone is not
sufficient proof because it reports the nearest symbol. First use `qC` to
identify the current exception thread, then exercise the foreign positive-`Hc`
fail-closed rule with a worker ID that is not that stopped thread:

```gdb
maintenance packet Hc<WORKER-0-HEX-SCEUID>
maintenance packet s
maintenance packet c
print uvdb_worker_step_ready
maintenance packet Hc-1
```

The initial foreign positive `Hc` and final `Hc-1` packets must return `OK`;
both legacy `s` and `c` must return `E16` without changing either ready value.
An exact positive `Hc` naming the current exception thread is different:
legacy `s` is its supported retained-token step-over path, while legacy `c`
still fails closed. Then step each foreign worker with the supported all-stop
action list. Record the numeric PC, current instruction, and CPSR immediately
before every step; after the stop, select that worker again and flush GDB's
register cache before reading its new PC:

```gdb
thread <GDB-NUMBER-FOR-WORKER-0>
maintenance flush register-cache
set $w0_vcont_before = $pc
p/x $w0_vcont_before
x/2i $pc
info registers cpsr
maintenance packet vCont;s:<WORKER-0-HEX-SCEUID>;c
maintenance packet qC
thread <GDB-NUMBER-FOR-WORKER-0>
maintenance flush register-cache
p/x $pc
x/i $pc
info symbol $pc
print uvdb_worker_step_ready
set $w0_hc0_before = $pc
p/x $w0_hc0_before
x/2i $pc
info registers cpsr
maintenance packet Hc0
maintenance packet s
maintenance packet qC
thread <GDB-NUMBER-FOR-WORKER-0>
maintenance flush register-cache
p/x $pc
x/i $pc
info symbol $pc
print uvdb_worker_step_ready
thread <GDB-NUMBER-FOR-WORKER-1>
maintenance flush register-cache
set $w1_vcont_before = $pc
p/x $w1_vcont_before
x/2i $pc
info registers cpsr
maintenance packet vCont;s:<WORKER-1-HEX-SCEUID>;c
maintenance packet qC
thread <GDB-NUMBER-FOR-WORKER-1>
maintenance flush register-cache
p/x $pc
x/i $pc
info symbol $pc
print uvdb_worker_step_ready
```

Each `vCont` must stop with `T05thread:<selected-id>;`. Once worker 0 is the
stopped thread, `Hc0` must return `OK` and its following legacy `s` must also
stop as `T05thread:<worker-0-id>;`. For each stop, use the complete function
disassembly, the pre-step instruction, CPSR, and the known zero release value to
calculate the decoder-expected one-instruction successor. The post-step numeric
PC must equal that address; merely remaining within the function or changing PC
is not sufficient. Worker 0 must remain within path 0, worker 1 must remain
within path 1, and every ready read must remain `{1, 1}`. Because the functions
have disjoint instruction addresses, the peer worker may run its own spin loop
but cannot win the selected worker's temporary breakpoint.

Release the fixture before detaching. After a short run and another Ctrl-C, all
arm and ready values must be zero and both result values must be nonzero. Then
detach normally.

```gdb
set var uvdb_worker_step_release[0] = 1
set var uvdb_worker_step_release[1] = 1
continue
# wait at least 100 ms, then press Ctrl-C
print uvdb_worker_step_arm
print uvdb_worker_step_ready
print/x uvdb_worker_step_result
detach
```

For a repeat run, reset both release values to zero before setting either arm
value. This fixture proves selected-PC calculation and stop attribution without
the known shared-code collision; it still does not prove scheduler-locked
single-thread execution.

If GDB exits or disconnects while the fixture is armed, the debugger watchdog
may resume the application but the two test workers intentionally remain in
their user-mode spin paths. Reconnect with the matching `test.elf`, write one to
both release values, continue briefly, interrupt, verify both arm and ready
arrays are zero, and detach. If the server cannot be reacquired, close and
relaunch the test application; the controls are BSS state and do not persist
outside that process.

## Live GDB gate

Use a test application with at least two named workers and the matching kernel
companion.

1. Attach, run `info threads`, and verify the reported stop thread is first,
   both workers are present once, unregistered process threads are visible, and
   the lease keeper is absent.
2. Select each worker and read registers/backtraces repeatedly. The selected
   thread must drive `g`; a nonexistent thread must be rejected without changing
   the previous selection.
3. Single-step the current stopped thread through both ARM and Thumb code using
   legacy `Hc0`/`s`, then repeat with GDB's `vCont` path. Exercise an ordinary
   persistent GDB software breakpoint twice and verify its automatic
   `z0`/positive-`Hc`/`s`/`Z0` step-over returns `T05` without `E16`. Confirm
   that legacy `c` after any positive `Hc`, and legacy `s` after a foreign or
   stale positive `Hc`, are rejected without resuming anything.
4. Arm the deterministic worker fixture and select each foreign worker in turn
   with `vCont;s:T;c`. The next PC must remain in that worker's distinct path
   and the stop reply must name the selected thread. A different stop thread or
   path is a failure for this fixture. Separately, treat a collision observed in
   ordinary shared application code as evidence for the stated isolation
   limitation rather than a passing selected-thread stop.
5. Continue, interrupt, detach, and reconnect. Repeat after terminating a GDB
   client without detaching. Application counters must resume, no breakpoint
   opcode may remain installed, and selectors from the old connection must not
   leak into the new one.
6. Run the existing abandoned-lease test and controlled renew/end failure
   fixtures. Verify re-reconciliation, breakpoint restoration before resume,
   bounded retry/abandon behavior, and client disconnection. Every target
   thread must resume within the watchdog bound and no UDF opcode may remain.

Record the GDB transcript, exact application/kernel hashes, firmware, and
whether each stop names the expected thread. Host tests and a VitaSDK cross-build
are necessary but do not replace this lifecycle gate.

## Phase 8: serialized failure and cleanup gate (not yet run)

Use one exclusive hardware window; do not run this gate concurrently with
another Vita workstream. Install the matching kernel plugin, reboot before
launching the matching application, and record SHA-256 for the exact SKPRX,
VPK, installed `eboot.bin`, and symbol ELF. Stop immediately on a reboot,
kernel panic, inability to reconnect, residual UDF bytes, a target thread
remaining suspended beyond the watchdog bound, or any artifact/hash mismatch.

The current build does not expose production renew/`EndStop` injection.
Therefore Phase 8 must not simulate those failures by guessing kernel APIs or
corrupting live tokens. First add a separately reviewed, test-build-only,
bounded kernel control that can fail exactly one renew or end operation while
preserving the normal watchdog and ownership bookkeeping. The control must be
absent from release builds and must report its consumed generation.

For each injected operation, capture monitor status and raw RSP traffic before
the injection, at disconnect, after the watchdog bound, after reconnect, and
after clean detach. Required evidence is: the injected generation is named;
the old generation cannot mark a replacement failed; late threads are included
before resume; original trap bytes are restored before ownership is released;
the application advances after watchdog recovery; restart accepts a new
client; and final status reports zero software breakpoints and no active stop
session. Recover by waiting for the watchdog, closing the client, reconnecting
once, and cleanly detaching. If that fails, terminate the application; only
after it exits may the operator disable the plugin entry and reboot.

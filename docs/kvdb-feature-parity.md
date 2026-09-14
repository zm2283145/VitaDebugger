# KVDB feature-parity target

VitaDebugger uses the public behavior documented by
[`cerwym/kvdb`](https://github.com/cerwym/kvdb) commit
`88742b760afa18cd11e251160d4c3b85357c30f1` as a compatibility checklist. The
goal is equivalent or safer developer-facing functionality, not a line-for-line
port or a requirement to duplicate KVDB's kernel-only architecture. No KVDB
source is copied into VitaDebugger.

This checklist deliberately separates implemented behavior from hardware-
validated behavior. A feature is not advertised to GDB until its failure,
cleanup, and restoration paths have passed on the named retail 3.65 test
configuration. Retail handheld Vita and Vita TV are the tested compatibility
targets; development hardware, other firmware releases, and other plugin
stacks remain unvalidated unless an individual result says otherwise.

| KVDB-documented capability | VitaDebugger status | Promotion gate |
| --- | --- | --- |
| GDB Remote Serial Protocol | Hardware tested over direct TCP | Harden packet parsing and remove blocking network work from the global debugger lock |
| 32 ARM/Thumb software breakpoints | Implemented; 32 slots | Add breakpoint-overlap, malformed-packet, disconnect-cleanup, and stress tests |
| Hardware data watchpoints (`Z2`/`Z3`/`Z4`) | Guarded encoder and kernel-session engine remain compile-time experimental; the staged retail ladder rebooted at DBGVCR, and a later audited runtime DIP 228 set/readback plus one-read A/B also rebooted without a final journal record; no comparator access is advertised | Keep fail-closed while researching boot-time policy, DSE/authentication, OS Lock, and debug-power state offline; only with a supported safe platform access path resume disabled-register, trap/restore, all-core/context-isolation, and watchdog gates |
| Hardware execution breakpoints (`Z1`) | Planned beyond KVDB's documented `Z2`-`Z4` handlers, but blocked by the same observed DSE-dependent register boundary | Require the same platform-access proof and staged safety gates, then advertise only the number of slots actually validated |
| ARM/Thumb software single-step | Broad practical decoder implemented. `Hc0` and `vCont;s:T;c` hardware-step deterministic foreign Thumb workers through distinct paths using state-dependent raw-bank selection. GDB's exact-stopped-thread positive-`Hc` breakpoint step-over now retains the all-stop token and passes live `T05`, detach, and reconnect tests; representative A32 `MOV PC`, `LDMDB {..., PC}`, and `LDR PC` steps also pass. Host coverage includes conditional branches, interworking, immediate/immediate-shifted A32 PC ALU operations, register/immediate PC loads, IT placement, alignment checks, and conservative rejection of unsafe forms | Add injected memory/trap-rollback tests and more live encoding fixtures; design scheduler-locked arbitrary-foreign-thread resume or displaced stepping; keep privileged exception returns, `BXJ`, register-controlled A32 shifts, exclusive-load sequences, and other unsafe forms fail-closed until independently modeled |
| VFP/NEON register reads | The guarded D0-D31/FPSCR kernel gate and automated foreign-thread live-GDB lifecycle gate pass on retail 3.65 | Keep the undocumented read path opt-in and revalidate each new firmware baseline; treat register writes as a separate mutation/restoration project |
| Prefetch abort, data abort, and undefined-instruction handling | Implemented with GDB signal and structured fault reporting | Add nested-fault containment and explicit previous-handler chaining |
| ARM register read/write | Strict `p` reads and selected exception-thread core/CPSR `P` writes pass the live retail 3.65 mutation/read-back/restore gate across clean detach/reconnect. The gate also confirms fail-closed foreign core and VFP writes with unchanged read-back. Full-register legacy writes predate this gate. Read-only foreign snapshots are hardware tested in runnable/current bank-0 and sleeping/syscall-return bank-1 states | Revalidate the transactional gate for each firmware/kernel ABI; add a separately restorable kernel mutation ABI before enabling foreign-thread or VFP writes |
| Application memory read/write | Implemented | Add strict syntax, overflow, page-boundary, and breakpoint-overlap validation plus fuzzing |
| Relocation/module information | `qOffsets`, chunk-safe `qXfer:libraries:read`, and the bounded fail-closed host loader now pass a five-session live main-plus-user-SUPRX source-breakpoint gate across detach/reconnect and two fresh-ASLR relaunches | Add deployed-artifact identity, dynamic module load/unload refresh, and automated IDE symbol regeneration |
| Thread enumeration, selection, names, and state | Hardware-tested discovery feeds a bounded kernel-authoritative inventory (with cooperative name annotations); exact `Hg`/`Hc`, `vCont;c;s`, fail-closed selection, and deterministic foreign-thread stop attribution pass live hardware tests | Complete controlled renew/end failure injection and long stress gates, then design isolated per-thread execution |
| Launch a target by path | Signed VitaDevDeploy can install and launch a build; arbitrary kernel-side attach/launch is not implemented | Finish deploy recovery tests, then add an IDE orchestration layer and optional unmodified-process attachment |
| Clean detach and reuse | Persistent detach/reconnect and abrupt-client recovery hardware tested | Complete long-duration multithread/fault-injection soaks |
| stdout to GDB console (`O` packets) | Completed bounded queue, single-owner no-ack transport, and restorable nonblocking stdout/stderr capture; the two-session retail 3.65 gate passed output, Ctrl-C, stopped-state silence, detach, and reconnect | Add longer pressure, abrupt-disconnect, restore/retry, and shutdown hardware soaks; keep DebugNet for sustained logging |
| Debugger monitor commands | Hardware-tested fixed read-only `qRcmd` registry for `help`, `status`, `threads`, and `modules`; bounded host tests plus two raw-RSP sessions and a real GDB 15.2 session passed on retail 3.65 with state preservation, clean detach, and reconnect | Keep the live gate reproducible and every future command explicit and read-only by default |
| Framebuffer/display diagnostics | Information path not implemented | Add a read-only `monitor display` equivalent without exposing unrestricted kernel memory |
| Cortex-A9 PMU counters | User-mode profiler foundation passes its first hardware probe; raw PMU ownership is not implemented | Inventory PMU state, define exclusive ownership/restoration, then add guarded cycle/event counters and profiler integration |
| UART or named-pipe transport | Not a core requirement because VitaDebugger has direct TCP and separate DebugNet UDP | Consider optional UART only if it materially helps recovery or kernel-plugin debugging |

## Implementation order

1. Keep the completed read-only VFP live-GDB path opt-in, archive its 3.65
   evidence, and revalidate it before supporting another firmware baseline.
2. Complete the remaining thread-control lifecycle gates. Unified inventory,
   honest `Hc`/`vCont` behavior, fail-closed selection, abandoned-client
   recovery, dynamic register-bank selection, and deterministic foreign-Thumb
   stepping are hardware tested. Exact-stopped-thread positive-`Hc` step-over
   and representative ARM-state PC writers also pass live GDB. Controlled
   renew/end failures, cleanup fault injection, arbitrary-foreign-thread
   scheduler locking, and longer stress runs remain.
3. Harden RSP parsing and move blocking socket operations outside the global
   debugger lock, with fake-transport tests and fuzzing.
4. Extend the completed bounded stdout/stderr `O`-packet path with long hardware
   soaks. Keep the completed read-only `qRcmd` registry bounded and its two-
   session `help`/`status`/`threads`/`modules`, state-preservation, detach, and
   reconnect hardware gate reproducible before extending it.
5. Keep the expanded ARM/Thumb decoder's host matrix, exact positive-`Hc`
   step-over, representative A32 live fixtures, and strict `p` plus selected-
   exception-thread core/CPSR `P` gates reproducible. Add injected memory-read,
   trap-install, and rollback coverage plus a bounded exclusive-sequence
   strategy before relaxing any remaining fail-closed instruction family.
   Expose foreign-thread or VFP writes only after a new
   snapshot/write/read-back/restore kernel gate passes.
6. Keep CP14 and memory-mapped hardware-debug access disabled. Read-only DIP
   inventory, exact runtime Set(228)/readback/Clear, and post-reboot restoration
   passed. The separately reviewed one-read A/B then rebooted after its durable
   `READ_PENDING` record and did not reach `COMPLETE`; recovery again found bits
   203/228 clear. The no-I/O critical window cannot journal the exact failing
   instruction, but the audited single-MRC path did not establish safe DBGVCR
   access and no comparator was touched. Research the earlier SKBL/boot-time
   consumer offline. Treat DEVTOOL identity only as a secondary control, not
   proof of access. Resume comparator restore/trap tests only after a supported
   safe DSE/authentication path passes every earlier gate. Develop page-
   protection/data-abort software watchpoints independently behind their own
   safety gates.
7. Extend the completed ASLR-aware main/user-SUPRX hardware workflow with build-
   identity attestation, dynamic module refresh, and IDE orchestration.
8. Add PMU ownership and restore gates, then connect the counters to both
   `monitor perf` and `libvitaprofiler`.
9. Complete IDE orchestration around build, signed deploy, launch, GDB, logs,
   profiles, stop, and recovery.

## Important architectural differences

VitaDebugger will keep its hybrid design: an application library handles GDB,
symbols, exceptions, and direct TCP while a narrow optional kernel companion
performs only operations that require kernel privilege. This keeps library-only
debugging available across more setups and limits the amount of persistent
kernel code.

If a supported hardware-debug path is ever established, hardware state is per
CPU core. VitaDebugger must therefore snapshot, program, verify, and restore
each application core, scope comparators to the target process context, refuse
to coexist with unknown enabled comparators in the first implementation, and
use a short ownership lease. Those safeguards remain mandatory promotion gates;
the current retail build does not acquire or advertise comparator access.

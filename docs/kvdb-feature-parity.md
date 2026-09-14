# KVDB feature-parity target

VitaDebugger uses the public behavior documented by
[`cerwym/kvdb`](https://github.com/cerwym/kvdb) commit
`88742b760afa18cd11e251160d4c3b85357c30f1` as a compatibility checklist. The
goal is equivalent or safer developer-facing functionality, not a line-for-line
port or a requirement to duplicate KVDB's kernel-only architecture. No KVDB
source is copied into VitaDebugger.

This checklist deliberately separates implemented behavior from hardware-
validated behavior. A feature is not advertised to GDB until its failure,
cleanup, and restoration paths have passed on the project's retail Vita running
system software 3.65. Those results are a single-target baseline, not evidence
for Vita TV, development hardware, another firmware, or another plugin stack.

| KVDB-documented capability | VitaDebugger status | Promotion gate |
| --- | --- | --- |
| GDB Remote Serial Protocol | Hardware tested over direct TCP | Harden packet parsing and remove blocking network work from the global debugger lock |
| 32 ARM/Thumb software breakpoints | Implemented; 32 slots | Add breakpoint-overlap, malformed-packet, disconnect-cleanup, and stress tests |
| Hardware data watchpoints (`Z2`/`Z3`/`Z4`) | Guarded encoder and kernel-session engine remain compile-time experimental; the staged retail ladder rebooted at DBGVCR, and a later audited runtime DIP 228 set/readback plus one-read A/B also rebooted without a final journal record; no comparator access is advertised | Keep fail-closed while researching boot-time policy, DSE/authentication, OS Lock, and debug-power state offline; only with a supported safe platform access path resume disabled-register, trap/restore, all-core/context-isolation, and watchdog gates |
| Hardware execution breakpoints (`Z1`) | Planned beyond KVDB's documented `Z2`-`Z4` handlers, but blocked by the same observed DSE-dependent register boundary | Require the same platform-access proof and staged safety gates, then advertise only the number of slots actually validated |
| ARM/Thumb software single-step | Broad decoder implemented and hardware tested for many common control-flow forms; `Hc0` and `vCont;s:T;c` now hardware-step selected foreign Thumb workers through distinct paths using state-dependent raw-bank selection | Hardware-test the selected ARM-state path, add scheduler-locked isolation or displaced stepping, and finish remaining PC-writing forms |
| VFP/NEON register reads | D0-D31/FPSCR layout and kernel snapshot gate pass | Complete live GDB read, continue, detach, reconnect, and restoration validation |
| Prefetch abort, data abort, and undefined-instruction handling | Implemented with GDB signal and structured fault reporting | Add nested-fault containment and explicit previous-handler chaining |
| ARM register read/write | Current exception-thread read/write plus hardware-tested read-only foreign snapshots in runnable/current bank-0 and sleeping/syscall-return bank-1 states | Add strict `p`/`P` packets and independently validate foreign-thread mutation/restoration |
| Application memory read/write | Implemented | Add strict syntax, overflow, page-boundary, and breakpoint-overlap validation plus fuzzing |
| Relocation/module information | `qOffsets` plus chunk-safe `qXfer:libraries:read` implemented; module discovery hardware tested | Automate symbol loading for matching unstripped modules |
| Thread enumeration, selection, names, and state | Hardware-tested discovery feeds a bounded kernel-authoritative inventory (with cooperative name annotations); exact `Hg`/`Hc`, `vCont;c;s`, fail-closed selection, and deterministic foreign-thread stop attribution pass live hardware tests | Complete controlled renew/end failure injection and long stress gates, then design isolated per-thread execution |
| Launch a target by path | Signed VitaDevDeploy can install and launch a build; arbitrary kernel-side attach/launch is not implemented | Finish deploy recovery tests, then add an IDE orchestration layer and optional unmodified-process attachment |
| Clean detach and reuse | Persistent detach/reconnect and abrupt-client recovery hardware tested | Complete long-duration multithread/fault-injection soaks |
| stdout to GDB console (`O` packets) | Fixed-memory generation-scoped queue and `O`-payload encoder pass host tests and VitaSDK cross-build; current file-I/O bridge remains experimental and is not wired to them | Single-owner no-ack RSP integration, restorable nonblocking stdout/stderr capture, fake-socket/real-GDB reconnect tests, and proof that target writers never wait on GDB |
| Debugger monitor commands | Not yet exposed through `qRcmd` | Add a read-only command registry beginning with `help`, `threads`, `modules`, and debugger status |
| Framebuffer/display diagnostics | Information path not implemented | Add a read-only `monitor display` equivalent without exposing unrestricted kernel memory |
| Cortex-A9 PMU counters | User-mode profiler foundation passes its first hardware probe; raw PMU ownership is not implemented | Inventory PMU state, define exclusive ownership/restoration, then add guarded cycle/event counters and profiler integration |
| UART or named-pipe transport | Not a core requirement because VitaDebugger has direct TCP and separate DebugNet UDP | Consider optional UART only if it materially helps recovery or kernel-plugin debugging |

## Implementation order

1. Complete the read-only VFP live-GDB lifecycle gate.
2. Complete the remaining thread-control lifecycle gates. Unified inventory,
   honest `Hc`/`vCont` behavior, fail-closed selection, abandoned-client
   recovery, dynamic register-bank selection, and deterministic foreign-Thumb
   stepping are hardware tested; selected ARM-state stepping, controlled
   renew/end failures, cleanup fault injection, and longer stress runs remain.
3. Harden RSP parsing and move blocking socket operations outside the global
   debugger lock, with fake-transport tests and fuzzing.
4. Add bounded stdout/stderr `O`-packet delivery and the read-only monitor
   command framework.
5. Finish remaining ARM/Thumb software-step decoding and strict `p`/`P`
   register access; expose foreign-thread writes only after independent
   mutation and restoration validation.
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
7. Complete ASLR-aware module symbol loading and relocated-breakpoint tests.
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

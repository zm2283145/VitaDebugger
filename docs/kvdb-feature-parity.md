# KVDB feature-parity target

VitaDebugger uses the public behavior documented by
[`cerwym/kvdb`](https://github.com/cerwym/kvdb) commit
`88742b760afa18cd11e251160d4c3b85357c30f1` as a compatibility checklist. The
goal is equivalent or safer developer-facing functionality, not a line-for-line
port or a requirement to duplicate KVDB's kernel-only architecture. No KVDB
source is copied into VitaDebugger.

This checklist deliberately separates implemented behavior from hardware-
validated behavior. A feature is not advertised to GDB until the corresponding
failure, cleanup, and restoration paths have passed on a retail Vita.

| KVDB-documented capability | VitaDebugger status | Promotion gate |
| --- | --- | --- |
| GDB Remote Serial Protocol | Hardware tested over direct TCP | Harden packet parsing and remove blocking network work from the global debugger lock |
| 32 ARM/Thumb software breakpoints | Implemented; 32 slots | Add breakpoint-overlap, malformed-packet, disconnect-cleanup, and stress tests |
| Hardware data watchpoints (`Z2`/`Z3`/`Z4`) | Guarded encoder and kernel-session engine remain compile-time experimental; the staged retail ladder passed DIDR/DSCRint but rebooted at the first DBGVCR read on core 1, so no comparator access is advertised | Keep fail-closed while researching DSE/authentication, OS Lock, and debug-power state offline; only with a supported safe platform access path resume disabled-register, trap/restore, all-core/context-isolation, and watchdog gates |
| Hardware execution breakpoints (`Z1`) | Planned beyond KVDB's documented `Z2`-`Z4` handlers, but blocked by the same observed DSE-dependent register boundary | Require the same platform-access proof and staged safety gates, then advertise only the number of slots actually validated |
| ARM/Thumb software single-step | Broad decoder implemented and hardware tested for many common control-flow forms | Finish remaining PC-writing forms and multithread/per-thread step behavior |
| VFP/NEON register reads | D0-D31/FPSCR layout and kernel snapshot gate pass | Complete live GDB read, continue, detach, reconnect, and restoration validation |
| Prefetch abort, data abort, and undefined-instruction handling | Implemented with GDB signal and structured fault reporting | Add nested-fault containment and explicit previous-handler chaining |
| ARM register read/write | Current exception-thread read/write plus read-only foreign-thread snapshots | Add strict `p`/`P` packets and independently validate foreign-thread mutation/restoration |
| Application memory read/write | Implemented | Add strict syntax, overflow, page-boundary, and breakpoint-overlap validation plus fuzzing |
| Relocation/module information | `qOffsets` plus chunk-safe `qXfer:libraries:read` implemented; module discovery hardware tested | Automate symbol loading for matching unstripped modules |
| Thread enumeration, selection, names, and state | Cooperative and kernel-assisted discovery hardware tested | Unify bookkeeping and complete `Hc`/`vCont` and per-thread stepping |
| Launch a target by path | Signed VitaDevDeploy can install and launch a build; arbitrary kernel-side attach/launch is not implemented | Finish deploy recovery tests, then add an IDE orchestration layer and optional unmodified-process attachment |
| Clean detach and reuse | Persistent detach/reconnect and abrupt-client recovery hardware tested | Complete long-duration multithread/fault-injection soaks |
| stdout to GDB console (`O` packets) | Fixed-memory generation-scoped queue and `O`-payload encoder pass host tests and VitaSDK cross-build; current file-I/O bridge remains experimental and is not wired to them | Single-owner no-ack RSP integration, restorable nonblocking stdout/stderr capture, fake-socket/real-GDB reconnect tests, and proof that target writers never wait on GDB |
| Debugger monitor commands | Not yet exposed through `qRcmd` | Add a read-only command registry beginning with `help`, `threads`, `modules`, and debugger status |
| Framebuffer/display diagnostics | Information path not implemented | Add a read-only `monitor display` equivalent without exposing unrestricted kernel memory |
| Cortex-A9 PMU counters | User-mode profiler foundation passes its first hardware probe; raw PMU ownership is not implemented | Inventory PMU state, define exclusive ownership/restoration, then add guarded cycle/event counters and profiler integration |
| UART or named-pipe transport | Not a core requirement because VitaDebugger has direct TCP and separate DebugNet UDP | Consider optional UART only if it materially helps recovery or kernel-plugin debugging |

## Implementation order

1. Complete the read-only VFP live-GDB lifecycle gate.
2. Unify cooperative and kernel thread inventory with one stop-session state
   model; complete honest `Hc`/`vCont` behavior and fail-closed lease cleanup.
3. Harden RSP parsing and move blocking socket operations outside the global
   debugger lock, with fake-transport tests and fuzzing.
4. Add bounded stdout/stderr `O`-packet delivery and the read-only monitor
   command framework.
5. Finish remaining ARM/Thumb software-step decoding and strict `p`/`P`
   register access; expose foreign-thread writes only after independent
   mutation and restoration validation.
6. Keep CP14 and memory-mapped hardware-debug access disabled. The staged
   ladder rebooted at its first DBGVCR read before any comparator access. First
   inventory KBL DIP switch 228 read-only; only after review, validate its
   documented runtime setter with an exact set/readback/restore rung that does
   not touch CP14. A separately approved, journaled DBGVCR A/B may follow only
   if that rung passes. If runtime timing is too late, statically trace the SKBL
   consumer before considering a bit-only boot-time test. DEVTOOL identity is a
   secondary control, not proof of access. Resume comparator restore/trap tests
   only after a supported safe DSE/authentication path passes every earlier
   gate. Develop page-protection/data-abort software watchpoints independently
   behind their own safety gates.
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

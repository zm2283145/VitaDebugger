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
| ARM/Thumb software single-step | Broad practical decoder implemented. `Hc0` and `vCont;s:T;c` hardware-step deterministic foreign Thumb workers through distinct paths using state-dependent raw-bank selection. GDB's exact-stopped-thread positive-`Hc` hidden-breakpoint step-over passes without `E16`; representative A32 PC writers and bounded Thumb-2/A32 `LDREX`-through-`STREX` sequences also pass live GDB. The [retail 3.65 practical-gate record](hardware/gdb-step-register-exclusive-3.65.json) includes successful single stores and exact trap-byte restoration after abrupt disconnect. Host coverage includes conditional branches, interworking, immediate/immediate-shifted A32 PC ALU operations, register/immediate PC loads, IT placement, alignment checks, and conservative rejection of unsafe forms | Add injected memory/trap-rollback tests and more live encoding fixtures; design scheduler-locked arbitrary-foreign-thread resume or displaced stepping; keep privileged exception returns, `BXJ`, register-controlled A32 shifts, unsupported exclusive sequences, and other unsafe forms fail-closed until independently modeled |
| VFP/NEON register reads | The guarded D0-D31/FPSCR kernel gate and automated foreign-thread live-GDB lifecycle gate pass on retail 3.65 | Keep the undocumented read path opt-in and revalidate each new firmware baseline; treat register writes as a separate mutation/restoration project |
| Prefetch abort, data abort, and undefined-instruction handling | Implemented with GDB signal and structured fault reporting | Add nested-fault containment and explicit previous-handler chaining |
| ARM register read/write | Strict `p` reads and selected exception-thread R0/CPSR `P` writes pass live retail 3.65 mutation/read-back/exact-restoration before resume or detach, including the [combined practical-gate record](hardware/gdb-step-register-exclusive-3.65.json). The gate also confirms fail-closed foreign core and VFP writes with unchanged read-back. Full-register legacy writes predate this gate. Read-only foreign snapshots are hardware tested in runnable/current bank-0 and sleeping/syscall-return bank-1 states. A separately versioned, one-bank kernel mutation transaction with retained-target lifetime and exact rollback is host-tested, while its default Vita backend exposes zero writable banks | Revalidate the existing live gate for each firmware/kernel ABI; connect the transaction only to a supported setter plus durable target-object provider, then pass target/process exit, UID churn, forced inventory failure, rollback, and lease-cleanup hardware gates before enabling foreign-thread writes |
| Application memory read/write | Implemented | Add strict syntax, overflow, page-boundary, and breakpoint-overlap validation plus fuzzing |
| Relocation/module information | ASLR correctness and verified-build identity are complete. `qOffsets`, chunk-safe `qXfer:libraries:read`, installed main/SUPRX hashing, mismatch rejection before RSP, idempotent refresh, and five main-plus-user-SUPRX source-breakpoint sessions across reconnect and two relaunches pass in the [retail 3.65 identity record](hardware/gdb-aslr-build-identity-3.65.json) | Stress same-process hot module load/unload churn and automate the existing command-driven refresh in IDE tasks as convenience integration, not as an ASLR correctness gate |
| Thread enumeration, selection, names, and state | Hardware-tested discovery feeds a bounded kernel-authoritative inventory (with cooperative name annotations); exact `Hg`/`Hc`, `vCont;c;s`, fail-closed selection, and deterministic foreign-thread stop attribution pass live hardware tests | Complete controlled renew/end failure injection and long stress gates, then design isolated per-thread execution |
| Launch a target by path | Signed VitaDevDeploy can install and launch a build; arbitrary kernel-side attach/launch is not implemented | Finish deploy recovery tests, then add an IDE orchestration layer and optional unmodified-process attachment |
| Clean detach and reuse | Persistent detach/reconnect and abrupt-client recovery hardware tested | Complete long-duration multithread/fault-injection soaks |
| stdout to GDB console (`O` packets) | Completed bounded queue, single-owner no-ack transport, restorable nonblocking stdout/stderr capture, and read-only `monitor console` queue/transport/loss statistics; the two-session retail 3.65 gates passed output, Ctrl-C, stopped-state silence, detach, reconnect, and counter inspection, with the monitor result archived in the [console/display record](hardware/gdb-monitor-console-display-3.65.json) | Add longer pressure, abrupt-disconnect, restore/retry, and shutdown hardware soaks; keep DebugNet for sustained logging |
| Debugger monitor commands | Hardware-tested fixed read-only `qRcmd` registry for `help`, `status`, `threads`, `modules`, `console`, and `display`; bounded host tests, two raw-RSP sessions, and GDB 15.2 passed on retail 3.65 with state preservation, clean detach, and reconnect in the [console/display record](hardware/gdb-monitor-console-display-3.65.json) | Keep every future command explicit, bounded, and read-only by default |
| Framebuffer/display diagnostics | Read-only `monitor display` metadata is host- and hardware-tested without a client-selected address or pixel read; the retail 3.65 gate reported coherent current/next 960x544 A8B8G8R8 buffers, 59.940 Hz, and advancing vcount | Keep pixel capture outside the monitor registry; add lifecycle stress and consume these diagnostics in the VitaDevDeploy display investigation |
| Cortex-A9 PMU counters | The user-mode profiler foundation and bounded name dictionary pass all 13 retail 3.65 checks. Both retail user-mode ScePerf loading paths failed, so direct initialization fails closed. Kernel ABI v1.13 read-only inventory passes, the [isolated session gate](hardware/profiler-pmu-session-gate-3.65.md) passed fixed lane-5 software increment on application cores 0-2, all three allowlisted real events passed separate bounded core-0 normal-close gates with exact restoration ([`0x01`](../kernel/pmu-profiler-gate/hardware-results/2026-09-15-event-01/README.md), [`0x03`](../kernel/pmu-profiler-gate/hardware-results/2026-09-15-event-03/README.md), [`0x10`](../kernel/pmu-profiler-gate/hardware-results/2026-09-15-event-10/README.md)), and a real-world VitaGL-based application passed a 75-read `0x01` lease with clean close. The default-off [dormant-thread gate](../kernel/pmu-profiler-thread-exit-gate/hardware-results/2026-09-15-first-attempt/README.md) also proves same-boot safe re-arm after owner-thread exit; UID/object mismatch and uncertain reference release permanently quarantine rather than guess | Hardware-gate ownership conflict, timeout/watchdog, whole-process exit/crash, and receiver disconnect before advertising unrestricted PMU counters |
| UART or named-pipe transport | Not a core requirement because VitaDebugger has direct TCP and separate DebugNet UDP | Consider optional UART only if it materially helps recovery or kernel-plugin debugging |

## Implementation order

1. Finish controlled thread renew/end failure injection and long multithreaded
   soaks, then add scheduler-locked arbitrary-foreign-thread execution or
   displaced stepping.
2. Move blocking work outside the debugger lock, harden remaining core and
   packet/memory/register parsers, contain nested faults, chain prior handlers,
   and expand fake-transport, fake-kernel, and fuzz coverage.
3. Run long console pressure, abrupt-disconnect, restore/retry, and shutdown
   hardware soaks.
4. Connect the host-tested restorable kernel mutation ABI to a supported setter
   and durable target-object provider, then pass process/thread exit, UID churn,
   forced inventory failure, rollback, and lease-cleanup hardware gates. Add
   injected-memory/trap-rollback gates before enabling privileged exception
   returns, unsupported exclusive forms, or other unsafe instruction families.
5. Continue offline hardware-debug access research while `Z1`-`Z4` stays
   disabled, and independently gate a page-protection/data-abort software
   watchpoint design.
6. Stress same-process hot module load/unload churn and automate the existing
   command-driven refresh in IDE tasks. This is dynamic-module/IDE convenience
   work; ASLR and verified-build correctness are complete.
7. Continue hardware-gating the versioned PMU/provider transport. The lane-5
   event-`0x00` scope and all three bounded allowlisted real events now pass
   with exact restoration; a real-world application lease also passed 75
   bounded `0x01` reads and clean close. The dormant-owner-thread safe-rearm
   gate passes; next gate ownership conflicts and every remaining cleanup path.
   The Vita TCP sink and both 300-frame real-world VitaGL captures pass. The
   dependency-free desktop GUI now uses the working receiver/analyzer/Perfetto
   pipeline; next add deeper source-owned graphics timing and live timeline
   framing beyond the EOF-delimited wire-v1 capture.
8. Complete authentication/pairing, peer allowlists, packaging, CI/firmware
   coverage, licensing, and release hardening.
9. Hardware-stress the VitaDevDeploy GPU lifecycle fix and finish interrupted-
   install and bootstrap-recovery fault injection.
10. Turn the strict read-only external-attach protocol and allocation-free broker
    foundation into a reviewed authenticated listener with a trusted Vita
    process-identity provider; only then design process control, injection, and
    live GDB attachment for applications not compiled with the library.
11. Validate LLDB remote-protocol behavior and add a Debug Adapter Protocol
    bridge without regressing GDB.

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

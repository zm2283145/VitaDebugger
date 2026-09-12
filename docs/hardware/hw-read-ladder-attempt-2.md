# Staged hardware-debug read ladder: attempt 2

Date: 2026-09-12

This disposable probe separated the register sequence from the first
disabled-comparator attempt into one strictly ordered operation per deliberate
X-button press. The first five rungs completed. The Vita rebooted on rung 6,
the first `DBGVCR` read. Rungs 7-10 were not run.

## Exact artifact

- Source commit: `15a4d54`
- Title ID: `VDCP00004`
- VPK SHA-256:
  `5FA4008166BD35C83C9DD4DB139CE8FF887A378A0E6E5C558D427C2AF2B5CD0D`
- Embedded SKPRX SHA-256:
  `983699B77C5CC208ED8E69EDFE3B94BF31AB47B9045F6024C16267352AC77FB5`
- VitaDevDeploy job: `630aa4fb4a3df6635291d5fb90c3737b`
- Signed deployment manifest SHA-256:
  `EF8064D124C43FC73BD7FAA31F7138FD21E40366B3259331BE5E094023FFF826`

The machine-code audit found seven CP14 `MRC` instructions, one CP15 MIDR
`MRC`, zero CP14 `MCR` instructions, and zero VFP/NEON instructions. The
private kernel module has no exports, hooks, callbacks, worker thread, boot
configuration entry, or resident service.

## Results

| Rung | Operation | Result | Value | Core |
| ---: | --- | --- | --- | ---: |
| 1 | Kernel lifecycle/no-op | Complete | `0x4C494645` | 2 |
| 2 | CPU-ID API | Complete | `0x00000001` | 1 |
| 3 | MIDR via CP15 | Complete | `0x412FC09A` | 1 |
| 4 | DIDR via CP14 | Complete | `0x3513702A` | 2 |
| 5 | DSCRint via CP14 | Complete | `0x03040002` | 2 |
| 6 | DBGVCR via CP14 | Reboot after durable kernel-entry record | None | 1 |
| 7 | BCR0 | Not run | - | - |
| 8 | BVR0 | Not run | - | - |
| 9 | WCR0 | Not run | - | - |
| 10 | WVR0 | Not run | - | - |

Every completed rung had a valid two-slot checksum/revision sequence, exact
completion flags, and a matching expected core. The lifecycle rung exercised
the same short interrupt guard without a register access. Rungs 1-5 therefore
validated the dynamic module, durable journal, scheduler-migration guard, and
completion path immediately before the rebooting rung.

## Recovered rung-6 journal

Both slots were recovered over FTP after the Vita returned to LiveArea:

- Slot A SHA-256:
  `EEC7DC27C8ECADB5A64E06DA798D26777290024C0FEC494B68E2BF8E4B87BECF`
- Slot B SHA-256:
  `73C06FA323891B570EBD19ED5E764FFAA4BA9645EB1E62570256156A60F9834B`

Slot B is the valid loader-attempt record at revision 16. Slot A is the newest
valid record at revision 17:

- Sequence: 6
- Step: `DBGVCR`
- State: `KERNEL_ENTERED`
- Result: `NOT_RUN` (`-100`)
- Flags: `0x00000006` (`CP14`, general-register-only build)
- Expected core: 1
- Value: unset

The interrupt-guard and core-match bits are added in RAM after this durable
entry record, so their absence in the recovered record is expected. It means
the journal cannot distinguish the `ksceKernelCpuSuspendIntr()` call, the core
recheck, and the following `DBGVCR` instruction in isolation. However, that
identical guard completed on all five preceding rungs, including immediately
before this attempt. The single new `DBGVCR` read is therefore the strongly
supported reboot trigger.

## Interpretation and stop decision

The observed boundary matches the ARMv7 access-control grouping in which
baseline `DBGDIDR` and `DBGDSCRint` reads remain available while `DBGVCR` and
the breakpoint/watchpoint comparator registers are UNDEFINED through the
extended CP14 interface. A low Debug Software Enable/`DBGSWENABLE` input is the
leading explanation, but OS Lock or debug power/state policy must not be
excluded by this experiment. See the
[ARMv7-A/R Architecture Reference Manual](https://documentation-service.arm.com/static/5f8daeb7f86e16515cdb8c4e)
and the
[Cortex-A9 Technical Reference Manual](https://documentation-service.arm.com/static/5f035b2ddbdee951c1cd7172).

This result establishes that the current privileged CP14 path cannot read the
DSE-dependent debug registers on CPU core 1 in this Vita's current platform
state. DIDR and DSCRint completed on core 2, so this run does not establish the
state of DBGVCR on cores 0 or 2. One inaccessible application core is still
enough to block reliable all-core hardware-debug support. The result does not
distinguish a hardware-tied authentication input, OS Lock, sticky debug-power
state, secure firmware, or a system peripheral controlling those inputs. It
also does not prove that all Vita hardware or firmware configurations behave
identically.

The probe is intentionally stopped and locked at rung 6. No comparator read,
comparator write, debug-enable write, or enabled breakpoint/watchpoint was
attempted. Rungs 7-10, memory-mapped debug access, and additional debug-status
register probes must not be run unless separate platform evidence first
establishes a supported safe access path. The experimental resident engine
remains compile-time disabled, and GDB must not advertise hardware `Z1`-`Z4`
support from this result.

The first monolithic probe most likely rebooted at its earlier `DBGVCR` read,
before reaching any comparator write, because it used the same ordering. That
is a supported inference rather than something its less granular journal could
prove directly.

Software breakpoints, software stepping, exception reporting, profiling, and
deployment do not depend on this hardware path. A page-protection/data-abort
watchpoint design is also a possible retail fallback, but it needs separate
MMU ownership, access-decoding, page-contention, rearm, and recovery gates
before it can be exposed through GDB.

# Staged hardware-debug read ladder: attempt 2

Date: 2026-09-12

This disposable probe separated the register sequence from the first
disabled-comparator attempt into one strictly ordered operation per deliberate
X-button press. The first five rungs completed. The Vita rebooted on rung 6,
the first `DBGVCR` read. Rungs 7-10 were not run.

> **Hardware baseline:** the owner later confirmed that this test device was a
> retail PS Vita running system software 3.65. The firmware value was not
> embedded in this artifact's journal or screenshots, so this is later device
> metadata rather than contemporaneous probe output. Do not generalize this
> result to another firmware or device class.

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

## 2026-09-14 research addendum

Debug-power, authorization, monitor-mode, and Vita-specific exception policy
remain worth static investigation. The Cortex-A9 TRM marks the OS Lock
registers RAZ/WI and unimplemented on this core, so OS Lock is no longer a sound
primary explanation. ARM Cortex-A9 erratum 764319 also says that reads of both
`DBGPRSR` and `DBGOSLSR` can themselves raise Undefined Instruction when
`DBGSWENABLE` is low. Upstream Linux therefore protects its generic OS-Lock
probe with an Undefined Instruction hook on affected CPUs. The existing ladder
remains locked; neither register is authorized for a live read until a Vita-
specific, unload-safe 3.65 exception recovery path has passed independent
lifecycle tests. See the
[full research review](research-review-2026-09-14.md).

## KBL DIP-switch 228 lead

After this run, the HENkaku KBL documentation was found to name global DIP
switch `0xE4`/228 `SYSTEM_FLAG_ENABLE_HW_BREAKPOINTS`. It records use by SKBL,
SceProcessmgr, and SceKernelThreadMgr, making this the strongest direct software
lead for the observed boundary. VitaSDK exposes read, set, and clear operations
for indexed DIP switches.

The value is a bit index, not a KBL byte offset. It maps to mask `0x10` in
`SceDipsw::system_control_flags`, which is at KBL offset `0x5C`. The separate
unknown KBL field at offset `0xE4` is unrelated and must not be written.

The bit's name and consumers are strong correlation, not proof that it directly
raises `DBGSWENABLE`. The wiki documents cache-only behavior for user-facing
setters on bits 0-63; it does not prove the hardware-side semantics of the
kernel setter for bit 228. In either case, a normal kernel plugin runs only
after SKBL may already have configured or denied an underlying debug device. A
new process may also be required for ProcessMgr to allocate its breakpoint-
related context. The staged read-only inventory, runtime
set/readback/restore gate, possible bit-only boot-time test, and explicit stop
conditions are documented in the
[KBL DIP-switch 228 report](dipsw-228-hw-debug.md).

The KBL fallback notes also say SKBL force-sets bit 228 for TEST/TOOL-class
units under a particular early fallback condition. This supports a relationship
between real devtool identity and the flag, but Enso_ex's public boot-manager
spoof runs later during NSKBL. It cannot be assumed to replay that SKBL path.

## Boot-time device-type spoof hypothesis

After this run, the public Enso_ex and Miaki sources were reviewed to determine
whether a retail-to-development-unit identity change is a supported way to raise
`DBGSWENABLE`. This is a source audit only; no device-type change was made and
the failed ladder was not retried.

Enso_ex's `lv0-typespoof` payload is more than a display-name spoof. It defines
retail (`0x0301`), DEX/testkit (`0x0201`), devtool (`0x0101`), and internal
system-debugger/TEST (`0x0001`) device types. Its lv0 payload, invoked through
the secure-monitor framework, copies keyslot `0x509`, substitutes the selected
16-bit type, asks the keyring program to rewrite that slot, and then mirrors the
type into both known kernel boot-argument copies. The example boot manager
currently calls `set_type(TEST)`.
See the
[Enso_ex type-spoof source](https://github.com/SKGleba/enso_ex/tree/bfedbd877994dd414a0b89e4efb8110d3a40c10a/bootmgr/lv0-typespoof).

Miaki takes a substantially more invasive route: it installs matching DevKit
firmware and supplies separate early kernel modules that scan mapped memory for
a product-code signature and overwrite an eight-byte record with a variant-
specific constant. Its optional DevMode module hooks four software policy
queries for development mode, screenshot
permission, debug-menu display, and a Vita-TV identity check. No explicit
`DBGSWENABLE`, CP14 debug-authentication, or comparator-control implementation
was found in the reviewed revisions. Miaki also documents that several debug
settings still crash on retail hardware and that Neighborhood remains
unavailable because retail units lack required hardware. See the
[Miaki source and limitations](https://github.com/cem-3000ve1/Miaki/tree/12ae9ff761ef14c7cd06c7fefc265de9e489b791).

This leaves a plausible but unproven secondary branch: secure Sony firmware
might drive the external per-core `DBGSWENABLE` input from the boot-time device
type, in which case an Enso_ex identity change could change the result.
DEVTOOL/devkit (`0x0101`) is the closest target for the first A/B test;
DEX/testkit (`0x0201`) is retail-like hardware and is useful only as a secondary
control. The signal
might instead be fixed by a fuse or board signal, controlled by Syscon or
another secure component, or latched before the type-spoof payload executes.
The Enso_ex boot-manager timing means it is already too late to exercise SKBL's
documented TEST/TOOL fallback, although it remains early enough to affect later
ProcessMgr and ThreadMgr consumers. The reviewed source cannot distinguish the
remaining cases. Miaki adds no demonstrated control for this signal and is not
justified for this experiment.

If this hypothesis is reached after the narrower bit-228 path, use a separate
read-only diagnostic title and a fresh journal under an already-recoverable
Enso_ex DEVTOOL boot. Repeat the
known-good lifecycle/MIDR/DIDR/DSCRint baseline and then perform only the single
DBGVCR read. A later DEX run may be used as a secondary control. Record the exact
Enso_ex payload hash, selected device type, firmware, and core. Do not unlock the
existing rung-6 journal, install Miaki, or proceed to comparator/MMIO access
merely because the UI reports a development-unit type.
The complete backup, test, and exact-restoration procedure is recorded in the
[Enso_ex DEVTOOL A/B runbook](enso-ex-devtool-ab.md).

## Register-probing safety boundary

The observed CP14 failure was volatile on this run: the Vita rebooted and
returned to LiveArea, with the ordinary risk of losing unsaved data. That does
not guarantee the same recovery for another access and does not make arbitrary
register probing safe. Unknown MMIO reads can acknowledge or clear device
state, or stall on an unclocked bus. Unknown writes can reach clock, reset,
power, storage, Syscon, security, OTP, or eFuse controls and can cause
persistent corruption, an unrecoverable boot failure, electrical stress, or
permanent damage. A power cycle is therefore not a guaranteed recovery
strategy.

`DBGSWENABLE` is an external Cortex-A9 input, not a writable bit in DBGVCR or
another standard CP14 register. ARM ADIv5 defines an optional external MEM-AP
`CSW.DbgSwEnable` field whose use is implementation-defined, but the reviewed
Vita sources establish neither an accessible CoreSight DAP path nor a known
CPU-side/on-device control. Any Vita path must be identified from a trusted
implementation or static reverse-engineering evidence, then exercised as one
exact read/modify/restore transaction with barriers and readback. Blind address
scans, guessed MMIO writes, broad bit-flipping, and writes to persistent,
security, or power domains are outside this project's hardware test boundary.
See the
[ARM Debug Interface ADIv5 specification](https://documentation-service.arm.com/static/622222b2e6f58973271ebc21).

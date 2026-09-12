# KBL DIP switch 228 hardware-debug lead

Status: source-audit finding; no Vita state, boot files, or kernel plugin were
changed as part of this review.

## Finding

The HENkaku KBL parameter documentation names DIP switch `0xE4` (decimal 228)
`SYSTEM_FLAG_ENABLE_HW_BREAKPOINTS` and describes it as enabling hardware
break/watch points. It records consumers in SKBL, SceProcessmgr, and
SceKernelThreadMgr; SKBL appears to enable or disable unidentified devices, and
ThreadMgr copies breakpoint-related process information into thread objects.

This is the strongest Vita-specific software-control lead found after the
retail test rebooted at its first `DBGVCR` read. It does **not** yet prove that
the bit directly raises the Cortex-A9 `DBGSWENABLE` input or makes comparator
access safe.

Sources:

- [HENkaku KBL parameter documentation, revision 21617](https://wiki.henkaku.xyz/index.php?title=KBL_Param&oldid=21617)
- [HENkaku SceProcessmgr structure notes, revision 21673](https://wiki.henkaku.xyz/index.php?title=SceProcessmgr&oldid=21673)
- [HENkaku platform-type notes](https://wiki.henkaku.xyz/vita/Platform_type)
- [VitaSDK kernel DIP-switch API](https://docs.vitasdk.org/group__SceDipswKernel.html)
- [VitaSDK KBL parameter structure](https://docs.vitasdk.org/group__SceKblKernel.html)
- [HENkaku revision 21617 enum transcription](https://gist.github.com/ntpopgetdope/9afcca51d242a34bcafe978539ef0a74)

The last source is a convenience transcription of the cited HENkaku wiki
revision, not independent confirmation of the firmware behavior.

## Exact mapping

`0xE4` is the global **DIP-switch bit index**, not a byte offset in
`SceKblParam`.

| Item | Value |
| --- | ---: |
| Global DIP-switch index | `228` / `0xE4` |
| DIP-switch word index | `228 / 32 = 7` |
| Bit within system-control word | `228 - 224 = 4` |
| Mask in `system_control_flags` | `1u << 4 = 0x00000010` |
| `SceDipsw::system_control_flags` offset | `0x1C` |
| `SceKblParam::dipsw` offset | `0x40` |
| Effective KBL word offset | `0x40 + 0x1C = 0x5C` |

VitaSDK therefore exposes two equivalent read forms for current kernel state:

```c
int enabled = ksceKernelCheckDipsw(228);
uint32_t system = ksceKernelGetDipswInfo(7);
int consistent = enabled == ((system >> 4) & 1u);
```

The distinct `SceKblParam::unk_E4` field is unrelated. A patch aimed at KBL
offset `0xE4` would corrupt or alter the wrong field.

## What this changes in the investigation

The earlier DEVTOOL identity idea is now secondary. Bit 228 is a narrower,
named control whose documented consumers line up directly with the feature we
are investigating. The next experiment should observe this control before
changing device identity or attempting another DSE-dependent CP14 access.

Timing still matters. The KBL notes say the user-facing setters for bits 0-63
overwrite SceSysmem's cached values without changing hardware. VitaSDK's kernel
set/clear functions accept a general bit index, but their public declaration
does not state whether a bit above 63 can have a hardware side effect. Treating
`ksceKernelSetDipsw(228)` as a cached-state mutation is therefore the leading
implementation inference, not a proven API guarantee. ProcessMgr or ThreadMgr
code that checks the cache dynamically may see a runtime change, but
ProcessMgr's documented `0x60`-byte breakpoint context exists only when bit 228
is enabled. A process created before the setter may therefore lack state that a
newly created process would receive. SKBL runs much earlier, and any
hardware/debug-device authorization it performs might already be latched before
a normal kernel plugin can call the setter. A runtime write that reads back
successfully but does not change `DBGVCR` behavior would reject only that
late-runtime setup, not the early-boot hypothesis.

Enso_ex exposes the KBL parameter to its boot manager before the ordinary base
kernel configuration is loaded. A narrowly scoped boot-manager experiment
could set mask `0x10` in the system-control word in both known boot-argument
copies before ProcessMgr and ThreadMgr load. That is earlier than a TaiHEN
kernel plugin, but it may still be later than the relevant SKBL device action.
Static analysis of the SKBL consumer is required before treating this as a
complete early-boot test.

The same KBL revision's fallback-switch notes strengthen the relationship with
real development hardware: under a specific early SKBL fallback condition
(both CP version and CP board ID are zero), TEST/TOOL-class units have bit 228
among the switches force-set in the temporary DIP buffer. Later SKBL processing
may still clear or replace fallback values. This supports TOOL/devkit as the
closer identity hypothesis, but only when that identity is already visible to
SKBL. Enso_ex's public boot-manager type spoof executes during the later NSKBL
module-load path, so it cannot be assumed to retrigger this SKBL fallback.

## Staged test order

Each mutation stage requires a separate review and the Vita owner present. A
passing rung authorizes only the next listed rung.

1. **Read-only state inventory.** In a fresh disposable diagnostic, call
   `ksceKernelCheckDipsw(228)` and `ksceKernelGetDipswInfo(7)`. Also record
   bit 203 and debug-control word 6 so later results are not interpreted without
   the SKBL-reconfiguration state, plus CP version/board ID because those select
   the documented fallback path. Record firmware, device type, core, and bit/
   word consistency. Do not read DBGVCR.
2. **Kernel DIP-state API round trip.** First inspect the tested firmware's
   `ksceKernelSetDipsw` implementation because the VitaSDK declaration promises
   neither persistence nor absence of hardware side effects. If bit 228 is
   already set, record that fact and skip this mutation rung. Otherwise snapshot
   the original word, call `ksceKernelSetDipsw(228)`, verify that only mask
   `0x10` changed, clear it, and verify exact restoration. Do not read DBGVCR.
   Journal each phase.
3. **Fresh-target runtime authorization test.** Only after confirming the
   setter's scope and reboot persistence behavior, set and verify bit 228 before
   launching a fresh disposable target so ProcessMgr and ThreadMgr can observe
   it during target/thread creation. With best-effort watchdog restoration and
   a fresh journal already active, perform one DBGVCR read. If execution returns,
   restore the original bit immediately and stop; do not touch comparators. If
   the Vita reboots, the watchdog cannot run: stop this runtime branch, recover
   the journal, and re-read bits 228 and 203 before any other test.
4. **Boot-time bit-only A/B.** If runtime state changes but DBGVCR remains
   inaccessible, statically locate the SKBL check and decide whether an earlier
   boot change can reach it. Patch only system-control mask `0x10`, with exact
   backup, hashes, bypass, and restoration prepared in advance. Do not combine
   this with a device-type change.
5. **Device identity only as a control.** Use the documented Enso_ex DEVTOOL
   runbook only if bit 228 evidence leaves an identity-derived policy question.

## Explicit safety boundaries

- Do not write `SceKblParam::unk_E4`; `0xE4` is the switch index.
- Do not change DIP switch 203 (`DEBUG_FLAG_DISABLE_DIPSW_RECONFIG`). Its name
  indicates that one disables SKBL reconfiguration, while the accompanying
  public description is ambiguous about the action it calls "enabling." Its
  exact relationship to bit 228 still requires tracing, and changing it would
  add a second variable to the experiment. For reference, bit 203 is mask
  `0x00000800` in `debug_control_flags` at KBL offset `0x58`.
- Do not combine a bit-228 test with DEVTOOL/DEX spoofing, firmware conversion,
  comparator access, guessed MMIO, or additional CP14 probes.
- A successful bit readback is not proof that debug authentication changed.
- A readable DBGVCR is not permission to program BCR/BVR/WCR/WVR. Comparator
  snapshot, restore, trap, all-core, context-isolation, and watchdog gates
  remain separate requirements.

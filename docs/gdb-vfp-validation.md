# Live GDB VFP validation gate

The kernel boundary probe proves that the guarded snapshot call preserves and
returns the expected VFP bank. This separate gate proves that the complete
debugger path selects a stopped foreign application thread and presents the
same D0-D31/FPSCR layout to an unmodified VitaSDK GDB client.

Do not use this diagnostic fixture in a production application. Its registered
worker deliberately holds D0-D31 in a call-free loop so every debugger stop
has deterministic values. The loop consumes processor time while the test is
running.

The initial live gate targets retail Vita hardware running system software
3.65. A pass there does not validate this undocumented VFP ABI on another
firmware, development hardware, or a different plugin stack; repeat the
boundary and lifecycle gates for every new target baseline.

## Prerequisites

1. The fail-closed VFP kernel probe has passed every check on this Vita and
   firmware.
2. The matching opt-in kernel companion is installed and advertises ABI v1.11
   plus `VD_KERNEL_CAP_THREAD_VFP_REGISTERS`.
3. The Vita and development computer are on a trusted network. The current RSP
   listener is not authenticated.

No kernel replacement is needed between the boundary probe and this test.

## Build and launch

Build the diagnostic package with all three application-side gates enabled:

```sh
make package UVDB_KERNEL_THREAD_CONTROL=1 UVDB_KERNEL_VFP_READS=1 \
  UVDB_GDB_VFP_FIXTURE=1 \
  VITADEBUG_KERNEL_DIR=kernel \
  VITADEBUG_KERNEL_BUILD_DIR=kernel/build-vfp
```

Install and launch `uvdb-test.vpk`. Continue only when the screen reports:

```text
GDB VFP fixture: ready (D0=1 D31=2 FPSCR=00400000)
```

`FAILED` means the fixture was not registered or did not publish its loaded
state within two seconds. Do not interpret any subsequent register output as a
passing result.

## Automated live-GDB gate

Use the VitaSDK GDB that will be used for development and the unstripped ELF
from the exact VPK installed on the Vita. The driver speaks GDB/MI to the real
GDB executable; it is not a substitute RSP implementation:

```text
py -3 tools/gdb_vfp_lifecycle.py --host VITA_IP --elf test.elf \
  --evidence vfp-live-gdb-evidence.json
```

The gate fails closed unless it can uniquely find `GDB VFP fixture` in GDB's
thread inventory and prove that it is different from the thread attributed in
the initial stop. It then performs this sequence without operator timing:

1. Attach, select the foreign fixture, and read D0, D31, FPSCR, and the
   application progress value.
2. Continue for one second, interrupt through GDB, reselect the fixture, repeat
   the reads, and prove the application progressed.
3. Detach cleanly, exit that GDB, wait 2.5 seconds, start a new GDB, reconnect,
   repeat the reads, and prove the application progressed while detached.
4. Ask GDB to disconnect its transport while stopped, which closes RSP without
   sending `D`; then let GDB exit normally. Wait 2.5 seconds for peer-loss
   recovery, start a third GDB, reconnect, repeat the reads, prove resumed
   application progress, and detach cleanly.

By default, the JSON evidence is safe to publish: it records the unstripped
ELF SHA-256 and size, VitaSDK GDB version, selected/stopped GDB thread numbers,
exact register and progress snapshots, and each session's kind, MI record
count, and transcript SHA-256. It deliberately omits the target's private IP,
absolute workstation paths, raw MI records, target thread identifiers, and
loaded-module inventory. This is enough to validate the four snapshots and
three lifecycle outcomes without publishing machine-local details.

For local diagnosis only, `--include-sensitive-transcript` adds the endpoint,
absolute ELF and GDB paths, raw MI transcripts, and the raw failure message.
Treat that output as sensitive and do not commit or publish it. Default failure
evidence remains redacted: it contains the failure type, a generic message,
and any completed session summaries, never arbitrary GDB output. A pass covers
GDB target-description negotiation, foreign-thread selection, continue/Ctrl-C,
clean detach, abrupt peer loss, and both reconnect paths. It does not claim
that the deployed VPK matches the ELF merely because an ELF exists; install
the VPK and retain the ELF from one build invocation.

If the tool fails or is interrupted, it first attempts a clean detach. If GDB
cannot detach, it closes the peer so the debugger's normal disconnect cleanup
and the kernel lease watchdog remain the recovery backstops. Wait for the Vita
application to resume before retrying; do not treat a reconnect alone as a
pass. Do not test port 1234 first with a separate TCP probe: the debugger has a
single-client listener and such a probe can consume the connection intended
for GDB.

## Manual exact-register check

The automated gate above is authoritative. These commands remain useful for
interactive diagnosis.

Connect with the unstripped ELF from the same build:

```text
arm-vita-eabi-gdb test.elf
(gdb) target remote VITA_IP:1234
(gdb) info threads
```

Find the GDB thread number named `GDB VFP fixture`, select that number, and read
the endpoint registers:

```text
(gdb) thread GDB_THREAD_NUMBER
(gdb) p $d0
(gdb) p $d31
(gdb) p/x $fpscr
```

The required values are exactly:

```text
$d0    = 1
$d31   = 2
$fpscr = 0x400000
```

The endpoint bit patterns are respectively `0x3ff0000000000000` and
`0x4000000000000000`. D1-D30 contain distinct diagnostic sentinels; the
already-passed kernel probe remains the authoritative exhaustive D0-D31 mapping
check. GDB reads are intentionally read-only. A full `G` register write must be
rejected while the VFP target description is active.

GDB may read other stopped threads while attaching. Only the kernel companion's
normalized `VD_KERNEL_ERROR_VFP_CONTEXT_UNAVAILABLE` result allows a thread to
retain its valid ARM core registers and report its D0-D31/FPSCR slots as
unavailable. The kernel emits that public result only for VitaSDK's exact
`SCE_KERNEL_ERROR_CAN_NOT_USE_VFP` or `SCE_KERNEL_ERROR_ILLEGAL_PERMISSION`
return, and only after both VFP canary regions remain intact. Session,
ownership, target, other raw VFP results, CPU-register, user-copy, canary/guard,
and layout failures reject the complete register read. The named fixture is
stricter still: unavailable VFP slots fail this gate.

## Pass criteria and watchdog evidence

The automated evidence must contain all four exact snapshots:

1. Initial attach.
2. `continue`, then Ctrl-C after the application runs briefly.
3. Clean `detach`, reconnect, and select the fixture again.
4. Terminate one stopped GDB client without detaching so its TCP connection
   closes, reconnect, and verify both application progress and the three
   registers.

A pass requires the application counter to resume after continue, detach, and
forced client closure; the fixture must remain listed and retain all three
expected values on every stop. The separate abandoned-lease probe must also
pass. Any hang, `E16`, unavailable VFP value, changed value, missing thread, or
inability to reconnect is a failure. Restore the known-good user application
build after collecting evidence; this fixture itself does not modify the
kernel plugin or its configuration.

### Retail 3.65 live lifecycle result

ABI v1.11 passed the automated gate on 2026-09-13 on a retail handheld Vita
running system software 3.65 with VitaSDK GDB 15.2. GDB thread 5 was the named
fixture at all four stops; thread 1 was the independently attributed stopped
thread, proving the foreign-thread path. D0, D31, and FPSCR were exactly `1`,
`2`, and `0x00400000` at initial attach, after continue/interrupt, after clean
detach/reconnect, and after transport disconnect/reconnect. Application
progress changed at every lifecycle transition.

The archived, portable JSON contains the three session summaries, all four
register/progress snapshots, and the matching unstripped ELF SHA-256
`cb74d74d76b4394f6e1d8a9a776e493e96e1d6eb0abc78ae6ed40489f8af0f24`:
[retail 3.65 live-GDB evidence](hardware/gdb-vfp-live-3.65.json). The matching
kernel plugin SHA-256 is
`b4bf79a53d88ef34c0f1ac00d29557f8b709e60acb35e14222e292d1f47792bf`;
the installed test VPK SHA-256 is
`4b5a3385e4a97cf3085ac8ad2ad2503b7104306816dd4fc9db5162cd50691851`.

The final fail-closed audit exposed two Vita ABI details before this pass.
Retail 3.65 returned `SCE_KERNEL_ERROR_ILLEGAL_PERMISSION` for every valid
suspended test thread without a readable VFP bank, while the active VFP
fixture succeeded. The kernel now normalizes only that exact result and
`SCE_KERNEL_ERROR_CAN_NOT_USE_VFP`, after checking both canary regions. A small
custom negative result was also transformed by the user/kernel syscall
boundary, so ABI v1.11 uses VitaSDK's stable
`SCE_KERNEL_ERROR_CAN_NOT_USE_VFP` encoding as the public normalized result.
All other negative results remain fatal. Exact ABI negotiation prevents any of
the intermediate artifacts from pairing with v1.11 accidentally.

Peer-loss recovery and kernel-watchdog expiry are deliberately different
tests. The live-GDB tool uses `-target-disconnect`; a host regression against a
wire-recording RSP peer proves this closes TCP without sending the clean-detach
`D` packet. Normal debugger cleanup must then resume the target. Separately rerun
the stable kernel probe's intentionally
abandoned-lease check with this same experimental plugin. A normally running
lease keeper renews an open GDB stop, so merely waiting at a GDB prompt cannot
prove watchdog expiration. Keep the kernel-probe result beside the live-GDB
JSON when promoting this capability.

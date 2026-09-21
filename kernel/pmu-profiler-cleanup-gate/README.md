# PMU lifecycle cleanup hardware gate

`VDCP00013` is a disposable, independently default-off hardware gate for five
serialized PMU cleanup cases. It never exposes arbitrary events, cores, lanes,
cycle counters, interrupts, or raw register writes. Every stage uses
application core 0, physical lane 5, one allowlisted event, and a bounded
lease. Building or passing this gate does not enable unrestricted production
sampling.

## Stages

Set `VITADEBUG_PMU_CLEANUP_GATE_STAGE` to exactly one value:

| Value | Stage | Required observation |
| --- | --- | --- |
| 1 | Competing owner | A second thread receives provider `BUSY` (raw host `-42` or the exact Vita syscall-boundary encoding `0xBFFFFFD6`), cannot obtain a handle, the first owner closes exactly, and a new lease opens and closes. Neighboring or unrelated errors are rejected. |
| 2 | Watchdog timeout | A 250 ms lease expires, a late read reports `RESTORE_REQUIRED`, the matching handle acknowledges cleanup, and a new lease opens and closes. |
| 3 | Receiver disconnect | A live 5 s lease outlasts the forced disconnect: the initial PMU open/read and network start/connect/prelude all succeed, the initial sample authenticates against the nonzero handle and fixed event/core/lane, and the production Vita TCP sink writes `VDPMU-DROP-V1\n`; the host sends an RST, a bounded later write reports `VP_ERROR_IO`, a required PMU read still succeeds before the matching handle closes, and a new lease opens and closes. |
| 4 | Normal process exit | The application returns normally with a live 5 s lease. Relaunch in the same boot proves process-event cleanup and opens/closes a new lease. |
| 5 | SceShell kill | After the application durably writes its armed record with a live 5 s lease, the host helper archives that exact record and sends the approved Vita Companion command `kill VDCP00013`. Relaunch in the same boot proves the `.kill` process event cleaned up and opens/closes a new lease. |

Stages are separate builds of the same title. Run them in order and never run
two stages, receivers, profilers, or PMU owners concurrently.

## Build boundary

Use one build directory per stage. The kernel candidate and gate are present
only when every switch below is explicit:

```powershell
$Stage = 1
$Build = "kernel/build-cleanup-stage$Stage"
$MsysRepo = (& C:\msys64\usr\bin\cygpath.exe -u (Resolve-Path .))
& C:\msys64\usr\bin\bash.exe -lc @"
export VITASDK=/c/vitasdk
cd '$MsysRepo'
cmake -S kernel -B '$Build' -G 'Unix Makefiles' `
  -DCMAKE_BUILD_TYPE=Release `
  -DVITADEBUG_EXPERIMENTAL_PMU_PROFILER=ON `
  -DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_REAL_EVENTS=ON `
  -DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_SAFE_REARM=ON `
  -DVITADEBUG_EXPERIMENTAL_PMU_PROFILER_PROCESS_EXIT_GATE=ON `
  -DVITADEBUG_PMU_CLEANUP_GATE_STAGE=$Stage
cmake --build '$Build' --target `
  vitadebug-pmu-profiler-cleanup-gate.vpk-vpk --parallel 2
"@
```

Stage 3 additionally requires the four explicit non-loopback host octets:

```text
-DVITADEBUG_PMU_CLEANUP_HOST_A=<A>
-DVITADEBUG_PMU_CLEANUP_HOST_B=<B>
-DVITADEBUG_PMU_CLEANUP_HOST_C=<C>
-DVITADEBUG_PMU_CLEANUP_HOST_D=<D>
-DVITADEBUG_PMU_CLEANUP_PORT=18196
```

CMake rejects a loopback receiver for stage 3. All targets compile with
`-Wall -Wextra -Werror`; the ordinary default build leaves all PMU options OFF.

Before hardware, run the full host suite and all five Werror stage builds.
Inspect the kernel imports for `SceProcEventForDriver` only in the explicit
process-event candidate and record SHA-256 hashes for each VPK, matching
`vitadebug.skprx`, Git commit, and Git tree.

The process-event callback never dispatches PMU work or waits. While the
terminating process is still authenticated, it performs one bounded UID
lookup, verifies that the object is the exact retained owner, releases the
temporary and retained references once, and records terminal proof under the
PMU lock. The watchdog performs exact PMU restoration. If the callback cannot
acquire the lock, the UID/object differs, or either release is uncertain,
watchdog context still restores exactly but permanently quarantines re-arm.

## Evidence contract

Each stage owns three exclusive 1,024-byte slots. The current stage-1 retry
uses an explicit `conflict-r2` namespace so the immutable historical
`conflict` slots cannot be deleted, reused, or mistaken for new evidence:

```text
ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-r2-a.bin
ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-r2-b.bin
ux0:data/VitaDebugger/pmu-cleanup-v2-conflict-r2-c.bin
ux0:data/VitaDebugger/pmu-cleanup-v2-<later-stage>-<slot>.bin
```

`<later-stage>` is `timeout`, `disconnect`, `normal-exit`, or `abrupt-exit`;
`<slot>` is `a`, `b`, or `c`. Ordinary stages write attempted and terminal
records. Exit stages write attempted, armed, and terminal records across two
launches.
Every write uses create-exclusive, file sync, volume sync, reopen,
schema/checksum validation, and byte comparison. Any present invalid slot,
duplicate revision, unexpected state, or write failure locks the stage.

The record contains transport results, three complete handles and samples,
timestamps, re-arm duration, capabilities, and complete baseline, restored,
and final status snapshots. PASS requires:

- idle transport and backend with no recovery, restore, terminal, or retained
  reference uncertainty at all three observable idle points;
- byte-exact equality of the baseline, post-cleanup, and post-re-arm PMU
  snapshots;
- an action-specific successful cleanup;
- a later successful real-event open/read/exact close within two seconds; and
- an increased re-arm count. Stage 4 additionally requires only the
  active-normal-exit cleanup counter to increase; stage 5 requires only the
  active-SceShell-kill cleanup counter to increase. These counters advance
  only when the process callback observes an `ACTIVE`, unexpired lease, so
  timeout-first restoration or a callback arriving after the deadline cannot
  satisfy either process gate.

The 1,024-byte record ABI fixes `results` at offset 68, `handles` at 164,
four zero alignment bytes at 284--287, `samples` at 288, re-arm duration at
432, and the three status snapshots at 440, 624, and 808. The C layout has
compile-time offset checks and the Python decoder uses the same named
constants. A sample written at the old unaligned offset 284 is invalid.

Stage 1 accepts exactly two representations of provider `BUSY`: raw `-42`
from the host model and `0xBFFFFFD6` observed across the Vita user/kernel
syscall boundary. It does not mask or generally normalize negative results.
The durable completion validator requires that exact result, complete
first-owner evidence, and the subsequent bounded re-arm open/read/close.

Decode each retrieved slot offline:

```powershell
py -3 tools/decode_pmu_cleanup_record.py `
  .\evidence\stage-1-b-<sha256>.bin `
  --source-journal-name pmu-cleanup-v2-conflict-r2-b.bin `
  --expected-stage 1 --expected-slot b `
  --output .\evidence\pmu-cleanup-v2-conflict-r2-b.json
```

The decoder exits nonzero for a malformed header, checksum mismatch, nonzero
layout padding, unknown field, non-idle state, uncertainty flag, missing exact
snapshot, mismatched snapshot, incomplete state transition, or journal
provenance that does not match the exact current stage/slot namespace. Its CLI
requires bound source-name/stage/slot provenance by default. The explicit
`--allow-unbound-source` option exists only to inspect previously archived
historical records; such output is not hardware acceptance evidence.

## Serialized retail procedure

The target is the designated retail Vita only. Before the first contact,
record the Git commit/tree, all artifact hashes, local timestamp, intended
stage, receiver address for stage 3, and confirm there is no other authorized
device user. At the start of **every** stage:

1. Keep the Vita at LiveArea. Record firmware/device identity, installed title
   inventory, active-title state, plugin path/hash, and absence of all three
   journal paths for that stage. An identity mismatch or existing slot is a
   hard stop.
2. Verify the stage VPK offline with
   `py -3 -m host.vitadevdeploy verify <vpk>`.
3. Use only the approved signed `host.vitadevdeploy deploy` install flow.
   Never force-close an application and never replace a plugin while the
   candidate or another PMU owner is active.
4. Launch `VDCP00013`, confirm the expected numeric stage, capability mask,
   and empty journal. Press X exactly once.
5. Retrieve every new slot without deleting it, hash it, decode it, and require
   a valid attempted record before accepting any terminal record. For exit
   stages, require a valid armed record before same-boot relaunch.
6. Require a valid PASS terminal record and matching screenshot before moving
   to the next stage. Install the next stage package only from LiveArea.

Use these PowerShell variables and commands for the signed install and
immutable host archive. `$PrivateKey` must point to the existing protected key;
never copy key material into the repository or evidence directory.

```powershell
$VitaIp = "10.1.1.217"
$Stage = 1
$StageName = @("conflict-r2", "timeout", "disconnect",
               "normal-exit", "abrupt-exit")[$Stage - 1]
$Build = Resolve-Path "kernel\build-cleanup-stage$Stage"
$StageVpk = Join-Path $Build "vitadebug-pmu-profiler-cleanup-gate.vpk"
$Kernel = Join-Path $Build "vitadebug.skprx"
$PrivateKey = Resolve-Path "<existing-approved-private-key.pem>"
$RunId = Get-Date -Format "yyyyMMddTHHmmssK"
$Archive = New-Item -ItemType Directory -Path `
  "kernel\pmu-profiler-cleanup-gate\hardware-results\$RunId-stage-$Stage-$StageName"

Get-FileHash $StageVpk -Algorithm SHA256
Get-FileHash $Kernel -Algorithm SHA256
$env:PYTHONPATH = (Resolve-Path "deploy").Path
py -3 -m host.vitadevdeploy verify $StageVpk
py -3 -m host.vitadevdeploy deploy $StageVpk `
  --vita $VitaIp --private-key $PrivateKey `
  --action install_launch --output (Join-Path $Archive "deploy")
```

After the device shows the expected terminal state, retrieve the immutable
slots read-only. Stages 1--3 have slots `a,b`; stages 4--5 have `a,b,c`.

```powershell
$Suffixes = if ($Stage -le 3) { @("a", "b") } else { @("a", "b", "c") }
foreach ($Suffix in $Suffixes) {
  $Record = "pmu-cleanup-v2-$StageName-$Suffix.bin"
  $Raw = Join-Path $Archive $Record
  curl.exe --silent --show-error --fail --disable-epsv `
    "ftp://${VitaIp}:1337/ux0:/data/VitaDebugger/$Record" `
    --output $Raw
  $Hash = (Get-FileHash $Raw -Algorithm SHA256).Hash.ToLowerInvariant()
  $Immutable = Join-Path $Archive `
    ("stage-{0}-{1}-{2}.bin" -f $Stage, $Suffix, $Hash)
  Move-Item -LiteralPath $Raw -Destination $Immutable
  py -3 tools/decode_pmu_cleanup_record.py $Immutable `
    --source-journal-name $Record `
    --expected-stage $Stage --expected-slot $Suffix `
    --output "$Immutable.json"
}
```

Do not issue FTP `DELE`, remove a journal on-device, or reuse a slot during
this matrix. Each later stage uses its own distinct three paths. Retain the
deployment output directory, hashed VPK/kernel copies, decoded JSON, raw
journals, screenshot, and a transcript of the exact commands and timestamps.

For stage 3, start the one-shot receiver before X:

```powershell
py -3 tools/pmu_disconnect_receiver.py `
  --bind 0.0.0.0 --port 18196 `
  --evidence .\evidence\disconnect-receiver.json
```

Require its durable JSON state to be `reset_sent`, its exact prelude to match,
and its peer address to match the target before accepting the device record.

For stage 5, start the kill helper **before** installing/launching the stage.
It first requires both the armed and failed slots to be absent. It then polls
both paths read-only, validates and durably archives a newly created stage-5
armed record, rechecks that the device did not write its timeout failure, and
only afterward sends exactly `kill VDCP00013`:

```powershell
py -3 tools/pmu_kill_gate.py `
  --vita $VitaIp `
  --evidence (Join-Path $Archive "kill-gate.json") `
  --armed-copy (Join-Path $Archive "stage-5-armed-before-kill.bin") `
  --max-kill-delay 2
```

Press X only after the helper reports `waiting_for_armed`. Require terminal
JSON state `kill_confirmed`, reply `Killed.`, the expected title ID, and an
`armed_to_kill_seconds` value no greater than two seconds, plus an armed
SHA-256 matching the separately retrieved slot `b`. The device independently
closes the lease and writes failed slot `c` if no kill arrives within four
seconds. The helper carries one absolute monotonic deadline through archive,
FTP, companion connect, send, and reply; the companion invokes the final
slot-`c` absence check after connecting and immediately before its deadline
recheck and command send. No command is transmitted if that check races with
slot `c` or the deadline has expired. Any pre-existing armed slot, invalid or
identity-free armed handle, timeout, FTP error, late or non-success kill reply
fails without issuing or repeating a kill.

The VPK is the hardware artifact, not merely a reproducible-build claim.
`vita-pack-vpk` embeds ZIP timestamps, so separate clean builds can produce
different VPK SHA-256 values while verified title, entries, payload sizes, and
the candidate SKPRX hash remain identical. Hash the exact post-review VPKs and
deploy those same files without rebuilding or substituting another package.

## Hard stops and exclusions

Stop the entire matrix immediately on identity mismatch, missing or torn
journal, checksum/schema failure, PMU snapshot mismatch, unexpected return
from the armed stage-5 wait, failed or repeated title kill, cleanup/re-arm
timeout, nonzero backend obligation,
retained-reference uncertainty, failed network/socket cleanup, unexpected
reboot/power loss, or any result not explicitly required above. Preserve all
evidence and leave the candidate resident; do not guess, retry, erase records,
unload the plugin, or continue to another stage in that boot.

This procedure does not authorize debugger/GDB attach, external-attach
listeners, loader/injection experiments, system-process targeting, unrelated
hardware work, concurrent hardware tests, reboot, or unrestricted sampling.
A reboot is not part of the matrix; if one becomes necessary, stop and report.

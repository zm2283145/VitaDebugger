# VitaDevDeploy Vita agent

This directory builds the one-shot Vita-side deployment agent (`VDEVDEP01`). A
verification-only build is a safe user-mode homebrew application and contains
no package-promotion code. Enabling installation adds PromoterUtil and produces
a highly privileged user-mode application using VitaSDK's normal `UNSAFE`
`0x2F00000000000001` authority profile. Neither variant is a kernel plugin. A
controlled A/B test on the target retail Vita showed that the explicit
VitaShell/SceShell `0x2808000000000000` profile was rejected before `main()`
while the normal VitaSDK profile launched, so install builds deliberately use
the latter.

The runtime root is `ux0:data/VitaDevDeploy`. On launch the agent writes,
file-syncs, and renames `challenge.v1`, then waits for one committed job below
`inbox/<32-lowercase-hex-job-id>/`, consumes the challenge, verifies the exact
signed package tree, writes an append/sync crash journal, atomically commits the
final journal and result, then exits. Production builds are headless:
display-status calls compile as no-ops and the binary does not link the display
helper. Progress and the durable result remain available to the host.

Install-enabled builds first try the parent-directory descriptor sync, then the
documented checked device-wide sync if firmware rejects the descriptor form.
They fail closed before promotion if neither barrier succeeds.
Verification-only builds may then accept a narrowly scoped `EACCES` fallback
that reopens the committed regular file writable for `SyncByFd`, closes it, and
independently re-reads it. That final fallback verifies the file but does not
claim rename-namespace or power-loss durability.

Set CMake's `VDEV_ENABLE_DISPLAY_UI=ON`, or pass
`-EnableExperimentalDisplayUi` to `tools/build_agent.ps1`, only for a supervised
display test. That opt-in build compiles VitaSDK's installed
`samples/common/debugScreen.c` and shows a stage-based progress bar, current
work, PromoterUtil state and elapsed time, completion details, or the stage and
error code on failure. It is not a separate GUI framework or an exact
byte-progress meter. Because the helper installs a process-owned CDRAM
framebuffer, close this build only by pressing Circle during the idle wait or by
letting a one-shot job finish and exit normally. Do not force-kill it or send
Vita Companion's `destroy` command; abrupt termination can bypass display
cleanup and wedge LiveArea.

Circle is sampled only before the first committed job is found. It requests a
clean idle exit and is not a verification or installation cancel button. After
job processing begins, let the agent finish and exit normally. The normal host
workflow likewise never force-closes the agent: launch it from LiveArea, or use
`--reuse-running-agent` only when an existing run is known to still be waiting.

Early startup diagnostics are written to `ux0:data/VitaDevDeploy.startup`.
Challenge publication also produces `ux0:data/VitaDevDeploy.startup_io`, whose
entered/result events distinguish the file open, write, file sync, close,
rename, post-stat, parent-directory open, directory sync, and directory close.
These raw best-effort records contain no nonce or payload and do not alter any
durability decision. See `docs/startup-diagnostics.md` for the fixed binary
format and PC decoder.

The manifest is canonical LF-only text. Its header is
`VITADEVDEPLOY-MANIFEST-1`; each remaining row is
`<lowercase-sha256><TAB><canonical-decimal-size><TAB><normalized-path><LF>`,
strictly sorted by normalized path. Package paths are printable ASCII and may
not be absolute, contain tabs, backslashes, colons, empty/`.`/`..` segments, or
ASCII-case-fold collisions. Directories not implied by a file row are rejected.

`request.v1` is the commit marker and must appear last. It has exactly this
order and a final LF: header, `job`, `nonce`, `action`, `title_id`,
`manifest_sha256`, `file_count`, `total_size`. Its Ed25519 signature covers:

```
VITADEVDEPLOY-SIGNED-JOB-1\0 || exact request.v1 || exact manifest.v1
```

`signature.bin` is exactly 64 bytes. The trusted 32-byte public key is required
at configure time as `-DVDD_PUBLIC_KEY_HEX=<64 lowercase hex>`; missing,
malformed, and all-zero keys are rejected. Installation is disabled by default
and requires `-DVDEV_ENABLE_INSTALL=ON` after verification-only testing passes.
For `install_launch`, the agent installs and commits success, then exits; the
host is the sole launcher and launches only after reading that durable result.

`VDEV_AGENT_TITLE_ID` defaults to `VDEVDEP01`, and every build rejects deploying
over itself. A bootstrap build uses `-DVDEV_AGENT_TITLE_ID=SLRS00001
-DVDEV_ONLY_TARGET_TITLE_ID=VDEVDEP01`; the optional allowlist then rejects all
other targets. Paths are limited to 240 bytes, each file to 256 MiB, and a job
to 1 GiB total.

The signed package tree must already include `eboot.bin`, `sce_sys/param.sfo`,
and `sce_sys/package/head.bin`; the agent never creates unsigned installation
input after verification.

For installation, the tree is verified again immediately before an
irreversible operation. The agent first atomically writes
`ux0:data/VitaDevDeploy/promote.state` with the owning job, title, manifest
digest, and fixed path. It then renames the tree on the same volume from the job
directory to `ux0:/data/vdd_pkg`, syncs, and verifies it once more immediately
before dispatch. Only the package tree moves; the signed request, manifest,
signature, and journal remain under `ux0:data/VitaDevDeploy`. The agent refuses
to overwrite or remove a pre-existing shallow stage or ownership marker and
reports it as stale.

`src/promoter_vitadb.c` adapts VitaDB-Downloader's internal-PAF argument block
and asynchronous installation sequence at commit
`415033d90e08a6bc0a30e2ee6e9456db600d7b22`. It loads PAF and PromoterUtil,
initializes the service, calls `scePromoterUtilityPromotePkg`, polls
`scePromoterUtilityGetState` while ticking power, and requires a successful
terminal value from `scePromoterUtilityGetResult`. VitaDevDeploy additionally
checks all pre-dispatch return values, exposes progress callbacks, and cleans up
initialized services in reverse order only after the operation outcome is
known. After dispatch, failed state or result queries are retried without a
hard Vita-side timeout. An experimental display-enabled build shows that status
or result is temporarily unavailable while continuing to display elapsed time;
the production headless build reports its eventual durable result to the host.

If a failure occurs before promotion is dispatched, the agent attempts to move
the intact stage back into the signed job directory. It clears the marker only
after that restoration and a durable failure result both succeed. After
dispatch it leaves the stage and marker untouched on failure because the system
installer may have consumed the package partially or completely. A successful
run clears the marker only after its durable success result commits. If the
process or Vita is interrupted before a terminal result can be retrieved, do
not retry automatically. A leftover marker or shallow stage blocks the next
install until a developer has inspected the result, journal, installed title,
marker, and staging path.

Results are atomically written as `results/<job>.result`. A journal is synced
after every complete event record as `<job>.journal.part` and atomically renamed
to `<job>.journal` before the result commit. A reader may use the `.part` file as
a crash journal and ignore a final incomplete line.

The PromoterUtil/PAF adapter is isolated in `src/promoter_vitadb.c` and marked
GPL-3.0-only because its undocumented PAF arguments and call sequence are
adapted from VitaDB-Downloader. Host-side `head.bin` package preparation remains
derived from VitaShell. The optional VitaSDK debug-screen helper and its
embedded PSPSDK font, plus vendored Monocypher, are documented in the
repository's `THIRD_PARTY.md`.

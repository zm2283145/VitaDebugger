# VitaDevDeploy direct-TCP retail 3.65 checkpoint

Status: the basic authenticated direct-TCP path passed on retail PS Vita
hardware running system software 3.65 on 2026-09-15. This checkpoint proves
signed verification, real package installation, durable-result reconciliation,
and host-requested launch through the direct package carrier. It does not close
the interruption/security matrix described below.

## Agent under test

- Title ID: `VDEVDEP01`
- VPK size: 528,855 bytes
- VPK SHA-256:
  `1afde6fa38aa131f28a733c5f1a21d5ee6d302097c135b29f5917f0a935f03a7`
- EBOOT SHA-256:
  `888e654dda6fbdf801a1b02bf29727c74660193adaa8e88c222aad080ce60986`
- Installed-EBOOT readback: exact SHA-256 match
- Build features: package promotion enabled, guarded vita2d UI enabled,
  direct TCP enabled on port 18196, LiveArea assets enabled

Only the Ed25519 public key was compiled into the agent. The corresponding
private key remained on the development PC and is not part of this report.

## Hardware results

The verification-only direct job `2c20403997da57bdc30e97099adb1fc1`
committed four files totaling 737,939 bytes. Both protocol acknowledgements
were received and validated, and the durable result reported success.

The install-and-launch job `5a7244c5ddd17dd018a407b3eccd960e`
committed three files totaling 10,616 bytes for the safe disposable title
`VDDT00001`. The system installer returned result zero, the host launched the
installed title only after reading that durable result, and the target wrote
its expected file-synced marker. A subsequent read-only recovery-status check
reported `clean`, with no promotion marker or shallow-stage ambiguity.

The graphical/direct agent then completed twelve consecutive automated
launch/Circle-exit cycles. Every launch command returned success, LiveArea
remained responsive, and no GPU dump newer than the two previously investigated
incidents appeared. This is a bounded normal-exit stress result, not a proof of
all startup, forced-termination, or network-interruption schedules.

## Host regression gates

- VitaDevDeploy host, agent-contract, and tool suite: 144/144 passed.
- Combined ASLR/symbol/VS Code suite: 51/51 passed.
- The VS Code suite defaults new profiles to TCP 18196, preserves explicit and
  migrated FTP profiles, validates all configured ports, blocks a direct F5
  run when recovery status is not clean, and verifies that the demo links every
  fixed debugger-core object.

The end-to-end VS Code F5 hardware rerun also passed over this carrier. The
successful job built, installed, launched, verified the exact installed EBOOT,
loaded live ASLR symbols, attached GDB, hit a source breakpoint, changed a
visible variable, resumed, detached, and performed exact-title cleanup. The
first post-fix attempt installed successfully but its initial SceShell launch
reported `C2-2752-6`; an immediate full retry passed. That recoverable launch
race remains a hardening item. Exact job and artifact identities are in the
[direct-TCP F5 record](vscode-debug-demo-direct-tcp-3.65.json).

## Remaining gates

- forged-signature and wrong-nonce rejection on hardware;
- interruption at metadata, file-body, file-sync, and final-commit boundaries,
  including reboot boundaries;
- large-file and configured 1 GiB limit behavior on real storage;
- cold-start, immediate app-transition, and Wi-Fi-reconnect stress;
- result return without Companion FTP;
- versioned device-authenticated responses; and
- authenticated bounded inventory/cleanup for abandoned uncommitted trees.

Until those gates pass, use direct intake only on a trusted private LAN and
retain signed host evidence for every ambiguous outcome.

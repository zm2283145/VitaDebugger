# Security policy

VitaDevDeploy performs a privileged operation on a homebrew-enabled PS Vita.
Its design therefore fails closed: an unsigned, stale, malformed, mismatched,
or partially uploaded job must never reach the package promoter.

## Network boundary

Vita Companion 1.06 does not authenticate its FTP or command ports. Use this
tool only on a trusted private LAN, keep the Vita off untrusted Wi-Fi, and do
not expose ports 1337 or 1338 through a router. Signed deployment jobs reject
stale, accidental, and unsigned input, but they do not make Vita Companion's
other remote capabilities private. In particular, a hostile client that can
write through the unauthenticated FTP service may attempt to race a verified
staging tree while the system promoter reads it. Version 1 does not claim to
defend against an active attacker already present on that LAN.

The experimental direct package intake on TCP port 18196 is opt-in. It sends a
fresh raw challenge first, then requires the canonical request, manifest, and
Ed25519 signature to pass before acknowledging metadata or accepting package
bytes. The complete pre-authentication exchange has an absolute deadline, and
the raw signature is checked before dynamic manifest parsing. Package paths,
sizes, order, and hashes come only from the authenticated manifest;
`request.v1` remains the last commit. A failed client may clean only paths named
by its authenticated manifest, and an unexpected entry makes that cleanup fail
closed. Completed or replayed jobs are never treated as uncommitted cleanup
targets.

TCP provides ordered reliable transport, not confidentiality or mutual
authentication. `VDDACK01` also has no job/nonce binding, MAC, or device
signature. A hostile endpoint can deny service, receive package bytes, or forge
positive and negative acknowledgements; it cannot reuse the captured job on
the real Vita because the signature binds the other endpoint's challenge. The
host never deletes signed evidence because of a v1 negative ACK and treats a
positive commit ACK as ambiguous until result reconciliation, but that does not
remove the trusted-LAN boundary. Do not expose port 18196 outside the trusted
private LAN. The current direct client still reads the small terminal result
over Companion FTP and uses Companion for application launch, so Companion's
existing boundary remains in force.

ACK authentication is not compatible with the fixed-size v1 wire frame. A
future authenticated revision must use distinct `VDDCHL02`, `VDDJOB02`, and
`VDDACK02` magics, plus a separately provisioned and pinned device identity or
shared secret. Its signature or MAC must cover a domain separator, protocol
version, session nonce, job ID, signed-request digest, phase, result code, and
progress counts. The current developer public key authenticates the PC to Vita;
it cannot authenticate Vita responses to the PC.

The install-enabled agent uses VitaSDK's normal `UNSAFE`
`0x2F00000000000001` user-mode authority profile. It is not a kernel plugin,
but its PromoterUtil/internal-PAF access is highly privileged. A controlled A/B
test on the target retail Vita found that the explicit VitaShell/SceShell
`0x2808000000000000` profile was rejected before `main()` while the normal
VitaSDK profile launched, so `0x2808` is intentionally excluded. Keep the
signing key off the Vita and do not weaken or bypass the signed-request gate.

## Key handling

- The deployer contains only an Ed25519 public key.
- CMake validates that the exact encoded key is canonical, non-identity, and in
  Ed25519's prime-order subgroup before compiling. Published `BUILD-INFO.txt`
  records `sha256` of those exact 32 public bytes, never key material from the
  private PEM.
- The private key belongs in `local/` or another ignored, access-controlled PC
  directory.
- Losing the private key requires rebuilding and reinstalling the deployer with
  a new public key.
- Never upload the private key to the Vita or commit it to Git.

## Recovery properties

- Uploads use `.part` names and the request is renamed last as the commit point.
- Directly received files are incrementally hashed, synchronized, atomically
  renamed, and re-hashed before `request.v1` is committed. A dropped direct
  connection currently causes an authenticated whole-job restart rather than
  byte-offset resume.
- Before a phase-2 success response, the direct receiver performs a checked
  device-wide storage barrier after the `request.v1` rename. This covers new
  job/package/nested-directory links; any barrier failure is commit-ambiguous.
- The receiver sends a negative acknowledgement after staging ownership only
  after a checked path probe, cleanup, inbox synchronization, and a second
  absence check. Stat errors and wrong file types never mean "missing." This is
  useful receiver behavior, but the v1 frame cannot prove it to the host. Any
  request rename or uncertain cleanup is reported as `-20021`; `-20012` replay
  is treated as a possible existing commit/result. Both enter reconciliation.
- Once a temporary direct-TCP job has been signed, the host never deletes it
  because of a v1 negative ACK. KeyboardInterrupt, SystemExit, replay, lost
  acknowledgements, and result-channel errors preserve and print the exact job
  ID and recovery path. A failed install result triggers recovery-status and
  remains retained if `promote.state` exists or cannot be checked.
- A fresh device challenge prevents replay after the deployer is relaunched.
- The requested title must match `sce_sys/param.sfo`.
- Existing applications are updated in place; they are not uninstalled first.
- Results and the bounded journal use temporary-file-plus-rename updates.
- The deployer refuses to overwrite itself.
- Before the same-volume rename to the fixed shallow path
  `ux0:/data/vdd_pkg`, the agent atomically commits
  `ux0:data/VitaDevDeploy/promote.state` with the owning job, title, manifest
  digest, and path. It then re-hashes the relocated tree immediately before
  dispatch. A pre-existing stage or marker is treated as stale and is never
  overwritten or deleted automatically.
- The VitaDB-style asynchronous promoter adapter checks PAF and PromoterUtil
  loading, initialization, dispatch, each state query, the terminal operation
  result, and reverse-order cleanup. A zero result from the dispatch call alone
  is not considered installation success.
- Before dispatch, a failed operation attempts to move the intact tree back to
  its signed job directory. The marker is cleared only after that restoration
  and a durable failure result both succeed. After dispatch, the agent does not
  move or delete the stage or marker on failure because PromoterUtil may already
  have consumed the package partially or completely. A successful run clears
  the marker only after committing its durable success result.

After dispatch, a failed state or result query does not prove that the
asynchronous installer stopped. The agent therefore keeps the Vita awake and
retries the query without a hard Vita-side timeout instead of unloading
PromoterUtil or committing a false failure. The opt-in graphical build shows
installer status as temporarily unavailable while retrying; the
production build is headless. The host waits 2,100 seconds by default, but a
PC-side timeout only stops the PC wait and does not cancel PromoterUtil. If the
process or Vita is interrupted, do not automatically retry; inspect the durable
result and journal, the target title's installed state,
`ux0:data/VitaDevDeploy/promote.state`, and `ux0:/data/vdd_pkg` before deciding
how to recover the stale operation.

The host `recovery-status` command is deliberately read-only. It treats every
promotion marker as manual-reconciliation state, even alongside a success
result, because FTP access confined to `ux0:data/VitaDevDeploy` cannot prove
the separate shallow stage is absent. Unknown FTP permission failures are not
classified as a missing marker.

Protocol v1 provides no authenticated bulk maintenance verb. It never silently
enumerates or recursively deletes abandoned uncommitted inbox trees. A future
version must use a fresh, domain-separated signed request, bounded paginated
inventory, an explicit second cleanup authorization naming exact job IDs and
metadata digests, durable per-job outcomes, and device-authenticated responses;
it must reject committed/result-bearing/promote-owned or unexpected trees.

## Application lifecycle

Production artifacts default to a headless implementation that does not create
a graphics framebuffer. Normal host deployment must begin at LiveArea;
the host launches the agent without first sending Vita Companion's
unauthenticated `destroy` command. `--reuse-running-agent` also avoids the
command port before upload, but a challenge file cannot prove process liveness,
so reuse only a session that is independently known to still be waiting.

The `-EnableExperimentalDisplayUi` build uses libvita2d and the system PGF font.
Its first six-cycle post-refresh gate passed on retail 3.65, but two intermittent
launch-time GPU faults were later preserved. Both dumps identified rapid reuse
around the third `vita2d_swap_buffers` presentation. The guarded build now waits
for prior rendering before resetting libvita2d's transient pool and completed
twelve consecutive automated launch/Circle-exit cycles without a new dump. This
bounded result does not authorize forced termination or unattended use. The
preferred exit still uses normal cleanup: press Circle while the agent is in the
idle wait, or let the one-shot job finish and exit. That path waits for
rendering, releases the font, and calls `vita2d_fini`. Circle is not checked
after a committed job begins and cannot cancel verification or promotion. Do
not send Companion's unauthenticated `destroy` command to a display-enabled
build; that path may bypass graphics cleanup. During an active promotion, do
not terminate either build merely because the host timed out.

Report security problems privately to the repository owner until a coordinated
fix is available.

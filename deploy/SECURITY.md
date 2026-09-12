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

The install-enabled agent uses VitaSDK's normal `UNSAFE`
`0x2F00000000000001` user-mode authority profile. It is not a kernel plugin,
but its PromoterUtil/internal-PAF access is highly privileged. A controlled A/B
test on the target retail Vita found that the explicit VitaShell/SceShell
`0x2808000000000000` profile was rejected before `main()` while the normal
VitaSDK profile launched, so `0x2808` is intentionally excluded. Keep the
signing key off the Vita and do not weaken or bypass the signed-request gate.

## Key handling

- The deployer contains only an Ed25519 public key.
- The private key belongs in `local/` or another ignored, access-controlled PC
  directory.
- Losing the private key requires rebuilding and reinstalling the deployer with
  a new public key.
- Never upload the private key to the Vita or commit it to Git.

## Recovery properties

- Uploads use `.part` names and the request is renamed last as the commit point.
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
PromoterUtil or committing a false failure. An experimental display-enabled
build shows installer status as temporarily unavailable while retrying; the
production build is headless. The host waits 2,100 seconds by default, but a
PC-side timeout only stops the PC wait and does not cancel PromoterUtil. If the
process or Vita is interrupted, do not automatically retry; inspect the durable
result and journal, the target title's installed state,
`ux0:data/VitaDevDeploy/promote.state`, and `ux0:/data/vdd_pkg` before deciding
how to recover the stale operation.

## Application lifecycle

Production artifacts default to a headless implementation that does not install
the debug-screen framebuffer. Normal host deployment must begin at LiveArea;
the host launches the agent without first sending Vita Companion's
unauthenticated `destroy` command. `--reuse-running-agent` also avoids the
command port before upload, but a challenge file cannot prove process liveness,
so reuse only a session that is independently known to still be waiting.

The `-EnableExperimentalDisplayUi` build uses VitaSDK's direct-framebuffer
debug-screen helper. It is safe only when allowed to run its normal cleanup:
press Circle while the agent is still in the idle wait, or let the one-shot job
finish and exit. Circle is not checked after a committed job begins and cannot
cancel verification or promotion. Never force-kill a display-enabled build or
send Companion's `destroy` command; abrupt termination can orphan its
process-owned CDRAM surface and wedge LiveArea. During an active promotion, do
not terminate either build merely because the host timed out.

Report security problems privately to the repository owner until a coordinated
fix is available.

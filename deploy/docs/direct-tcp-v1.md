# VitaDevDeploy direct TCP intake v1

Direct intake removes Vita Companion FTP from the **package-byte transfer**.
It is an additive carrier for the existing signed-job protocol, not a second
install trust path. The Vita still parses the canonical request and manifest,
checks the one-use challenge, verifies the Ed25519 signature, hashes every
received file, and commits `request.v1` last before the existing verifier or
installer can see the job.

This increment is opt-in. Agent builds leave it disabled unless
`-EnableDirectTcp` is passed, and the generic host CLI defaults to
`--transport ftp`; newly configured profiles in the VS Code sample default to
direct TCP. Basic signed verification and disposable-target install-and-launch
gates passed on retail hardware running system software 3.65. Direct mode still
uses Companion FTP to read the small durable result file and Companion's command
port to launch applications; a later protocol can return the result over the
agent connection.

## Endpoint and byte order

The default listener is TCP port `18196`. Integers are network byte order.
Every magic value is exactly eight ASCII bytes, with no terminating NUL.

After accepting one client, the Vita sends this fixed 40-byte greeting:

| Offset | Size | Value |
| ---: | ---: | --- |
| 0 | 8 | `VDDCHL01` |
| 8 | 32 | raw one-time challenge nonce |

The PC's first connection reads this greeting and closes immediately. It then
extracts the VPK, builds and hashes the complete manifest, signs the ordinary
job, and performs a second full local verification while no receiver deadline
is running. It reconnects and requires the greeting to contain the same live
nonce before sending this 28-byte metadata header:

| Offset | Size | Value |
| ---: | ---: | --- |
| 0 | 8 | `VDDJOB01` |
| 8 | 4 | request size, maximum 4,096 |
| 12 | 4 | manifest size, maximum 4 MiB |
| 16 | 4 | signature size, exactly 64 |
| 20 | 8 | total package bytes, maximum 1 GiB |

It is followed immediately by `request.v1`, `manifest.v1`, and the raw
64-byte signature. The receiver applies one 30-second absolute deadline to this
entire pre-authentication exchange in addition to its idle timeout. It hashes
the bounded manifest and verifies the signature before dynamically parsing the
manifest. No package byte is sent until the Vita returns its metadata-
acceptance acknowledgement. The receiver sends that frame only after it has
authenticated the signed metadata; the v1 ACK frame itself is not
cryptographically authenticated. This lets the Vita reject a malformed, wrong-key, disallowed
requested title, wrong-nonce, or oversized request before accepting its large
body. The requested title is compared with the package's `param.sfo` after the
authenticated package files arrive, as in FTP mode.

## Acknowledgements and payload

Both acknowledgement frames are 28 bytes:

| Offset | Size | Value |
| ---: | ---: | --- |
| 0 | 8 | `VDDACK01` |
| 8 | 4 | phase: 1 authenticated, 2 committed |
| 12 | 4 | signed 32-bit Vita/result code; zero means success |
| 16 | 4 | complete manifest entries |
| 20 | 8 | complete package bytes |

`VDDACK01` has no job ID, nonce, MAC, or device signature. Its codes are useful
receiver reports on a trusted LAN, but are not proof to an adversarial host.
The host therefore retains signed evidence after every negative ACK. A zero
phase-2 ACK is only `receiver_reported_commit` until the matching durable result
is read; a result with the exact job ID and title advances the local state to
`known_committed`.

ACK authentication cannot be added by extending the fixed-size v1 frame in
place: a v1 client could misparse a longer response and consume bytes from the
next protocol state. An authenticated revision must use distinct `VDDCHL02`,
`VDDJOB02`, and `VDDACK02` magics so old clients fail closed and new clients
select the frame layout from the greeting. Its signature or MAC must cover a
domain separator, protocol version, session nonce, job ID, signed-request
digest, phase, result code, file count, and byte count. That revision also
needs a separately provisioned and pinned Vita device identity or shared
secret: the developer public key already on Vita authenticates the PC and
cannot authenticate the Vita back to the PC.

Receiver-side phase 2 has three outcome classes:

- code zero reports that the receiver committed `request.v1`; the reported
  progress must equal the signed manifest;
- an ordinary negative code is `fail_before_commit`: the receiver proved
  `request.v1` absent, removed the uncommitted tree, and synchronized the inbox
  directory before replying (the host still retains its evidence because the
  ACK is unauthenticated); and
- `-20021` is `ambiguous_after_commit`: the request rename succeeded or cleanup
  could not be made durable. The receiver does not delete that evidence, and
  the host must retain its signed job and poll the durable result.

`-20012` (`VDEV_ERR_REPLAY`) in either phase means a durable result or committed
`request.v1` may already exist. It is always mapped to
`ambiguous_after_commit`, preserves the exact signed job, and enters result
reconciliation; it is never treated as deletion-safe negative evidence.

Phase 1 can also return `-20021` after authenticating metadata when the
receiver cannot durably prove cleanup of a staging tree. No package payload was
authorized in that case, so an install result cannot appear; the host retains
the exact signed job for recovery without performing a long result poll. An
ordinary phase-1 negative is classified locally as `fail_before_commit`, but
v1 never deletes the signed job on that unauthenticated frame alone.

After a successful phase-1 frame, the host sends files in canonical manifest
order. Paths and lengths are not repeated on the wire: the signed manifest is
the only interpretation. The current Vita receiver always reports zero files
and bytes at phase 1 and restarts an interrupted job from its first file. The
host validates acknowledgement progress as an exact manifest-entry boundary
so a later receiver can add prefix resume without introducing an arbitrary
offset.

For every entry, the receiver writes a `.part` sibling, incrementally hashes
the stream, synchronizes and closes it, compares the signed SHA-256, renames it
to its final path, then reopens and re-hashes the committed file. After every
entry is committed, it atomically writes `request.v1`, then requires one
checked device-wide storage barrier. This final barrier covers the new job,
package, and nested-directory links even on a filesystem where synchronizing
only `request.v1`'s immediate parent succeeds. A barrier error becomes
`-20021`; only a successful barrier permits the phase-2 success
acknowledgement. The ordinary one-shot processor subsequently
re-validates the layout, signature, hashes, and title before any promotion.

## Interruption and retry

A disconnect, idle timeout, short file, wrong hash, or local I/O error before
the `request.v1` rename leaves no installable job only after cleanup and its
directory synchronization succeed. While the agent remains alive, it removes
only paths named by that authenticated manifest. An unexpected file, directory,
or cleanup/durability failure becomes `ambiguous_after_commit` instead of a
false pre-commit success claim. A power loss can leave an ignored job directory
without `request.v1`; the job scanner will not process it.

Once the authenticated payload starts, loss of the connection is conservative:
the PC cannot know whether the last bytes and commit acknowledgement crossed in
opposite directions. It therefore polls the durable FTP result. If that result
cannot be obtained, an automatically created temporary job is deliberately
retained and the error reports its job ID and exact local path. No v1 negative
ACK discards a temporary job. A matching terminal result can discard it after
a verification failure, or after an installation failure only when the
read-only recovery classifier also proves that `promote.state` is absent.

The same retention rule applies to local interruption. After creating a signed
direct job, Ctrl-C, SystemExit, an unexpected host exception, or interruption
during result polling leaves the temporary job in place and prints its exact
job ID and path. Automatic deletion occurs only after a matching terminal
result and, for a failed install, clean recovery status.

Retain the signed job on the PC when testing interruption recovery:

```powershell
py -3 -m host.vitadevdeploy deploy MyApp.vpk `
  --vita 192.0.2.10 `
  --private-key C:\private\deploy_private.pem `
  --transport tcp `
  --output .\local\retained
```

If the same agent and challenge are still live, retry that exact job without
signing anything new:

```powershell
py -3 -m host.vitadevdeploy resume-direct `
  .\local\retained\0123456789abcdef0123456789abcdef `
  --vita 192.0.2.10
```

The command first accepts an already-committed terminal result, covering a PC
disconnect after the Vita commit. If reconnecting is ambiguous because the
one-shot receiver is already processing the job, it polls for the full
configured result timeout rather than performing a single racy read. Otherwise
the signed job nonce must match the current TCP greeting. Relaunching the agent
creates a new nonce, so an old retained job is rejected and a new `deploy`
command is required.

## Recovery and current limits

`recovery-status` is a read-only classifier for interrupted installation
evidence:

```powershell
py -3 -m host.vitadevdeploy recovery-status --vita 192.0.2.10
```

No marker is the only automatically retry-safe classification. Any
`promote.state` marker blocks automatic retry, including when a success result
exists, because Companion FTP access confined to the deployment root does not
prove whether `ux0:/data/vdd_pkg` is absent. The operator must reconcile the
marker, result, journal, installed title, and fixed shallow stage.

The TCP VS Code F5 path runs this same read-only classification before it builds
or deploys. An unreadable or non-clean status blocks F5. When a terminal install
failure is returned, the host runs the classifier again and retains the signed
job whenever a marker remains or the check itself fails.

### Abandoned uncommitted trees

Protocol v1 intentionally has no cleanup-all operation. Its ACK cannot
authenticate a maintenance response, and the existing job verb cannot safely
express authority to enumerate or delete unrelated job IDs. Automatically
walking or recursively deleting `inbox/` would weaken the signed-request model,
so this implementation does neither.

A compatible maintenance design requires a new protocol version and all of the
following properties:

- a fresh challenge and a separately domain-separated developer signature over
  the operation, page cursor, exact job IDs, expected metadata digests, and an
  explicit `inventory` or `cleanup` verb;
- bounded pages (at most 128 canonical job IDs), bounded metadata reads, the
  existing depth/file/byte limits, and no recursive traversal before the
  maintenance request authenticates;
- inventory as a mandatory read-only first step; cleanup as a distinct second
  signed request naming every selected job, never an implicit age-based action;
- rejection of any tree containing `request.v1`, a result, an entry not named by
  its validated signed manifest, a non-regular file, or the active
  `promote.state` owner; and
- durable per-job cleanup results bound to the maintenance nonce and request,
  with authenticated device responses before the host may discard evidence.

Until such a version provisions a device authentication key and completes its
hardware fault-injection gate, abandoned uncommitted directories require
explicit, supervised inspection and are never silently deleted.

TCP provides reliable ordered delivery, but this protocol does not encrypt the
package or authenticate the Vita or its ACKs to the PC. An impostor can cause
denial of service, receive package bytes, or forge positive/negative ACKs. It
cannot reuse the captured job against the real Vita because the signature binds
the impostor's different challenge. Host evidence retention limits the damage
from forged negative ACKs, but does not remove the trusted-LAN assumption. Use
a private trusted LAN and do not forward port 18196.

The completed 2026-09-15 hardware checkpoint covers an authenticated
verification-only transfer, a real disposable-target install and launch, exact
installed-EBOOT readback, a clean recovery-status result, and twelve consecutive
guarded-agent launch/Circle-exit cycles without a new GPU dump. Artifact hashes,
job evidence, and scope are recorded in the
[retail-3.65 report](../../docs/hardware/vitadevdeploy-direct-tcp-retail-3.65.md).

Remaining gates before making direct TCP the generic CLI default or using it
unattended:

- forged signature and wrong-nonce rejection on hardware;
- interrupted transfer at metadata, file-body, file-sync, and final-commit
  boundaries, including reboot between each boundary;
- large-file and 1 GiB limit behavior on real storage;
- cold-start, immediate app-transition, and Wi-Fi-reconnect stress beyond the
  completed twelve-cycle normal-exit run;
- result return without Companion FTP; and
- a versioned device-authenticated response protocol and the bounded,
  explicitly signed maintenance flow above.

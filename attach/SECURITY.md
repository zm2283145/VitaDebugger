# External-attach security boundary

External attachment is substantially more privileged than the current linked
library because it introduces a component that can select and modify another
process. This subtree therefore ships no target-mutating Vita code.

## Version-1 guarantees

- Only `HELLO`, exact-title `DISCOVER`, and identity-ticket `RELEASE` records
  exist.
- The host cannot provide a PID, address, module path, or wildcard.
- Unknown fields, capability bits, modes, record versions, and noncanonical
  values are rejected.
- Network frames are bounded at 4096 bytes and every host request/response uses
  one absolute exchange deadline rather than a resettable per-read timeout.
- Broker messages must be canonical UTF-8 without terminal or bidirectional
  control characters before the CLI can display them.
- Results are bound to request/client/server nonces and service generation.
- Failed discovery cannot return PID/module/ticket data.
- The reference CLI releases its ticket before reporting success.
- A ticket owns no process stop, module, memory, or exception-handler state.

Version 1 is not authenticated or encrypted. It must remain read-only and be
used only on a trusted private LAN. The repository includes a bounded,
Vita-cross-built broker state machine, but no resident Vita listener and no
trusted foreign-target identity adapter. The shipped adapter fails closed and
cannot issue a ticket.

## Authentication-only version 2

Protocol version 2 has fixed, bounded `HELLO`, `CHALLENGE`, `PROOF`, and
`RESULT` records only. It performs replay-resistant mutual Ed25519
authentication against explicit key-ID/generation allowlists and establishes
an expiring session nonce. It does not define target discovery or any mutation
command. Version 1 records cannot be promoted into version 2.

The signed transcripts bind host and server key generations, service
generation, session ID, opaque transport binding, request/client/server
nonces, monotonic expiry, final status, and session nonce. Failed attempts use
bounded exponential backoff. Replay storage is bounded and fails closed at
capacity. Host storage supports explicit provisioning, rotation, and revocation. The
device metadata core persists no seed and requires separate opaque-key,
handle-bound persistence, and independent monotonic backends. Retail 3.65 has
no approved implementation: `*main` is not isolated from co-resident SceShell
code, public file APIs do not prove no-follow or power-loss-safe commit, and
ordinary persistent stores share the rollback domain. The sentinel VPK cannot
accept an assurance override, provision a key, or start networking.

Version 2 provides authentication and integrity, not transport encryption.
Network metadata and payload contents are visible. No documentation or UI may
describe it as encrypted.

## Host-tested control-plane boundary

`broker/src/vitadebug_attach_control.c` now models the authenticated and leased
state needed by a future version without adding any command to protocol v1. It
cannot listen on TCP or call a Vita module-manager API. All authentication,
trusted target resolution, and fixed-module lifecycle callbacks are mandatory;
initialization fails if any is absent.

The model enforces these properties before a future hardware backend exists:

- an attach requires both an authenticated, peer-bound shell session and a
  fresh signed operation authorization; the challenge exposes the transport
  binding and broker monotonic time/expiry needed to construct that signature;
- only an exact title and previously observed target generation are caller
  inputs; the trusted provider supplies PID and complete main-module identity;
- the signed operation is bound to that complete identity and is revalidated
  immediately before load/start;
- the loader boundary receives one fixed module slot and no path;
- the signed operation binds the authenticated-session expiry and requested
  duration; only the privileged loader allocates and journals the resulting
  lease ID/expiry;
- acquisition callbacks share one absolute deadline clamped to proof, session,
  and lease expiry, with time resampled after every blocking callback;
- stop and unload revalidate the complete original identity independently, in
  reverse order, using one cleanup deadline; and
- failures preserve ownership and acquired-resource flags in recovery state.

Host `DETACH`/`RECOVER` cleanup forwards the complete signed authorization for
independent privileged verification. Watchdog, disconnect, shutdown, initial
rollback, and recovery retry use distinct release-only journal capabilities.
Those capabilities match one exact existing lease record and cannot acquire,
start, retarget, or synthesize a lease. Repeated shutdown calls retry retained
recovery cleanup while continuing to reject new sessions.

Operation replay history is scoped to the unique service-generation/session/
challenge transcript. A connection is closed and reopened after 64 accepted
operations after an explicit `ROLLOVER_REQUIRED` result. The replacement
connection must authenticate again, and only a newly signed replacement-session
transcript is accepted; an old-session proof remains invalid. A read-only
privileged probe may terminally clear recovery
after target exit/relaunch only when it proves the exact generation/lease/module
resource is gone; it never authorizes mutation of the replacement PID.

Recovery authorization may come from the same still-authenticated owner
session, or after disconnect from a newly authenticated session for that same
paired-host key. It always requires a fresh signed `RECOVER` transcript.

The cryptographic verifier, persistent kernel replay journal, Vita-side title
and module allowlists, fixed path/digest mapping, listener, and loader backend
are intentionally absent. Host fakes prove state-machine behavior, not Vita
authority or loader safety.

## Mandatory rules for a future mutating version

- Require a paired host key and a new domain-separated Ed25519 authorization
  that is verified again by the kernel loader; do not rely on IP address,
  caller process identity, or possession of an old discovery ticket.
- Do not expose attach mutation through Vita Companion's current plaintext
  command grammar. A shell-resident module may be an architectural precedent,
  but the attach control plane requires its own authenticated protocol.
- Keep the allowlist on the Vita. A signed host request alone must not authorize
  an arbitrary title.
- Validate the SceShell container by title and program auth ID in kernel
  context, but do not mistake that for unique broker authentication: every
  `*main` user module shares SceShell's process identity and address space.
- Bind each mutation to the paired-host signature, both fresh nonces, exact
  operation and target identity, expiry, and kernel-maintained replay state.
- Revalidate target title, status, main module, fingerprint, and generation
  immediately before every lifecycle call.
- Use one compiled-in canonical debugger-module path. Never accept a path from
  the network.
- Verify the preinstalled debugger module's identity before load/start.
- Refuse system processes, SceShell, kernel processes, the broker itself,
  already-instrumented targets, and ambiguous/multiple ownership.
- Allocate the lease ID and bounded expiry inside the privileged loader while
  atomically journaling every acquired resource. Never trust shell-generated
  lease fields. A failed start must be treated as possibly partial, so cleanup
  first verifies stop/already-stopped and then unload.
- Never expose general foreign-process memcpy, arbitrary module load, arbitrary
  thread entry, or unrestricted kernel calls.
- Treat stop/unload failure as a recovery state; do not discard ownership until
  cleanup is verified.

The first hardware gate must use only a disposable target compiled specifically
for injection testing. Installing a loader plugin or trying another user
application requires a separate explicit review and approval.

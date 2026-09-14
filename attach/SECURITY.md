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
- Use a short kernel lease and record every successfully acquired resource for
  reverse-order rollback.
- Never expose general foreign-process memcpy, arbitrary module load, arbitrary
  thread entry, or unrestricted kernel calls.
- Treat stop/unload failure as a recovery state; do not discard ownership until
  cleanup is verified.

The first hardware gate must use only a disposable target compiled specifically
for injection testing. Installing a loader plugin or trying another user
application requires a separate explicit review and approval.

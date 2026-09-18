# Vita attach broker cores

This directory contains allocation-free broker, control, authentication
listener, and persistent-store state machines. The compile target packages
them and their Vita adapters into `libvitadebug_attach_broker.a`.

The protocol-v2 authentication storage/crypto boundary
`vitadebug_attach_auth.c` validates exact active key records, signs only through
an injected private-key callback, verifies Ed25519 with the repository's
vendored Monocypher, and provides constant-time fixed-value comparison.
The persistent store records only public local identity,
allowlist/revocation state, revision, and service generation without dynamic
allocation. Private-key generation/signing/destruction are delegated to an
opaque backend that must attest non-exportability and isolation. Persistence
must own handle-bound open/create/commit operations, and the monotonic floor
must attest an independent trust domain. Missing capabilities are rejected;
there is no Vita implementation of these trusted backends. Non-destructive
deinit wipes all resident metadata and copied backend vtables; destructive
uninstall is a separate tombstone operation.

`vitadebug_attach_auth_listener.c` implements only the four protocol-v2
authentication records. It uses one absolute deadline, exact frame sizes,
private-subnet source policy, fixed replay/backoff tables, and one serialized
connection. `vitadebug_attach_auth_listener_vita.c` owns the worker and
registered sockets; shutdown cancels blocked I/O and requires the worker to be
joined before network teardown. Disposable-title mode owns SceNet
load/init/term/unload. Shell-borrowed mode owns none of those operations,
performs no NetCtl transition, and cannot tear down shared networking.

`vitadebug_attach_broker.c` remains the protocol-v1 read-only discovery core.
`vitadebug_attach_control.c` is a host-tested lifecycle and authorization model
for a future, separate mutating protocol. Protocol v1 has not gained a mutation
record.

The core implements only:

- `HELLO` and read-only capability/ABI reporting;
- exact nine-character title discovery through an injected inventory callback;
- random, short-lived identity tickets bound to a peer, connection session,
  service generation, and complete target identity;
- one-use `RELEASE` with full identity revalidation;
- a 4096-byte fixed frame limit and one absolute deadline per exchange;
- request-ID replay rejection; and
- ticket/session invalidation during connection close or broker shutdown.

There is no PID request, target selection, memory/register access, process
stop/resume, module path, module load, injection, kernel-loader call, or GDB
attach in these components.

## Authenticated control model

The control core adds the state that a future shell-resident listener and
privileged fixed-module backend need, without implementing either component:

- a fresh service generation and monotonic per-generation session ID, plus a
  per-connection challenge that exposes its opaque transport binding and
  broker time/expiry basis to the signing host;
- a mandatory paired-host verifier, bounded authentication window, three
  attempts per challenge, and a 64-operation replay table per connection;
- a second mandatory verifier for every attach, detach, or recovery operation;
- separate domain-separated canonical peer/operation signing encoders with
  fixed-width, network-byte-order fields and signatures excluded;
- exact title plus expected launch-generation input, with PID and the rest of
  the identity supplied only by the trusted target provider;
- full title/PID/main-module/fingerprint/generation binding in the signed
  authorization forwarded to the future privileged boundary;
- revalidation after authorization, before start, and before each reverse-order
  stop/unload action;
- one globally owned, short-lived module lease with disconnect and watchdog
  cleanup, allocated and journaled by the privileged load boundary rather than
  synthesized by the shell core;
- signed host cleanup forwarding plus exact-journal, release-only capabilities
  for rollback, expiry, disconnect, shutdown, and recovery retries; and
- durable in-memory recovery state when stop/unload cannot be verified, plus a
  read-only privileged probe that may clear it only after proving the exact
  leased module resource is gone.

No public request or backend callback contains a module path. The core fills
`VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT`; a future kernel adapter must map that
slot to exactly one compiled-in, preinstalled, identity-checked debugger module
and independently verify the forwarded host authorization and replay state.

The host test backend only records modeled load/start/stop/unload calls. No Vita
adapter for those callbacks exists, so compiling this archive cannot inject a
module. See [the control model](../docs/control-model.md) for the boundary and
state transitions.

## Integration boundary

`VdAttachBrokerConfig` injects monotonic time, cryptographic entropy, and an
exact-title inventory provider. The inventory provider receives the same
absolute deadline as the exchange. `VdAttachTransport` injects bounded reads,
writes, peer identity, and close behavior. This makes the state machine
host-testable without pretending that a desktop mock proves Vita permissions.

The inventory callback is security-sensitive. It must enforce a Vita-side
title allowlist, resolve exactly one title ID, reverse-map the PID to the same
title, reject system targets, and return a trusted snapshot containing the main
module ID, fingerprint, and a nonzero target generation. The broker calls the
same provider at release and compares every field.

The protocol-v1 `vitadebug_attach_vita.c` adapter returns
`VD_ATTACH_INVENTORY_UNAVAILABLE`. Public user-mode AppMgr lookup can perform
the preliminary title/PID pair, but it cannot independently supply the trusted
foreign main-module identity required by protocol v1. Integrators must report
the service as unavailable until a separately reviewed read-only identity
provider is available. The broker never fabricates a fingerprint or issues a
weaker ticket.

The v2 listener serializes calls for its authentication authority and its
transport callbacks honor one absolute deadline. Shutdown closes the listening
and active descriptors before the Vita runtime joins its sole worker.

Each connection accepts at most 64 unique request IDs (including `HELLO`). A
client should reconnect before that fixed replay table is exhausted. Closing a
connection immediately invalidates its read-only ticket.

## Tests and cross-build

From this directory:

```text
make host-test
make vita-lib
```

The read-only host test covers normal discovery/release, identity change,
expiration, one-use behavior, request replay, wrong peer/session, partial
transport I/O, oversized frame rejection without draining the advertised
payload, one absolute exchange deadline, fail-closed Vita inventory, and
shutdown.

The control host test covers mandatory peer and operation authentication,
C/Python `ATTACH`/`DETACH`/`RECOVER` golden signing vectors, min/max leases,
invalid SceUID boundaries, every signed-field mutation, host-visible
peer/time challenge binding, explicit connection-scoped replay rollover,
stale-generation rejection at every revalidation point, the fixed module slot,
callback-deadline expiry rollback, late cleanup callback resampling,
partial-start rollback, reverse-order
stop/unload, disconnect cleanup, watchdog expiry, resumable unload failure,
PID-reuse refusal and verified-gone reconciliation, privileged lease-grant and
journal-capability enforcement, reinitialization refusal, and idempotent
shutdown recovery retries.

`make vita-lib` is a compile gate only. It creates a static ARM library; it
does not install, enable, or run anything on a Vita. The separate
`vita-auth-test` VPK links the same sources and defaults to runtime fail-closed
until the hardware storage proofs exist.

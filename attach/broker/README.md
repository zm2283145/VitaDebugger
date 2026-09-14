# Read-only Vita attach broker core

This directory contains the allocation-free protocol-v1 broker state machine.
It cross-compiles into `libvitadebug_attach_broker.a`, but it is **not** a
resident Vita plugin, listener, VPK, or external debugger injector.

The core implements only:

- `HELLO` and read-only capability/ABI reporting;
- exact nine-character title discovery through an injected inventory callback;
- random, short-lived identity tickets bound to a peer, connection session,
  service generation, and complete target identity;
- one-use `RELEASE` with full identity revalidation;
- a 4096-byte fixed frame limit and one absolute deadline per exchange;
- request-ID replay rejection; and
- ticket/session invalidation during connection close or broker shutdown.

There is no PID request, memory/register access, process stop/resume, module
path, module load, injection, or kernel call in this component.

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

The current `vitadebug_attach_vita.c` adapter returns
`VD_ATTACH_INVENTORY_UNAVAILABLE`. Public user-mode AppMgr lookup can perform
the preliminary title/PID pair, but it cannot independently supply the trusted
foreign main-module identity required by protocol v1. Integrators must report
the service as unavailable until a separately reviewed read-only identity
provider is available. The broker never fabricates a fingerprint or issues a
weaker ticket.

The core does not provide internal locks. A service must serialize calls for a
broker instance, and its transport callbacks must honor the supplied absolute
deadline. Shutdown invalidates all in-memory state immediately; a listener is
responsible for cancelling/closing any blocked transport so its callback can
return by that deadline.

Each connection accepts at most 64 unique request IDs (including `HELLO`). A
client should reconnect before that fixed replay table is exhausted. Closing a
connection immediately invalidates its read-only ticket.

## Tests and cross-build

From this directory:

```text
make host-test
make vita-lib
```

The host test covers normal discovery/release, identity change, expiration,
one-use behavior, request replay, wrong peer/session, partial transport I/O,
oversized frame rejection without draining the advertised payload, one
absolute exchange deadline, fail-closed Vita inventory, and shutdown.

`make vita-lib` is a compile gate only. It creates a static ARM library; it
does not install, enable, or run anything on a Vita.

# VitaDebugger external-application attachment

This subtree is the first, deliberately read-only milestone toward attaching
VitaDebugger to a Vita application that was not linked with `libuvdb.a`.

It does **not** attach to a process today. It does not load a module, suspend a
process, write memory, or start GDB. It now contains an allocation-free C broker
core, an authentication-only protocol-v2 listener/lifecycle implementation,
and a persistent key-store implementation. Retail 3.65 authentication remains
hard-blocked because no approved Vita backend provides an isolated
non-exportable key, trusted handle-bound persistence, and an independent
durable monotonic floor. This fixes the identity and authentication boundary
before any privileged loader operation is added:

- a strict versioned wire format;
- an exact-title discovery request with no host-selected PID;
- a capability and kernel-ABI handshake;
- a short-lived identity ticket bound to the broker generation, title, PID,
  main module ID, and main-module fingerprint;
- an explicit ticket release operation; and
- a bounded host client that has no mutation command.

The separation is intentional. The current kernel companion scopes its thread,
stop, register, VFP, and hardware-debug operations to the process that called
the export. A separate attach broker would therefore control only its own
process, not the requested target. That is a useful security property and is
not weakened by this milestone.

## Candidate architecture

VitaSDK exposes a better future path than copying a payload into arbitrary
memory or hijacking a target thread:

1. A dedicated user-mode broker resident in the shell (a narrowly scoped
   `*main` module, following the lifecycle pattern used by Vita Companion)
   receives an authenticated request for one allowlisted title ID. A normal
   foreground homebrew application is not a suitable always-on broker.
2. It resolves the exact title with `sceAppMgrGetIdByName()` and checks the
   reverse `sceAppMgrGetNameById()` mapping.
3. A narrow future kernel loader export independently verifies a paired-host
   signature and replay state, then revalidates the PID, title, and main module
   before loading one fixed, preinstalled, identity-checked debugger `.suprx`
   with `ksceKernelLoadStartModuleForPid()`.
4. Once running inside the target process, that module uses VitaDebugger's
   existing caller-scoped kernel ABI. No general foreign-process memory API is
   needed for normal GDB operation.
5. A lease and ownership record guarantee cleanup with the matching per-PID
   stop/unload APIs if initialization fails or the controller disappears.

[Vita Companion](https://github.com/devnoname120/vitacompanion) demonstrates
that a `*main` user module can remain available for development services while
another title runs. Its current unauthenticated
command transport must not be reused for attach mutation; only the lifecycle
pattern is relevant here.

The VitaSDK declarations establish that this design is possible to investigate;
they do not establish that it is safe or permitted for every retail title or
firmware. No loader call has been added or hardware-tested here. See
[the API audit](docs/api-audit.md) for the exact boundary and remaining gates.

A `*main` broker runs inside SceShell; it is not a separately identified
process. SceShell title/auth-ID checks can validate the container but cannot
distinguish that broker from another installed SceShell module. Therefore a
future kernel mutation path must verify the signed host authorization itself
and must not treat caller process identity as sufficient authorization.

## Broker core and host scaffold

`broker/` implements the version-1 Vita-side state machine behind injected
transport, clock, entropy, and exact-title inventory callbacks. It has fixed
memory bounds, uses one absolute deadline per framed exchange, rejects request
ID replay, binds each session to an opaque transport peer, expires tickets, and
revalidates the full target identity during one-use release. Its host tests
cover wrong-peer/session requests, expiration, replay, partial I/O, oversized
frames, identity changes, and shutdown.

The broker directory also contains a separate authenticated control-plane
model plus an authentication-only protocol version 2 implementation. It owns
challenge/authentication state, operation replay protection, exact
target-generation binding, one fixed debugger-module slot,
privileged-allocated lease grants, independently authorized cleanup
capabilities, and reverse-order leased rollback. The version-2 listener has no
target or control wire command and no module path. Version 2 currently defines
only fixed `HELLO`, `CHALLENGE`, `PROOF`, and `RESULT` records. Its host model
uses the repository's Ed25519 provider, and the C verifier reuses vendored
Monocypher. The serialized Vita listener owns one worker and tracks its listen
and accepted sockets for cancellation. The device key-store core now separates
public metadata persistence from opaque non-exportable signing operations and
rejects untrusted path-only, same-namespace rollback, or raw-seed backends.
There is no Vita secure-storage backend. Protocol v1 remains read-only. See
[the control model](docs/control-model.md).

The ARM static-library target remains a compile-only gate. A safe disposable
`VDAT00001` VPK is now a sentinel-only hard-block artifact: it records the
retail-3.65 secure-storage block and exits without provisioning a key or
initializing networking. It is not shell-resident. The protocol-v1 Vita inventory
adapter remains unavailable because public user APIs cannot provide the trusted
foreign main-module identity required to issue a ticket. See
[the broker boundary](broker/README.md) and
[the hardware gate](docs/hardware-auth-gate.md).

Run all broker and host-client tests from the repository root:

```powershell
make host-test-attach
```

The protocol-v1 launcher exposes only `status` and `discover`:

```powershell
py -3 attach/tools/vdattach.py --help
py -3 attach/tools/vdattach.py status `
  --host 192.0.2.10 --port 1235 --json
py -3 attach/tools/vdattach.py discover `
  --host 192.0.2.10 --port 1235 --title-id UVDBDEMO1 --json
```

`192.0.2.10` is a documentation-only address. There is intentionally no
default broker port: a developer must select the endpoint explicitly. These
commands cannot work against the current VitaDebugger installation because no
resident v1 listener or trusted Vita identity provider exists yet. They are
usable against the bounded fake broker in the tests and will become the
compatibility gate for a future service.

The discovery command releases its opaque ticket before printing. It never
prints the ticket, accepts a raw PID, or sends a module path.

Protocol v2 has separate authentication-only tools:

```powershell
py -3 attach/tools/vdattach_auth_provision.py --help
py -3 attach/tools/vdattach_auth.py --help
.\attach\tools\build_auth_gate.ps1
```

The provisioning/client commands are limited to host fixtures while the
retail-3.65 hard block remains. Do not point them at `10.1.1.217`. The build
publishes a safe sentinel-only `VDAT00001` VPK and hash/source manifest under
`attach/dist/auth-gate/`; the VPK never starts networking. The future protocol
transport remains signed plaintext with no encryption.

## Layout

- `include/vitadebug_attach_protocol.h` contains constants for a future C
  broker without defining a mutating ABI.
- `broker/` contains the read-only C broker core, fail-closed Vita adapter, and
  native tests.
- `host/vdattach/` contains the strict codec and bounded stateful client.
- `host/vdattach/control_signing.py` mirrors the canonical peer and operation
  signing transcripts for cross-language golden-vector verification.
- `host/vdattach/auth_protocol.py`, `auth.py`, and `auth_keys.py` implement the
  bounded protocol-v2 authentication model and atomic host key store.
- `host/vdattach/auth_cli.py` and `auth_provision.py` provide the separate
  authentication and public-only provisioning tools.
- `vita-auth-test/` builds the safe sentinel-only hard-block VPK.
- `tools/vdattach.py` runs the package without installation.
- `tests/` covers canonical encoding, frame limits, capability/ABI gates,
  session binding, exact-title discovery, and ticket release.
- `docs/protocol-v1.md` is the complete version-1 wire contract.
- `docs/protocol-v2-auth.md` is the authentication-only version-2 contract.
- `docs/auth-provisioning.md` defines host and injected Vita key storage.
- `docs/hardware-auth-gate.md` is the serialized future hardware runbook.
- `docs/retail-365-secure-storage-audit.md` seals the public-API NO-GO
  decision and exact evidence required to unblock it.
- `docs/api-audit.md` records the relevant current and VitaSDK APIs.
- `SECURITY.md` defines promotion gates for any later loader implementation.

## Milestone status

| Component | Status |
| --- | --- |
| Canonical read-only protocol and frame bounds | Implemented and host-tested |
| Exact-title request and identity-ticket model | Implemented and host-tested |
| Capability/kernel-ABI handshake | Implemented and host-tested |
| Ticket release and connection state machine | Implemented and host-tested |
| Allocation-free Vita broker state machine | Implemented and host-tested; current control contract passes its ARM rebuild |
| Authenticated listener-facing control state | Host-tested model; no TCP/crypto adapter |
| Serialized Vita authentication listener/lifecycle wrapper | Implemented and host-tested; SceNet ownership modes are explicit; sentinel VPK never starts it |
| Trusted Vita foreign-target identity adapter | Fail-closed stub; provider required |
| Read-only kernel target-identity/ticket export | Not implemented |
| Broker authentication/pairing crypto | Host model, Monocypher, public-only metadata store, listener, and provisioning formats implemented; production Vita authentication is hard-blocked and has no secure-storage backend |
| Fixed debugger-module loader request model | Implemented and host-tested; privileged backend owns lease grant, no path input or Vita backend |
| Foreign-process `.suprx` loader export | Not implemented or hardware-tested |
| Injected debugger `.suprx` | Not implemented |
| Loader rollback/unload lease model | Implemented and host-tested with signed host cleanup and release-only journal capabilities; hardware validation not started |
| Live GDB attach to an unmodified application | Not available yet |

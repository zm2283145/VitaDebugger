# VitaDebugger external-application attachment

This subtree is the first, deliberately read-only milestone toward attaching
VitaDebugger to a Vita application that was not linked with `libuvdb.a`.

It does **not** attach to a process today. It does not load a module, suspend a
process, write memory, or start GDB. It now contains an allocation-free C broker
core that cross-compiles for Vita, while its platform identity adapter remains
fail-closed and no resident listener is shipped. This fixes the discovery and
identity boundary before any privileged loader operation is added:

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
model plus an authentication-only protocol version 2 foundation. It owns challenge/authentication state,
operation replay protection, exact target-generation binding, one fixed
debugger-module slot, privileged-allocated lease grants, independently
authorized cleanup capabilities, and reverse-order leased rollback. It has no wire command,
TCP listener, Vita lifecycle adapter, or module path. Version 2 currently
defines only fixed `HELLO`, `CHALLENGE`, `PROOF`, and `RESULT` records. Its host
model uses the repository's Ed25519 provider, and the C verifier reuses vendored
Monocypher. The device key-store adapter remains deliberately unavailable.
Its lifecycle callbacks are exercised only by host fakes. Protocol v1 remains
read-only. See [the control model](docs/control-model.md).

The ARM static-library target remains a compile-only gate. The current
privileged-lease contract revision is host-tested and passes its serialized
VitaSDK rebuild. This is still not a running Vita service: no
socket listener or shell-resident module is built, and the current Vita
inventory adapter returns unavailable because public user APIs cannot provide
the trusted foreign main-module identity required to issue a ticket. See
[the broker boundary](broker/README.md).

Run all broker and host-client tests from the repository root:

```powershell
make host-test-attach
```

The standalone launcher exposes only `status` and `discover`:

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
resident listener or trusted Vita identity provider exists yet. They are usable
against the bounded fake broker in the tests and will become the compatibility
gate for a future service.

The discovery command releases its opaque ticket before printing. It never
prints the ticket, accepts a raw PID, or sends a module path.

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
- `tools/vdattach.py` runs the package without installation.
- `tests/` covers canonical encoding, frame limits, capability/ABI gates,
  session binding, exact-title discovery, and ticket release.
- `docs/protocol-v1.md` is the complete version-1 wire contract.
- `docs/protocol-v2-auth.md` is the authentication-only version-2 contract.
- `docs/auth-provisioning.md` defines host and injected Vita key storage.
- `docs/hardware-auth-gate.md` is the serialized future hardware runbook.
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
| Resident Vita listener/lifecycle wrapper | Not implemented or installed |
| Trusted Vita foreign-target identity adapter | Fail-closed stub; provider required |
| Read-only kernel target-identity/ticket export | Not implemented |
| Broker authentication/pairing crypto | Host model and Monocypher verifier implemented; Vita persistent key storage and resident listener unavailable/fail closed |
| Fixed debugger-module loader request model | Implemented and host-tested; privileged backend owns lease grant, no path input or Vita backend |
| Foreign-process `.suprx` loader export | Not implemented or hardware-tested |
| Injected debugger `.suprx` | Not implemented |
| Loader rollback/unload lease model | Implemented and host-tested with signed host cleanup and release-only journal capabilities; hardware validation not started |
| Live GDB attach to an unmodified application | Not available yet |

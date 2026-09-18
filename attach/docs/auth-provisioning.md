# Authenticated attach key provisioning

Authentication uses Ed25519 through the repository's existing
`VitaDevDeploy` provider on the host and vendored Monocypher verification in
the portable Vita protocol core. There is no fallback algorithm, unsigned
mode, or encryption; the protocol is signed plaintext.

## Host storage

`vdattach.auth_keys.JsonKeyStore` is the host implementation. Its injected
Ed25519 provider is `deploy/host/vitadevdeploy/crypto.py`; provider absence is
an error, never a downgrade.

The host store contains:

- `state.json`, schema 1, with a monotonically increasing revision;
- one active local key ID/generation and private/public PEM filenames; and
- exact peer IDs/generations, public PEM filenames, and active/revoked status.

Private files request owner-only permissions where supported. Reads reject
links, non-regular files, oversized files, unsupported schemas, unknown
fields, noncanonical IDs, and missing material. Do not log PEM content,
allowlists, signatures, or nonces.

The host-only provisioning helper may generate a public 72-byte `VDAP` bundle
and parse a public 56-byte `VDAR` receipt. Those formats contain no private
material. Under the current hardware decision they are test fixtures only:
do not copy a bundle to a Vita and do not provision a real device identity.

## Device storage contract

`VdAttachAuthKeyStorage` is listener-facing and intentionally exposes only:

- active local-public-key loading;
- exact peer ID/generation lookup with active/revoked distinction; and
- signing by opaque local key ID/generation.

It has no provisioning, rotation, seed import, seed export, or peer
administration callback. Administration is a separate local store API.

The schema-2 metadata core persists only public local identity, exact peer
allowlist/revocation state, revision, and service generation. It never stores
or retains an Ed25519 seed or expanded private key. Its three injected
backends are separate:

1. a trusted persistence backend owns handle-bound current/staged
   open/create/read/write/sync/close/commit/discard operations;
2. a monotonic backend owns a durable floor in an independent trust domain;
3. an opaque private-key backend generates, identifies, signs with, and
   destroys a non-exportable key isolated from the metadata principal.

Initialization rejects a raw-seed key backend, a floor in the metadata
namespace, missing handle-bound/durable-commit capability, missing
non-exportability/isolation capability, or any incomplete callback set.
There is deliberately no `export_seed` contract.

Capability bits and distinct trust-domain tokens are structural fail-closed
checks, not hardware assurance. A Vita implementation must not populate them
until each backend and its caller isolation have independent evidence. A
file-backed implementation must supply no-follow, already-open regular-file
handles; the core never performs a pathname check followed by a separate open.

The core is bounded and allocation-free. It rejects malformed, truncated,
trailing, oversized, unknown-schema, duplicate, stale, and mismatched-key
state. A commit writes and syncs a staged handle, closes it, asks the trusted
backend to publish it durably, and only then advances the independent floor.
The core receives no storage path and makes no boolean claim that a pathname
is private.

Schema 1 contained a raw device seed and is rejected rather than migrated.
No seed-bearing schema-1 file was provisioned on the authorized device. A
future migration would require a separate reviewed retirement procedure; the
schema-2 core never opens or exports that seed.

`vd_attach_auth_store_deinit()` is non-destructive and idempotent. It wipes
all resident metadata and copied backend vtables without deleting persistent
state or opaque keys. Call it after normal stop and on every initialization,
bind, listener-start, worker-start, or unload failure. The destructive
`vd_attach_auth_store_uninstall()` instead commits a key-free tombstone above
the rollback floor and asks the opaque key backend to destroy the retired
identity. Deleting metadata alone is not uninstall and cannot lower the
monotonic floor or reactivate an older identity.

## Retail 3.65 decision

Production Vita authentication is a **hard block**. Public or safely callable
retail 3.65 APIs do not provide both:

- a non-exportable Ed25519 key inaccessible to ordinary/co-resident SceShell
  code; and
- an independent durable monotonic anti-rollback floor.

Running as `*main` shares the SceShell identity and memory. `0600` mode bits
are not documented per-title isolation. Public `sceIo` has no documented
no-follow/openat contract and no documented atomic, power-loss-safe
rename-plus-directory-sync contract. Registry, savedata, and SQLite state can
be rolled back with the metadata. IdStorage, Syscon, NVS, PFS, and SceSbl
would require unsupported or reverse-engineered privileged mechanisms. The
public RNG is suitable for entropy but does not solve persistence or key
isolation.

Accordingly, there is no Vita backend for the three trusted interfaces. The
sentinel VPK cannot accept a backend override, never generates a real seed,
never consumes the public provisioning bundle, and never starts networking.

## Rotation and revocation model

For a future approved backend, local rotation must generate a new opaque key,
commit the new public identity and incremented metadata revision above the
floor, then retire the old opaque key. Peer rotation still requires explicit
allowlisting of the new exact ID/generation followed by durable revocation of
the old entry. A revoked generation is never silently replaced by another
generation with the same ID.

Receipt publication through public Vita file APIs must not be described as an
atomic replacement. A future characterization artifact may write a
non-sensitive, one-shot sentinel receipt only after proving the destination
does not exist; that result is evidence about the test run, not evidence of
secure provisioning or durability.

Authentication provides integrity and peer authorization, not
confidentiality. If network observers are in scope, keep the listener disabled
until an encrypted transport is separately designed and reviewed.

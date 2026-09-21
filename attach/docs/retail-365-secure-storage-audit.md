# Retail 3.65 secure-storage API audit

## Decision

**NO-GO.** As of 2026-09-20, the reviewed public VitaSDK surface does not
prove that a retail Vita on firmware 3.65 can provide all three properties
required by the authentication design:

1. an Ed25519 signing key that is non-exportable and inaccessible to ordinary
   or co-resident SceShell code;
2. trusted handle-bound, no-follow, durable persistence and commit semantics;
3. an independent durable monotonic anti-rollback floor.

The properties are conjunctive. A partial match is a failed gate. No device
probe can establish a missing public contract, so `10.1.1.217` was not
contacted during this audit.

Even future proof of those three platform properties would not promote the
current store interface by itself. The floor update must also be atomically or
cryptographically bound to the exact staged metadata object, as described
under "Composition defect in the portable model."

This decision was made against:

- authenticated-listener foundation commit
  `39a072467710d53b821d61bb1425872bc4249254`;
- installed `vita-headers` commit
  `ebc8f4f7ac8313fe4f74ca95724116b224bad184`, as recorded by the local
  VitaSDK installation.

The installed import databases include pre-release 0.931 and 0.990 catalogs
and release catalogs labeled 3.60 and 3.63; none is labeled 3.65. Their
contents can identify candidates but cannot prove a public retail-3.65
contract. A declaration or import stub alone also does not prove
authorization, isolation, durability, or power-loss behavior.

## Threat model

The gate assumes an attacker may:

- run ordinary code or another plugin co-resident in SceShell and therefore
  share the shell process identity and address space with a `*main` module;
- read, replace, link-swap, or restore ordinary shell-accessible metadata;
- restore an earlier snapshot across reboot;
- interrupt power or terminate the process between any two persistence
  operations; and
- replay previously valid authentication material.

The platform kernel and any future independently reviewed trusted service may
remain trusted. Private NIDs, guessed imports, reverse-engineered privileged
behavior, kernel probes, and undocumented firmware behavior are outside this
audit and cannot satisfy the gate.

## Reviewed surface

### Property 1: isolated non-exportable Ed25519 key

The public PSP2 headers and installed VitaSDK catalog expose no documented
user-mode Ed25519 keystore that generates a key, returns only its public key
and signatures, forbids export, and denies use or inspection by co-resident
SceShell code.

The repository's Ed25519 implementation is software Monocypher. Its key bytes
would reside in the caller's address space and therefore do not satisfy the
SceShell isolation requirement. The opaque callback boundary in
`broker/include/vitadebug_attach_auth_store.h` is an interface, not evidence
that a Vita implementation exists.

Catalog entries associated with SceSbl, NVS, IdStorage, Syscon, or PFS are
kernel/private candidates, not documented public user-mode Ed25519 services.
They are not approved substitutes.

**Result: unavailable.**

### Property 2: handle-bound no-follow durable commit

The PSP2 I/O surface declares path-based operations including:

- `sceIoOpen(const char *, ...)` in `psp2/io/fcntl.h`;
- `sceIoRename(...)` in `psp2/io/fcntl.h`;
- `sceIoSync(...)` in `psp2/io/fcntl.h`; and
- `sceIoSyncByFd(...)` in `psp2/io/fcntl.h`.

The reviewed PSP2 contract does not document descriptor-relative open or
rename, atomic no-follow open, same-handle regular-object validation,
directory synchronization, or power-loss-safe rename/publication semantics.
`SCE_S_ISLNK` and `SCE_S_ISREG` in `psp2common/kernel/iofilemgr.h` do not close
a pathname-stat followed by pathname-open race.

Generic newlib declarations such as `O_NOFOLLOW`, `openat`, `fsync`,
`fdatasync`, and `renameat` are not a documented Vita `sceIo` import contract
or a retail-3.65 durability guarantee.

**Result: unavailable and unprovable.**

### Property 3: independent durable monotonic floor

No reviewed public user-mode API provides a persistent counter that cannot be
decremented, reset, or restored with ordinary metadata. The closest named
counter declaration, `ksceKernelAtomicIncrementHighwaterCounter()` in
`psp2kern/kernel/cpu/atomic.h`, operates on caller-supplied memory and is not a
durable service.

Registry, savedata, SQLite, and ordinary files remain in the metadata rollback
domain. IdStorage, Syscon, NVS, PFS, and SceSbl surfaces are privileged or
undocumented for this purpose and are excluded.

**Result: unavailable.**

## Composition defect in the portable model

Even hypothetical implementations of the current callback interfaces would
not complete the proof. `vd_attach_auth_store.c` publishes staged metadata
before calling the revision-only `advance_floor()` callback. Interruption
after publication and before floor advancement permits restoration of the
previous metadata revision while it is still equal to the old accepted floor.

Reversing those two calls is not sufficient: interruption after advancing a
revision-only floor but before publication would strand recovery unless the
floor is cryptographically bound to the exact staged object. The current
`load_floor(uint64_t *)` and `advance_floor(uint64_t)` interface carries no
metadata digest or unforgeable transaction token.

Accordingly, capability bits and distinct context pointers remain structural
negative checks only. They cannot promote this model to a production Vita
store.

## Consequences

The implementation stack stops at the existing sentinel:

- no Vita identity is provisioned;
- no bundle or private material is transferred;
- no authentication listener or target identity provider is started;
- no loader, injection, target-process operation, or GDB attach is built as a
  deployable artifact in the external-attach stack;
- SceShell and system processes remain forbidden targets; and
- the future transport remains described as signed plaintext with no
  encryption.

`vd_attach_auth_store_vita_init()` must continue wiping the supplied state and
returning `VD_ATTACH_AUTH_STORE_ERROR_UNAVAILABLE`. The `VDAT00001` package
must remain sentinel-only and import only its reviewed status-file/runtime
dependencies.

## Exact evidence required to unblock

A new GO review requires all of the following:

1. A documented public retail-3.65 service that generates Ed25519 keys,
   returns only public keys/signatures, never exports key material, and
   enforces an authorization boundary against ordinary and co-resident
   SceShell code.
2. A documented handle/object persistence API with exclusive staged creation,
   no-follow semantics at open time, same-handle regular-object validation,
   durable data sync, atomic durable publication, and explicit
   directory/journal persistence and interrupted-commit recovery guarantees.
3. A durable monotonic service outside the metadata rollback domain, with
   authenticated load/advance operations and no caller-accessible
   decrement/reset path.
4. Either one trusted service that atomically commits metadata and monotonic
   state, or a redesigned monotonic record that binds the new revision to a
   cryptographic digest or unforgeable token for the exact durable staged
   object.
5. Independent retail-3.65 evidence for co-resident access denial, reboot
   persistence, old-snapshot restoration rejection, link/path substitution
   rejection, and fault injection after every durable operation.

Until every item is reviewed and proven, the hard block is final. Hardware
access is unnecessary and blocked by this prerequisite gate.

# Authenticated attach key provisioning

Authentication uses Ed25519 through the repository's existing
`VitaDevDeploy` provider on the host and vendored Monocypher verification on
Vita. No fallback algorithm or unsigned mode exists.

## Host storage

`vdattach.auth_keys.JsonKeyStore` is an atomic host implementation. Its injected
Ed25519 provider should be `deploy/host/vitadevdeploy/crypto.py`; that provider
uses Python `cryptography` or an explicitly selected Ed25519-capable OpenSSL.
Provider absence is an error, never a downgrade.

The store contains:

- `state.json`, schema 1, with a monotonically increasing revision;
- one active local key ID and generation plus private/public PEM filenames;
- an explicit list of peer key IDs, generations, public PEM filenames, and
  `active` or `revoked` status.

State and private keys are atomically replaced. Private files request
owner-only permissions where the operating system supports them. Reads reject
links, non-regular files, oversized files, unsupported schemas, unknown fields,
noncanonical key IDs, and missing material. Applications must not log PEM
content, raw public-key allowlists, signatures, or nonces.

Provisioning is an explicit API operation:

```python
from pathlib import Path
from vdattach.auth_keys import JsonKeyStore
from vitadevdeploy.crypto import get_backend

store = JsonKeyStore(Path("private-host-key-store"), get_backend("auto"))
identity = store.provision(local_key_id=0x0123456789abcdef)
```

Copy only `identity.public_pem` and its exact key ID/generation through an
authenticated out-of-band channel to the Vita provisioning process. Copy the
Vita public key, key ID, and generation back through that channel and call
`allow_peer`. Never copy either private key.

Rotation creates a different key ID and increments the local generation. The
new key files and state become durable before old private files are removed.
The remote peer must explicitly allow the new ID/generation; there is no
automatic trust inheritance. Keep the old remote allowlist entry only for a
bounded transition, then revoke it. A revoked exact generation fails
immediately and is never replaced by another generation with the same ID.

## Vita storage boundary

`VdAttachAuthKeyStorage` is the exact injected device boundary. It requires:

- active local-public-key loading;
- exact peer key ID/generation lookup with active/revoked distinction;
- local signing without exporting the private key;
- atomic local provision and rotation;
- durable peer allow and revoke.

All callbacks are mandatory. The active local key ID, generation, and public
key must come from one persistent revision. Rotation must durably activate the
replacement before retiring the old key. Revocation must survive reboot before
returning success.

No reviewed Vita implementation exists yet.
`vd_attach_auth_vita_unavailable_storage()` returns a vtable with no callbacks;
validation and every authentication attempt therefore fail closed. This layer
does not prescribe a device path because choosing permissions, backup,
anti-rollback behavior, and deletion semantics requires hardware review.

The eventual device implementation must protect the 32-byte Ed25519 seed,
prevent SceShell peers from reading or replacing it, authenticate state
revisions against rollback, and zero temporary secret buffers with
`crypto_wipe`. Signature verification already uses the vendored Monocypher
`crypto_ed25519_check`; fixed authentication-value comparisons use the
provided constant-time helper.

## Revocation response

On suspected compromise:

1. stop the listener and invalidate all sessions;
2. revoke the compromised exact key ID/generation on both peers;
3. persist and verify the new revisions before restarting;
4. provision distinct replacement key IDs out of band;
5. confirm old proofs fail and backoff is applied;
6. never reuse an old service generation, session ID, or nonce.

Authentication provides integrity and peer authorization, not confidentiality.
If network observers are in scope, keep the listener disabled until an
encrypted transport is separately designed and reviewed.

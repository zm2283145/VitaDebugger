# Later Vita authentication hardware gate

The authorized Vita at `10.1.1.217` is **not** accessed by this layer. Hardware
changes must be serialized by the coordinating session. The current hardware
gate result is **STOP** because this repository does not yet build a resident
listener or a reviewed Vita key store. The host tests and ARM static archive
are not deployable service artifacts.

## Required future artifacts

Do not start a hardware run until a separate change produces all of these:

1. a shell-resident authentication-only broker module, proposed name
   `vitadebug_attach_auth_broker.suprx`;
2. a signed build manifest containing SHA-256 for that module, its source
   commit, VitaSDK version, Monocypher source hashes, protocol version `2`, and
   the compiled listener port;
3. a reviewed persistent `VdAttachAuthKeyStorage` implementation and storage
   threat analysis;
4. a host test bundle from the same commit;
5. an offline-generated host public key and exact key ID/generation;
6. an offline-generated device public key and exact key ID/generation;
7. an uninstall/restore package for the exact shell-module configuration.

Record SHA-256 with:

```powershell
Get-FileHash -Algorithm SHA256 <artifact>
```

Hashes must match the reviewed manifest before FTP or direct TCP transfer.
Never transfer host or device private keys. VitaDevDeploy direct TCP may carry
the reviewed artifact only after central approval; its transport does not
replace the version-2 peer authentication.

## Endpoint and configuration

Reserve TCP `18195` for the first authentication-only experiment. Do not reuse
Vita Companion FTP `1337`, VitaDevDeploy direct TCP, the existing debugger
port, or an unauthenticated Vita Companion command grammar. Bind only the
private-LAN interface and allow one serialized test connection.

The first broker must be either the reviewed shell-resident `*main` module or a
foreground disposable test title built solely for authentication validation.
If a foreground title is used, assign a unique development title ID and do not
claim always-on behavior. No target-title allowlist, debugger module, kernel
loader, process mutation, or GDB endpoint belongs in this run.

## Provisioning sequence

1. Disconnect the Vita from untrusted networks and record firmware, installed
   plugin list, current shell-module configuration, and free storage.
2. Generate host and device Ed25519 keys offline with the reviewed provider.
3. Record key IDs, generations, and SHA-256 of public keys only.
4. Provision the device private seed through the reviewed storage adapter;
   verify it cannot be read back through the broker API.
5. Add the host public key as one explicit active Vita peer.
6. Add the device public key as one explicit active host peer.
7. Reboot, reload both stores, and verify IDs/generations and revision counters.
8. Install/start the authentication-only artifact and confirm port `18195` is
   the only new listener.

## Expected records

Capture metadata, not key or signature bytes. One successful exchange is:

1. host `HELLO`, 88-byte payload;
2. Vita `CHALLENGE`, 240-byte payload;
3. host `PROOF`, 248-byte payload;
4. Vita `RESULT`, 288-byte payload, status `0`.

Verify changing every key generation, service/session/transport binding,
expiry, or nonce causes denial. Verify replaying `HELLO` or `PROOF` fails,
three failures increase backoff, an old key fails after rotation/revocation,
partial and oversized frames close without allocation growth, and protocol-v1
records never enter the v2 parser. Traffic is signed but plaintext; packet
capture must show no private key material.

## Cancellation, recovery, and rollback

Host cancellation shuts down and closes the TCP socket. Broker shutdown first
rejects new work, invalidates challenges/sessions, then closes accepted sockets
so partial reads terminate within the absolute handshake deadline. Reboot must
not resurrect a session or replay cache generation.

There are no target resources to recover in this layer. Rollback means:

1. stop the authentication listener;
2. uninstall the reviewed broker module or disposable test title;
3. restore the recorded shell-module configuration;
4. revoke test peer keys and remove device private material using the reviewed
   storage deletion procedure;
5. reboot and confirm port `18195` is closed and Vita Companion behavior is
   unchanged.

## Immediate stop conditions

Stop without retry if any artifact hash differs, storage reload changes key
identity unexpectedly, a revoked key authenticates, a stale generation is
accepted, an unknown peer receives a session, a malformed/oversized frame keeps
the connection alive, shutdown leaves port `18195` listening, any private key
or nonce is logged, a v1 record reaches v2 state, another title becomes
unstable, or any process/module/GDB mutation surface appears.

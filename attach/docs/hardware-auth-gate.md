# Vita authentication-only hardware gate

The authorized device for this gate is exactly `10.1.1.217`. This layer does
not access it. All transfer, installation, provisioning, launch, capture, and
rollback steps must be performed later by the single coordinating hardware
session.

## Current decision: STOP

The repository now builds `VDAT00001` (`VitaDebuggerAuthGate.vpk`) and contains
the serialized listener, Vita lifecycle wrapper, persistent key-store format,
public-only provisioning exchange, host client, and host fakes. The default
artifact still exits before networking because Vita public APIs do not prove:

1. that `ur0:data/VitaDebugger/private/attach-auth.store` and its temporary
   file are unreadable and irreplaceable by other SceShell-resident code;
2. that `sceIoRename` plus file/parent sync is atomic and durable across power
   loss on the selected filesystem; or
3. an independent monotonic rollback floor that an attacker cannot restore
   together with the store.

Do not install or start the listener until a separately reviewed hardware
assurance source implements all three callbacks and independent review changes
this decision to GO. Omitting or failing any callback produces
`reason=hardware-storage-assurance-missing` and never opens TCP `18195`.

## Exact build and artifact set

From the reviewed commit and a clean worktree:

```powershell
make host-test-attach
make -C attach vita-lib
.\attach\tools\build_auth_gate.ps1
```

The last command publishes only:

```text
attach/dist/auth-gate/eboot.bin
attach/dist/auth-gate/VitaDebuggerAuthGate.vpk
attach/dist/auth-gate/manifest.json
```

`manifest.json` records the source commit, VitaSDK compiler version, title ID,
protocol version, endpoint, absolute deadline, hardware-assurance state,
artifact SHA-256 values, and exact hashes of both Monocypher sources and every
listener/store source. It must say:

```text
title_id=VDAT00001
protocol=2
scope=authentication-only
transport=signed-plaintext
encryption=none
bind_ipv4_hex=0a0101d9
peer_network_hex=0a010100
peer_netmask_hex=ffffff00
port=18195
handshake_deadline_ms=3000
```

Independently recompute before any transfer:

```powershell
Get-FileHash -Algorithm SHA256 .\attach\dist\auth-gate\eboot.bin
Get-FileHash -Algorithm SHA256 .\attach\dist\auth-gate\VitaDebuggerAuthGate.vpk
Get-FileHash -Algorithm SHA256 `
  .\deploy\agent\third_party\monocypher\monocypher.c
Get-FileHash -Algorithm SHA256 `
  .\deploy\agent\third_party\monocypher\monocypher-ed25519.c
```

Every value must equal the reviewed manifest. A build with a hardware
assurance source must additionally record and independently review that source
hash. The VPK must remain a safe user-mode fSELF containing only `eboot.bin`
and `sce_sys/param.sfo`.

## Endpoint and ownership

The candidate binds exactly `10.1.1.217:18195`, permits source addresses only
from `10.1.1.0/24` excluding network/broadcast addresses, and accepts one
serialized connection. Builds may select another exact RFC1918 address,
contiguous subnet, TCP port `18000` through `18999`, and handshake deadline
`100` through `30000` milliseconds. Record any override in the manifest and
review it before transfer. There is no network discovery or runtime text
configuration.

The disposable title owns Vita network initialization, one worker, one listen
socket, and at most one accepted socket. Cross requests explicit shutdown.
Shutdown rejects new work, closes both registered sockets, joins/deletes the
worker, then terminates owned networking. A shell-resident integration must
replace network initialization with a reviewed shared owner and must call the
same shutdown/join path before unload.

## Public-only provisioning

Prepare a host store offline and create the exact 72-byte public bundle:

```powershell
py -3 .\attach\tools\vdattach_auth_provision.py make-bundle `
  --key-store .\private-host-key-store `
  --device-key-id 0x2122232425262728 `
  --output .\auth-provision-v1.bin
Get-FileHash -Algorithm SHA256 .\auth-provision-v1.bin
```

The bundle is `VDAP`, schema `1`, device key ID/generation, host key
ID/generation, and the 32-byte host public key, all fixed-width network byte
order. It contains no private material. Copy it only after recording the hash
to:

```text
ux0:data/VitaDebugger/auth-provision-v1.bin
```

On first approved launch, the gate:

1. opens the hardware-assured store;
2. generates the 32-byte device seed locally with
   `sceKernelGetRandomNumber`;
3. derives the device public key with vendored Monocypher;
4. atomically provisions the device identity;
5. durably adds the exact host public key to the allowlist;
6. wipes seed and expanded secret buffers;
7. writes the 56-byte public receipt
   `ux0:data/VitaDebugger/auth-device-public-v1.bin`; and
8. removes the consumed public bundle before opening the listener.

Copy the receipt back, hash it, and import only its public key:

```powershell
py -3 .\attach\tools\vdattach_auth_provision.py import-receipt `
  --key-store .\private-host-key-store `
  --receipt .\auth-device-public-v1.bin
```

Reboot before the first authentication test. The store revision and service
generation must increase, the local key ID/generation must remain exact, and
the consumed provisioning bundle must not reappear. Never transfer a host or
device private key. Never log the store, seed, signature, or nonce.

## Serialized hardware procedure

Proceed only after the STOP decision above is formally cleared.

1. Disconnect `10.1.1.217` from untrusted networks. Record firmware, installed
   plugins, shell-module configuration, free storage, and whether `VDAT00001`
   already exists. Stop if the title ID is occupied.
2. Record the reviewed commit and all manifest/hash values. Stop on any
   mismatch or unreviewed build option.
3. Snapshot the exact pre-test shell configuration and confirm no existing
   listener on TCP `18195`.
4. Generate the host store and public bundle offline. Transfer only the VPK
   and 72-byte bundle through the centrally approved path.
5. Install and launch `VDAT00001`. If
   `ux0:data/VitaDebugger/auth-gate.status` is `blocked` or `failed`, stop;
   never patch around the gate.
6. Retrieve/import the public receipt, reboot, relaunch, and confirm
   `state=listening`. Confirm TCP `18195` is the only new listener.
7. Authenticate:

   ```powershell
   py -3 .\attach\tools\vdattach_auth.py `
     --host 10.1.1.217 `
     --port 18195 `
     --key-store .\private-host-key-store `
     --json
   ```

   Expected wire payloads are exactly `HELLO` 88, `CHALLENGE` 240, `PROOF`
   248, and `RESULT` 288 bytes. Output must say
   `transport=signed-plaintext` and `encrypted=false`.
8. Run one case at a time: one-byte partial I/O, stalled prefix, stalled body,
   mid-frame disconnect, oversized frame, malformed v2 header, v1 frame,
   wrong source subnet, stale key generation, revoked peer, replayed `HELLO`,
   replayed old-generation `PROOF`, 32 distinct failed rate-limit keys,
   backoff expiry, Cross shutdown during blocked read, and stop/relaunch.
9. After every stop, confirm TCP `18195` is closed and the next launch receives
   a strictly greater service generation. Reboot and confirm an old proof and
   restored older store are rejected.

Capture only timestamps, sizes, statuses, IDs/generations, retry delays,
service generations, hashes, and pass/fail outcomes. Authentication is signed
plaintext with no encryption; key IDs, generations, timing, nonces,
signatures, and traffic sizes are observable.

## Rotation, revocation, and recovery

Rotation uses a new device key ID and increments its generation. The
replacement revision must be durable and above the rollback floor before the
old identity is retired. Add the new device public receipt to the host, test
it, then revoke the old exact ID/generation. Host rotation follows the same
order: add the new public identity on Vita, test it, then revoke the old exact
entry.

For a compromised host peer:

```powershell
py -3 .\attach\tools\vdattach_auth_provision.py revoke-peer `
  --key-store .\private-host-key-store `
  --key-id 0x<exact-peer-id> `
  --generation <exact-generation>
```

The matching Vita-side revoke must be performed through the reviewed local
administration path while the listener is stopped. Network administration is
not implemented.

An interrupted store update must leave either the previous complete revision
or the next complete revision. A malformed temporary file is removed only by
the reviewed recovery path; ambiguity, a floor mismatch, or an incomplete
rename is a hard stop, not a reason to choose the highest-looking file.

## Rollback and uninstall

1. Press Cross and verify `state=stopped`.
2. Confirm TCP `18195` is closed.
3. Uninstall `VDAT00001`.
4. Invoke `vd_attach_auth_store_uninstall()` only through the reviewed local
   administration artifact. It removes both fixed store files, syncs the
   parent, and advances the rollback floor.
5. Revoke both test peer identities in the host store.
6. Remove the public provisioning bundle, public receipt, and status file.
7. Restore the recorded shell configuration, reboot, and verify Vita
   Companion behavior is unchanged and TCP `18195` remains closed.

Do not delete only the store file: without advancing the independent floor, a
backup could reactivate an old identity. Normal listener stop never deletes
keys.

## Immediate stop conditions

Stop without retry if any artifact/source hash differs; any private key crosses
the device boundary; a seed, nonce, or signature is logged; storage assurance
is missing; directory sync or rollback-floor advancement fails; store reload
changes identity unexpectedly; a stale revision or service generation loads; a
revoked/stale/unknown peer authenticates; a malformed, oversized, v1, or
wrong-subnet frame keeps the connection alive; partial I/O extends the absolute
deadline; a replay is accepted; rate-table exhaustion evicts an entry; shutdown
leaves TCP `18195`, a worker, or a descriptor alive; another title becomes
unstable; or any target selection, AppMgr, kernel loader, module injection,
process mutation, memory/register operation, or GDB surface appears.

## Layer 3 prerequisites

Layer 3 may begin only after all of the following are recorded against one
reviewed commit:

1. this authentication-only gate passes on `10.1.1.217`, including
   provision/reboot, rotation/revocation, rollback rejection, replay/backoff,
   malformed/partial I/O, cancellation, restart, and resource-cleanup cases;
2. the private-path and independent rollback-floor implementations have
   accepted threat-model and hardware evidence, with no bypass build flag;
3. a shell-resident lifecycle integration reuses an established shared network
   owner and proves stop/join/unload across shell restart without changing the
   protocol-v2 record set;
4. the host and Vita stores contain only the intended active exact
   ID/generation pairs and old test identities are durably revoked;
5. a read-only trusted identity provider can resolve one exact allowlisted
   title and revalidate title, PID, main module ID, fingerprint, and launch
   generation without exposing raw PID selection;
6. any mutation request is assigned a new separately reviewed protocol
   version/domain and is independently verified inside the future privileged
   boundary; protocol v1 remains read-only and protocol v2 remains
   authentication-only;
7. one fixed preinstalled debugger module path and digest are compiled into the
   future loader, with no host path/address/module/PID input; and
8. disposable-target load/start/stop/unload, lease expiry, disconnect,
   relaunch/PID reuse, partial-start rollback, and recovery gates have exact
   backups and a central rollback owner.

Until then, no target selection, privileged operation, kernel loader call,
module injection, process mutation, or live GDB attach belongs in this stack.

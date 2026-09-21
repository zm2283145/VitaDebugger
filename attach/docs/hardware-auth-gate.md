# Vita authentication hardware gate

The authorized device remains exactly `10.1.1.217`. This layer does not access
it.

## Authoritative decision: HARD BLOCK

Production authentication on retail firmware 3.65 is blocked. No public or
safely callable API establishes all three required properties:

1. a non-exportable Ed25519 key inaccessible to ordinary or co-resident
   SceShell code;
2. trusted handle-bound, no-follow, durable persistence/commit semantics; and
3. an independent durable monotonic anti-rollback floor.

`*main` shares SceShell identity and memory. File mode `0600` is not documented
per-title isolation. Public file APIs provide no documented no-follow/openat
contract and no atomic, power-loss-safe rename-plus-directory-sync contract.
Registry, savedata, and SQLite state are rollbackable with the metadata.
IdStorage, Syscon, NVS, PFS, and SceSbl require unsupported or
reverse-engineered privileged mechanisms. `sceKernelGetRandomNumber` is usable
entropy but does not solve key persistence or isolation.

Therefore:

- do not provision a real Vita seed or private key;
- do not transfer the public provisioning bundle to the device;
- do not start the protocol-v2 listener or open TCP `18195`;
- do not add guessed NIDs, privileged probes, or reverse-engineered storage;
- do not treat a pathname check or file mode as hardware assurance; and
- do not contact `10.1.1.217` for authentication testing.

The decision is not bypassable by a build flag or callback source.
The complete reviewed surface, threat model, composition defect, and exact
unblock evidence are sealed in
[`retail-365-secure-storage-audit.md`](retail-365-secure-storage-audit.md).

## Sentinel artifact and hashes

`VDAT00001` is now sentinel-only. It writes:

```text
state=blocked
reason=retail-365-secure-storage-hard-block
scope=sentinel-only
network=not-started
transport=signed-plaintext
encryption=none
```

and exits without generating a key, loading a store, initializing SceNet, or
opening a socket.

From a reviewed commit and clean worktree:

```powershell
make host-test-attach
make -C attach vita-lib
.\attach\tools\build_auth_gate.ps1
```

The package command publishes only:

```text
attach/dist/auth-gate/eboot.bin
attach/dist/auth-gate/VitaDebuggerAuthGate.vpk
attach/dist/auth-gate/manifest.json
```

The manifest must record:

```text
title_id=VDAT00001
protocol=2
scope=sentinel-only
network=not-started
transport=signed-plaintext
encryption=none
hardware_assurance=retail-3.65-hard-block
secure_storage_backend=null
secure_storage_audit.decision=no-go
secure_storage_audit.required_properties=3
secure_storage_audit.all_properties_proven=false
secure_storage_audit.transaction_binding_proven=false
secure_storage_audit.hardware_contacted=false
secure_storage_audit.evidence=attach/docs/retail-365-secure-storage-audit.md
```

It also records the future listener candidate endpoint
`10.1.1.217:18195`, peer subnet `10.1.1.0/24`, and 3000 ms absolute deadline.
Those fields describe a disabled candidate; they are not active VPK behavior.

Independently recompute before retaining any artifact:

```powershell
Get-FileHash -Algorithm SHA256 .\attach\dist\auth-gate\eboot.bin
Get-FileHash -Algorithm SHA256 `
  .\attach\dist\auth-gate\VitaDebuggerAuthGate.vpk
Get-FileHash -Algorithm SHA256 `
  .\deploy\agent\third_party\monocypher\monocypher.c
Get-FileHash -Algorithm SHA256 `
  .\deploy\agent\third_party\monocypher\monocypher-ed25519.c
Get-FileHash -Algorithm SHA256 `
  .\attach\docs\retail-365-secure-storage-audit.md
```

Every value must match `manifest.json`. The VPK must remain an ordinary
user-mode fSELF containing only `eboot.bin` and `sce_sys/param.sfo`, with no
SceNet, NetCtl, AppMgr, kernel-loader, injection, target-process, or GDB import.

## Trusted backend requirements

A future GO review requires three concrete backends, not an assurance boolean:

1. **Opaque key backend:** generates and signs with a non-exportable Ed25519
   key that ordinary/co-resident SceShell code cannot read, map, or invoke
   outside the policy. It returns only public identity and signatures and has
   no seed export operation.
2. **Persistence backend:** owns handle-bound current/staged open, exclusive
   create, bounded I/O, sync, close, durable commit, and discard. The metadata
   core receives no path and cannot substitute check-then-open path proofs.
3. **Monotonic backend:** stores and advances the revision floor in an
   independent trust domain that cannot be restored with metadata.

These three backend labels are not sufficient for promotion. The current
revision-only monotonic callback cannot bind a floor update to the exact staged
metadata object. A future implementation must additionally provide either one
trusted atomic metadata/floor transaction or a monotonic record bound to the
staged object's digest/token, with deterministic interrupted-operation
recovery.

The schema-2 core rejects raw-seed, path-proof-only, same-namespace-floor, and
shell-owned-network configurations. Initialization/recovery failures wipe all
resident store state before bind. Normal stop and every start/unload failure
must call non-destructive store deinit; only an explicit administration flow
may commit an uninstall tombstone and destroy an opaque key.

There is no Vita implementation of these backends in this repository.

## Network ownership and stop order

The portable listener remains host-tested but hardware-disabled.

- **Standalone-owned mode** is for a disposable title only. It owns network
  module load, `sceNetInit`, worker, sockets, `sceNetTerm`, and module unload.
- **Shell-borrowed mode** assumes networking is already ready. It owns only its
  worker and sockets. It never loads/unloads the shared module, never calls
  `sceNetInit`/`sceNetTerm`, and performs no NetCtl transition.

Hybrid ownership configurations are invalid. Stop order is fixed: reject new
work, cancel listen/accepted sockets, join the worker, delete the worker, then
and only then terminate/unload resources explicitly owned by standalone mode.
A join/delete failure retains cleanup ownership and forbids module unload.

## Sentinel-only characterization

No current device run is authorized. If a later central hardware session is
approved solely to characterize the sentinel, it may:

1. verify the reviewed commit and all hashes;
2. verify `VDAT00001` is unoccupied and the VPK has only the two expected
   files;
3. verify no listener exists on TCP `18195` before launch;
4. launch the sentinel once;
5. retrieve only `auth-gate.status`;
6. verify the exact blocked status above and that TCP `18195` never opens;
7. uninstall the sentinel, restore the recorded shell state, and reboot.

It must not place `auth-provision-v1.bin`, create a store, generate a key,
publish a device receipt, run the host authentication client, test network
frames, or modify shell/network configuration. The public receipt format may
be exercised only in host tests. Any future Vita receipt write is a
non-atomic, non-sensitive characterization aid and cannot prove provisioning.

## Immediate stop conditions

Stop without retry if any source/artifact hash differs; the manifest does not
say `sentinel-only` and `network=not-started`; a store, seed, bundle, receipt,
SceNet/NetCtl transition, socket, worker, or TCP `18195` appears; a private key
crosses the device boundary; any shared shell network state is terminated or
unloaded; another title becomes unstable; or any target selection, AppMgr,
kernel loader, module injection, process mutation, memory/register operation,
or GDB surface appears.

## Layer 3 prerequisites

Layer 3 may begin only after all of the following are recorded against one
reviewed commit:

1. independently accepted hardware evidence for all three trusted backends
   and a floor-to-staged-object transaction binding;
2. no raw-seed, seed-export, path-proof, same-namespace-floor, or bypass
   contract exists;
3. the authentication-only gate passes provision/reboot,
   rotation/revocation, rollback rejection, replay/backoff,
   malformed/partial I/O, cancellation, restart, secret-wipe, and resource
   cleanup tests on `10.1.1.217`;
4. shell integration proves borrowed SceNet ownership, no NetCtl transition,
   and stop/join/delete/unload behavior across shell restart;
5. stores contain only intended active exact ID/generation pairs and old test
   identities are durably revoked;
6. a read-only trusted identity provider revalidates exact title, PID, main
   module ID, fingerprint, and launch generation without raw PID selection;
7. mutation uses a separately reviewed protocol/domain and is independently
   verified inside the future privileged boundary;
8. one fixed preinstalled debugger module path and digest are compiled into
   the future loader with no host path/address/module/PID input; and
9. disposable-target load/start/stop/unload, lease expiry, disconnect,
   relaunch/PID reuse, partial-start rollback, and recovery gates have exact
   backups and one central rollback owner.

Until all nine pass, no target selection, privileged operation, kernel loader
call, module injection, process mutation, or live GDB attach belongs in this
stack.

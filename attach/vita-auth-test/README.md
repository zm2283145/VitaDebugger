# Vita authentication gate

`VDAT00001` is a disposable, safe user-mode VPK containing only the
protocol-v2 authentication listener, its lifecycle wrapper, the persistent
key-store adapter, and vendored Monocypher. It has no AppMgr, kernel-loader,
module-injection, process-mutation, target-selection, or GDB dependency.

The default build is intentionally **blocked at runtime**. Vita public file
APIs do not prove that
`ur0:data/VitaDebugger/private/attach-auth.store` is confidential from other
SceShell-resident code, and they provide no trusted monotonic rollback floor.
Without a separately reviewed source defining all three weak hardware
assurance callbacks, the title writes
`ux0:data/VitaDebugger/auth-gate.status` with
`reason=hardware-storage-assurance-missing` and exits before initializing
networking or opening TCP `18195`.

If those proofs are later supplied, an unprovisioned store accepts only the
fixed 72-byte public bundle at
`ux0:data/VitaDebugger/auth-provision-v1.bin`. The device seed is generated
locally, never exported, and wiped after the store transaction. The title
writes the fixed 56-byte public receipt
`auth-device-public-v1.bin`, removes the consumed bundle, durably advances the
service generation, and only then opens the endpoint. It accepts one serialized
peer from the configured private subnet, performs one bounded mutual-
authentication exchange, and closes the connection. Press Cross to take the
explicit stop path. Both status output and the wire transport state that
authentication is signed plaintext with no encryption.

The build-time endpoint limits are TCP `18000` through `18999` and an exact
private IPv4 address/network/mask. The defaults are `10.1.1.217:18195` with
peers restricted to `10.1.1.0/24`.

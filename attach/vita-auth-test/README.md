# Vita authentication gate

`VDAT00001` is a disposable, safe user-mode **sentinel** VPK. It writes the
current retail-3.65 secure-storage decision and exits. It has no networking,
AppMgr, kernel-loader, module-injection, process-mutation, target-selection,
or GDB dependency.

The build is unconditionally **blocked at runtime**. Vita public APIs do not
provide both a non-exportable Ed25519 key isolated from ordinary/co-resident
SceShell code and an independent durable monotonic rollback floor. Path checks
and mode bits are not a substitute for those properties. The title writes
`ux0:data/VitaDebugger/auth-gate.status` with
`reason=retail-365-secure-storage-hard-block` and exits before generating or
provisioning a key, initializing networking, or opening TCP `18195`.

There is no assurance-source build switch and no Vita secure-storage backend.
Future work must supply an independently reviewed opaque key backend,
handle-bound durable persistence backend, and independent monotonic backend
before any listener-enabled artifact is restored. Public receipt publication
through `sceIo` must be treated as a non-atomic characterization aid, never as
evidence of secure provisioning.

The manifest retains the bounded future-listener candidate: TCP `18000`
through `18999`, an exact private IPv4 address/network/mask, and a 100-to-30000
ms absolute deadline. The recorded defaults are `10.1.1.217:18195`,
`10.1.1.0/24`, and 3000 ms. The sentinel does not bind that endpoint.

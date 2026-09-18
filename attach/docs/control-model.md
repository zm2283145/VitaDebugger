# Authenticated external-attach control model

This document describes `VdAttachControl`, an allocation-free C state machine
for a possible later privileged milestone. It is not a Vita injector.
Version-1 discovery remains permanently read-only. Authentication-only wire
protocol version 2 now establishes a peer-authorized session, but exposes no
operation record and does not call this control model. A future layer must
separately review and connect an operation frame before any lifecycle callback
can become reachable.

## What is implemented

A future shell-resident listener can open one of four bounded sessions and
receive a fresh challenge containing the service generation, monotonic
per-generation session ID, server nonce, opaque transport binding, broker
monotonic time, and challenge expiry. The signing host therefore has every
field and time basis needed to construct its proof. Authentication is accepted
only when an injected paired-host verifier validates a transcript also bound to
the client nonce, host-key ID, and requested expiration.

Every attach, detach, and explicit recovery requires a second signature through
the operation verifier. The signed authorization includes:

- operation and protocol version;
- service/session/peer/host-key identity;
- both connection nonces and a fresh operation nonce;
- expiration and requested lease duration;
- exact title, PID, main module ID, main-module fingerprint, and target launch
  generation; and
- for detach/recovery, the owned lease and injected module UID.

These C structures describe logical fields only. The C and Python encoders now
define the canonical signing bytes below; verifiers must never sign native
structure memory, padding, or endianness. This is a future operation signing format, not a currently accepted TCP record.
It adds no mutation command to protocol v1 or protocol v2.

## Canonical signing transcripts

All integers are unsigned and encoded in network byte order. Domains are exact
ASCII bytes without a trailing NUL. Fixed strings/nonces have no length prefix.
The signature fields are inputs to verification but are deliberately excluded
from the bytes being signed. `vitadebug_attach_control_wire.c` and
`host/vdattach/control_signing.py` must produce the shared golden vectors.

Peer authentication is exactly 153 bytes:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 29 | `VITADEBUG-ATTACH/PEER-AUTH/v1` |
| 29 | 4 | version |
| 33 | 8 | service generation |
| 41 | 8 | session ID |
| 49 | 8 | opaque transport binding |
| 57 | 8 | paired-host key ID |
| 65 | 8 | server monotonic time, milliseconds |
| 73 | 8 | challenge expiry on that time basis |
| 81 | 8 | requested proof expiry on that time basis |
| 89 | 32 | server nonce |
| 121 | 32 | client nonce |

Operation authorization is exactly 243 bytes:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 34 | `VITADEBUG-ATTACH/OPERATION-AUTH/v1` |
| 34 | 4 | version |
| 38 | 4 | operation (`ATTACH`, `DETACH`, or `RECOVER`) |
| 42 | 4 | fixed debugger-module slot |
| 46 | 8 | service generation |
| 54 | 8 | session ID |
| 62 | 8 | opaque transport binding |
| 70 | 8 | paired-host key ID |
| 78 | 8 | proof expiry on broker monotonic time basis |
| 86 | 8 | authenticated-session expiry from peer proof |
| 94 | 4 | requested lease milliseconds (attach only) |
| 98 | 8 | owned lease ID (cleanup only) |
| 106 | 8 | owned lease expiry (cleanup only) |
| 114 | 4 | injected module UID (cleanup only) |
| 118 | 9 | exact ASCII title ID |
| 127 | 4 | target PID |
| 131 | 4 | target main-module ID |
| 135 | 4 | target main-module fingerprint |
| 139 | 8 | target launch generation |
| 147 | 32 | server nonce |
| 179 | 32 | client nonce |
| 211 | 32 | operation nonce |

Accepted operation nonces are retained in a fixed 64-entry table for that
connection. At exhaustion the core returns
`VD_ATTACH_CONTROL_ERROR_ROLLOVER_REQUIRED`; the client must explicitly close
the session (which attempts cleanup of any owned lease), reconnect, and
authenticate again. A new monotonic session ID and random server nonce change
the signed transcript. An old-session proof is rejected after rollover; only a
new signature over the replacement session transcript can succeed. The future
privileged backend still needs its own persistent replay journal.

The host-facing operation proof has no PID, main module ID, module UID, address,
entry point, or path. The host supplies an exact title and a nonzero launch
generation learned through discovery. A trusted provider resolves everything
else and must enforce the Vita-side title/system-process allowlist.

## Fixed loader boundary

The backend receives `VdAttachControlFixedLoadRequest`. Its only module selector
is `VD_ATTACH_CONTROL_FIXED_DEBUGGER_SLOT`; the value is filled by the core and
cannot be selected by the host. A future privileged adapter must map that slot
to one compiled-in canonical path and verified module digest. It must also
verify the forwarded paired-host signature and its own persistent replay state
before calling a per-PID module lifecycle API.

The shell core does not create the lease ID or expiry. The privileged load
callback atomically allocates and journals a `VdAttachControlLeaseGrant`, then
returns that grant to the core. The journal chooses a fresh lease ID and clamps
expiry to both its load-start time plus the signed requested duration and the
signed authenticated-session expiry. The core rejects a missing, invalid, or
overlong grant and never substitutes its own unsigned values.

Every start/cleanup callback carries a journal capability and the exact grant,
target, fixed slot, host key, and service generation. The privileged provider
must apply these rules:

- `START` requires an independently verified signed `ATTACH` authorization and
  the exact newly acquired journal record.
- `HOST_CLEANUP` requires an independently verified signed `DETACH` or
  `RECOVER` authorization. Its nonce is claimed once for the complete
  stop/unload transaction, and every signed lease/target field must match the
  journal.
- `ROLLBACK`, `LEASE_EXPIRED`, `DISCONNECT`, `SHUTDOWN`, and
  `RECOVERY_RETRY` are release-only capabilities. They may stop, unload, or
  prove gone only for an exact existing journal record; they can never load,
  start, retarget, or synthesize a lease. `LEASE_EXPIRED` additionally requires
  the privileged clock to have reached the journaled expiry.

No such adapter is present. The archive contains no canonical path and makes no
module-manager call. The host test uses recording callbacks only.

## Identity and lease lifecycle

The broker resolves and validates the complete target snapshot before operation
authorization, repeats the lookup after authorization immediately before the
fixed load, repeats it before start, and verifies it once more after start
before publishing the lease as active. Cleanup repeats it before stop and again
before unload. All fields must match, not only PID. Acquisition uses one
absolute callback deadline clamped to the operation proof, authenticated
session, and module lease. Time is resampled after every blocking callback; a
load/start that returns success after expiry is immediately rolled back and is
never published as `ACTIVE`.

Only one lease can exist. A successful load is recorded before start. Start is
conservatively marked as possibly executed before invoking the callback, so a
failed start rolls back with stop/already-stopped followed by unload. A normal
detach, connection close, lease watchdog expiry, and shutdown all use the same
reverse-order cleanup routine. Shutdown remains closed to new sessions after
its first call, but subsequent shutdown calls retry any retained recovery
record until cleanup succeeds; later calls are idempotent.

If identity changes or a lifecycle callback fails, ownership and acquired
resource flags remain in `RECOVERY`. New attaches are refused. A successful
stop is remembered, so an unload retry does not issue stop twice. Explicit
recovery requires a new signed recovery operation from either the same
still-authenticated owner session, or after disconnect a newly authenticated
session for the same host key. Automatic lease/disconnect recovery is allowed
only to undo the already-authorized owned resource; it can never acquire a new
one.

The core has no internal lock; a listener must serialize calls for each control
instance and cancel blocked transport work during shutdown.

If a PID is reused, the core calls neither stop nor unload against the new
generation. A separate read-only backend probe may clear the recovery record
only by returning `GONE` for the exact original generation, lease ID, and module
UID. `UNKNOWN` or `PRESENT` retains recovery and blocks new attach. This lets a
verified target exit/relaunch reconcile terminally without mutating the new
process.

Control storage must be zero-initialized before its first initialization. A
second initialization is refused even after shutdown, so it cannot erase an
active or recovery obligation. The owner must finish cleanup (and persist any
future kernel journal) before deliberately replacing the state object.

## Remaining Vita hardware gate

Before any live injection work, a separate review must supply and validate:

1. a shell-resident TCP listener with bounded framing, cancellation, rate
   limits, secure pairing/key storage, and the new authenticated wire format;
2. a trusted kernel identity provider with launch-generation semantics and a
   persistent replay journal;
3. a compiled-in title allowlist and fixed debugger-module path plus digest;
4. a privileged backend that independently verifies authorization and identity
   before each per-PID lifecycle call; and
5. failure-injection tests on a disposable purpose-built target across rapid
   exit/relaunch, partial start, disconnect, lease expiry, and failed unload.

Until those gates pass, this API must be treated as a host-tested model only.

# Authenticated control framing (host-testable version 2)

This is the binary framing contract around `VdAttachControl`. It is a
host-testable input to a future resident service, not a shipped Vita listener.
There is no socket bind/accept implementation, paired-key store, production
signature verifier, kernel loader, or deployable module in this subtree.
Protocol v1 remains read-only and is not reinterpreted as version 2.

## Transport and connection lifecycle

Every record uses a four-byte unsigned big-endian payload length followed by
that many payload bytes. The payload limit is 512 bytes. Zero, oversized,
short, noncanonical, or unknown records close the connection. An oversized
length is rejected before its peer-selected payload is read or drained.

An accepted transport supplies one nonzero opaque connection binding. The
dispatcher opens a control session and sends one 104-byte challenge payload
before reading requests. Each later framed request/response exchange gets one
absolute I/O deadline that is never extended. It is clamped to an active module
lease expiration and may only be shortened before the response if that request
created an earlier lease. The control watchdog is serviced before each
blocking read. The injected transport callbacks are the only I/O surface; this
code does not create or listen on a socket.

Disconnect always closes the matching control session. If it owns a module
lease, that invokes the existing stop-then-unload disconnect cleanup path.

## Common encoding

All integers are unsigned and big-endian. Records are fixed-size and contain
no padding, flags, variable strings, or extension fields. Magic values below
are exact eight-byte ASCII strings without a NUL.

The common 40-byte prefix is:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | record magic |
| 8 | 4 | protocol version, exactly `2` |
| 12 | 4 | request type |
| 16 | 16 | request ID |
| 32 | 8 | control session ID |

The challenge magic is `VDCTL2CH`, its type and request ID are zero, and the
remaining 64 bytes are:

| Offset | Width | Field |
| ---: | ---: | --- |
| 40 | 8 | service generation |
| 48 | 8 | opaque transport binding |
| 56 | 8 | broker monotonic time in milliseconds |
| 64 | 8 | challenge expiration on the same time basis |
| 72 | 32 | server nonce |

## Requests

Request magic is `VDCTL2RQ`. The only request types are `AUTHENTICATE` (1),
`ATTACH` (2), `DETACH` (3), and `RECOVER` (4).

An authentication request is exactly 152 bytes:

| Offset | Width | Field |
| ---: | ---: | --- |
| 40 | 8 | paired-host key ID |
| 48 | 8 | requested authentication expiration |
| 56 | 32 | client nonce |
| 88 | 64 | peer-auth signature |

An operation request is exactly 165 bytes:

| Offset | Width | Field |
| ---: | ---: | --- |
| 40 | 9 | exact title ID |
| 49 | 8 | previously observed target generation |
| 57 | 4 | requested lease milliseconds; zero for cleanup |
| 61 | 8 | operation-proof expiration |
| 69 | 32 | operation nonce |
| 101 | 64 | operation signature |

The request ID is not an independent unsigned identifier. It must equal the
first 16 bytes of the signed client nonce for `AUTHENTICATE`, or the first 16
bytes of the signed operation nonce for every other request. This binds the
framing replay key to the existing canonical signature transcripts without
inventing another signature format. Changing a request type changes the
operation inserted into the signed authorization, so a valid proof cannot be
retagged as another operation.

Each connection retains 64 accepted request IDs. A duplicate is rejected
before authentication, target lookup, or lifecycle logic is re-entered, and
the connection is closed without a response. The control core independently
retains the full 32-byte accepted operation nonces. Exhausting either bounded
table requires a fresh challenge and authenticated connection; tables never
evict an entry for reuse.

## Responses

Response magic is `VDCTL2RS`. A response is exactly 76 bytes and echoes the
request type, signed request ID, and session ID in the common prefix:

| Offset | Width | Field |
| ---: | ---: | --- |
| 40 | 4 | stable unsigned result code |
| 44 | 4 | lease state |
| 48 | 8 | loader-allocated lease ID |
| 56 | 8 | lease expiration |
| 64 | 4 | injected module UID |
| 68 | 8 | target generation |

An idle snapshot must encode all lease fields as zero. A non-idle snapshot
must encode all of them as valid nonzero values. Result values are defined in
`vitadebug_attach_listener.h`; they are not casts of negative C return codes.
The dispatcher reports a non-idle lease only to a session authenticated with
that lease's owner key; another authenticated host receives an idle/redacted
snapshot even when its operation returns `BUSY`. Responses carry no key
material or authorization capability. A production transport still needs a
reviewed confidentiality/integrity policy if response tampering or metadata
disclosure matters for its threat model.

## Fixed loader catalog and journal boundary

`VdAttachFixedLoader` is a separate host-testable adapter for the privileged
side of `VdAttachControl`. Trusted startup configuration supplies:

- one copied-in list of at most eight exact title IDs;
- exactly one fixed debugger slot mapped to one canonical `ux0:` or `ur0:`
  `.suprx` path and one nonzero SHA-256 digest; and
- injected verification, identity, entropy, clock, and platform lifecycle
  callbacks.

No request contains a module path or digest. The adapter copies the catalog at
initialization, binds exactly once to the control service generation, verifies
the full operation signature again, revalidates target identity, checks the
fixed module descriptor, and claims the full signed operation nonce before a
load callback can run.

The adapter allocates each lease ID from injected cryptographic entropy and
keeps bounded, non-evicting in-memory histories for operation nonces and lease
IDs. It journals the target, host key, grant, and attach authorization around
the lifecycle calls. Authorization verification and lifecycle callback
deadlines are only shortened by proof, session, and acquisition-lease bounds;
they are never reset after a blocking callback. A start is marked as possibly
executed before its callback. Therefore a failed or late start requires `stop`
(where already-stopped is success) followed by `unload`. Host cleanup claims one
signed cleanup nonce for the complete stop/unload transaction; rollback,
expiry, disconnect, shutdown, and recovery-retry capabilities are release-only
and must match the exact existing journal record. Lease-expiry cleanup is
rejected before the journaled expiry.

This in-memory adapter closes interface and state-machine gaps only. Its
catalog values are test fixtures, not a production allowlist. A Vita backend
must still provide crash-persistent replay/lease journaling, secure paired-key
storage, atomic digest verification at module use, trusted launch-generation
identity, and reviewed per-PID module-manager calls before hardware use is
enabled.

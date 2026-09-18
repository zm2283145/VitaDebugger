# Authenticated attach protocol version 2

Protocol version 2 currently establishes a mutually authenticated, expiring
session and nothing else. It has no discovery, attach, detach, loader, process,
module, memory, register, or GDB record. Protocol version 1 remains permanently
read-only and is never accepted on a version-2 connection.

Version 2 authenticates records but does **not** encrypt transport. Key IDs,
generations, timing, nonces, statuses, and traffic sizes remain visible to the
network. Use it only on a trusted private LAN until a separately reviewed
confidential transport exists.

## Framing and limits

Each TCP record is:

```text
uint32_be payload_length
payload_length bytes
```

The payload length is checked before reading or allocating the body and must be
from 1 through 1024 bytes. Every payload begins:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII `VDA2` |
| 4 | 1 | version, exactly `2` |
| 5 | 1 | record type |
| 6 | 2 | body length, network byte order |

The body length and total payload length must exactly match the record type.
There are no optional or trailing fields. All integers are unsigned and
network byte order. Every nonce is 32 bytes and every Ed25519 signature is 64
bytes. A required nonce may not be all zero.

| Type | Name | Payload bytes | Direction |
| ---: | --- | ---: | --- |
| 1 | `HELLO` | 88 | host to Vita |
| 2 | `CHALLENGE` | 240 | Vita to host |
| 3 | `PROOF` | 248 | host to Vita |
| 4 | `RESULT` | 288 | Vita to host |

No other record type is defined. In particular, there is no target or mutation
record.

## Records

`HELLO` contains, in order:

```text
host_key_id:u64
host_key_generation:u64
request_nonce:bytes[32]
client_nonce:bytes[32]
```

The Vita checks the exact key ID and generation against its explicit active
peer allowlist before issuing a challenge. A request nonce is accepted once per
service generation. The fixed replay table contains 128 entries; exhaustion
requires a controlled service restart and new service generation rather than
eviction that could re-enable an old request.

`CHALLENGE` contains:

```text
host_key_id:u64
host_key_generation:u64
server_key_id:u64
server_key_generation:u64
service_generation:u64
session_id:u64
transport_binding:u64
server_time_ms:u64
challenge_expires_at_ms:u64
request_nonce:bytes[32]
client_nonce:bytes[32]
server_nonce:bytes[32]
server_signature:bytes[64]
```

The transport binding is opaque listener-provided connection identity. It is
signed but is not an IP-address authorization policy. The server signs the
first 168 body bytes prefixed by the exact 36-byte ASCII domain
`VITADEBUG-ATTACH/SERVER-CHALLENGE/v2`, producing a 204-byte transcript.
The host must find the exact server key ID and generation in its active
allowlist and verify the signature before disclosing a host signature.

`PROOF` repeats every challenge field, adds `proof_expires_at_ms:u64` after the
challenge expiration, and replaces the server signature with the host
signature. Its proof expiry must be later than the server time and no later
than the challenge expiry. The host signs the first 176 body bytes prefixed by
the exact 32-byte ASCII domain
`VITADEBUG-ATTACH/CLIENT-PROOF/v2`, producing a 208-byte transcript.

`RESULT` repeats all proof fields except the host signature, then contains:

```text
status:u32
retry_after_ms:u32
request_nonce:bytes[32]
client_nonce:bytes[32]
server_nonce:bytes[32]
session_nonce:bytes[32]
server_signature:bytes[64]
```

The status is `0` (success), `1` (denied), `2` (rate limited), or `3`
(shutdown). Only success may contain a nonzero session nonce, and success must
have a zero retry delay. The Vita signs the first 216 body bytes prefixed by
the exact 34-byte ASCII domain
`VITADEBUG-ATTACH/SESSION-RESULT/v2`, producing a 250-byte transcript.

## Binding and replay rules

Every signature is bound to both key IDs and generations, service generation,
session ID, opaque transport binding, request/client/server nonces, and the
broker monotonic time window. The final result additionally binds the session
nonce and status. An old proof therefore fails after service restart, key
rotation, connection replacement, or challenge expiry.

Authentication failures use exponential backoff starting at 250 ms and capped
at 8000 ms, keyed by opaque peer binding plus claimed host key ID. Successful
authentication clears that key's failure state. The model holds at most 16
outstanding challenges and eight authenticated sessions.

Future privileged operations, if separately approved, must use a fresh
operation nonce and bind the exact service/session/key generations and target
launch generation. The existing canonical control transcript models that
requirement. A future host request may supply only an exact title ID and
previously observed target generation; it must not contain a PID, module ID,
path, address, or entry point.

## Cancellation and shutdown

Each handshake uses one absolute 0.1-to-30-second deadline; partial reads do not
reset it. Host close performs socket shutdown and close and drops its session.
The authority tracks accepted handshake sockets. Shutdown first rejects new
work, clears pending challenges and authenticated sessions, then closes every
tracked socket so blocked reads terminate. Repeated shutdown is idempotent.
A production listener must serialize authority state transitions and call the
same shutdown path before unloading.

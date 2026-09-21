# VitaDebugger companion control protocol version 1

This protocol belongs to the source-owned `libvitadebug_companion.a` layer.
It does not authorize an external attach, resident module, system target, or
kernel operation. The application explicitly initializes the service with its
own title ID, PID, process generation, random session ID, 32-byte random
secret, callbacks, capabilities, and network policy.

The control secret is distinct from the screen token. Screen protocol v1
transmits its token as an admission preface and therefore that value must
never be reused as the companion record MAC key. Both protocols bind to the
same title/PID/generation/session identity.

All integers are unsigned network byte order. A signed integer such as an
input axis uses its two's-complement fixed-width representation. One transport
record is an 80-byte header followed by `payload_size` bytes:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII `VDCP` |
| 4 | 2 | version, exactly `1` |
| 6 | 2 | header size, exactly `80` |
| 8 | 2 | message type |
| 10 | 2 | request status, exactly zero; response status below |
| 12 | 4 | payload size, at most 4096 |
| 16 | 8 | sequence, starting at 1 and increasing exactly by 1 |
| 24 | 8 | relative TTL in milliseconds, 1 through 5000 |
| 32 | 8 | nonzero random session ID |
| 40 | 8 | nonzero source-owned process generation |
| 48 | 8 | requested or negotiated capability mask |
| 56 | 8 | reserved, exactly zero |
| 64 | 16 | keyed BLAKE2b authentication tag |

The tag is keyed with the exact 32-byte nonzero session secret. Its canonical
input is the 29 ASCII bytes `VITADEBUG-COMPANION/RECORD/v1`, header bytes
0 through 63, and the complete payload. The tag does **not** encrypt the
record. Use only loopback or an explicitly approved trusted private LAN; do
not forward the port or use shared/public Wi-Fi.

Requests have types 1 through 7. Responses set bit 15 on the request type,
echo sequence and TTL, and carry the service identity and negotiated
capability mask. Response status is zero for success or the positive magnitude
of the exact `vd_companion_result` error. Authentication, framing, identity,
sequence, TTL, and capability checks occur before operation dispatch.
The caller must treat a transport disconnect or operation error as session
termination and call `vd_companion_service_disconnect()`. Authentication,
framing, identity, replay, deadline, and capability-envelope failures already
abort the service terminally. Either path releases input, closes the bound
transport and screen state, and wipes the control secret. A reconnect requires
fresh explicit initialization with a new secret and session ID; sequence 1 is
never accepted again by the old service instance.

The TTL is authenticated and starts when the service receives the complete
record. It never compares a host timestamp with a Vita timestamp and assumes
no synchronized clock or shared monotonic epoch. The service records the local
receive time and checked `receive_time + TTL` expiry without overflow. The
transport separately bounds partial-record reads to five seconds.
Sequence zero, duplicates, gaps, regressions, stale generations, stale session
IDs, nonzero reserved fields, response-shaped requests, oversized records,
and unknown enum values fail closed.

## Capability negotiation

The first request is `HELLO` (type 1), sequence 1, with an empty payload. Its
capability field is the requested mask. The 32-byte response is:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | enabled service capabilities |
| 8 | 8 | granted intersection |
| 16 | 4 | maximum payload bytes, 4096 |
| 20 | 4 | maximum file-read bytes, 2048 |
| 24 | 4 | maximum list entries, 32 |
| 28 | 4 | maximum input lease milliseconds, 1000 |

Later requests must repeat the granted mask exactly. Capability values are:

| Bit | Capability | Status |
| ---: | --- | --- |
| 0 | paired source-owned target status | implemented |
| 1 | cooperative application input callback | implemented, separate mutation consent required |
| 2 | read-only application debug-root files | implemented |
| 3 | application-owned framebuffer stream | implemented by the same archive, not carried in control records |
| 4 | cooperative physical-input recording | implemented, separate recording consent required |
| 5 | cooperative application playback | implemented, separate playback and mutation consent required |
| 6 | development-title inventory | unsupported |
| 7 | development-title launch | unsupported |

The service may grant a subset during `HELLO`; the response exposes both masks
so the client cannot mistake omission for support. Requests for the title inventory/launch message types
return `VD_COMPANION_ERROR_UNSUPPORTED`. No title allowlist is committed in
this foundation, so launch mutation remains default-disabled and unavailable.

## Implemented requests

`STATUS` (type 2) has no request payload. Its 72-byte response contains the
configured 9-byte title ID at offset 0, three zero bytes, PID at offset 12,
generation at 16, session ID at 24, and negotiated mask at 32. It cannot
enumerate or select another target. Trace state and event count are `u32` at
40 and 44; trace duration and observed maximum playback drift are `u64` at
48 and 56. Input cleanup-pending and the positive magnitude of the latest
terminal error are `u32` at 64 and 68. Recording and cleanup quarantine are
therefore visible to a paired client; the same fields are available locally
through `vd_companion_service_get_status()` after transport teardown.

`INPUT` (type 3) has a 36-byte payload: buttons `u32`, signed left X/Y and
right X/Y axes as four `i16` values, touch count `u8`, three zero bytes, two
touch slots of ID/X/Y/force as four `u16` values each, and lease milliseconds
`u32`. Leases are 50 through 1000 ms. Only the application button mask from
[input trace v1](input-trace-v1.md) is accepted; unused touch slots are zero.
The registered application callback receives the state; the service never
uses a system input API. `tick`, disconnect, and shutdown force one neutral
callback when a lease is active. Callback failure is terminal.

Recording/playback use the local source-owned API rather than accepting
unbounded trace bytes in a control record. They become callable only after the
corresponding capabilities are paired. Exact format, storage, timing,
identity, callback, host tooling, and cleanup behavior are defined in
[input trace v1](input-trace-v1.md).

`FILE_LIST` (type 4) carries `path_length:u16` and that many relative ASCII
path bytes. The empty path names the configured root. A success response starts
with `entry_count:u16` and at most 32 fixed 76-byte entries: name length `u8`,
type `u8` (`1` regular, `2` directory), two zero bytes, size `u64`, then a
63-byte maximum name and one zero padding byte.

`FILE_READ` (type 5) carries `path_length:u16`, a nonempty relative path,
offset `u64`, and requested length `u32`. Length is 1 through 2048. The
response contains offset `u64`, returned length `u32`, EOF `u32`, and bytes.
A session may return at most 1 MiB of file data.

Paths are already canonical relative forms: no leading/trailing slash, empty
segment, `.` or `..` segment, backslash, colon, NUL, control byte, or
non-ASCII byte. The configured provider must return the exact same canonical
path and attest `INSIDE_ROOT`, `NO_SYMLINKS`, and the exact object type.
Reads must repeat those checks on the opened regular file and return the same
size observed during resolution. A symlink, device, mount/path escape, type
change, count overflow, or total-byte overflow fails closed. There is no
write, upload, delete, or arbitrary mount operation.

## Explicitly absent

Version 1 has no arbitrary process inventory, kill/quit-all, reboot, module
loading or injection, protected/system target, arbitrary address, FTP
mutation, PlayStation/power/system input, global hook, or MCP operation.
Unknown message types are protocol errors rather than success-shaped
fallbacks. A future feature needs a new reviewed capability and exact wire
contract.

# VitaDebugger attach discovery protocol version 1

Protocol version 1 is a read-only compatibility and identity gate for a future
user-mode attach broker. It cannot authorize or request process suspension,
memory access, module loading, module unloading, or GDB startup.

## Transport framing

Each TCP message is one frame:

```text
uint32_be payload_length
payload_length bytes of canonical ASCII record
```

The payload length must be from 1 through 4096 bytes. A receiver rejects an
invalid length before allocating or draining the peer-selected payload. Records
use LF only, end in LF, contain no NUL, have a fixed field order, and reject
missing or extra fields.

All hexadecimal values use fixed-width lowercase digits. Decimal values are
canonical: zero is `0`, and a nonzero value has no leading zero. Messages are
UTF-8 percent-encoded; `%00` alone represents an empty message, while NUL in a
real message is forbidden. Decoded messages reject Unicode control, format,
surrogate, line-separator, and paragraph-separator categories (`Cc`, `Cf`,
`Cs`, `Zl`, and `Zp`) and bidirectional embedding, override, isolate, pop, and
boundary-neutral controls. This keeps CLI/log output on one safe display line.
The current C broker emits only fixed printable ASCII messages.

## Capability bits

| Bit | Meaning |
| --- | --- |
| 0 | Read-only service/kernel status |
| 1 | Exact-title discovery |
| 2 | Identity-bound expiring target ticket |
| 3 | Explicit ticket release |

All other version-1 bits are invalid. A future mutating capability must use a
new protocol version rather than silently changing version 1.

## Handshake

Client request:

```text
VITADEBUG-ATTACH-HELLO-1
request_id=<32 hex>
client_nonce=<64 hex>
mode=observe
expected_kernel_abi=<8 hex>
required_caps=<8 hex>
```

Broker result:

```text
VITADEBUG-ATTACH-HELLO-RESULT-1
request_id=<same 32 hex>
client_nonce=<same 64 hex>
server_nonce=<64 nonzero hex>
service_generation=<16 nonzero hex>
state=ready|unavailable
attach_caps=<8 hex>
kernel_abi=<8 hex>
kernel_caps=<8 hex>
ticket_lease_ms=<250..60000>
control_policy=disabled
message=<percent-encoded UTF-8>
```

The host requires the exact request ID, client nonce, kernel ABI, and requested
capabilities. `control_policy` has only one legal value because version 1 cannot
mutate a target.

## Exact-title discovery

Client request:

```text
VITADEBUG-ATTACH-DISCOVER-1
request_id=<32 hex>
server_nonce=<handshake server nonce>
service_generation=<handshake generation>
target_title_id=<9 uppercase ASCII letters/digits>
mode=observe
```

The request deliberately has no PID, address, module path, or wildcard.

Broker result:

```text
VITADEBUG-ATTACH-DISCOVER-RESULT-1
request_id=<same 32 hex>
server_nonce=<same server nonce>
service_generation=<same generation>
state=found|not_found|denied|changed|error
target_title_id=<same title ID>
pid=<8 hex>
main_modid=<8 hex>
main_fingerprint=<8 hex>
target_generation=<16 hex>
target_ticket=<64 hex>
message=<percent-encoded UTF-8>
```

For `found`, PID and main module ID must be positive `SceUID` values and the
main-module fingerprint, target generation, and ticket must be nonzero. Every
identity field must be zero for all failure states. The broker owns the mapping
between the random ticket and the complete revalidated target snapshot; clients
do not promote PID to an authorization handle.

## Ticket release

Client request:

```text
VITADEBUG-ATTACH-RELEASE-1
request_id=<32 hex>
server_nonce=<handshake server nonce>
service_generation=<handshake generation>
target_ticket=<64 nonzero hex>
```

Broker result:

```text
VITADEBUG-ATTACH-RELEASE-RESULT-1
request_id=<same 32 hex>
server_nonce=<same server nonce>
service_generation=<same generation>
target_ticket=<same ticket>
state=released|missing|changed|error
message=<percent-encoded UTF-8>
```

`released` and an already-expired `missing` result are successful safe states.
`changed` and `error` fail the host operation. A ticket owns only read-only
identity metadata; connection loss lets it expire without leaving a process
stopped or modified.

## Required server checks

The broker core enforces framing, session, replay, ticket, and result
invariants. Its injected Vita inventory provider must:

1. resolve the exact title ID to a PID;
2. reverse-resolve that PID to the same title;
3. reject non-user/system targets and any title outside the Vita-side allowlist;
4. have the kernel revalidate title, process status, main module ID, and module
   fingerprint;
5. provide a nonzero target generation suitable for detecting PID/module reuse.

The broker core generates an unpredictable ticket, binds it to the peer,
connection session, service generation, and returned target snapshot, expires
it, and repeats the injected complete identity check at one-use release.

A connection accepts at most 64 unique request IDs, including `HELLO`, using a
fixed replay table. A client reconnects before exhausting that table. Closing a
connection invalidates its ticket immediately.

## Versioning and authentication boundary

The framing and parsers are hardened, but version 1 does not authenticate or
encrypt the network transport. It is suitable only for a trusted private LAN
and read-only development probes. An allocation-free broker core is shipped
and cross-built for Vita, but no resident Vita listener or trusted
foreign-target identity adapter is available yet.

A later loader protocol must use a different header/version and a replay-proof
signed authorization that binds both nonces, service/target generations, exact
title ID, fixed debugger-module identity, requested operation, and short
expiration. It must never reinterpret a version-1 record as permission to
change target state.

# Related-work review and capability boundaries

This review records architectural inspiration only. No source, vendored asset,
protocol implementation, firmware NID, or binary from either project is copied
into VitaDebugger.

## Reviewed revisions and licenses

| Project | Reviewed revision | License/status |
| --- | --- | --- |
| [mvizensk/vita-agent-bridge](https://github.com/mvizensk/vita-agent-bridge) | `0ce18215fbe78745af8c8c1fa9a4c9c260faaa0d` | MIT; new proof of concept with no tests, CI, or releases at the reviewed revision |
| [devnoname120/vitacompanion](https://github.com/devnoname120/vitacompanion) | `d46772344d926e6765943703adf1a1518c20e10e` (`1.07`) | MIT at top level; vendored `libftpvita` has a separate MIT notice |

Because this implementation is independent and derives no substantial code,
those notices are not redistributed here. If future work imports or adapts
code, it must preserve the applicable MIT notice and identify the derived
files. The `ds4vita` touch approach mentioned by vita-agent-bridge was treated
only as a concept because no license was identified during review.

## Compatible concepts adopted

- Keep device transport, host receiver, and any local automation/MCP adapter
  as distinct trust boundaries inside one source-owned companion design. The
  current static archive and Python screen receiver implement the first two
  boundaries; no MCP server is exposed.
- Start with one serialized client, fixed/versioned records, exact payload
  bounds, format conversion, checksums, sequence/timestamp validation, frame
  rate limits, timeout limits, drop-old behavior, and explicit cleanup.
- Bind output to the exact application title ID, PID, process generation, and
  random session ID so stale frames cannot silently describe a later launch.
- Return typed errors to callers. A future MCP adapter must map protocol,
  authorization, timeout, and stale-generation errors to tool errors rather
  than terminate its process or manufacture success.
- The source-owned control foundation negotiates an exact ABI and capability
  mask before operation dispatch. It uses injected transport callbacks and
  owns no worker; malformed sessions and disconnects release input and close
  the bound transport terminally.
- Keep side-by-side development ports and service names compile-time explicit
  so experimental services cannot replace an installed broad service.

## Mechanisms explicitly rejected

- shell-plane or cross-process framebuffer capture, including
  `ksceDisplayGetProcFrameBufInternal`;
- SceShell injection, loader expansion, arbitrary process/module targeting, or
  DRM/protected-content bypass;
- unauthenticated listeners on all interfaces, fake FTP login, plaintext
  command multiplexers, active-mode FTP callbacks, or arbitrary
  device-qualified filesystem paths;
- global controller/touch hooks, PS-button capture, synthetic global input,
  quit-all, reboot, or unrestricted title launch;
- missing-row substitution, ignored pixel formats, unbounded FPS, silent
  protocol recovery, or success-shaped fallback frames; and
- default-on sleep suppression or a long-lived broad `*main` service.

vita-agent-bridge's framebuffer path lacks the fencing, checksum, format,
rate, and failure rules required here. vitacompanion has no screenshot or
framebuffer implementation; its display operation controls power only.
Neither protocol is a compatible transport for this stream.

## Companion capability model

Screen observation remains a read-only capability. Do not place broader tools
behind the plaintext screen token. The companion control foundation therefore
uses a distinct nonzero secret, keyed BLAKE2b record authentication,
monotonically increasing sequence, terminal replay rejection, short deadlines,
exact title/PID/process-generation/session binding, and explicit negotiated
capabilities. It provides integrity and admission on a trusted private LAN,
not confidentiality.

| Capability | Minimum acceptable boundary |
| --- | --- |
| Screen latest-frame read | Local-only API over checksum-verified atomic output; optional MCP tool accepts no path or address |
| Application file read | Implemented through an application-owned root provider; canonical relative paths and opened-object attestations; regular files only; entry, request, and session bounds; no device/mount prefix from the peer |
| Title/process inventory | User-mode provider returns only allowlisted development title IDs plus opaque launch generations; no addresses, modules, system titles, or implicit mutation |
| Title launch | Exact build-time allowlist, explicit opt-in, user titles only, no arbitrary path/URI/arguments, and no quit/reboot operation |
| Input | Implemented as an explicit mutation capability and application callback only, with 50-1000 ms leases, deadline/sequence checks, watchdog expiry, and forced neutral release |
| Input recording/replay | Separate per-session capabilities and consents; physical samples enter only through the source-owned app API; fixed trace bounds and identity; 1x tick-driven playback only through the app callback; neutral release on every terminal path |
| MCP adapter | Loopback by default, narrow schemas without arbitrary paths/PIDs/addresses, per-capability grants, bounded responses, and typed failures |

Title launch/inventory and MCP remain unsupported. File reads and cooperative
input are implemented only by the source-owned companion control protocol;
they do not broaden or share the screen token/protocol. There is no
authenticated-encryption claim, so the hardware gate remains limited to one
trusted private LAN and must not carry secrets in control payloads.

## Future compatibility matrix

In addition to the serialized color/stride/CRC hardware procedure, any future
resident service or input capability must test:

- exact ABI rejection and unsupported-firmware failure before side effects;
- coexistence with the matching `vitadebug.skprx`, `vdbtcp.suprx`, and
  application-linked debugger/profiler revisions;
- suspend/resume, display power transitions, Wi-Fi loss/reconnect, application
  relaunch, and service reload;
- malformed, slow, replayed, duplicated, and abruptly disconnected clients;
- socket abort before worker join and complete retryable network teardown; and
- input watchdog expiry and neutral state after every disconnect or crash.

The coexistence gate is mandatory because historical vitacompanion issue 17
reported freezes/stutters around sleep/network use with older VitaDebugger
modules. That report is a test requirement, not evidence that this host-only
foundation is affected.

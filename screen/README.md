# VitaDebugger source-owned companion foundation

This directory provides one **default-off, application-linked, user-mode**
developer companion archive. `libvitadebug_companion.a` integrates the
reviewed application-owned screen stream with a host/compile-qualified control
foundation. It does not modify `libuvdb.a`, the kernel companion, attach
broker, loader, deployment agent, VitaCompanion, or any system process.
Nothing starts a socket, reads a framebuffer, or accepts control merely by
building the repository.

The producer accepts only exact pointers registered by the embedding
application and labels every frame source-owned. The application must call
`vd_screen_submit_displayed_frame()` only for a buffer it owns and has selected
for display. There is deliberately no PID, arbitrary address, memory reader,
system compositor access, DRM path, injection, or external-attach fallback.
Control status is limited to the same configured title/PID/generation/session;
input reaches only a registered application callback under a short lease; and
file access reaches only a registered read-only provider for one explicit
application-owned debug root.
The same layer can explicitly record physical controller/touch samples handed
to it by that application and replay a verified trace only through the
registered application callback.

This revision is a host-tested foundation, **not a claim of continuous
capture or control support on Vita hardware**.

## Architecture and safety choice

Repository review found:

- `monitor display` uses public `sceDisplayGetFrameBuf()` only to cache
  metadata in ordinary application server context. It intentionally never
  reads pixels.
- the debugger's kernel companion handles all-stop/thread state and has no
  narrow, proven framebuffer primitive;
- the profiler already owns a bounded, nonblocking user-mode SceNet TCP sink
  with explicit connect/send deadlines and cleanup obligations;
- authenticated attach v2 is a host-tested, unencrypted identity protocol
  whose hardware listener remains disabled; expanding it would also couple
  screen observation to external attach; and
- VitaDevDeploy's optional vita2d framebuffer is process-owned but has a
  separate fragile GPU lifecycle and is unrelated to observing another
  application's own render target.

Accordingly, screen capture and control are one source-owned companion
library, not permanent plugins. The screen component injects a complete-buffer
writer callback and can compose with
`vp_vita_tcp_sink_write()` from `libvitaprofiler.a`; it does not duplicate or
weaken that transport. It does not call `sceDisplayGetFrameBuf()` because this
repository has hardware evidence only for metadata, not for safe pixel-format
interpretation or framebuffer read timing. The application already knows its
render target format and presentation point and is the authority that can make
that assertion. The legacy
`libvitadebug_screen.a` output remains for source compatibility, but new
integration uses `libvitadebug_companion.a`; both are built from the same
screen source rather than separate runtime plugins.

The control component is transport- and filesystem-abstracted. This layer
opens no Vita socket and calls no Vita filesystem, process, input, module, or
kernel API. A source-owned application supplies the bind/send/close,
cooperative input, and confined debug-root callbacks. Initialization validates
all policy first and treats a bind callback failure as terminal. The callback
contracts, exact record format, keyed BLAKE2b authentication, capabilities,
sequence protection, deadlines, and bounds are specified in
[companion protocol v1](docs/companion-protocol-v1.md).

## Producer integration

Build only when the application deliberately opts in:

```sh
make -C screen vita-check
```

The primary output is
`screen/build/vita/libvitadebug_companion.a`. It includes the screen producer
and Monocypher implementation used elsewhere in this repository. The
lower-level screen API remains available, but a companion integration should
initialize `vd_companion_config`, explicitly select capabilities, set the
source-owned target identity and nonzero random secret/session, and provide
callbacks. Mutation stays disabled unless
`VD_COMPANION_MUTATION_CONSENT` is separately set.
The nested screen configuration requires its own nonzero token; never reuse
the control MAC secret because screen protocol v1 sends its token in the
admission preface.

The screen-only call pattern below documents the underlying framebuffer
contract. Companion callers supply the same sources through
`vd_companion_screen_config` and use `vd_companion_screen_begin()` plus
`vd_companion_submit_displayed_frame()`:

```c
static struct vd_screen_stream screen;
static struct vd_screen_source sources[2];

void start_screen_stream(void *front, void *back, size_t bytes,
                         const uint8_t token[32], uint32_t process_id,
                         uint64_t process_generation, uint64_t session_id)
{
    struct vd_screen_config config;
    vd_screen_config_init(&config);
    sources[0] = (struct vd_screen_source){front, bytes};
    sources[1] = (struct vd_screen_source){back, bytes};
    config.explicit_consent = VD_SCREEN_EXPLICIT_CONSENT;
    config.write = vp_vita_tcp_sink_write;
    config.write_user = &tcp_sink;
    config.sources = sources;
    config.source_count = 2;
    memcpy(config.title_id, "MYAPP0001", 10);
    config.process_id = process_id;
    config.process_generation = process_generation;
    config.session_id = session_id;
    memcpy(config.auth_token, token, 32);
    if (vd_screen_stream_init(&screen, &config) == VD_SCREEN_OK)
        vd_screen_stream_begin(&screen);
}
```

After the application has presented a registered buffer, submit that exact
buffer with its known byte format:

```c
const struct vd_screen_frame frame = {
    .pixels = displayed_buffer,
    .width = 960,
    .height = 544,
    .stride_bytes = 1024 * 4,
    .pixel_format = VD_SCREEN_PIXEL_RGBA8888,
    .timestamp_us = application_monotonic_us(),
};
vd_screen_submit_displayed_frame(&screen, &frame);
```

Submitting faster than `min_frame_interval_us` returns
`VD_SCREEN_DROPPED`; no frame is queued. A writer error permanently fails the
stream because a partial frame may already be visible. Stop submitting before
closing the transport. The library allocates no memory and owns no thread,
socket, SceNet module, display API, or framebuffer.

The default producer interval is 100 ms (10 fps), and values faster than
16,667 us (approximately 60 fps) are rejected. Use a fresh random session ID
and increment the application process generation on each source-owned launch.
The receiver binds every frame and `latest.json` to the exact title, PID,
process generation, and session so local automation can reject stale output.

## Host receiver

Create a fresh 32-byte random token without putting it on a command line:

```powershell
$token = New-Object byte[] 32
[Security.Cryptography.RandomNumberGenerator]::Fill($token)
[IO.File]::WriteAllBytes(".\screen-token.bin", $token)
```

The receiver binds loopback by default. A mock/local producer can use:

```powershell
py -3 .\screen\tools\vdscreen.py receive `
  --token-file .\screen-token.bin `
  --output .\screen-capture `
  --port 18197
```

A Vita cannot reach loopback. A supervised hardware gate must explicitly bind
the host's private-LAN address and acknowledge the exposure:

```powershell
py -3 .\screen\tools\vdscreen.py receive `
  --token-file .\screen-token.bin `
  --output .\screen-capture `
  --bind 192.168.1.10 --port 18197 --allow-lan
```

### Side-by-side hardware-test identity

The screen receiver permits only ports 18000 through 18999 and rejects
VitaDebugger's existing 18194 DebugNet, 18195 profiler, and 18196 deployment
ports plus companion control 18198. Companion control accepts only a distinct
validated 18000-18999 port and rejects 18194-18197, defaulting to 18198. These
ranges cannot select VitaCompanion 1337/1338 or vita-agent-bridge 1348. A bind
failure is terminal for that invocation and never selects another port.
Loopback is the control default; private-LAN scope requires a separate consent
value.

The screen receiver validates limits and its nonzero authentication token
before socket creation; after accept, the listener owns the socket only until
the receive handoff begins. The receiver is then its sole cleanup owner, so
every setup/protocol failure and valid session attempts temp cleanup,
shutdown, and exactly one close. An active protocol/setup error remains
primary if temp cleanup also fails; without a primary failure, the first
cleanup error is surfaced after all cleanup steps have been attempted.

Use these exact identities for the first serialized hardware gate:

| Surface | Hardware-gate identity |
| --- | --- |
| Screen listener/service | `vdscreen-v1`, TCP `18197` |
| Control service | `vitadebug-companion-v1`, TCP `18198` |
| Application archive | `libvitadebug_companion.a` |
| Disposable application title ID | `VDSCRN001` |
| Disposable application/module name | `vitadebug_companion_gate` |
| Candidate VPK artifact | `vitadebug-companion-gate.vpk` |
| Resident SUPRX/SKPRX | none |

The same values are machine-checked in
[`config/hardware-gate-side-by-side.json`](config/hardware-gate-side-by-side.json).
`HOST_PRIVATE_IPV4` is a required placeholder, not a default address; replace
it only in process-local run configuration after hardware authorization.

The current foundation intentionally does not build the candidate VPK or
install a module. The gate application must remain a normal source-owned
user-mode title and set the matching profiler TCP sink endpoint explicitly:

```c
tcp_config.endpoint.port = 18197u;
```

Host build/run override:

```powershell
.\tools\invoke-vita-env.ps1 make -C screen vita-check
py -3 .\screen\tools\vdscreen.py receive `
  --token-file .\screen-token.bin `
  --output .\screen-capture `
  --bind 192.168.1.10 --port 18197 --allow-lan
```

Do not reuse `VitaCompanion`, `vitacompanion.suprx`, `vitacompanion.skprx`,
`VITA_COM`, ports 1337/1338, or any of its paths/artifacts. This work neither
reads nor changes VitaCompanion configuration or files, so it can remain
installed as a separately invoked fallback. Any later resident companion is a
new reviewed feature and must use distinct module names plus exact ABI
negotiation; it is not authorized by this gate.

Development-title inventory and launch are intentionally unsupported because
this repository does not yet contain a reviewed static user-approved allowlist
and lifecycle backend. Protocol requests return the typed unsupported result.
There is also no arbitrary process inventory, kill/quit-all, reboot, module
loading/injection, protected/system target, arbitrary mount access, FTP
write/delete/upload, PS/power/system input, global hook, or MCP exposure.

Cooperative input recording/replay has separate negotiated capabilities and
separate record/playback consents. Recording is visible in paired status and
accepts only samples submitted by the current source-owned app; there are no
global `SceCtrl`/`SceTouch` hooks. Playback is real-time 1x, tick-driven,
drift-bounded, never automatic after reconnect, and always neutralizes the
application callback on completion or failure. See
[input trace v1](docs/input-trace-v1.md).

The deterministic consumer entry point verifies the atomic manifest and image:

```powershell
py -3 .\screen\tools\vdscreen.py latest --output .\screen-capture
```

Local tools can import `vdscreen.receiver.read_latest`. The selected PPM file
and `latest.json` contain only a complete checksum-verified frame. Disk storage
is bounded to two image slots plus metadata.

## Threat and privacy model

- Streaming is off unless an application links the companion archive, passes
  `VD_SCREEN_EXPLICIT_CONSENT`, supplies a nonzero token, registers its own
  buffers, starts a transport, and submits frames.
- The receiver requires the exact session token and defaults to loopback.
  LAN listening requires `--allow-lan`.
- The token and CRC do not provide confidentiality. Anyone able to sniff the
  trusted LAN can see pixels and the token. Stop the receiver and delete the
  token after the session. Do not capture secrets or protected content.
- The producer cannot verify human intent, buffer ownership, or whether content
  is DRM-protected; those remain source-application responsibilities. Do not
  integrate this API into applications that render protected or third-party
  content.
- Malformed, oversized, fast, stale, or incomplete peers fail closed. The
  receiver keeps no unbounded queue and preserves the last complete frame
  across disconnect while removing temporary files.
- Control is separately default-off and authenticated. It binds loopback
  unless private-LAN scope is explicitly authorized, grants only enabled
  capabilities, and releases cooperative input on timeout, malformed-session
  abort, disconnect, or shutdown. Authentication provides integrity and
  admission, not confidentiality.

## Validation status and future hardware gate

Host tests cover wire compatibility, ownership/bounds checks, producer
drop-old behavior, terminal partial writes, authentication, CRC, malformed and
truncated frames, duplicate/gap handling, rate rejection, PPM conversion,
atomic publication, control framing/replay/deadline rejection, confined file
operations, input leases, trace round trips, button/analog/touch fidelity,
markers, overflow, malformed traces, wrong identities, bounded replay drift,
cancel/failure/disconnect cleanup, and exact neutral release. `vita-check`
compiles with `-Wall -Wextra -Werror` and links a Vita ELF.

Hardware validation remains serialized and must not begin while another task
owns the device. The full control, Wi-Fi/suspend, malformed-client,
generation-change, watchdog, cleanup, and fallback matrix is in the
[future companion hardware runbook](docs/companion-hardware-runbook.md).
The screen-specific portion remains:

1. Obtain explicit coordinator release and record device/firmware, repository
   commit, VitaSDK revision, application revision, IP/port, and token handling.
2. Use a disposable source-owned application with a static color-bar buffer;
   do not use `sceDisplayGetFrameBuf()` or inspect another process.
3. Start the host receiver on one private interface with conservative
   1 frame/second and two-minute limits, then connect through the existing
   profiler TCP sink.
4. Compare exact color, dimensions, stride, sequence, CRC, and timestamps;
   disconnect the host mid-header and mid-payload and verify bounded cleanup.
5. Exercise normal close and application exit, confirming no retained socket,
   worker, framebuffer, or SceNet ownership.
6. Repeat with the application's real double-buffer presentation callback at
   no more than 10 fps. Record drops and frame coherence.
7. Only after evidence for a specific renderer/format may documentation call
   that integration continuously capture-capable. A future public display API
   adapter requires its own reviewed format/coherency gate and must require an
   exact registered-base match.

See [protocol v1](docs/protocol-v1.md) for the exact wire contract.
See [companion protocol v1](docs/companion-protocol-v1.md) for the control
wire contract and capability boundaries.
The clean-room review of the two inspiration projects and the deliberately
unimplemented automation boundaries are recorded in
[related work](docs/related-work.md).

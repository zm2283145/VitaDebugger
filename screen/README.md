# VitaDebugger screen streaming foundation

This directory provides a **default-off, application-linked, user-mode**
screen stream. It does not modify `libuvdb.a`, the kernel companion, attach
broker, loader, deployment agent, or any system process. Nothing starts a
socket or reads a framebuffer merely by building the repository.

The producer accepts only exact pointers registered by the embedding
application and labels every frame source-owned. The application must call
`vd_screen_submit_displayed_frame()` only for a buffer it owns and has selected
for display. There is deliberately no PID, arbitrary address, memory reader,
system compositor access, DRM path, injection, or external-attach fallback.

This revision is a host-tested foundation, **not a claim of continuous
capture support on Vita hardware**.

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

Accordingly, screen capture is a separate source-owned library. It injects a
complete-buffer writer callback and can compose with
`vp_vita_tcp_sink_write()` from `libvitaprofiler.a`; it does not duplicate or
weaken that transport. It does not call `sceDisplayGetFrameBuf()` because this
repository has hardware evidence only for metadata, not for safe pixel-format
interpretation or framebuffer read timing. The application already knows its
render target format and presentation point and is the authority that can
make that assertion.

## Producer integration

Build only when the application deliberately opts in:

```sh
make -C screen vita-check
```

The output is `screen/build/vita/libvitadebug_screen.a`. Initialize a
caller-owned transport first, then register one to three exact framebuffer
bases:

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
ports. This range also cannot collide with VitaCompanion defaults 1337/1338 or
vita-agent-bridge 1348. A bind failure is terminal for that invocation and the
failed listener is closed before the error returns. Limits and the nonzero
authentication token are validated before socket creation; after accept, both
the listener handoff and receiver own unconditional close paths so setup or
validation failures cannot leak the accepted connection.

Use these exact identities for the first serialized hardware gate:

| Surface | Hardware-gate identity |
| --- | --- |
| Host listener/service | `vdscreen-v1`, TCP `18197` |
| Application archive | `libvitadebug_screen.a` |
| Disposable application title ID | `VDSCRN001` |
| Disposable application/module name | `vitadebug_screen_gate` |
| Candidate VPK artifact | `vitadebug-screen-gate.vpk` |
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

The deterministic consumer entry point verifies the atomic manifest and image:

```powershell
py -3 .\screen\tools\vdscreen.py latest --output .\screen-capture
```

Local tools can import `vdscreen.receiver.read_latest`. The selected PPM file
and `latest.json` contain only a complete checksum-verified frame. Disk storage
is bounded to two image slots plus metadata.

## Threat and privacy model

- Streaming is off unless an application links the separate archive, passes
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

## Validation status and future hardware gate

Host tests cover wire compatibility, ownership/bounds checks, producer
drop-old behavior, terminal partial writes, authentication, CRC, malformed and
truncated frames, duplicate/gap handling, rate rejection, PPM conversion,
atomic publication, and disconnect cleanup. `vita-check` compiles with
`-Werror` and links a Vita ELF.

Hardware validation remains serialized and must not begin while another task
owns the device:

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
The clean-room review of the two inspiration projects and the deliberately
unimplemented automation boundaries are recorded in
[related work](docs/related-work.md).

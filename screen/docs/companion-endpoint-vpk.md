# Source-owned companion endpoint VPK

`screen/endpoint` is the concrete, normal user-mode application endpoint for
the VitaDebugger companion foundation. It builds the separately installable
`vitadebug-companion-gate.vpk` with title ID `VDSCRN001` and executable
`vitadebug_companion_gate`. It does not install a SUPRX/SKPRX, modify
VitaCompanion, inject into SceShell, hook global input, inspect another
process, launch software, reboot, or expose a mutable filesystem.

The title allocates and displays its own two framebuffers and submits only
those registered buffers to the existing screen stream. Control input is
delivered only to its application-local callback. Its read-only debug root is
synthetic and contains only `status.bin` and a completed `input.vdtr`; it never
resolves arbitrary Vita paths. Physical input recording and cooperative
playback use separate capabilities and consents and are mutually exclusive in
the trace state machine.

## Offline opt-in configuration

The application starts no network service when this exact file is absent or
invalid:

```text
ux0:data/VitaDebuggerCompanion/config.bin
```

Create the file and two separate host-side secret files offline:

```powershell
py -3 .\screen\tools\vdcompanion_config.py `
  --output-directory .\companion-config `
  --host 127.0.0.1
```

This status-only loopback configuration is useful for deterministic host
validation. A future authorized private-LAN session must name both the exact
host destination and the exact Vita interface address:

```powershell
py -3 .\screen\tools\vdcompanion_config.py `
  --output-directory .\companion-config `
  --host HOST_PRIVATE_IPV4 --bind-address VITA_PRIVATE_IPV4 --allow-lan `
  --screen --input --debug-files --record --playback
```

The generator creates files exclusively, never overwrites an existing config,
does not print either secret, and performs no network or device operation.
Copying the resulting config to a Vita is a later hardware action and requires
separate authorization.

`config.bin` is a fixed 128-byte `VDCG` v1 record. It contains explicit
consent, an exact capability mask, fixed control/screen ports 18198/18197,
the host and Vita IPv4 addresses, and distinct nonzero 32-byte control and
screen secrets. Loopback is exactly `127.0.0.1`. Private-LAN scope requires
explicit LAN consent and distinct RFC1918 host/bind addresses. The Vita binds
the configured address exactly; wildcard and public binds are impossible.
Reserved bytes must be zero. Invalid, truncated, oversized, unknown, or
inconsistent records leave all services off.

## Build and package

From a VitaSDK environment:

```powershell
.\tools\invoke-vita-env.ps1 bash -c 'cd screen && make clean && make check'
```

The endpoint build uses `-Wall -Wextra -Werror`, links an ordinary safe
user-mode fSELF, and emits:

```text
screen/build/endpoint-vita/vitadebug-companion-gate.vpk
```

The build is offline and does not install, deploy, publish, or contact a
device. The generated `param.sfo` must identify `VDSCRN001`,
`VitaDebug Companion Gate`, version `01.00`; the VPK contains only
`eboot.bin` and `sce_sys/param.sfo`.

## Runtime lifecycle

Control binds TCP 18198 and the screen stream connects to TCP 18197. The
endpoint accepts one control client for one process generation/session.
Malformed framing, partial-record timeout, authentication failure,
disconnect, callback error, or title exit is terminal for that instance.
Cleanup first neutralizes cooperative input and trace playback, then closes
the screen and control ownership. A failed close remains quarantined and is
retried; the service never clears ownership optimistically.

The endpoint does not reconnect or resume a stale session. Exit with
Select+Start and relaunch with a fresh random PID/generation/session binding.
Local Select+Triangle toggles explicitly consented recording and
Select+Square starts explicitly consented playback of a ready trace.

Known limitations before hardware qualification:

- no hardware, Wi-Fi/suspend, or install behavior has been exercised;
- no confidentiality is provided beyond use of an authorized trusted LAN;
- the example title renders a deterministic diagnostic surface rather than
  integrating a third-party application renderer;
- only one control connection is accepted per title instance; and
- title inventory/launch remain intentionally unsupported.

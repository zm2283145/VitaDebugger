# VitaDebugger VS Code live-edit demo

This small VitaSDK application demonstrates the intended desktop workflow:
press **F5** in VS Code to build a debug VPK, install and launch it through
VitaDevDeploy, verify that the installed executable matches the retained ELF,
load its current ASLR addresses, and attach `arm-vita-eabi-gdb`. Stopping the
debug session closes only this sample title (`UVDBDEMO1`).

The sample starts with a visible global variable named `demo_value`. VS Code
automatically stops in `vscode_demo_breakpoint`; change `demo_value` from `1`
to `42`, Continue, and the message on the Vita changes without a rebuild.

## Requirements

- Windows, PowerShell, Python 3.10 or newer, GNU Make, and a working VitaSDK.
- The Microsoft **C/C++** VS Code extension (`ms-vscode.cpptools`). VS Code will
  recommend it when this folder is opened.
- The official Kubridge headers/import library on the PC and compatible
  `kubridge.skprx` loaded on the Vita.
- Vita Companion 1.06 active on an awake Vita connected to the same trusted
  private network.
- A VitaDevDeploy agent built with direct TCP intake, installed and paired with
  a local Ed25519 private key as described in the
  [deployment guide](../../deploy/README.md). An older FTP-only agent can still
  be used by selecting the documented fallback.
- The Vita at LiveArea before a new deployment. The workflow never force-closes
  an unrelated running application.

The sample uses the application-linked debugger only. It does not require the
optional VitaDebugger kernel companion, modify `ur0:tai/config.txt`, or reboot
the Vita.

## First-time setup

1. Open **this folder**, `examples/vscode-debug-demo`, as the VS Code workspace.
2. Install the recommended Microsoft C/C++ extension if VS Code offers it.
3. Run **Terminal > Run Task > Vita: Configure device**.
4. Enter the Vita's current dotted-decimal IPv4 address. The setup validates the
   address and uses it for FTP, Vita Companion, the symbol snapshot, and GDB.
5. The helper detects conventional VitaSDK, MSYS2, Kubridge, OpenSSL, and deploy
   key locations. If one cannot be detected, enter its absolute path in the
   task terminal.

The setup writes three machine-local files:

- `.vscode/vita.local.json` — the single source of truth for the device address,
  deployment transport, ports, and local tool paths;
- `.vscode/launch.json` — generated C++/GDB launch configuration;
- `.vscode/c_cpp_properties.json` — generated VitaSDK IntelliSense paths.

All three are ignored by Git. Only the private key's path is stored; the key
contents are never copied. To use another Vita or a changed DHCP address, run
**Vita: Configure device** again and enter the new IP once.

For a noninteractive setup or nonstandard paths, run:

```powershell
py -3 tools/session.py configure `
  --vita-ip 192.0.2.10 `
  --vita-sdk C:\vitasdk `
  --msys2-runtime C:\msys64\mingw64\bin `
  --msys2-usr C:\msys64\usr\bin `
  --kubridge C:\path\to\kubridge `
  --kubridge-lib C:\path\to\kubridge\build-local `
  --private-key C:\private\path\deploy_private.pem `
  --openssl C:\msys64\usr\bin\openssl.exe `
  --deploy-tcp-port 18196
```

`192.0.2.10` is documentation-only; replace it with the address shown by your
Vita. A new profile defaults to VitaDevDeploy's authenticated direct TCP
package carrier on port 18196. FTP, Companion, and GDB default to ports 1337,
1338, and 1234. The configure command accepts `--deploy-tcp-port`,
`--ftp-port`, `--companion-port`, and `--gdb-port` when a local setup uses
different ports. The selected GDB port is compiled into the sample and used by
both symbol capture and VS Code, so the device and desktop cannot silently
disagree.

Direct TCP sends the VPK bytes to the running VitaDevDeploy agent without first
placing the package in its FTP inbox. Protocol v1 still uses Companion FTP for
the small read-only recovery-status preflight and durable result record, and it
uses Companion's command port for exact-title launch/stop operations. All
addresses and ports remain in the ignored local profile rather than generated
source files.

FTP remains supported as an explicit fallback. Select it when using an older
agent without direct intake or while diagnosing the direct listener:

```powershell
py -3 tools/session.py configure `
  --vita-ip 192.0.2.10 `
  --deploy-transport ftp `
  --ftp-port 1337
```

Rerunning **Vita: Configure device** without `--deploy-transport` preserves an
existing profile's selected carrier; only a new profile defaults to TCP. The
private-key path is local and its contents never enter generated VS Code files.
Before every TCP F5 session, the helper runs VitaDevDeploy's read-only recovery
classifier. An unreadable status or any surviving `promote.state` marker blocks
the build/deploy sequence before compilation; reconcile the reported marker,
result, journal, installed title, and shallow package stage before trying
again.

## One-click debug and live variable edit

1. Wake the Vita, confirm Vita Companion is active, and leave the system at
   LiveArea.
2. In VS Code's **Run and Debug** view, select
   **Vita: Build, deploy, and debug demo** and press **F5**.
3. Wait for VS Code to stop in `vscode_demo_breakpoint`.
4. Add `demo_value` to the Watch panel, right-click its value, choose
   **Set Value**, and enter `42`.
5. Press **Continue**. The Vita changes to
   `Changed live from VS Code - success!`, and the Debug Console receives the
   matching `stdout` message through GDB `O` packets.

The same write can be made in the VS Code Debug Console with:

```text
-exec set variable demo_value = 42
```

Press **X** on the Vita to revisit the named breakpoint for another edit. Press
**Circle** for orderly in-app cleanup, or stop the VS Code debug session; the
post-debug task waits for debugger restoration and then sends only
`kill UVDBDEMO1` through Vita Companion.

## What F5 does

The pre-debug task runs one serialized sequence:

1. validates the ignored local profile and generated VS Code files;
2. for direct TCP only, requires a clean read-only recovery status before any
   build or deploy action;
3. validates the full Windows VitaSDK/MSYS2 runtime, preventing the recurring
   `cc1.exe` missing-DLL dialogs, then builds an isolated, unoptimized debug ELF
   and VPK under `build/` without reusing the repository's feature-sensitive
   object files;
4. records a build-identity receipt for that exact ELF/VPK pair;
5. closes only a stale `UVDBDEMO1` instance, if one is running;
6. creates and validates a signed job, transfers through the configured direct
   TCP or FTP carrier, installs, and launches the VPK with VitaDevDeploy; the
   host validates the matching durable result and retains signed evidence when
   an interrupted or ambiguous direct transfer cannot be proved complete;
7. verifies the installed `eboot.bin`, uses the first real RSP connection to
   capture current ASLR/module addresses, and detaches cleanly;
8. leaves the application thread itself at a second deterministic debugger stop
   so VS Code GDB becomes the next and only client with a readable PC, loads the
   verified symbol script, installs the function breakpoint, and continues to
   it; the persistent service then owns later interrupts and reconnects.

There is deliberately no TCP readiness probe for the configured GDB port.
VitaDebugger has a single-client listener; the first successful socket is
always used as an actual RSP session so a health check cannot consume the
debugger connection.

## Validation status

The TCP-default profile, legacy-FTP preservation, recovery-status preflight,
configured ports, deploy command, ASLR/build-identity flow, and generated editor
files pass the 51-test combined symbol/IDE host suite. A source-parity test also
requires this sample to link every fixed core object used by the main
`libuvdb.a`; it caught and now covers a linker failure after new core modules
were added.

The end-to-end direct-TCP F5 gate now passes on retail 3.65: build, signed
install, launch, installed-EBOOT verification, live-ASLR symbol loading, GDB
attach, source breakpoint, live variable mutation, resume, detach, and
exact-title cleanup all completed. The first post-fix attempt installed cleanly
but SceShell reported `C2-2752-6` on its initial target launch and the attach
timed out; an immediate full F5 retry passed. That recoverable first-launch race
remains a lifecycle-hardening item. See the
[direct-TCP F5 record](../../docs/hardware/vscode-debug-demo-direct-tcp-3.65.json).

## Useful tasks and troubleshooting

- **Vita: Validate local setup** checks the profile, generated editor files, and
  validated build environment without touching the Vita.
- **Vita: Build demo** creates the ELF/VPK without connecting to the Vita.
- **Vita: Stop demo** performs the same exact-title, idempotent stop used after
  a debug session.

If F5 reports that generated files are stale, rerun **Vita: Configure device**.
If the C++ debugger adapter fails after the task prints `Verified symbols are
ready`, run **Vita: Stop demo** before retrying; this closes only `UVDBDEMO1`.
If deployment cannot launch the agent, return the Vita to LiveArea and close any
unrelated running app. If installed-build verification fails, do not bypass it;
run F5 again so the exact current VPK is reinstalled. Use only a private trusted
LAN. Direct intake authenticates the signed job but does not encrypt it, while
the remaining Vita Companion FTP/command channels are neither authenticated nor
encrypted.

`demo_value` is intentionally global, volatile, writable, and built without
optimization. A debugger cannot promise the same Set Value behavior for a
constant, read-only object, or a local variable that another project's optimizer
removed or kept only in a transient register.

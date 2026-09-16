# VitaDebugNet owner-exit hardware gate

This logger-only VPK validates that the bounded UDP sender cannot hold Vita
application teardown when an application forgets `uvdb_debugnet_stop()`. It
does not start GDB, load a `.suprx`, or require the VitaDebugger kernel
companion. Its separate title is `VitaDebugNet Exit Gate` / `VDLG00001`.

Build from the repository root, overriding the receiver address when needed:

```sh
make debugnet-exit-gate DEBUGNET_GATE_HOST=PC_LAN_IP \
    DEBUGNET_GATE_PORT=18194
```

Replace `PC_LAN_IP` with the LAN IPv4 address of the computer running the
receiver.

The package is written to
`tests/vita/debugnet-exit-gate/build/debugnet-exit-gate.vpk`.

Start the receiver before launching the VPK:

```sh
python tools/debugnet_listener.py --port 18194 --source VITA_IP
```

Each launch prints and transmits a unique run ID. The controls are:

- **X:** emit an `OWNER_EXIT` marker and return from `main()` without calling
  `uvdb_debugnet_stop()`.
- **Square:** emit a `QUEUE_FILL` marker, rapidly submit 2,048 large datagrams
  to saturate the bounded queue, and return without calling stop.
- **Triangle:** emit an `EXPLICIT_STOP` marker and call
  `uvdb_debugnet_stop()`. Press Circle after the result appears to close the
  control run cleanly.
- **PS / forced shell close (the original regression):** do not press any gate
  button. Leave the logger running, press PS, then either launch another app and
  accept Vita's prompt to close this gate, or peel the gate from LiveArea. The
  main loop emits one `SHELL_CLOSE_WAIT` heartbeat per second so the receiver
  proves that DebugNet was still active when SceShell terminated the process.

After every no-stop or forced-close path, immediately launch VitaDevDeploy,
VitaShell, or this gate again. Use this stress matrix:

| Path | Receiver active | Receiver disconnected |
| --- | ---: | ---: |
| X: return without stop | 20 cycles | 20 cycles |
| Square: full queue then return | 20 cycles | 20 cycles |
| PS: launch another app and accept Close | 20 cycles | 20 cycles |
| PS: peel the gate from LiveArea | 20 cycles | 20 cycles |
| Triangle: explicit-stop control | 5 cycles | 5 cycles |

For each PS run with the receiver active, confirm at least one
`SHELL_CLOSE_WAIT` heartbeat immediately precedes the forced close. Passing
means every launch succeeds, LiveArea stays responsive, the next application
opens, and no crash dump is produced.

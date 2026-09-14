# Read-only GDB monitor commands

VitaDebugger implements a deliberately small `qRcmd` registry for inspecting
the stopped debugger session from an ordinary GDB prompt. The currently
registered commands are:

```gdb
monitor help
monitor status
monitor threads
monitor modules
```

These commands are host-tested and hardware-tested on a retail Vita running
system software 3.65. The live gate passed two complete raw-RSP sessions and an
interactive-display check through GDB 15.2. Other firmware versions remain
untested.

## Command output

### `monitor help`

Prints the complete registered command list and a one-line description of each
command. It does not require a runtime snapshot.

### `monitor status`

Prints a bounded snapshot containing:

- debugger state, stopped/running state, and stop signal;
- acknowledgement mode, TCP port, and advertised packet size;
- thread count and the stopped, `Hg`, `Hc`, and exception-thread selections;
- active software-breakpoint count;
- whether the kernel companion is compiled in and, when available, its
  compatibility result, ABI, capability mask, and maximum thread count;
- stop-session health and whether guarded VFP reads are enabled; and
- the most recent structured fault type, status, address, and PC, or `none`.

### `monitor threads`

Refreshes the stopped thread inventory and prints each bounded entry as a
thread ID, zero or more state/selection flags, and a display name. The flags
are `stopped`, `Hg`, `Hc`, and `exception`. Cooperative names are used when
available; otherwise the command reports `stopped thread` or `process thread`.

### `monitor modules`

Lists the loaded modules visible through the process module APIs. Each entry
contains its module ID and display name followed by as many as four non-empty
segments, with the segment index, runtime address, memory size, and raw Vita
permission value. The header distinguishes modules shown, modules reported by
the system, entries skipped because their metadata could not be read, and
entries omitted when the system reports more than the fixed snapshot limit.
A module-query failure is returned as an explicit hexadecimal error value.

## Read-only boundary

The `qRcmd` handler is a fixed registry, not a command interpreter:

- Only the exact, case-sensitive command names `help`, `status`, `threads`, and
  `modules` are accepted. Leading and trailing spaces or tabs are ignored, but
  arguments, subcommands, and every unregistered name are rejected.
- The command must be valid even-length hexadecimal which decodes to no more
  than 32 bytes of printable ASCII or tab. Embedded NULs, other control bytes,
  invalid hex, and oversized input are rejected before dispatch.
- Decoded text is matched to an enum and `switch`; it is never passed to a
  shell, evaluator, function lookup, file API, or unrestricted memory API.
- The registry exposes no register write, memory write, breakpoint mutation,
  resume, detach, process-launch, filesystem, or kernel-memory operation.
- Status and thread queries refresh the debugger's bounded stopped inventory.
  In a kernel-assisted all-stop session, that refresh can perform the normal
  lease renewal and late-thread reconciliation needed to preserve the existing
  stop. It does not provide a caller-selected mutation primitive.
- Thread and module names are copied into fixed 32-byte fields and non-printing
  output bytes are replaced with `_`. Snapshots are capped at 64 threads, 128
  modules, and four segments per module.

This narrow surface does not change the broader network trust model. The GDB
transport is still unauthenticated and unencrypted, so use it only on a private,
trusted LAN.

## Response bounds

Each command renders plain text into a fixed buffer, then returns that text as
the final hex-encoded `qRcmd` reply. It is separate from the asynchronous GDB
console `O`-packet path and does not depend on stdout/stderr capture or no-ack
mode.

The raw-text capacity is the smaller of 16 KiB and half the negotiated RSP
packet payload, because every output byte becomes two hexadecimal characters.
If the complete report will not fit, the renderer keeps only complete lines
when possible and finishes the response with:

```text
... output truncated
```

The response never silently overruns the negotiated packet. Malformed and
unknown commands also produce bounded human-readable error replies directing
the developer to `monitor help`.

## Host validation

Run the focused parser/renderer test from the repository root:

```sh
make host-test-monitor
```

Run it with the complete native and lifecycle suite using:

```sh
make host-tests
```

The focused test covers the exact registry, whitespace handling, uppercase and
lowercase hex, malformed/control/oversized input, argument and unknown-command
rejection, every report shape, name sanitization, snapshot bounds, query
failure reporting, non-mutation of input snapshots, exact-capacity output, and
line-safe truncation.

## Live-GDB gate

Build and install the matching full-feature test application with kernel thread
control and guarded VFP reads enabled, load its matching compatible kernel
companion, stop it in VitaDebugger, and run the automated two-session raw-RSP
gate:

```powershell
py -3 tools/gdb_monitor_smoke.py --host VITA_IP --reconnect
```

The current smoke script deliberately expects `kernel: compatible`, an active
healthy stop session, and `VFP reads: enabled`; this makes a missing or mismatched
test configuration fail instead of being mistaken for monitor success.

For an interactive display check with the matching unstripped ELF, run:

```gdb
target remote VITA_IP:1234
monitor help
monitor status
monitor threads
monitor modules
info threads
info sharedlibrary
detach
```

The hardware gate verifies that all four commands return without a
transport error; `status` reports the actual stopped kernel session; `threads`
agrees with `qfThreadInfo`; `modules` contains the names returned by
`qXfer:libraries:read`; normal output is not unexpectedly truncated; malformed
and unknown commands return bounded help-directed errors without changing the
thread inventory or selected PC; and clean detach plus a fresh reconnect work
afterward. The retail 3.65 run passed with five stopped threads, 14 loaded
modules, kernel ABI `0x0001000b`, a healthy stop session, and guarded VFP reads
enabled. GDB 15.2 displayed each report and detached normally. The archived
machine-readable result is
[`hardware/gdb-monitor-commands-3.65.json`](hardware/gdb-monitor-commands-3.65.json).
A separate bounded fixture can exercise the visible truncation marker if the
normal target does not naturally fill a packet. Future hardware runs should
archive the device firmware, build identity, captured command output, and
reconnect results alongside the existing evidence.

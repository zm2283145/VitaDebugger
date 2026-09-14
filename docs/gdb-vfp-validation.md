# Live GDB VFP validation gate

The kernel boundary probe proves that the guarded snapshot call preserves and
returns the expected VFP bank. This separate gate proves that the complete
debugger path selects a stopped foreign application thread and presents the
same D0-D31/FPSCR layout to an unmodified VitaSDK GDB client.

Do not use this diagnostic fixture in a production application. Its registered
worker deliberately holds D0-D31 in a call-free loop so every debugger stop
has deterministic values. The loop consumes processor time while the test is
running.

The initial live gate targets the same owner-confirmed retail Vita running
system software 3.65. A pass there does not validate this undocumented VFP ABI
on another firmware, Vita TV, development hardware, or a different plugin
stack; repeat the boundary and lifecycle gates for every new target baseline.

## Prerequisites

1. The fail-closed VFP kernel probe has passed every check on this Vita and
   firmware.
2. The matching opt-in kernel companion is installed and advertises ABI v1.8
   plus `VD_KERNEL_CAP_THREAD_VFP_REGISTERS`.
3. The Vita and development computer are on a trusted network. The current RSP
   listener is not authenticated.

No kernel replacement is needed between the boundary probe and this test.

## Build and launch

Build the diagnostic package with all three application-side gates enabled:

```sh
make package UVDB_KERNEL_THREAD_CONTROL=1 UVDB_KERNEL_VFP_READS=1 \
  UVDB_GDB_VFP_FIXTURE=1 \
  VITADEBUG_KERNEL_DIR=kernel \
  VITADEBUG_KERNEL_BUILD_DIR=kernel/build-vfp
```

Install and launch `uvdb-test.vpk`. Continue only when the screen reports:

```text
GDB VFP fixture: ready (D0=1 D31=2 FPSCR=00400000)
```

`FAILED` means the fixture was not registered or did not publish its loaded
state within two seconds. Do not interpret any subsequent register output as a
passing result.

## Exact register check

Connect with the unstripped ELF from the same build:

```text
arm-vita-eabi-gdb test.elf
(gdb) target remote VITA_IP:1234
(gdb) info threads
```

Find the GDB thread number named `GDB VFP fixture`, select that number, and read
the endpoint registers:

```text
(gdb) thread GDB_THREAD_NUMBER
(gdb) p $d0
(gdb) p $d31
(gdb) p/x $fpscr
```

The required values are exactly:

```text
$d0    = 1
$d31   = 2
$fpscr = 0x400000
```

The endpoint bit patterns are respectively `0x3ff0000000000000` and
`0x4000000000000000`. D1-D30 contain distinct diagnostic sentinels; the
already-passed kernel probe remains the authoritative exhaustive D0-D31 mapping
check. GDB reads are intentionally read-only. A full `G` register write must be
rejected while the VFP target description is active.

## Lifecycle matrix

Repeat the exact three-register check after each transition:

1. Initial attach.
2. `continue`, then Ctrl-C after the application runs briefly.
3. Clean `detach`, reconnect, and select the fixture again.
4. Terminate one stopped GDB client without detaching so its TCP connection
   closes, reconnect, and verify both application progress and the three
   registers.
5. Separately rerun the stable kernel probe's intentionally abandoned-lease
   check with this same experimental plugin. A normally running lease-keeper
   renews the GDB stop, so merely waiting at an open GDB prompt does not test
   watchdog expiration.

A pass requires the application counter to resume after continue, detach, and
forced client closure; the fixture must remain listed and retain all three
expected values on every stop. The separate abandoned-lease probe must also
pass. Any hang, `E16`, unavailable VFP value, changed value, missing thread, or
inability to reconnect is a failure. Restore the known-good user application
build after collecting evidence; this fixture itself does not modify the
kernel plugin or its configuration.

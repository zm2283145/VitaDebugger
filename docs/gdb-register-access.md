# GDB individual register access

VitaDebugger implements the GDB Remote Serial Protocol `p` and `P` packets with
the register numbers advertised by the active target description. The parser
accepts one complete hexadecimal register number, requires the exact value
width for a write, accepts upper- or lowercase hexadecimal, and rejects signs,
prefixes, trailing data, overflow-width numbers, and invalid target layouts.
All register bytes use the same little-endian order as the existing `g` packet.

## Register numbering

The normal core-only build uses GDB's legacy ARM layout:

| Register number | Register | `p` | `P` |
| --- | --- | --- | --- |
| `0`-`f` | R0-R15 | value | selected exception thread only |
| `10`-`17` | legacy FPA F0-F7, 96 bits each | unavailable (`x`) | rejected |
| `18` | legacy FPS, 32 bits | unavailable (`x`) | rejected |
| `19` | CPSR | value | selected exception thread only |

When the opt-in D32 target description is negotiated, register numbers `10`
through `18` are intentionally absent, CPSR remains `19`, D0-D31 occupy `1a`
through `39`, and FPSCR is `3a`. `p` returns a D register as 16 hexadecimal
characters and a core register or FPSCR as eight. If the selected stopped
thread has no readable saved VFP bank, only the requested VFP register is
returned as a correctly sized unavailable value.

## Thread and mutation policy

`p` follows the current `Hg` selection. The exception thread supplies its ARM
core/CPSR values directly from the KuBridge exception context. With the matching
kernel companion, another stopped application thread supplies its dynamically
selected current-user or syscall-return core bank and, in an experimental VFP
build, its guarded read-only VFP snapshot.

`P` is deliberately narrower. It may update R0-R15 or CPSR only when `Hg`
resolves to the exception thread. The packet executes inside a renewed
all-stop operation and does not acknowledge a write until the complete packet
has been validated and the exception context has been updated. A selected
foreign thread, a legacy FPA register, D0-D31, or FPSCR receives an error. The
stub never reports `OK` for a discarded floating-point tail.

Foreign core and VFP mutation need a kernel contract rather than a parser
change. A separately versioned transaction scaffold now implements the bounded
snapshot, one-bank stage, exact read-back, commit-or-restore, retained-target
lifetime, and lease-cleanup rules under native tests. The Vita backend still
advertises zero writable banks: it cannot be promoted until a supported setter
and durable exact process/thread-object provider pass the corresponding hardware
failure gates. The transaction also requires a fully stopped process with no
additional exempt thread.

## Validation status

`tests/host/test_rsp_registers.c` covers both layouts, every individual read
offset against its corresponding `g` slot, exact little-endian values, absent
and unavailable registers, malformed and overflow inputs, unsupported VFP/FPA
writes, and exact inverse restoration of core/CPSR mutations. The complete host
suite and Vita cross-build pass for library-only, kernel-thread-control, and
kernel-thread-control-plus-VFP configurations.

The live gate now passes on retail 3.65 using the matching unstripped ELF and
VitaSDK GDB 15.2. Two stopped exception-thread sessions, separated by a clean
detach and reconnect, forced individual packets and completed this exact
transaction for both R0 and CPSR:

1. Read the original value with `p`.
2. Write a temporary value with `P` and require `OK`.
3. Read the temporary value back byte-for-byte with `p`.
4. Restore the exact original with `P` before any resume or detach.
5. Read the original back byte-for-byte with `p`.

The CPSR transaction changed only the V flag. A third connection verified that
the application was still progressing after the second clean detach. On the
foreign VFP fixture, individual `p` packets returned R0, D0, D31, and FPSCR;
the corresponding core/VFP `P` attempts returned `E16`, and every subsequent
read matched its original value. This validates the implemented policy; it does
not add foreign-thread or VFP writes.

The portable [retail 3.65 evidence](hardware/gdb-register-pp-3.65.json) records
the artifact identities, exact packet replies, restoration assertions, clean
detach/reconnect, and hashes of the private wire logs. It intentionally omits
the Vita address, local paths, and raw transcripts.

Every future firmware or kernel-ABI baseline must repeat the gate. If a
temporary write or restoration read-back ever fails, keep the target stopped:
do not continue, detach, or terminate GDB until the exact original value has
been restored and independently read back.

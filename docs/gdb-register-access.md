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

Foreign core and VFP mutation need a new kernel contract rather than a parser
change. That contract must prove target-thread ownership and suspension,
snapshot the original bank, apply one write, read it back, restore it on normal
resume/detach and lease expiry, and survive an abandoned client before the
capability can be advertised.

## Validation status

`tests/host/test_rsp_registers.c` covers both layouts, every individual read
offset against its corresponding `g` slot, exact little-endian values, absent
and unavailable registers, malformed and overflow inputs, unsupported VFP/FPA
writes, and exact inverse restoration of core/CPSR mutations. The complete host
suite and Vita cross-build pass for library-only, kernel-thread-control, and
kernel-thread-control-plus-VFP configurations.

The remaining hardware gate must use a matching unstripped ELF and real
VitaSDK GDB. Stop at a disposable fixture, force GDB's fetch-register and
set-register packets on, mutate a harmless core value with `P`, read it back
with `p`, restore the original value before continuing, and verify the fixture
result. Repeat across detach/reconnect. In the opt-in VFP build, verify D0, D31,
and FPSCR with individual `p` packets, attempt their `P` writes, require an
error, and confirm every value and the target's execution state are unchanged.
Do not describe core writes as hardware validated, or VFP writes as supported,
until those separate results are recorded.

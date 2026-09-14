# ASLR-aware GDB symbols

`tools/gdb_symbols.py` turns the modules reported by a live VitaDebugger
session into a GDB command file and a verified local symbol view. It handles
the main executable and each loaded user module separately; it never assumes
that one relocation delta applies to the whole process.

The workflow follows GDB's two different address contracts:

- `qOffsets` `TextSeg`/`DataSeg` values are the absolute runtime starts of the
  main ELF's first and second `PT_LOAD` segments.
- Each `qXfer:libraries:read` `<segment address=...>` value is the absolute
  runtime start of that module's corresponding `PT_LOAD` segment, not a
  relocation delta.

Those distinctions come from GDB's
[`qOffsets` packet](https://sourceware.org/gdb/current/onlinedocs/gdb.html/General-Query-Packets.html)
and
[`library-list` format](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Library-List-Format.html).

## Create a verified symbol view

Builds need an unstripped ELF on the PC. Load the application on the Vita, then
run, for example:

```powershell
py -3 tools/gdb_symbols.py `
  --host VITA_IP `
  --main-elf build/game.elf `
  --search-root build `
  --output build/game-live.gdb
```

The tool connects to port 1234, negotiates RSP no-ack mode when available,
stops the process, obtains every library-list chunk, obtains `qOffsets`, and
cleanly detaches. It then:

1. parses the main ARM ELF's `PT_LOAD` and allocated section layout;
2. calculates every main runtime segment implied by `qOffsets`;
3. requires exactly one library-list entry to have that complete address list;
4. matches other modules only by a unique embedded Vita module name or exact
   filename and a compatible segment/section layout;
5. copies each selected unstripped module into an immutable, content-addressed
   `.uvdb-solib` directory under its exact runtime name;
6. writes `file`, `target remote`, `set solib-search-path`, and `sharedlibrary`
   commands so GDB applies the current `qOffsets` and library segment addresses
   on every connection.

Start GDB with the generated file:

```powershell
C:\vitasdk\bin\arm-vita-eabi-gdb.exe -x build/game-live.gdb
```

Or have the snapshot tool start it immediately:

```powershell
py -3 tools/gdb_symbols.py `
  --host VITA_IP `
  --main-elf build/game.elf `
  --search-root build `
  --output build/game-live.gdb `
  --gdb C:\vitasdk\bin\arm-vita-eabi-gdb.exe
```

The default `solib` mode enables automatic shared-library loading only after
confining `solib-search-path` to that content-addressed directory. A
same-named file elsewhere on the PC therefore cannot be selected. GDB loads
the main ELF before `target remote`, applies the stub's fresh `qOffsets`, and
uses the fresh library list to relocate each cached module.

Each cache generation is `.uvdb-solib/<mapping-hash>/` beside the generated
script unless `--solib-cache-root` selects another location. Entries are
copied, hashed again, and never overwrite inconsistent content; `manifest.json`
records the runtime-name-to-SHA-256 mapping. The cache contains derived files
only and can be removed when its scripts are no longer needed.

The generated solib script is safe to reconnect and reuse after an ASLR-only
relaunch **when the deployed main executable and modules are still the exact
same builds**. Regenerate it after deploying different binaries, changing the
module set, or changing an explicit mapping. The optional `--gdb` form
minimizes the gap during the initial validation. If modules can load or unload
during that gap, stop at a deterministic application gate before generating
the symbol view.

For diagnosis, `--mode explicit` emits an `add-symbol-file` command for each
module with the runtime address of every non-empty allocated ELF section. That
mode deliberately disables automatic shared-library loading. Its addresses
belong to one process launch, so regenerate an explicit script after every
relaunch.

## Module matching

VitaSDK commonly produces several representations of one module:

- `.elf`: the linked, unstripped development ELF;
- `.velf`: the Vita-converted `ET_SCE_RELEXEC` file, which carries the embedded
  Vita module name but is not loaded into GDB as the symbol file;
- `.suprx`/`.skprx` or `eboot.bin`: normally a packed SELF used on the device.

Packed files beginning with `SCE\0` do not contain the ELF symbol/section view
GDB needs and are rejected. Point the tool at the original unstripped `.elf`.
VitaSDK rewrites relocation metadata when producing an `ET_SCE_RELEXEC`
`.velf`, so loading that converted file directly can give GDB incorrect symbol
values even though its `.symtab` remains present. The scanner uses a same-stem
`.velf` only as a module-name sidecar, and only when its symbol/debug digest and
load-segment bases match the original `.elf`. A `.suprx` is accepted only if
that particular file is a normal unstripped ELF rather than a converted or
packed Vita image.

When automatic exact matching is not possible, specify the association:

```powershell
py -3 tools/gdb_symbols.py `
  --main-elf build/game.elf `
  --search-root build `
  --module MyPlugin=plugins/MyPlugin.elf
```

Explicit files are still checked for ARM32 format, a usable `.symtab`, the same
number of runtime load segments, and a complete mapping for every allocated
section. Duplicate or ambiguous automatic matches stop the tool with an error.
The looser `foo.elf` to `foo.suprx` filename-stem rule is available only with
`--allow-stem-match`; an explicit mapping is preferable.

System modules normally appear as `UNMATCHED` comments because their matching
unstripped ELFs are not distributed with a homebrew project. To make a
particular user module mandatory, add `--require-module ModuleName`. The tool
then fails instead of silently producing a partial application symbol set.

## Trust boundary

The standard library-list XML provides names and segment starts, but no build
ID or cryptographic digest. A unique exact name and compatible ELF layout avoid
unsafe guesses; they cannot prove that a same-named local file is byte-for-byte
the installed build. Generated scripts record SHA-256 hashes of every selected
local ELF for build/deploy logs. A release or IDE pipeline should retain the
ELFs that produced the deployed VPK and use explicit mappings for project
modules.

The parser is deliberately bounded and fail-closed: at most 1 MiB of RSP/XML,
256 modules, four segments per module, 4,096 local candidates, and 4,096 ELF
sections. It rejects DTDs/entities, malformed addresses, mixed `qOffsets`
styles, allocated sections outside `PT_LOAD`, stripped files, unsafe generated
GDB section names, and reuse of one ELF for multiple live modules.

## Automated main-plus-user-SUPRX gate

The repository includes an opt-in resident user module with one deterministic
source breakpoint and a matching main-executable breakpoint. Build and install
the test package from one invocation:

```sh
make package UVDB_KERNEL_THREAD_CONTROL=1 UVDB_GDB_ASLR_FIXTURE=1
```

Keep `test.elf`, `build-aslr-fixture/uvdb_aslr_fixture.elf`, and its same-stem
`.velf` together. Only the packed `.suprx` belongs on the Vita. Once the test
screen reports `ASLR SUPRX fixture: PASS`, run:

```powershell
py -3 tools/gdb_aslr_lifecycle.py `
  --host VITA_IP `
  --title-id SLRS00001 `
  --main-elf test.elf `
  --suprx-elf build-aslr-fixture/uvdb_aslr_fixture.elf `
  --suprx-velf build-aslr-fixture/uvdb_aslr_fixture.velf `
  --suprx-self build-aslr-fixture/uvdb_aslr_fixture.suprx `
  --vpk uvdb-test.vpk `
  --search-root build-aslr-fixture `
  --allow-relaunch `
  --evidence docs/hardware/gdb-aslr-suprx-3.65.json
```

`--allow-relaunch` is mandatory because the complete gate asks Vita Companion
to kill and launch exactly the supplied nine-character title ID twice. It does
not send the broader `destroy` command. The target must already contain the
same VPK represented by the supplied local artifacts.

The gate performs five independent GDB sessions:

1. automatic solib loading and both source breakpoints on the initial launch;
2. clean detach, reconnect to the same process, and repeat with the same script;
3. relaunch and reuse that solib script against fresh runtime module metadata;
4. generate explicit section addresses and hit both breakpoints;
5. relaunch again, regenerate the explicit script, and hit both breakpoints.

Every phase validates exact request sequences, source filenames, GDB return
status, and that both function and stop PCs land inside the corresponding live
`.text` range. Same-process reconnect must retain the exact module layout.
Relaunches may legitimately choose the same random addresses, so address change
is recorded but optional by default; `--require-address-change` makes it a hard
requirement for a diagnostic run.

Default JSON evidence contains artifact hashes and sizes, module match reason,
runtime address summaries and module counts, generated-script hashes,
breakpoint markers, a validated short Vita Companion version label, GDB
version, and lifecycle results. It deliberately omits remotely supplied module
names outside the required fixture identity. Optional device, firmware, kernel
ABI, and kernel-plugin metadata can be supplied with `--device-class`,
`--firmware`, `--kernel-abi`, and `--kernel-plugin`. It omits the Vita endpoint,
local paths, and raw GDB output. `--include-sensitive-transcript` is only for
local diagnosis and its output must not be published.

### Retail 3.65 result

The complete gate passed on a retail handheld Vita running system software
3.65 with kernel ABI v1.11 and VitaSDK GDB 15.2. All five sessions reached both
source breakpoints and detached cleanly. The main and `UVDBAslrFixture` segment
addresses remained identical across the same-process reconnect, then both
changed on each of the two relaunches. Automatic solib loading followed the
first fresh layout; explicit section addresses were regenerated for the final
layout.

The portable record includes exact main ELF, module ELF/VELF/SUPRX, VPK, and
kernel-plugin hashes and sizes without publishing the Vita IP or workstation
paths: [retail 3.65 ASLR/SUPRX evidence](hardware/gdb-aslr-suprx-3.65.json).
That archived v1 record predates remote-name minimization; its captured module
inventory was reviewed before publication and contains only the test fixture
and standard system-module names. Newly generated v2 evidence stores only the
module count.
This completes the main-plus-user-SUPRX hardware gate. Build identity beyond
retained artifact hashes, dynamic module load/unload refresh, and IDE-managed
symbol regeneration remain future work.

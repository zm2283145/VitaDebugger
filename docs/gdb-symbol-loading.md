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

Builds need the exact VPK and its unstripped ELF files on the PC. First create
a versioned identity immediately after packaging, while those artifacts are
known to belong to the same build:

```powershell
py -3 tools/gdb_build_identity.py `
  --vpk build/game.vpk `
  --main-elf build/game.elf `
  --title-id GAME00001 `
  --output build/game.identity.json
```

For a packaged user module, also bind its retained unstripped ELF and
VPK-relative installed binary:

```powershell
py -3 tools/gdb_build_identity.py `
  --vpk build/game.vpk `
  --main-elf build/game.elf `
  --module MyPlugin=build/MyPlugin.elf `
  --module-installed MyPlugin=module/MyPlugin.suprx `
  --title-id GAME00001 `
  --output build/game.identity.json
```

The generator validates the complete VPK with VitaDevDeploy's bounded parser,
reads its `TITLE_ID`, requires normal unstripped ARM ELFs, and hashes the VPK,
every ELF, `eboot.bin`, and every named packaged module. It refuses to overwrite
an existing identity unless `--force` is explicit.

Install that VPK, load the application on the Vita, then create the symbol view:

```powershell
py -3 tools/gdb_symbols.py `
  --host VITA_IP `
  --main-elf build/game.elf `
  --vpk build/game.vpk `
  --build-identity build/game.identity.json `
  --verify-installed `
  --search-root build `
  --output build/game-live.gdb
```

Before touching the GDB connection, `--verify-installed` opens a read-only Vita
Companion FTP session and hashes `ux0:app/<TITLE_ID>/eboot.bin` plus every
recorded packaged user module. A missing file, FTP error, size difference, or
hash difference stops the tool; there is no offline or name-only fallback. It
then connects to port 1234, negotiates RSP no-ack mode when available, stops the
process, obtains every library-list chunk, obtains `qOffsets`, and cleanly
detaches. It then:

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
  --vpk build/game.vpk `
  --build-identity build/game.identity.json `
  --verify-installed `
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

Identity enforcement is the default CLI policy. `--build-identity` requires
the exact `--vpk`; the local VPK, main ELF, and every currently matched module
ELF must match the receipt. `--verify-installed` is the stronger live gate and
is required when claiming that the running Vita contains that build. The only
way to use the old name/layout-only behavior is the visibly explicit
`--allow-unverified-build` option. A missing, malformed, unknown-version, or
mismatched identity never silently enters compatibility mode.

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
  --host VITA_IP `
  --main-elf build/game.elf `
  --vpk build/game.vpk `
  --build-identity build/game.identity.json `
  --verify-installed `
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
ID or cryptographic digest. The identity receipt closes that gap for the normal
VitaDevDeploy workflow: it binds retained symbols to one exact VPK, and the
opt-in live gate compares installed executable/module bytes to that receipt.
The receipt is not a remote attestation or signature. Its authenticity depends
on retaining it with trusted build output; VitaDevDeploy separately
authenticates the signed installation job. Creating a new receipt from an
arbitrary mismatched VPK/ELF pair asserts that pair intentionally, so generate
it as part of the same build/package job rather than later by guesswork.

The parser is deliberately bounded and fail-closed: at most 1 MiB of RSP/XML,
256 modules, four segments per module, 4,096 local candidates, and 4,096 ELF
sections. Identity JSON is capped at 64 KiB and individual recorded artifacts
at 1 GiB. It rejects DTDs/entities, malformed addresses, mixed `qOffsets`
styles, allocated sections outside `PT_LOAD`, stripped files, unsafe generated
GDB section names, duplicate runtime module names, unsafe installed paths, and
reuse of one ELF for multiple binaries.

## Refresh loaded modules and ASLR state

Every successful invocation writes a hash-linked companion state beside the
script as `OUTPUT.state.json` (or at `--state-file`). On the next invocation it
validates that state and the previous script hash before opening the debugger
connection. The fresh bounded snapshot is compared with the prior one and
reports:

- `added`: modules loaded since the previous snapshot;
- `removed`: modules unloaded since the previous snapshot;
- `rebased`: same-named modules whose complete segment address list changed;
- `symbol_changes`: modules whose matched ELF or match reason changed;
- `build_changed`: a different verified receipt or main ELF.

Rerun the same command after a plug-in load/unload, target reconnect, or app
relaunch. Identity-backed explicit mappings may be temporarily absent, but
their local hashes are still checked, so an unloaded module can be matched
safely when a later refresh sees it. Unrecorded matched user modules still fail
closed. A malformed previous state, failed identity check, failed RSP snapshot,
or failed publication leaves the previous usable script/state in place.
Publication stages both files and rolls back a partial commit.

VitaDebugger accepts one GDB client at a time. Detach the current GDB session
before running a refresh; the refresh itself stops only long enough to collect
one snapshot and then sends a clean detach. This is command-driven refresh, not
background polling, so it does not repeatedly interrupt the game.

## Noninteractive and IDE task workflow

`--gdb` turns identity verification, dynamic refresh, and applying the
generated script into one command. For a bounded CI smoke test:

```powershell
py -3 tools/gdb_symbols.py `
  --host VITA_IP `
  --main-elf build/game.elf `
  --vpk build/game.vpk `
  --build-identity build/game.identity.json `
  --verify-installed `
  --search-root build `
  --output build/game-live.gdb `
  --gdb C:\vitasdk\bin\arm-vita-eabi-gdb.exe `
  --gdb-batch `
  --gdb-arg=--quiet
```

For an IDE adapter that consumes GDB/MI, replace the final two options with
`--gdb-arg=--interpreter=mi2`. Standard input/output remain attached directly
to GDB, and the launcher uses an argument vector with no command shell. Launch
arguments are deliberately allowlisted; extra command files and `-ex`/shell
overrides are rejected so they cannot bypass the generated identity-checked
script. The state JSON is stable machine-readable input for a pre-launch task
or later Debug Adapter Protocol integration.

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

### Retail 3.65 results

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

The follow-up installed-build gate also passed on retail 3.65. It verified the
installed `eboot.bin` and user SUPRX against the versioned receipt, rejected a
deliberately mismatched VPK before opening RSP or changing the last good symbol
view, and proved that a same-process refresh is idempotent. It then incorporated
the five-session main-plus-user-SUPRX lifecycle across two clean relaunches and
published a final identity-backed refresh for the new layout. The combined
record is [retail 3.65 build-identity and ASLR evidence](hardware/gdb-aslr-build-identity-3.65.json).

This completes the ASLR and verified-build correctness milestone. Stressing
same-process hot module load/unload churn and triggering the existing command-
driven refresh automatically from IDE tasks are later dynamic-module/IDE
convenience integration, not unfinished ASLR correctness.

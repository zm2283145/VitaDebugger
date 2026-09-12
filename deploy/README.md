# VitaDevDeploy

VitaDevDeploy is an experimental, signed remote deployment path for PS Vita
homebrew development. After the one-time installer setup, a developer can build
a VPK on a PC, send it to a Vita that is already at LiveArea, install or update
it, and launch it through one host command.

The project exists to make day-to-day Vita development feel closer to a devkit
workflow when using VitaSDK and retail homebrew hardware. It is not an official
Sony tool, and it does not emulate every devkit service.

> **Development status:** the host protocol has automated coverage, the Vita
> agent builds successfully, and the normal signed install-and-launch path has
> passed on retail hardware. Bootstrap recovery and interrupted-install fault
> injection are still in hardware validation. Use a disposable test title and
> verified backups until an on-device release is explicitly marked stable.

## What it contains

| Component | Runs on | Purpose |
| --- | --- | --- |
| Host CLI | Development PC | Validates a VPK, creates a canonical manifest, signs the job, transfers it, waits for a durable result, and optionally launches the target |
| Vita agent | Vita, normally title ID `VDEVDEP01` | Publishes a fresh challenge, verifies the signed package tree, optionally installs it when that capability was compiled in, records the result, and exits |
| Disposable test target | Vita, title ID `VDDT00001` | Harmless safe-user-mode package used to prove installation and launch without replacing another development app |
| Vita Companion | Vita | Provides the FTP and title-launch transports used by normal deployment; the advanced bootstrap helper also uses exact-title lifecycle commands |
| Bootstrap helper | Development PC | Optional advanced recovery path for temporarily substituting a disposable test application's eboot when the active FTP service permits app-directory writes |

Verification-only variants omit all promoter code and libraries and are built
as safe fSELF applications. Install-enabled variants are unsafe **user-mode**
homebrew applications using VitaSDK's normal `UNSAFE`
`0x2F00000000000001` authority profile, not kernel plugins. Neither mode
requires a new project-specific kernel plugin or kuBridge. A controlled A/B
test on the target retail Vita showed that an otherwise identical
`0x2808000000000000` build was rejected before `main()` while the normal
VitaSDK profile launched, so the explicit `0x2808` profile is deliberately not
used.

## Safety and trust model

Only the Ed25519 public key is compiled into the Vita agent. Every job is signed
on the PC with the corresponding private key. The agent also checks:

- a fresh, one-use Vita nonce to prevent replay;
- the exact request and manifest signature;
- every file path, size, and SHA-256 hash;
- the file count and total byte count;
- the VPK's `sce_sys/param.sfo` title ID;
- fixed per-file, path, and total-size limits;
- a complete upload committed by renaming `request.v1` last; and
- that the target is not the running deployer itself.

These checks protect the install decision. They do **not** secure Vita
Companion itself.

> **Network warning:** Vita Companion 1.06 exposes unauthenticated FTP and
> command ports. Use it only on a trusted private LAN. Never forward ports 1337
> or 1338, and do not use this workflow on public or shared Wi-Fi. A signed job
> cannot stop another client on the LAN from abusing Companion's other commands
> or attempting to race files while installation is in progress.

The private key authorizes software installation on every agent built with its
public key. The current host stores it as an unencrypted PEM, so filesystem
access control matters. Keep it in `local/` or another access-controlled
directory, never copy it to the Vita, never pass it to the build script, and
never commit it. The repository ignores common private-key names, but
`.gitignore` is not a security boundary.

See [SECURITY.md](SECURITY.md) for the full security boundary and
[docs/protocol-v1.md](docs/protocol-v1.md) for the wire format.

## Requirements

### Vita

- A homebrew-enabled PS Vita. Unsafe homebrew must be enabled before running an
  install-enabled variant; verification-only variants use safe fSELF.
- Vita Companion 1.06 installed and active.
- The Vita and PC connected to the same trusted private network.
- For the initial bootstrap only, the disposable `SLRS00001` test application
  and a trusted copy of its exact original `eboot.bin`.

The bootstrap helper is deliberately pinned to `SLRS00001`. It cannot be
redirected to another title from the command line.

### Development PC

- Windows PowerShell 5.1 or PowerShell 7.
- Python 3.10 or newer.
- VitaSDK, with `VITASDK` set or installed at `C:\vitasdk`.
- CMake 3.16 or newer.
- MSYS2 `make.exe` and the MinGW64 runtime that accompanies the compiler.
- One Ed25519 implementation:
  - Python's `cryptography` package; or
  - an Ed25519-capable OpenSSL executable.

The build helpers check the selected paths, VitaSDK's two core CMake files, and
the MinGW64 DLLs needed when `cc1.exe` starts (`libgcc_s_seh-1.dll`,
`libgmp-10.dll`, `libisl-23.dll`, and `libmpc-3.dll`). They temporarily prepend
the detected MinGW64 runtime, the selected Make directory, and VitaSDK's `bin`
directory to `PATH`, then restore the caller's original `PATH` even if a build
fails. This avoids relying on a globally modified Windows environment.

By default, the scripts locate `mingw64\bin` from the selected MSYS2 Make path,
then fall back to `C:\msys64\mingw64\bin`. Use `-CMakePath`, `-MakePath`,
`-Msys2RuntimePath`, or `-VitaSdkPath` when the tools are installed somewhere
nonstandard. `-Msys2RuntimePath` must name the MinGW64 `bin` directory itself,
not the MSYS2 root or its `usr\bin` directory. CMake and VitaSDK perform the
remaining compiler and packaging checks during configuration and compilation.

Production artifacts are headless by default and do not compile or link a
display helper. Passing `-EnableExperimentalDisplayUi` to the build helper adds
the direct-framebuffer status interface from VitaSDK's
`samples/common/debugScreen.c`. Only that opt-in configuration requires the
installed helper and its header, and CMake checks their expected SHA-256 hashes
before compiling them. Their upstream and license notices are recorded in
[THIRD_PARTY.md](THIRD_PARTY.md).

## Verify the checkout

From `deploy/` (the deployment subproject root):

```powershell
$env:PYTHONPATH = (Resolve-Path -LiteralPath ".").Path
py -3 -m unittest discover -s tests -t . -p "test_*.py" -v
py -3 -m host.vitadevdeploy --help
```

The tests use local fakes; they do not connect to or change a Vita.

## Create the signing keys

Generate the key pair once:

```powershell
py -3 -m host.vitadevdeploy keygen --private .\local\deploy_private.pem --public .\local\deploy_public.pem --raw-public .\local\deploy_public.raw
```

If automatic crypto selection cannot find a suitable implementation, either
install `cryptography` or select OpenSSL explicitly:

```powershell
py -3 -m host.vitadevdeploy keygen --private .\local\deploy_private.pem --public .\local\deploy_public.pem --raw-public .\local\deploy_public.raw --crypto-backend openssl --openssl "C:\path\to\openssl.exe"
```

Do not use `--force` unless deliberately rotating an existing key. Losing the
private key means rebuilding and reinstalling `VDEVDEP01` with a new public key.

The PEM is currently unencrypted. On Windows, replace inherited permissions
with one explicit rule for the current account, then verify that rule:

```powershell
$PrivateKeyPath = (Resolve-Path -LiteralPath ".\local\deploy_private.pem").Path
$CurrentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
$KeyAcl = [Security.AccessControl.FileSecurity]::new()
$KeyAcl.SetAccessRuleProtection($true, $false)
$KeyAcl.SetOwner($CurrentSid)
$KeyRule = [Security.AccessControl.FileSystemAccessRule]::new(
    $CurrentSid,
    [Security.AccessControl.FileSystemRights]::FullControl,
    [Security.AccessControl.AccessControlType]::Allow
)
$KeyAcl.AddAccessRule($KeyRule)
Set-Acl -LiteralPath $PrivateKeyPath -AclObject $KeyAcl

$SavedAcl = Get-Acl -LiteralPath $PrivateKeyPath
$SavedRules = @($SavedAcl.Access)
if (
    -not $SavedAcl.AreAccessRulesProtected -or
    $SavedRules.Count -ne 1 -or
    $SavedAcl.GetOwner([Security.Principal.SecurityIdentifier]).Value -ne $CurrentSid.Value -or
    $SavedRules[0].IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -ne $CurrentSid.Value -or
    $SavedRules[0].AccessControlType -ne [Security.AccessControl.AccessControlType]::Allow -or
    ($SavedRules[0].FileSystemRights -band [Security.AccessControl.FileSystemRights]::FullControl) -ne
        [Security.AccessControl.FileSystemRights]::FullControl
) {
    throw "Private-key ACL verification failed."
}
```

This limits ordinary access by other Windows accounts. It does not protect the
key from an administrator or malware already running as the current user; use
an encrypted drive and normal endpoint protections as appropriate.

If the PEM key pair already exists and only the raw public-key file is missing,
export it without touching the private key:

```powershell
py -3 -m host.vitadevdeploy export-public --public .\local\deploy_public.pem --raw .\local\deploy_public.raw
```

Convert only the 32-byte raw **public** key to the hex value accepted by the
agent build:

```powershell
$PublicKeyBytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath ".\local\deploy_public.raw"))
if ($PublicKeyBytes.Length -ne 32) { throw "Expected a 32-byte Ed25519 public key." }
$PublicKeyHex = ([BitConverter]::ToString($PublicKeyBytes)).Replace("-", "").ToLowerInvariant()
```

The build helper has no private-key parameter. It embeds only `$PublicKeyHex`.

## Build the Vita agent

Build all four intentionally separate variants:

```powershell
.\tools\build_agent.ps1 -PublicKeyHex $PublicKeyHex
```

Or build one:

```powershell
.\tools\build_agent.ps1 -PublicKeyHex $PublicKeyHex -Variant InstallerVerify
.\tools\build_agent.ps1 -PublicKeyHex $PublicKeyHex -Variant VerifyBootstrap
.\tools\build_agent.ps1 -PublicKeyHex $PublicKeyHex -Variant InstallBootstrap
.\tools\build_agent.ps1 -PublicKeyHex $PublicKeyHex -Variant PermanentInstaller
```

For a nonstandard MSYS2 layout, point both build helpers at its runtime, for
example:

```powershell
.\tools\build_agent.ps1 -PublicKeyHex $PublicKeyHex -Msys2RuntimePath "D:\msys64\mingw64\bin"
.\tools\build_disposable_target.ps1 -Msys2RuntimePath "D:\msys64\mingw64\bin"
```

These commands produce the lifecycle-safe headless variants. For a supervised
display test only, opt in explicitly:

```powershell
.\tools\build_agent.ps1 -PublicKeyHex $PublicKeyHex -Variant InstallerVerify -EnableExperimentalDisplayUi
```

The experimental interface installs a process-owned CDRAM framebuffer
directly. Close that build only by pressing Circle while it is still waiting
for a job, or let its one-shot operation finish and exit normally. Never use
Vita Companion's `destroy` command or another force-kill on a display-enabled
build; abrupt termination can bypass framebuffer cleanup and wedge LiveArea.

The outputs are:

| Output directory | Agent identity | fSELF/auth ID | Install permission | Allowed target |
| --- | --- | --- | --- | --- |
| `dist/installer-verify/` | `VDEVDEP01` | Safe / `0x2F00000000000002` | Disabled | Any valid title except `VDEVDEP01` |
| `dist/verify-bootstrap/` | `SLRS00001` | Safe / `0x2F00000000000002` | Disabled | `VDEVDEP01` only |
| `dist/install-bootstrap/` | `SLRS00001` | VitaSDK `UNSAFE` / `0x2F00000000000001` | Enabled | `VDEVDEP01` only |
| `dist/permanent-installer/` | `VDEVDEP01` | VitaSDK `UNSAFE` / `0x2F00000000000001` | Enabled | Any valid title except `VDEVDEP01` |

Each directory contains `eboot.bin`, `VitaDevDeployAgent.vpk`,
`SHA256SUMS.txt`, and `BUILD-INFO.txt`.

`BUILD-INFO.txt` records whether `display_ui` is the default
`disabled (headless lifecycle-safe build)` or the explicitly enabled
experimental framebuffer interface.

> **Do not install either bootstrap VPK.** A bootstrap artifact uses the
> existing test application's title ID. Only its `eboot.bin` is temporarily
> activated through the hash-checking bootstrap helper. The permanent
> installable package is
> `dist/permanent-installer/VitaDevDeployAgent.vpk`.

The build directories and `dist/` are ignored by Git. The helper serializes
builds from one checkout, stages and verifies a complete artifact set, and then
publishes the directory by same-volume rename. CMake caches contain the public
key, which is expected; they never receive the private key.

## Configure a deployment session

Run the remaining hardware commands from one PowerShell session:

```powershell
$VitaIp = "10.1.1.93"
$PrivateKey = (Resolve-Path -LiteralPath ".\local\deploy_private.pem").Path

function Invoke-PythonChecked {
    & py -3 @args
    if ($LASTEXITCODE -ne 0) {
        throw "Python command failed with exit code $LASTEXITCODE."
    }
}
```

## Recommended first hardware test

Before giving any build installation permission, manually install
`dist/installer-verify/VitaDevDeployAgent.vpk` with VitaShell. This is the
recommended first test app: it has the permanent `VDEVDEP01` identity, contains
no package promoter implementation or promoter libraries, and was compiled
with installation disabled.

VitaShell 2.02 can show a false extended-permissions warning for a modern safe
fSELF. Its release scanner can pass a decompressed ARM segment to an ELF parser,
then treat the resulting parse error as an unsafe result. Before accepting that
warning, compare the VPK against `dist/installer-verify/SHA256SUMS.txt`. The
verification artifact itself has the safe `0x2F00000000000002` auth ID and no
`ScePromoterUtil`, `SceShellSvc`, or dangerous `SceVshBridge` imports. A newer
VitaShell build with the corrected scanner avoids this false positive.

Build the included disposable target. It is an ordinary safe fSELF with the
dedicated title ID `VDDT00001`; it has no network, display, AppMgr, promoter,
kernel-plugin, or unsafe-authority code. Each successful launch increments a
file-synced, fully re-read marker at
`ux0:data/VitaDevDeploy/disposable-target.last-run` and then returns normally.
On firmware that rejects directory-descriptor sync, this confirms the complete
file but does not prove rename-namespace durability across sudden power loss.
See [disposable-target/README.md](disposable-target/README.md) for its complete
safety boundary. Before its first install, confirm that `VDDT00001` is not
already present on the particular Vita.

```powershell
.\tools\build_disposable_target.ps1
$TestVpk = (Resolve-Path -LiteralPath ".\dist\disposable-test\VitaDevDeployDisposableTest.vpk").Path
Invoke-PythonChecked -m host.vitadevdeploy verify $TestVpk
```

With Vita Companion active and the Vita at LiveArea:

```powershell
Invoke-PythonChecked -m host.vitadevdeploy deploy $TestVpk --vita $VitaIp --private-key $PrivateKey --verify-only
```

This exercises Companion launch, the fresh challenge, signing, FTP commit,
signature checking, and exact package-tree verification. It cannot call the
package promoter. Continue to the install-enabled bootstrap only after this
test succeeds.

After the verification tests pass, install
`dist/permanent-installer/VitaDevDeployAgent.vpk` once with VitaShell. That
variant genuinely requests extended user-mode permissions because it contains
the package promoter. It uses VitaSDK's normal `UNSAFE`
`0x2F00000000000001` user-mode profile, matching VitaDB-Downloader's proven
PromoterUtil/internal-PAF approach. It is still a user application rather than
a kernel plugin, but it is highly privileged. The explicit VitaShell/SceShell
`0x2808000000000000` profile was rejected before `main()` in controlled A/B
testing on the target retail Vita and is not used. Verify the VPK hash first
and accept the warning only for the artifact you built. After this one-time
replacement, normal VPK updates use the remote deployment command below.

## Optional bootstrap installation of VDEVDEP01

The VitaShell flow above is the recommended setup. This section is an advanced
alternative for a disposable `SLRS00001` test installation. Read the whole
section first, keep Vita Companion active, close the test application, and have
the restore command ready before activating a bootstrap.

Use only the default headless bootstrap artifacts in this recovery workflow.
The helper may stop the exact temporary title while restoring its original
eboot, which is incompatible with the experimental direct-framebuffer build's
normal-cleanup requirement.

The stock Vita Companion 1.06 service running in the system shell may allow
reads from `ux0:app` while refusing creation of the temporary files needed for
an atomic eboot swap. In that configuration the helper fails closed before
changing the live eboot; use the one-time VitaShell installation instead. Do
not weaken the helper to overwrite the live file in place.

### 1. Establish the trusted original

Start from a trusted PC copy of the exact original `SLRS00001` eboot. Do not
choose an expected hash by simply trusting a new network download.

```powershell
$OriginalEboot = "C:\path\to\trusted\SLRS00001-eboot.bin"
$OriginalHash = (Get-FileHash -LiteralPath $OriginalEboot -Algorithm SHA256).Hash.ToLowerInvariant()
```

Create and independently verify both the PC and Vita backups:

```powershell
Invoke-PythonChecked .\tools\bootstrap_eboot.py backup --vita-ip $VitaIp --title SLRS00001 --expected-sha256 $OriginalHash
Invoke-PythonChecked .\tools\bootstrap_eboot.py status --vita-ip $VitaIp --title SLRS00001 --expected-sha256 $OriginalHash
```

Continue only when status reports verified matching backups and
`ready_to_activate: true`.

### 2. Optional: prove the bootstrap identity with installation disabled

The recommended `installer-verify` test above already validates the signed
verification path. This optional step additionally tests the temporary
`SLRS00001` identity and its `VDEVDEP01`-only allowlist before enabling
promotion:

```powershell
$VerifyEboot = (Resolve-Path -LiteralPath ".\dist\verify-bootstrap\eboot.bin").Path
$VerifyHash = (Get-FileHash -LiteralPath $VerifyEboot -Algorithm SHA256).Hash.ToLowerInvariant()
try {
    Invoke-PythonChecked .\tools\bootstrap_eboot.py activate --vita-ip $VitaIp --title SLRS00001 --expected-sha256 $OriginalHash --bootstrap $VerifyEboot --bootstrap-sha256 $VerifyHash
    Invoke-PythonChecked -m host.vitadevdeploy deploy .\dist\permanent-installer\VitaDevDeployAgent.vpk --vita $VitaIp --private-key $PrivateKey --installer-title SLRS00001 --verify-only
} finally {
    try {
        Invoke-PythonChecked .\tools\bootstrap_eboot.py restore --vita-ip $VitaIp --title SLRS00001 --expected-sha256 $OriginalHash
    } finally {
        Invoke-PythonChecked .\tools\bootstrap_eboot.py status --vita-ip $VitaIp --title SLRS00001 --expected-sha256 $OriginalHash
    }
}
```

The verification-only run must report success without installing the package.
Run the entire block as one unit rather than selecting individual lines. Its
`finally` clauses attempt restore and status even when activation or
verification fails. A process termination or PC power loss still requires the
manual recovery procedure below.

### 3. Install the permanent agent

The bootstrap helper intentionally refuses to switch directly from one
bootstrap to another. Confirm that `SLRS00001` is restored first, then activate
the install-enabled bootstrap:

```powershell
$InstallEboot = (Resolve-Path -LiteralPath ".\dist\install-bootstrap\eboot.bin").Path
$InstallHash = (Get-FileHash -LiteralPath $InstallEboot -Algorithm SHA256).Hash.ToLowerInvariant()
try {
    Invoke-PythonChecked .\tools\bootstrap_eboot.py activate --vita-ip $VitaIp --title SLRS00001 --expected-sha256 $OriginalHash --bootstrap $InstallEboot --bootstrap-sha256 $InstallHash
    Invoke-PythonChecked -m host.vitadevdeploy deploy .\dist\permanent-installer\VitaDevDeployAgent.vpk --vita $VitaIp --private-key $PrivateKey --installer-title SLRS00001 --action install
} finally {
    try {
        Invoke-PythonChecked .\tools\bootstrap_eboot.py restore --vita-ip $VitaIp --title SLRS00001 --expected-sha256 $OriginalHash
    } finally {
        Invoke-PythonChecked .\tools\bootstrap_eboot.py status --vita-ip $VitaIp --title SLRS00001 --expected-sha256 $OriginalHash
    }
}
```

Do not consider setup complete until the final status reports
`live_state: original`. The bootstrap helper uploads through a fixed `.part`
path, verifies every copy by size and SHA-256, and attempts to recover and
relaunch the exact original if activation cannot commit.

## How installation is performed

The install-enabled agent keeps VitaDevDeploy's signed-job checks around the
user-mode PAF/PromoterUtil sequence exercised by VitaDB-Downloader. The normal
signed install-and-launch path has passed an end-to-end retail-hardware test;
interrupted-install and recovery behavior still require broader fault-injection
coverage. Its metadata barriers fail closed rather than dispatching an
installation when firmware rejects a required sync. The agent verifies the
expanded package tree and validates its title ID. Before moving the package it
atomically writes `ux0:data/VitaDevDeploy/promote.state`, an ownership and
recovery marker containing the job ID, target title, manifest digest, and fixed
staging path. It then moves only that verified package tree by same-volume
rename to the shallow path `ux0:/data/vdd_pkg`. The request, signature,
manifest, and journal remain under the job's inbox directory. The shallow path
avoids failures observed when PromoterUtil receives a deeply nested package
directory.

The agent then loads internal PAF and PromoterUtil, initializes the service,
re-hashes every signed byte at the shallow path, and immediately dispatches
`scePromoterUtilityPromotePkg`. It polls `scePromoterUtilityGetState` and reads
the terminal operation result with `scePromoterUtilityGetResult`.
VitaDevDeploy requires a successful terminal result, keeps the Vita awake while
polling, and unloads the services in reverse order only after the outcome is
known. This adapter is based on VitaDB-Downloader at the pinned revision
recorded in [THIRD_PARTY.md](THIRD_PARTY.md), with stricter error, result,
cleanup, and journal handling added here.

The shallow stage and `promote.state` marker are intentionally fail-closed. The
agent will not delete or overwrite either one when it reaches the staging step.
If a failure occurs before promotion is dispatched, the agent attempts to move
the intact verified tree back into its job directory. It clears the marker only
after that restoration and a durable failure result both succeed. After
dispatch, the agent leaves the stage and marker untouched on failure because
the system installer may already have consumed some or all of the package. A
successful run clears the marker only after its durable success result commits.

Once PromoterUtil accepts a package, state or result-query errors are retried
without a Vita-side deadline, and the agent keeps the Vita awake. An
experimental display-enabled build reports that installer status is
temporarily unavailable and continues showing elapsed time. The agent
deliberately does not unload the service or claim failure while an asynchronous
installation may still be running. If the process or Vita is interrupted
during that wait, the marker remains and a later run reports a stale-stage
error. Do not retry automatically; inspect the result, journal, installed
title, marker, and staging path before manual recovery.

## Normal remote deployment

Once `VDEVDEP01` is installed, the regular edit/build/test loop is one host
command:

```powershell
py -3 -m host.vitadevdeploy deploy "C:\path\to\MyHomebrew.vpk" --vita $VitaIp --private-key $PrivateKey --action install_launch
```

Begin a normal deployment with the Vita at LiveArea. The host asks Vita
Companion to launch `VDEVDEP01` without forcibly closing any application, waits
for its new challenge, uploads the package, waits for a durable success result,
and only then launches the installed title. The normal host path never sends
Companion's `destroy` command. If another application is still open, the Vita
can display its "close the existing application" confirmation instead of
entering the deployer. VitaDevDeploy does not accept that prompt or force the
application closed; return to LiveArea, close the application normally, and
rerun the command.

The Vita-side agent does not race the host by launching the target itself.
After success, the host waits four seconds—longer than the agent's normal
two-second success grace period—so the agent can clean up and exit naturally,
then asks Companion to launch the installed title without a forced close. The
default host result wait is 2,100 seconds (35 minutes) to accommodate large
Vita installations; use `--result-timeout` to change it. A host timeout only
stops waiting on the PC. It does not cancel PromoterUtil and must not be treated
as proof that installation failed.

If the agent is already running and is known to still be in its idle wait,
reuse that exact session without closing or relaunching it:

```powershell
py -3 -m host.vitadevdeploy deploy "C:\path\to\MyHomebrew.vpk" --vita $VitaIp --private-key $PrivateKey --verify-only --reuse-running-agent
```

This explicit mode reads the waiting agent's current one-time challenge and
does not contact Vita Companion's command port before uploading the job. The
host cannot prove process liveness from a challenge file alone, so use the flag
only when that running session is independently known to be current. An
experimental display build makes the idle state visible; for a headless build,
reserve this option for a run you deliberately launched and know has not yet
accepted a job. A challenge left by a stopped or crashed process is stale and
will eventually produce a result timeout; do not retry the same session. Each
agent run still accepts at most one job. With `install_launch`, Companion is
contacted only after a durable install success to start the installed title;
use `install` or `verify` when no post-result launch is wanted.

The production build is headless; follow progress and the final result from the
host. With `-EnableExperimentalDisplayUi`, the Vita instead shows a lightweight
text interface with a stage-based progress bar and explanatory detail. During
PromoterUtil work it displays the installer state and elapsed time. A completed
job identifies the target title; a failed job displays its stage, decimal and
hexadecimal error code, and the same concise reason written to the host result
and crash journal. The bar represents workflow stages and ongoing activity,
not an exact byte-level installation percentage.

Circle requests a clean exit only while the agent is waiting for its first
committed job. It is not an install cancel button and is not checked after job
processing begins. Once a job has been committed, let verification or
installation finish and allow the one-shot agent to exit normally.

Useful lower-risk commands:

```powershell
# Inspect a VPK entirely offline.
py -3 -m host.vitadevdeploy verify "C:\path\to\MyHomebrew.vpk"

# Prepare and sign everything locally without network access.
py -3 -m host.vitadevdeploy deploy "C:\path\to\MyHomebrew.vpk" --vita $VitaIp --private-key $PrivateKey --dry-run --output .\local\dryrun

# Exercise the Vita verifier without promoting or launching the target.
py -3 -m host.vitadevdeploy deploy "C:\path\to\MyHomebrew.vpk" --vita $VitaIp --private-key $PrivateKey --verify-only

# Install successfully but leave the target stopped.
py -3 -m host.vitadevdeploy deploy "C:\path\to\MyHomebrew.vpk" --vita $VitaIp --private-key $PrivateKey --action install
```

Add `--crypto-backend openssl --openssl "C:\path\to\openssl.exe"` to signing
commands if OpenSSL must be selected explicitly. The default Vita Companion
ports are FTP 1337 and command 1338; alternate ports can be supplied to the
`deploy` command.

## Bootstrap recovery

If the PC, network, or installer fails while a bootstrap is active:

1. Stop issuing deploy commands.
2. Make Vita Companion reachable again.
3. Run `bootstrap_eboot.py status` with the same trusted original hash.
4. Run `bootstrap_eboot.py restore`.
5. Confirm `live_state: original` before launching the original test app or
   attempting another bootstrap.

A `SAFETY REFUSAL` is intentional. Do not bypass it or replace files manually;
check which expected hash or backup does not match. Restore requires the
independently matching PC and Vita backups. Keep the PC backup somewhere safe
after the first installation.

## Current limitations

- Vita Companion transport has no authentication or encryption.
- Version 1 transfers the expanded package file by file, so large VPKs may take
  longer than a single-container protocol.
- The Vita agent is one-shot: one launch creates one challenge and consumes at
  most one committed job.
- Jobs are serialized; concurrent deployments are not supported.
- Normal host deployment starts from LiveArea and never force-closes the
  current application. `--reuse-running-agent` is explicit because a challenge
  file alone cannot prove that its process is still alive.
- The optional direct-framebuffer status interface is experimental and cannot
  be safely force-killed; production artifacts therefore default to headless.
- Installation uses the single fixed shallow stage `ux0:/data/vdd_pkg`. A
  pre-existing stage or `ux0:data/VitaDevDeploy/promote.state` marker blocks the
  next install instead of being overwritten, and anything left after
  PromoterUtil dispatch requires manual outcome review.
- Existing apps are updated in place. There is no general target-app rollback,
  so keep known-good VPKs.
- The permanent deployer refuses to update itself. Rotate or replace it through
  a separately reviewed maintenance/bootstrap procedure.
- This installs Vita homebrew VPKs. It is not a firmware installer, a license
  bypass, or a tool for official encrypted applications.
- An active attacker already on the trusted LAN is outside the version 1 threat
  model because Companion's FTP and command services remain unauthenticated.
- Full on-device fault-injection and interrupted-install testing is still
  required before calling the workflow production-ready.

## Repository layout

```text
agent/                 VitaSDK user-mode deployment agent
host/vitadevdeploy/    Python validation, signing, transfer, and launch client
tools/build_agent.ps1  Repeatable four-variant Vita agent build
tools/bootstrap_eboot.py
                       Pinned, hash-checked SLRS00001 bootstrap/recovery helper
docs/protocol-v1.md    Canonical on-device protocol
tests/                 Host and bootstrap safety tests
third_party/           Audited third-party installation template material
```

The on-device working root is fixed at `ux0:data/VitaDevDeploy`. Challenges,
inbox jobs, journals, results, and the `promote.state` recovery marker live
beneath that directory. During an install, only the verified package tree is
renamed temporarily to the separate shallow path `ux0:/data/vdd_pkg`.
Pre-job startup failures can be decoded from the fixed binary
`VitaDevDeploy.startup` and `VitaDevDeploy.startup_io` records as described in
`docs/startup-diagnostics.md`; the latter identifies the exact challenge-file
or parent-directory I/O substep without changing the enforced sync policy.

## License and third-party code

VitaDevDeploy is distributed under GPL-3.0-only. The project adapts parts of
VitaShell's package preparation, VitaDB-Downloader's package-promotion flow,
and it vendors Monocypher for Ed25519 verification. Experimental display builds
also compile VitaSDK's debug-screen sample. Exact provenance and licenses are
recorded in [THIRD_PARTY.md](THIRD_PARTY.md).

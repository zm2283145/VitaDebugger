[CmdletBinding()]
param(
    [string] $VitaSdkPath = $env:VITASDK,
    [string] $CMakePath,
    [string] $MakePath,
    [string] $Msys2RuntimePath = "C:\msys64\mingw64\bin",
    [ValidateRange(18000, 18999)]
    [int] $Port = 18195,
    [ValidatePattern("^[0-9a-f]{8}$")]
    [string] $BindIpv4Hex = "0a0101d9",
    [ValidatePattern("^[0-9a-f]{8}$")]
    [string] $PeerNetworkHex = "0a010100",
    [ValidatePattern("^[0-9a-f]{8}$")]
    [string] $PeerNetmaskHex = "ffffff00",
    [ValidateRange(100, 30000)]
    [int] $HandshakeDeadlineMs = 3000,
    [ValidateRange(1, 256)]
    [int] $Jobs = [Math]::Max(1, [Environment]::ProcessorCount)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-Executable {
    param(
        [string] $Requested,
        [string] $Name,
        [string[]] $Fallbacks
    )
    if (-not [string]::IsNullOrWhiteSpace($Requested)) {
        if (-not (Test-Path -LiteralPath $Requested -PathType Leaf)) {
            throw "$Name was not found at '$Requested'."
        }
        return (Resolve-Path -LiteralPath $Requested).Path
    }
    foreach ($candidate in $Fallbacks) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $command) {
        throw "$Name was not found."
    }
    return $command.Source
}

function Get-Sha256Lower {
    param([string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-DirectChild {
    param([string] $Path, [string] $Parent)
    $itemParent = [IO.Directory]::GetParent([IO.Path]::GetFullPath($Path))
    if ($null -eq $itemParent -or
        -not [string]::Equals(
            $itemParent.FullName.TrimEnd("\", "/"),
            [IO.Path]::GetFullPath($Parent).TrimEnd("\", "/"),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to manage non-child path '$Path'."
    }
}

$attachRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $attachRoot "..")).Path
$sourceDirectory = Join-Path $attachRoot "vita-auth-test"
$buildDirectory = Join-Path $sourceDirectory "build-vdat"
$distRoot = Join-Path $attachRoot "dist"
$outputDirectory = Join-Path $distRoot "auth-gate"

if ([string]::IsNullOrWhiteSpace($VitaSdkPath)) {
    $VitaSdkPath = "C:\vitasdk"
}
if (-not (Test-Path -LiteralPath $VitaSdkPath -PathType Container)) {
    throw "VitaSDK was not found at '$VitaSdkPath'."
}
$VitaSdkPath = (Resolve-Path -LiteralPath $VitaSdkPath).Path
$cmake = Resolve-Executable -Requested $CMakePath -Name "cmake" -Fallbacks @(
    "C:\vitasdk-tools\cmake-4.4.3-windows-x86_64\bin\cmake.exe",
    "C:\Program Files\CMake\bin\cmake.exe"
)
$make = Resolve-Executable -Requested $MakePath -Name "make" -Fallbacks @(
    "C:\msys64\usr\bin\make.exe"
)
if (-not (Test-Path -LiteralPath $Msys2RuntimePath -PathType Container)) {
    throw "MSYS2 runtime was not found at '$Msys2RuntimePath'."
}

$oldPath = $env:PATH
$oldVitaSdk = $env:VITASDK
$oldPythonPath = $env:PYTHONPATH
try {
    $env:VITASDK = $VitaSdkPath
    $env:PYTHONPATH = Join-Path $repoRoot "deploy"
    $env:PATH = "$Msys2RuntimePath;$([IO.Path]::GetDirectoryName($make));$(Join-Path $VitaSdkPath 'bin');$oldPath"
    $makeForCmake = $make.Replace("\", "/")
    $arguments = @(
        "-UVD_ATTACH_AUTH_ASSURANCE_SOURCE",
        "-S", $sourceDirectory,
        "-B", $buildDirectory,
        "-G", "MSYS Makefiles",
        "-DCMAKE_MAKE_PROGRAM=$makeForCmake",
        "-DCMAKE_TOOLCHAIN_FILE=$($VitaSdkPath.Replace('\', '/'))/share/vita.toolchain.cmake",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DVD_ATTACH_AUTH_LISTENER_PORT=$Port",
        "-DVD_ATTACH_AUTH_BIND_IPV4_HEX=$BindIpv4Hex",
        "-DVD_ATTACH_AUTH_PEER_NETWORK_HEX=$PeerNetworkHex",
        "-DVD_ATTACH_AUTH_PEER_NETMASK_HEX=$PeerNetmaskHex",
        "-DVD_ATTACH_AUTH_HANDSHAKE_MS=$HandshakeDeadlineMs"
    )
    & $cmake @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Authentication gate configuration failed."
    }
    & $cmake --build $buildDirectory --parallel $Jobs
    if ($LASTEXITCODE -ne 0) {
        throw "Authentication gate build failed."
    }

    $eboot = Join-Path $buildDirectory "eboot.bin"
    $vpk = Join-Path $buildDirectory "VitaDebuggerAuthGate.vpk"
    foreach ($artifact in @($eboot, $vpk)) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or
            (Get-Item -LiteralPath $artifact).Length -le 0) {
            throw "Build did not produce '$artifact'."
        }
    }

    $elf = Join-Path $buildDirectory "vitadebug_attach_auth_gate"
    $readelf = Join-Path $VitaSdkPath "bin\arm-vita-eabi-readelf.exe"
    $nm = Join-Path $VitaSdkPath "bin\arm-vita-eabi-nm.exe"
    $sections = (& $readelf -SW $elf) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "Could not inspect sentinel ELF sections."
    }
    $symbols = (& $nm $elf) -join "`n"
    if ($LASTEXITCODE -ne 0 -or
        $sections -match "SceNet|SceNetCtl|SceSysmodule" -or
        $symbols -match
            "sce(Net|NetCtl|Sysmodule|AppMgr|Kernel(Create|Start)Thread|KernelGetRandomNumber)|vd_attach_auth_(listener|store)") {
        throw "Sentinel unexpectedly contains networking or authentication runtime code."
    }

    $ebootBytes = [IO.File]::ReadAllBytes($eboot)
    if ($ebootBytes.Length -lt 0x88 -or
        [BitConverter]::ToUInt64($ebootBytes, 0x80) -ne
            [Convert]::ToUInt64("2F00000000000002", 16)) {
        throw "Authentication gate is not an ordinary safe user-mode fSELF."
    }
    $inspection = (
        & py -3 -m host.vitadevdeploy verify $vpk |
            ConvertFrom-Json
    )
    if ($LASTEXITCODE -ne 0 -or $inspection.title_id -ne "VDAT00001" -or
        $inspection.file_count -ne 2) {
        throw "Authentication gate VPK identity or contents are unexpected."
    }

    New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
    $stage = Join-Path $distRoot (".stage-auth-gate-" + [guid]::NewGuid().ToString("N"))
    Assert-DirectChild -Path $stage -Parent $distRoot
    New-Item -ItemType Directory -Path $stage | Out-Null
    try {
        Copy-Item -LiteralPath $eboot -Destination (Join-Path $stage "eboot.bin")
        Copy-Item -LiteralPath $vpk -Destination (Join-Path $stage "VitaDebuggerAuthGate.vpk")
        $sourceFiles = @(
            "attach/vita-auth-test/CMakeLists.txt",
            "attach/vita-auth-test/src/main.c",
            "attach/broker/include/vitadebug_attach_auth.h",
            "attach/broker/include/vitadebug_attach_auth_listener.h",
            "attach/broker/include/vitadebug_attach_auth_listener_vita.h",
            "attach/broker/include/vitadebug_attach_auth_store.h",
            "attach/broker/src/vitadebug_attach_auth.c",
            "attach/broker/src/vitadebug_attach_auth_listener.c",
            "attach/broker/src/vitadebug_attach_auth_listener_vita.c",
            "attach/broker/src/vitadebug_attach_auth_store.c",
            "attach/broker/src/vitadebug_attach_auth_store_vita.c",
            "deploy/agent/third_party/monocypher/monocypher.c",
            "deploy/agent/third_party/monocypher/monocypher-ed25519.c"
        )
        $sourceHashes = [ordered]@{}
        foreach ($relative in $sourceFiles) {
            $sourceHashes[$relative] = Get-Sha256Lower (Join-Path $repoRoot $relative)
        }
        $compilerVersion = (
            & (Join-Path $VitaSdkPath "bin\arm-vita-eabi-gcc.exe") --version |
                Select-Object -First 1
        )
        $manifest = [ordered]@{
            schema = 1
            source_commit = (& git -C $repoRoot rev-parse HEAD).Trim()
            vita_sdk = $compilerVersion
            title_id = "VDAT00001"
            protocol = 2
            scope = "sentinel-only"
            transport = "signed-plaintext"
            encryption = "none"
            network = "not-started"
            future_listener_endpoint = [ordered]@{
                bind_ipv4_hex = $BindIpv4Hex
                peer_network_hex = $PeerNetworkHex
                peer_netmask_hex = $PeerNetmaskHex
                port = $Port
                handshake_deadline_ms = $HandshakeDeadlineMs
            }
            hardware_assurance = "retail-3.65-hard-block"
            artifacts = [ordered]@{
                "eboot.bin" = Get-Sha256Lower (Join-Path $stage "eboot.bin")
                "VitaDebuggerAuthGate.vpk" = Get-Sha256Lower (Join-Path $stage "VitaDebuggerAuthGate.vpk")
            }
            secure_storage_backend = $null
            sources = $sourceHashes
        }
        $manifest | ConvertTo-Json -Depth 6 |
            Set-Content -LiteralPath (Join-Path $stage "manifest.json") -Encoding ascii

        Assert-DirectChild -Path $outputDirectory -Parent $distRoot
        if (Test-Path -LiteralPath $outputDirectory) {
            $item = Get-Item -LiteralPath $outputDirectory -Force
            if (-not $item.PSIsContainer -or
                ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing to replace unexpected '$outputDirectory'."
            }
            Remove-Item -LiteralPath $outputDirectory -Recurse -Force
        }
        [IO.Directory]::Move($stage, $outputDirectory)
    } finally {
        if (Test-Path -LiteralPath $stage -PathType Container) {
            Assert-DirectChild -Path $stage -Parent $distRoot
            Remove-Item -LiteralPath $stage -Recurse -Force
        }
    }
    Write-Host "Authentication gate artifacts: $outputDirectory"
} finally {
    $env:PATH = $oldPath
    if ($null -eq $oldVitaSdk) {
        Remove-Item Env:VITASDK -ErrorAction SilentlyContinue
    } else {
        $env:VITASDK = $oldVitaSdk
    }
    if ($null -eq $oldPythonPath) {
        Remove-Item Env:PYTHONPATH -ErrorAction SilentlyContinue
    } else {
        $env:PYTHONPATH = $oldPythonPath
    }
}

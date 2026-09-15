[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $PublicKeyHex,

    [ValidateSet("All", "InstallerVerify", "VerifyBootstrap", "InstallBootstrap", "PermanentInstaller")]
    [string] $Variant = "All",

    [string] $VitaSdkPath = $env:VITASDK,
    [string] $CMakePath,
    [string] $MakePath,
    [string] $Msys2RuntimePath,

    [switch] $EnableExperimentalDisplayUi,
    [switch] $EnableDirectTcp,

    [ValidateRange(1, 65535)]
    [int] $DirectTcpPort = 18196,

    [switch] $DisableLiveAreaAssets,

    [ValidateRange(1, 256)]
    [int] $Jobs = [Math]::Max(1, [Environment]::ProcessorCount)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RequiredExecutable {
    param(
        [string] $RequestedPath,
        [string] $DisplayName,
        [string] $CommandName,
        [string[]] $FallbackPaths
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        if (-not (Test-Path -LiteralPath $RequestedPath -PathType Leaf)) {
            throw "$DisplayName was not found at '$RequestedPath'."
        }
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }

    foreach ($candidate in $FallbackPaths) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $command = Get-Command -Name $CommandName -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $command) {
        return $command.Source
    }

    throw "$DisplayName was not found. Pass its full path explicitly."
}

function Assert-Msys2RuntimeDirectory {
    param([string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "MSYS2 MinGW64 runtime directory was not found at '$Path'."
    }

    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    $requiredCc1Dlls = @(
        "libgcc_s_seh-1.dll",
        "libgmp-10.dll",
        "libisl-23.dll",
        "libmpc-3.dll"
    )
    $missingDlls = @($requiredCc1Dlls | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $resolvedPath $_) -PathType Leaf)
    })
    if ($missingDlls.Count -ne 0) {
        throw (
            "MSYS2 MinGW64 runtime '$resolvedPath' is missing DLLs required by cc1.exe: {0}." -f
            ($missingDlls -join ", ")
        )
    }

    return $resolvedPath
}

function Resolve-Msys2RuntimeDirectory {
    param(
        [string] $RequestedPath,
        [string] $ResolvedMakePath
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        return Assert-Msys2RuntimeDirectory -Path $RequestedPath
    }

    $makeBinDirectory = [IO.Path]::GetDirectoryName($ResolvedMakePath)
    $makeEnvironmentDirectory = [IO.Directory]::GetParent($makeBinDirectory)
    $candidates = [Collections.Generic.List[string]]::new()
    if ($null -ne $makeEnvironmentDirectory -and $null -ne $makeEnvironmentDirectory.Parent) {
        $candidates.Add((Join-Path $makeEnvironmentDirectory.Parent.FullName "mingw64\bin"))
    }
    $candidates.Add($makeBinDirectory)
    $candidates.Add("C:\msys64\mingw64\bin")

    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($candidate in $candidates) {
        $candidateFull = [IO.Path]::GetFullPath($candidate)
        if (-not $seen.Add($candidateFull) -or
            -not (Test-Path -LiteralPath $candidateFull -PathType Container)) {
            continue
        }

        try {
            return Assert-Msys2RuntimeDirectory -Path $candidateFull
        } catch {
            # A nearby but incomplete bin directory is not a usable MinGW64
            # runtime. Continue to the standard fallback before failing.
        }
    }

    throw (
        "MSYS2 MinGW64 runtime was not found with the DLLs required by cc1.exe. " +
        "Install the mingw-w64-x86_64 toolchain or pass -Msys2RuntimePath."
    )
}

function Join-PathEnvironment {
    param([string[]] $Entries)

    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $ordered = [Collections.Generic.List[string]]::new()
    foreach ($entry in $Entries) {
        if ([string]::IsNullOrWhiteSpace($entry)) {
            continue
        }
        foreach ($part in $entry.Split([IO.Path]::PathSeparator)) {
            if (-not [string]::IsNullOrWhiteSpace($part) -and $seen.Add($part)) {
                $ordered.Add($part)
            }
        }
    }
    return [string]::Join([IO.Path]::PathSeparator, $ordered)
}

function Invoke-Checked {
    param(
        [string] $Executable,
        [string[]] $Arguments,
        [string] $Description
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Get-StreamSha256Lower {
    param([System.IO.Stream] $Stream)

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha256.ComputeHash($Stream)
        return ([BitConverter]::ToString($digest)).Replace("-", "").ToLowerInvariant()
    } finally {
        $sha256.Dispose()
    }
}

function Assert-VpkLiveAreaAssets {
    param(
        [string] $VpkPath,
        [string] $AssetRoot
    )

    $expectedAssets = [ordered]@{
        "sce_sys/icon0.png" = Join-Path $AssetRoot "icon0.png"
        "sce_sys/pic0.png" = Join-Path $AssetRoot "pic0.png"
        "sce_sys/livearea/contents/bg.png" = Join-Path $AssetRoot "livearea\contents\bg.png"
        "sce_sys/livearea/contents/startup.png" = Join-Path $AssetRoot "livearea\contents\startup.png"
        "sce_sys/livearea/contents/template.xml" = Join-Path $AssetRoot "livearea\contents\template.xml"
    }
    foreach ($sourcePath in $expectedAssets.Values) {
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "Required LiveArea source asset is missing: '$sourcePath'."
        }
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($VpkPath)
    $verifiedHashes = [ordered]@{}
    try {
        foreach ($entryName in $expectedAssets.Keys) {
            $matches = @($archive.Entries | Where-Object { $_.FullName -ceq $entryName })
            if ($matches.Count -ne 1) {
                throw (
                    "VPK must contain exactly one '$entryName' entry; found {0}." -f
                    $matches.Count
                )
            }

            $sourcePath = $expectedAssets[$entryName]
            $sourceLength = (Get-Item -LiteralPath $sourcePath).Length
            $sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($matches[0].Length -ne $sourceLength) {
                throw "VPK LiveArea entry '$entryName' differs in length from its reviewed source."
            }

            $entryStream = $matches[0].Open()
            try {
                $entryHash = Get-StreamSha256Lower -Stream $entryStream
            } finally {
                $entryStream.Dispose()
            }
            if ($entryHash -cne $sourceHash) {
                throw "VPK LiveArea entry '$entryName' differs from its reviewed source."
            }
            $verifiedHashes[$entryName] = $sourceHash
        }
    } finally {
        $archive.Dispose()
    }

    return $verifiedHashes
}

function Assert-DirectChildPath {
    param(
        [string] $Candidate,
        [string] $Parent
    )

    $candidateFull = [IO.Path]::GetFullPath($Candidate)
    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd([char[]]@("\", "/"))
    $candidateParent = [IO.Directory]::GetParent($candidateFull)
    if ($null -eq $candidateParent -or
        -not [string]::Equals(
            $candidateParent.FullName.TrimEnd([char[]]@("\", "/")),
            $parentFull,
            [StringComparison]::OrdinalIgnoreCase
        )) {
        throw "Refusing to manage a path outside the direct dist directory: '$Candidate'."
    }
}

function Remove-ManagedDirectory {
    param(
        [string] $Path,
        [string] $DistRoot
    )

    Assert-DirectChildPath -Candidate $Path -Parent $DistRoot
    if (Test-Path -LiteralPath $Path) {
        if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
            throw "Refusing to remove non-directory publish path '$Path'."
        }
        $directoryItem = Get-Item -LiteralPath $Path -Force
        if (($directoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to recursively remove reparse-point directory '$Path'."
        }
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Repair-PublishState {
    param(
        [string] $OutputDirectory,
        [string] $BackupDirectory,
        [string] $DistRoot
    )

    Assert-DirectChildPath -Candidate $OutputDirectory -Parent $DistRoot
    Assert-DirectChildPath -Candidate $BackupDirectory -Parent $DistRoot
    $outputExists = Test-Path -LiteralPath $OutputDirectory
    $backupExists = Test-Path -LiteralPath $BackupDirectory

    if ($outputExists -and -not (Test-Path -LiteralPath $OutputDirectory -PathType Container)) {
        throw "Publish destination is not a directory: '$OutputDirectory'."
    }
    if ($backupExists -and -not (Test-Path -LiteralPath $BackupDirectory -PathType Container)) {
        throw "Publish recovery path is not a directory: '$BackupDirectory'."
    }
    foreach ($existingDirectory in @($OutputDirectory, $BackupDirectory)) {
        if (Test-Path -LiteralPath $existingDirectory -PathType Container) {
            $existingItem = Get-Item -LiteralPath $existingDirectory -Force
            if (($existingItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing reparse-point publish directory '$existingDirectory'."
            }
        }
    }

    if ($backupExists -and $outputExists) {
        # The new complete directory was published before a previous process
        # stopped; only the no-longer-needed old directory remains.
        Remove-ManagedDirectory -Path $BackupDirectory -DistRoot $DistRoot
    } elseif ($backupExists) {
        # A previous process stopped between the two same-volume directory
        # renames. Restore the last complete published directory.
        [IO.Directory]::Move($BackupDirectory, $OutputDirectory)
    }
}

function Publish-ArtifactDirectory {
    param(
        [string] $StagingDirectory,
        [string] $OutputDirectory,
        [string] $DistRoot
    )

    Assert-DirectChildPath -Candidate $StagingDirectory -Parent $DistRoot
    Assert-DirectChildPath -Candidate $OutputDirectory -Parent $DistRoot
    if (-not (Test-Path -LiteralPath $StagingDirectory -PathType Container)) {
        throw "Complete staging directory is missing: '$StagingDirectory'."
    }

    $outputName = Split-Path -Leaf $OutputDirectory
    $backupDirectory = Join-Path $DistRoot (".$outputName.previous")
    Repair-PublishState -OutputDirectory $OutputDirectory -BackupDirectory $backupDirectory -DistRoot $DistRoot

    $hadPreviousOutput = Test-Path -LiteralPath $OutputDirectory -PathType Container
    if ($hadPreviousOutput) {
        [IO.Directory]::Move($OutputDirectory, $backupDirectory)
    }

    try {
        # Staging and dist are siblings on the same volume, so this publishes
        # the already-complete directory as one rename instead of mixing files.
        [IO.Directory]::Move($StagingDirectory, $OutputDirectory)
    } catch {
        if (-not (Test-Path -LiteralPath $OutputDirectory) -and
            (Test-Path -LiteralPath $backupDirectory -PathType Container)) {
            [IO.Directory]::Move($backupDirectory, $OutputDirectory)
        }
        throw
    }

    if (Test-Path -LiteralPath $backupDirectory) {
        Remove-ManagedDirectory -Path $backupDirectory -DistRoot $DistRoot
    }
}

if ($PublicKeyHex -cnotmatch "^[0-9a-f]{64}$") {
    throw "PublicKeyHex must be exactly 64 lowercase hexadecimal characters."
}
if ($PublicKeyHex -eq ("0" * 64)) {
    throw "PublicKeyHex cannot be the all-zero key."
}

[byte[]] $publicKeyBytes = [byte[]]::new(32)
for ($publicKeyIndex = 0; $publicKeyIndex -lt $publicKeyBytes.Length; ++$publicKeyIndex) {
    $publicKeyBytes[$publicKeyIndex] = [Convert]::ToByte(
        $PublicKeyHex.Substring($publicKeyIndex * 2, 2),
        16
    )
}
$publicKeyHasher = [Security.Cryptography.SHA256]::Create()
try {
    $publicKeyFingerprint = (
        [BitConverter]::ToString($publicKeyHasher.ComputeHash($publicKeyBytes))
    ).Replace("-", "").ToLowerInvariant()
} finally {
    $publicKeyHasher.Dispose()
    [Array]::Clear($publicKeyBytes, 0, $publicKeyBytes.Length)
}

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$sourceDirectory = Join-Path $projectRoot "agent"
$distRoot = Join-Path $projectRoot "dist"

if (-not (Test-Path -LiteralPath (Join-Path $sourceDirectory "CMakeLists.txt") -PathType Leaf)) {
    throw "The Vita agent source tree is missing from '$sourceDirectory'."
}

$effectiveVitaSdk = $VitaSdkPath
if ([string]::IsNullOrWhiteSpace($effectiveVitaSdk)) {
    if (Test-Path -LiteralPath "C:\vitasdk" -PathType Container) {
        $effectiveVitaSdk = "C:\vitasdk"
    } else {
        throw "VitaSDK was not found. Set VITASDK or pass -VitaSdkPath."
    }
}
if (-not (Test-Path -LiteralPath $effectiveVitaSdk -PathType Container)) {
    throw "VitaSDK directory was not found at '$effectiveVitaSdk'."
}
$effectiveVitaSdk = (Resolve-Path -LiteralPath $effectiveVitaSdk).Path
$toolchainFile = Join-Path $effectiveVitaSdk "share\vita.toolchain.cmake"
$vitaCmakeFile = Join-Path $effectiveVitaSdk "share\vita.cmake"
if (-not (Test-Path -LiteralPath $toolchainFile -PathType Leaf) -or
    -not (Test-Path -LiteralPath $vitaCmakeFile -PathType Leaf)) {
    throw "The selected VitaSDK is missing share\vita.toolchain.cmake or share\vita.cmake."
}

$resolvedCMake = Resolve-RequiredExecutable -RequestedPath $CMakePath -DisplayName "CMake" -CommandName "cmake" -FallbackPaths @(
    "C:\vitasdk-tools\cmake-4.4.3-windows-x86_64\bin\cmake.exe",
    "C:\Program Files\CMake\bin\cmake.exe"
)
$resolvedMake = Resolve-RequiredExecutable -RequestedPath $MakePath -DisplayName "MSYS2 make" -CommandName "make" -FallbackPaths @(
    "C:\msys64\usr\bin\make.exe",
    "C:\msys64\usr\bin\gmake.exe"
)
$resolvedMsys2Runtime = Resolve-Msys2RuntimeDirectory -RequestedPath $Msys2RuntimePath -ResolvedMakePath $resolvedMake
$makeBinDirectory = [IO.Path]::GetDirectoryName($resolvedMake)
$vitaSdkBinDirectory = Join-Path $effectiveVitaSdk "bin"
if (-not (Test-Path -LiteralPath $vitaSdkBinDirectory -PathType Container)) {
    throw "The selected VitaSDK is missing its bin directory at '$vitaSdkBinDirectory'."
}

$allVariants = @(
    [pscustomobject]@{
        Name = "InstallerVerify"
        OutputName = "installer-verify"
        AgentTitle = "VDEVDEP01"
        OnlyTarget = ""
        EnableInstall = "OFF"
        SelfType = "safe"
        Purpose = "Recommended first hardware-test app; verifies signed jobs but cannot install."
    },
    [pscustomobject]@{
        Name = "VerifyBootstrap"
        OutputName = "verify-bootstrap"
        AgentTitle = "SLRS00001"
        OnlyTarget = "VDEVDEP01"
        EnableInstall = "OFF"
        SelfType = "safe"
        Purpose = "Temporary verification-only eboot; never install its VPK over SLRS00001."
    },
    [pscustomobject]@{
        Name = "InstallBootstrap"
        OutputName = "install-bootstrap"
        AgentTitle = "SLRS00001"
        OnlyTarget = "VDEVDEP01"
        EnableInstall = "ON"
        SelfType = "unsafe user-mode"
        Purpose = "Temporary eboot permitted to install only VDEVDEP01."
    },
    [pscustomobject]@{
        Name = "PermanentInstaller"
        OutputName = "permanent-installer"
        AgentTitle = "VDEVDEP01"
        OnlyTarget = ""
        EnableInstall = "ON"
        SelfType = "unsafe user-mode"
        Purpose = "Permanent deployer app; it may install any valid title except itself."
    }
)

if ($Variant -eq "All") {
    $selectedVariants = $allVariants
} else {
    $selectedVariants = @($allVariants | Where-Object { $_.Name -eq $Variant })
}
if ($selectedVariants.Count -eq 0) {
    throw "No build variant matched '$Variant'."
}

$vitaSdkEnvironmentExisted = Test-Path Env:VITASDK
$previousVitaSdkEnvironment = if ($vitaSdkEnvironmentExisted) { $env:VITASDK } else { $null }
$pathEnvironmentExisted = Test-Path Env:PATH
$previousPathEnvironment = if ($pathEnvironmentExisted) { $env:PATH } else { $null }
$summaries = @()
$buildLock = $null
$displayUiCMakeValue = if ($EnableExperimentalDisplayUi) { "ON" } else { "OFF" }
$displayUiDescription = if ($EnableExperimentalDisplayUi) {
    "enabled (opt-in native vita2d interface; retail 3.65 lifecycle gate passed)"
} else {
    "disabled (headless lifecycle-safe build)"
}
$directTcpCMakeValue = if ($EnableDirectTcp) { "ON" } else { "OFF" }
$directTcpDescription = if ($EnableDirectTcp) {
    "enabled (authenticated intake on TCP $DirectTcpPort; basic retail-3.65 gate passed, interruption/security gates pending)"
} else {
    "disabled (hardware-tested FTP staging remains active)"
}
$liveAreaAssetsCMakeValue = if ($DisableLiveAreaAssets) { "OFF" } else { "ON" }
$liveAreaAssetsDescription = if ($DisableLiveAreaAssets) {
    "disabled by explicit build override"
} else {
    "enabled and verified byte-for-byte in VPK"
}
$liveAreaAssetRoot = Join-Path $sourceDirectory "assets\sce_sys"

try {
    $env:VITASDK = $effectiveVitaSdk
    $env:PATH = Join-PathEnvironment -Entries @(
        $resolvedMsys2Runtime,
        $makeBinDirectory,
        $vitaSdkBinDirectory,
        $previousPathEnvironment
    )
    New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
    $lockPath = Join-Path $distRoot ".build.lock"
    try {
        $buildLock = [IO.File]::Open(
            $lockPath,
            [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::None
        )
    } catch [IO.IOException] {
        throw "Another VitaDevDeploy agent build is already publishing from this checkout."
    }

    foreach ($variantConfig in $selectedVariants) {
        $buildDirectory = Join-Path $sourceDirectory ("build-vdd-" + $variantConfig.OutputName)
        $outputDirectory = Join-Path $distRoot $variantConfig.OutputName
        $publishBackup = Join-Path $distRoot (".$($variantConfig.OutputName).previous")
        Repair-PublishState -OutputDirectory $outputDirectory -BackupDirectory $publishBackup -DistRoot $distRoot
        $cmakeMakePath = $resolvedMake.Replace("\", "/")

        Write-Host ("Configuring {0}..." -f $variantConfig.OutputName)
        $configureArguments = @(
            "-S", $sourceDirectory,
            "-B", $buildDirectory,
            "-G", "MSYS Makefiles",
            "-DCMAKE_MAKE_PROGRAM=$cmakeMakePath",
            "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DVDD_PUBLIC_KEY_HEX=$PublicKeyHex",
            "-DVDEV_AGENT_TITLE_ID=$($variantConfig.AgentTitle)",
            "-DVDEV_ONLY_TARGET_TITLE_ID=$($variantConfig.OnlyTarget)",
            "-DVDEV_ENABLE_INSTALL=$($variantConfig.EnableInstall)",
            "-DVDEV_ENABLE_LIVEAREA_ASSETS=$liveAreaAssetsCMakeValue",
            "-DVDEV_ENABLE_DISPLAY_UI=$displayUiCMakeValue",
            "-DVDEV_ENABLE_DIRECT_TCP=$directTcpCMakeValue",
            "-DVDEV_DIRECT_TCP_PORT=$DirectTcpPort"
        )
        Invoke-Checked -Executable $resolvedCMake -Arguments $configureArguments -Description ("CMake configuration for " + $variantConfig.OutputName)
        $validatedFingerprintPath = Join-Path $buildDirectory "vdd-public-key-fingerprint.txt"
        if (-not (Test-Path -LiteralPath $validatedFingerprintPath -PathType Leaf)) {
            throw "CMake did not publish its validated Ed25519 public-key fingerprint."
        }
        $validatedPublicKeyFingerprint = [IO.File]::ReadAllText(
            $validatedFingerprintPath,
            [Text.Encoding]::ASCII
        ).Trim()
        if ($validatedPublicKeyFingerprint -cne $publicKeyFingerprint) {
            throw "Validated Ed25519 public-key fingerprint does not match the exact build input."
        }

        Write-Host ("Building {0}..." -f $variantConfig.OutputName)
        Invoke-Checked -Executable $resolvedCMake -Arguments @("--build", $buildDirectory, "--parallel", $Jobs) -Description ("Agent build for " + $variantConfig.OutputName)

        $builtEboot = Join-Path $buildDirectory "eboot.bin"
        $builtVpk = Join-Path $buildDirectory "VitaDevDeployAgent.vpk"
        foreach ($requiredArtifact in @($builtEboot, $builtVpk)) {
            if (-not (Test-Path -LiteralPath $requiredArtifact -PathType Leaf)) {
                throw "Build completed without required artifact '$requiredArtifact'."
            }
            if ((Get-Item -LiteralPath $requiredArtifact).Length -le 0) {
                throw "Build produced an empty artifact '$requiredArtifact'."
            }
        }
        $liveAreaAssetHashes = if ($DisableLiveAreaAssets) {
            $null
        } else {
            Assert-VpkLiveAreaAssets -VpkPath $builtVpk -AssetRoot $liveAreaAssetRoot
        }

        $ebootBytes = [IO.File]::ReadAllBytes($builtEboot)
        if ($ebootBytes.Length -lt 0x88) {
            throw "Built eboot is too short to contain a complete SELF header."
        }
        if (-not [BitConverter]::IsLittleEndian) {
            throw "SELF auth-ID validation currently requires a little-endian host."
        }
        $expectedAuthIdHex = if ($variantConfig.EnableInstall -eq "ON") {
            # Match the normal VitaSDK UNSAFE profile used by VitaDB's
            # upstream-tested user-mode PAF/PromoterUtil installer flow.
            "2F00000000000001"
        } else {
            "2F00000000000002"
        }
        $expectedAuthId = [Convert]::ToUInt64($expectedAuthIdHex, 16)
        $actualAuthId = [BitConverter]::ToUInt64($ebootBytes, 0x80)
        if ($actualAuthId -ne $expectedAuthId) {
            throw (
                "SELF auth-ID mismatch for {0}: expected 0x{1}, got 0x{2:X16}." -f
                $variantConfig.OutputName, $expectedAuthIdHex, $actualAuthId
            )
        }

        $stagingName = ".stage-{0}-{1}-{2}" -f $variantConfig.OutputName, $PID, [guid]::NewGuid().ToString("N")
        $stagingDirectory = Join-Path $distRoot $stagingName
        Assert-DirectChildPath -Candidate $stagingDirectory -Parent $distRoot
        New-Item -ItemType Directory -Path $stagingDirectory | Out-Null
        try {
            $stagedEboot = Join-Path $stagingDirectory "eboot.bin"
            $stagedVpk = Join-Path $stagingDirectory "VitaDevDeployAgent.vpk"
            Copy-Item -LiteralPath $builtEboot -Destination $stagedEboot
            Copy-Item -LiteralPath $builtVpk -Destination $stagedVpk

            $ebootHash = (Get-FileHash -LiteralPath $stagedEboot -Algorithm SHA256).Hash.ToLowerInvariant()
            $vpkHash = (Get-FileHash -LiteralPath $stagedVpk -Algorithm SHA256).Hash.ToLowerInvariant()
            $hashLines = [string[]]@(
                "$ebootHash  eboot.bin",
                "$vpkHash  VitaDevDeployAgent.vpk"
            )
            [IO.File]::WriteAllLines(
                (Join-Path $stagingDirectory "SHA256SUMS.txt"),
                $hashLines,
                [Text.Encoding]::ASCII
            )

            $onlyTargetText = if ([string]::IsNullOrEmpty($variantConfig.OnlyTarget)) {
                "none (self-deployment is still refused)"
            } else {
                $variantConfig.OnlyTarget
            }
            $metadataSyncPolicy = if ($variantConfig.EnableInstall -eq "ON") {
                "strict directory SyncByFd then checked device sceIoSync; any failure blocks promotion"
            } else {
                "directory SyncByFd then checked device sceIoSync; EACCES may use weak file resync/re-read"
            }
            $buildInfo = [string[]]@(
                "variant=$($variantConfig.OutputName)",
                "agent_title_id=$($variantConfig.AgentTitle)",
                "only_target_title_id=$onlyTargetText",
                "install_enabled=$($variantConfig.EnableInstall)",
                "self_type=$($variantConfig.SelfType)",
                "self_auth_id=0x$expectedAuthIdHex",
                "display_ui=$displayUiDescription",
                "direct_tcp=$directTcpDescription",
                "livearea_assets=$liveAreaAssetsDescription",
                "metadata_sync_policy=$metadataSyncPolicy",
                "purpose=$($variantConfig.Purpose)",
                "trusted_public_key_fingerprint=sha256:$validatedPublicKeyFingerprint",
                "private_key_embedded=no",
                "note=The validated prime-subgroup Ed25519 public key is compiled into the agent."
            )
            if ($null -ne $liveAreaAssetHashes) {
                foreach ($entryName in $liveAreaAssetHashes.Keys) {
                    $metadataName = $entryName.Replace("/", "_").Replace(".", "_")
                    $buildInfo += "livearea_asset_${metadataName}_sha256=$($liveAreaAssetHashes[$entryName])"
                }
            }
            [IO.File]::WriteAllLines(
                (Join-Path $stagingDirectory "BUILD-INFO.txt"),
                $buildInfo,
                [Text.Encoding]::ASCII
            )

            if ((Get-FileHash -LiteralPath $stagedEboot -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ebootHash -or
                (Get-FileHash -LiteralPath $stagedVpk -Algorithm SHA256).Hash.ToLowerInvariant() -ne $vpkHash) {
                throw "An artifact changed while staging '$($variantConfig.OutputName)'."
            }
            Publish-ArtifactDirectory -StagingDirectory $stagingDirectory -OutputDirectory $outputDirectory -DistRoot $distRoot
        } finally {
            if (Test-Path -LiteralPath $stagingDirectory) {
                Remove-ManagedDirectory -Path $stagingDirectory -DistRoot $distRoot
            }
        }

        $distEboot = Join-Path $outputDirectory "eboot.bin"
        $distVpk = Join-Path $outputDirectory "VitaDevDeployAgent.vpk"
        if ((Get-FileHash -LiteralPath $distEboot -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ebootHash -or
            (Get-FileHash -LiteralPath $distVpk -Algorithm SHA256).Hash.ToLowerInvariant() -ne $vpkHash) {
            throw "Published artifact verification failed for '$($variantConfig.OutputName)'."
        }

        $summaries += [pscustomobject]@{
            Variant = $variantConfig.OutputName
            TitleId = $variantConfig.AgentTitle
            Install = $variantConfig.EnableInstall
            Self = $variantConfig.SelfType
            EbootSha256 = $ebootHash
            VpkSha256 = $vpkHash
            Output = $outputDirectory
        }
    }
} finally {
    if ($pathEnvironmentExisted) {
        $env:PATH = $previousPathEnvironment
    } else {
        Remove-Item Env:PATH -ErrorAction SilentlyContinue
    }
    if ($vitaSdkEnvironmentExisted) {
        $env:VITASDK = $previousVitaSdkEnvironment
    } else {
        Remove-Item Env:VITASDK -ErrorAction SilentlyContinue
    }
    if ($null -ne $buildLock) {
        $buildLock.Dispose()
    }
}

Write-Host ""
Write-Host "VitaDevDeploy agent build complete."
$summaries | Format-Table -AutoSize

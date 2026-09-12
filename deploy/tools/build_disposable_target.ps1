[CmdletBinding()]
param(
    [string] $VitaSdkPath = $env:VITASDK,
    [string] $CMakePath,
    [string] $MakePath,
    [string] $Msys2RuntimePath,
    [string] $PythonLauncherPath,

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

function Get-Sha256Lower {
    param([string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$sourceDirectory = Join-Path $projectRoot "disposable-target"
$buildDirectory = Join-Path $sourceDirectory "build-vddt"
$distRoot = Join-Path $projectRoot "dist"
$outputDirectory = Join-Path $distRoot "disposable-test"

$effectiveVitaSdk = $VitaSdkPath
if ([string]::IsNullOrWhiteSpace($effectiveVitaSdk)) {
    if (Test-Path -LiteralPath "C:\vitasdk" -PathType Container) {
        $effectiveVitaSdk = "C:\vitasdk"
    } else {
        throw "VitaSDK was not found. Set VITASDK or pass -VitaSdkPath."
    }
}
$effectiveVitaSdk = (Resolve-Path -LiteralPath $effectiveVitaSdk).Path
$toolchainFile = Join-Path $effectiveVitaSdk "share\vita.toolchain.cmake"
$vitaCmakeFile = Join-Path $effectiveVitaSdk "share\vita.cmake"
if (-not (Test-Path -LiteralPath $toolchainFile -PathType Leaf) -or
    -not (Test-Path -LiteralPath $vitaCmakeFile -PathType Leaf)) {
    throw "The selected VitaSDK is missing its CMake integration files."
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
$resolvedPython = Resolve-RequiredExecutable -RequestedPath $PythonLauncherPath -DisplayName "Python launcher" -CommandName "py" -FallbackPaths @(
    "$env:SystemRoot\py.exe"
)

$vitaSdkEnvironmentExisted = Test-Path Env:VITASDK
$previousVitaSdkEnvironment = if ($vitaSdkEnvironmentExisted) { $env:VITASDK } else { $null }
$pythonPathEnvironmentExisted = Test-Path Env:PYTHONPATH
$previousPythonPathEnvironment = if ($pythonPathEnvironmentExisted) { $env:PYTHONPATH } else { $null }
$pathEnvironmentExisted = Test-Path Env:PATH
$previousPathEnvironment = if ($pathEnvironmentExisted) { $env:PATH } else { $null }

try {
    $env:VITASDK = $effectiveVitaSdk
    $env:PYTHONPATH = $projectRoot
    $env:PATH = Join-PathEnvironment -Entries @(
        $resolvedMsys2Runtime,
        $makeBinDirectory,
        $vitaSdkBinDirectory,
        $previousPathEnvironment
    )
    $cmakeMakePath = $resolvedMake.Replace("\", "/")
    Invoke-Checked -Executable $resolvedCMake -Arguments @(
        "-S", $sourceDirectory,
        "-B", $buildDirectory,
        "-G", "MSYS Makefiles",
        "-DCMAKE_MAKE_PROGRAM=$cmakeMakePath",
        "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
        "-DCMAKE_BUILD_TYPE=Release"
    ) -Description "Disposable target configuration"
    Invoke-Checked -Executable $resolvedCMake -Arguments @(
        "--build", $buildDirectory, "--parallel", $Jobs
    ) -Description "Disposable target build"

    $builtEboot = Join-Path $buildDirectory "eboot.bin"
    $builtVpk = Join-Path $buildDirectory "VitaDevDeployDisposableTest.vpk"
    foreach ($artifact in @($builtEboot, $builtVpk)) {
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or
            (Get-Item -LiteralPath $artifact).Length -le 0) {
            throw "Build did not produce a valid '$artifact'."
        }
    }

    $ebootBytes = [IO.File]::ReadAllBytes($builtEboot)
    if ($ebootBytes.Length -lt 0x88 -or -not [BitConverter]::IsLittleEndian) {
        throw "Built eboot does not have a readable little-endian SELF header."
    }
    $expectedAuthId = [Convert]::ToUInt64("2F00000000000002", 16)
    $actualAuthId = [BitConverter]::ToUInt64($ebootBytes, 0x80)
    if ($actualAuthId -ne $expectedAuthId) {
        throw ("Expected safe auth ID 0x2F00000000000002, got 0x{0:X16}." -f $actualAuthId)
    }

    $inspectionJson = & $resolvedPython -3 -m host.vitadevdeploy verify $builtVpk
    if ($LASTEXITCODE -ne 0) {
        throw "The host VPK validator rejected the disposable target."
    }
    $inspection = $inspectionJson | ConvertFrom-Json
    if ($inspection.title_id -ne "VDDT00001" -or $inspection.file_count -ne 2) {
        throw "Disposable VPK identity or file count is unexpected."
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($builtVpk)
    try {
        $entryNames = @($archive.Entries | ForEach-Object { $_.FullName })
        $expectedEntries = @("eboot.bin", "sce_sys/param.sfo")
        if (@(Compare-Object $entryNames $expectedEntries).Count -ne 0) {
            throw "Disposable VPK contains an unexpected archive entry."
        }
        $embeddedEntry = $archive.GetEntry("eboot.bin")
        if ($null -eq $embeddedEntry) {
            throw "Disposable VPK is missing its embedded eboot."
        }
        $embeddedStream = $embeddedEntry.Open()
        try {
            $memory = [IO.MemoryStream]::new()
            try {
                $embeddedStream.CopyTo($memory)
                $embeddedBytes = $memory.ToArray()
            } finally {
                $memory.Dispose()
            }
        } finally {
            $embeddedStream.Dispose()
        }
        if (-not [Linq.Enumerable]::SequenceEqual([byte[]]$ebootBytes, [byte[]]$embeddedBytes)) {
            throw "The VPK's embedded eboot does not match the standalone eboot."
        }
    } finally {
        $archive.Dispose()
    }

    New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
    $stageDirectory = Join-Path $distRoot (".stage-disposable-test-{0}" -f [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $stageDirectory | Out-Null
    try {
        Copy-Item -LiteralPath $builtEboot -Destination (Join-Path $stageDirectory "eboot.bin")
        Copy-Item -LiteralPath $builtVpk -Destination (Join-Path $stageDirectory "VitaDevDeployDisposableTest.vpk")
        $ebootHash = Get-Sha256Lower (Join-Path $stageDirectory "eboot.bin")
        $vpkHash = Get-Sha256Lower (Join-Path $stageDirectory "VitaDevDeployDisposableTest.vpk")
        [IO.File]::WriteAllLines(
            (Join-Path $stageDirectory "SHA256SUMS.txt"),
            [string[]]@(
                "$ebootHash  eboot.bin",
                "$vpkHash  VitaDevDeployDisposableTest.vpk"
            ),
            [Text.Encoding]::ASCII
        )
        [IO.File]::WriteAllLines(
            (Join-Path $stageDirectory "BUILD-INFO.txt"),
            [string[]]@(
                "variant=disposable-test",
                "title_id=VDDT00001",
                "version=01.00",
                "self_type=safe user-mode",
                "self_auth_id=0x2F00000000000002",
                "display_ui=none",
                "network=none",
                "metadata_sync_policy=parent-directory SyncByFd; EACCES uses weak marker-file resync/re-read (not namespace durable)",
                "purpose=Harmless marker-writing install and launch target.",
                "marker=ux0:data/VitaDevDeploy/disposable-target.last-run"
            ),
            [Text.Encoding]::ASCII
        )

        if (Test-Path -LiteralPath $outputDirectory) {
            $outputItem = Get-Item -LiteralPath $outputDirectory -Force
            if (-not $outputItem.PSIsContainer -or
                ($outputItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
                -not [string]::Equals($outputItem.Parent.FullName, $distRoot, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to replace unexpected publish path '$outputDirectory'."
            }
            Remove-Item -LiteralPath $outputDirectory -Recurse -Force
        }
        [IO.Directory]::Move($stageDirectory, $outputDirectory)
    } finally {
        if (Test-Path -LiteralPath $stageDirectory) {
            $stageItem = Get-Item -LiteralPath $stageDirectory -Force
            if ($stageItem.PSIsContainer -and
                ($stageItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 -and
                [string]::Equals($stageItem.Parent.FullName, $distRoot, [StringComparison]::OrdinalIgnoreCase)) {
                Remove-Item -LiteralPath $stageDirectory -Recurse -Force
            }
        }
    }

    Write-Host ""
    Write-Host "Disposable test target build complete."
    [pscustomobject]@{
        TitleId = "VDDT00001"
        AuthId = "0x2F00000000000002"
        EbootSha256 = $ebootHash
        VpkSha256 = $vpkHash
        Output = $outputDirectory
    } | Format-List
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
    if ($pythonPathEnvironmentExisted) {
        $env:PYTHONPATH = $previousPythonPathEnvironment
    } else {
        Remove-Item Env:PYTHONPATH -ErrorAction SilentlyContinue
    }
}

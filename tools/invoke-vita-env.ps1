Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# This is intentionally a non-advanced script. Advanced scripts acquire
# PowerShell's common parameters, where a native argument such as GCC's `-o`
# becomes ambiguous with -OutVariable/-OutBuffer before it can be forwarded.
# Wrapper options are parsed manually until the executable is found; every
# subsequent token remains untouched, including its leading dash.
$invocationArguments = @($args)
$vitaSdkPath = $env:VITASDK
$msys2RuntimePath = "C:\msys64\mingw64\bin"
$msys2UsrBinPath = "C:\msys64\usr\bin"
$validateOnly = $false
$executable = $null
$remainingArguments = @()
$argumentIndex = 0

:argumentParsing while ($argumentIndex -lt $invocationArguments.Count) {
    $argument = [string] $invocationArguments[$argumentIndex]
    switch ($argument.ToLowerInvariant()) {
        "-validateonly" {
            $validateOnly = $true
            $argumentIndex++
            continue
        }
        "-vitasdkpath" {
            if ($argumentIndex + 1 -ge $invocationArguments.Count) {
                throw "-VitaSdkPath requires a directory argument."
            }
            $vitaSdkPath = [string] $invocationArguments[$argumentIndex + 1]
            $argumentIndex += 2
            continue
        }
        "-msys2runtimepath" {
            if ($argumentIndex + 1 -ge $invocationArguments.Count) {
                throw "-Msys2RuntimePath requires a directory argument."
            }
            $msys2RuntimePath = [string] $invocationArguments[$argumentIndex + 1]
            $argumentIndex += 2
            continue
        }
        "-msys2usrbinpath" {
            if ($argumentIndex + 1 -ge $invocationArguments.Count) {
                throw "-Msys2UsrBinPath requires a directory argument."
            }
            $msys2UsrBinPath = [string] $invocationArguments[$argumentIndex + 1]
            $argumentIndex += 2
            continue
        }
        "--" {
            $argumentIndex++
            if ($argumentIndex -ge $invocationArguments.Count) {
                throw "-- must be followed by an executable."
            }
            $executable = [string] $invocationArguments[$argumentIndex]
        }
        default {
            $executable = $argument
        }
    }

    if ($argumentIndex + 1 -lt $invocationArguments.Count) {
        $remainingArguments = @(
            $invocationArguments[($argumentIndex + 1)..($invocationArguments.Count - 1)]
        )
    }
    break argumentParsing
}

function Resolve-RequiredDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [string] $DisplayName
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$DisplayName was not found at '$Path'."
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

function Join-ProcessPath {
    param([string[]] $Entries)

    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
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

if ([string]::IsNullOrWhiteSpace($VitaSdkPath)) {
    $VitaSdkPath = "C:\vitasdk"
}

$resolvedVitaSdk = Resolve-RequiredDirectory -Path $VitaSdkPath -DisplayName "VitaSDK directory"
$resolvedVitaSdkBin = Resolve-RequiredDirectory -Path (Join-Path $resolvedVitaSdk "bin") -DisplayName "VitaSDK bin directory"
$resolvedMsys2Runtime = Resolve-RequiredDirectory -Path $Msys2RuntimePath -DisplayName "MSYS2 MinGW64 runtime directory"
$resolvedMsys2UsrBin = Resolve-RequiredDirectory -Path $Msys2UsrBinPath -DisplayName "MSYS2 usr bin directory"

# These are the non-system DLLs imported directly by the installed MinGW64
# GCC 16 cc1.exe. Checking the complete set prevents a sequence of loader
# dialogs where fixing one missing DLL merely exposes the next one.
$requiredCc1Dlls = @(
    "libgcc_s_seh-1.dll",
    "libgmp-10.dll",
    "libisl-23.dll",
    "libmpc-3.dll",
    "libmpfr-6.dll",
    "libwinpthread-1.dll",
    "zlib1.dll",
    "libzstd.dll"
)
$missingCc1Dlls = @($requiredCc1Dlls | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path $resolvedMsys2Runtime $_) -PathType Leaf)
})
if ($missingCc1Dlls.Count -ne 0) {
    throw (
        "MSYS2 MinGW64 runtime '$resolvedMsys2Runtime' is missing DLLs " +
        "required by cc1.exe: {0}." -f ($missingCc1Dlls -join ", ")
    )
}

if ($validateOnly) {
    [pscustomobject]@{
        VitaSdkBin       = $resolvedVitaSdkBin
        Msys2RuntimeBin  = $resolvedMsys2Runtime
        Msys2UsrBin      = $resolvedMsys2UsrBin
        Cc1RuntimeDlls   = $requiredCc1Dlls.Count
        Status           = "OK"
    }
    return
}

if ([string]::IsNullOrWhiteSpace($executable)) {
    throw "Pass an executable as the first argument, or use -ValidateOnly."
}

$pathEnvironmentExisted = Test-Path Env:PATH
$previousPath = if ($pathEnvironmentExisted) { $env:PATH } else { $null }
$vitaSdkEnvironmentExisted = Test-Path Env:VITASDK
$previousVitaSdk = if ($vitaSdkEnvironmentExisted) { $env:VITASDK } else { $null }
$childExitCode = 0

try {
    # Environment assignments affect only this PowerShell process and the child
    # process. The caller's values are restored below even when invocation fails.
    $env:VITASDK = $resolvedVitaSdk
    $env:PATH = Join-ProcessPath -Entries @(
        $resolvedMsys2Runtime,
        $resolvedMsys2UsrBin,
        $resolvedVitaSdkBin,
        $previousPath
    )

    $command = Get-Command -Name $executable -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $command) {
        throw "Executable '$executable' was not found in the validated build environment."
    }

    & $command.Source @remainingArguments
    $childExitCode = $LASTEXITCODE
    if ($null -eq $childExitCode) {
        $childExitCode = 0
    }
} finally {
    if ($pathEnvironmentExisted) {
        $env:PATH = $previousPath
    } else {
        Remove-Item Env:PATH -ErrorAction SilentlyContinue
    }

    if ($vitaSdkEnvironmentExisted) {
        $env:VITASDK = $previousVitaSdk
    } else {
        Remove-Item Env:VITASDK -ErrorAction SilentlyContinue
    }
}

exit $childExitCode

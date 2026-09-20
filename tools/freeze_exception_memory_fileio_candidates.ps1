[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ExpectedCommit,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$KuBridgeDirectory = 'D:\Claude\kuBridge',
    [string]$Make = 'C:\msys64\usr\bin\make.exe'
)

$ErrorActionPreference = 'Stop'
$expectedBase = '838e235740968583cee3fbfc2ef26f17fd0423ce'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$kernelBuild = Join-Path $root 'kernel\build'
$kuBridgeBuild = Join-Path $KuBridgeDirectory 'build-local'

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

Set-Location $root
$head = (& git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $head -ne $ExpectedCommit) {
    throw "HEAD must be exact expected commit $ExpectedCommit (actual: $head)"
}
Invoke-Checked git @('merge-base', '--is-ancestor', $expectedBase, 'HEAD')
if ((& git status --porcelain).Length -ne 0) {
    throw 'Refusing to freeze candidates from a dirty worktree'
}

$required = @(
    $Make,
    (Join-Path $KuBridgeDirectory 'kubridge.h'),
    (Join-Path $kuBridgeBuild 'libkubridge_stub.a'),
    (Join-Path $kuBridgeBuild 'kubridge.skprx'),
    (Join-Path $kernelBuild 'vitadebug_stubs\libvitadebug_kernel_stub.a'),
    (Join-Path $kernelBuild 'vitadebug.skprx')
)
foreach ($path in $required) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required local build input is missing: $path"
    }
}

if (Test-Path -LiteralPath $output) {
    throw "Refusing to overwrite frozen output directory: $output"
}
[void](New-Item -ItemType Directory -Path $output)
$env:PATH = 'C:\msys64\usr\bin;' + $env:PATH

$variants = @(
    @{ Name = 'predecessor'; NullType = $null },
    @{ Name = 'null-data-abort'; NullType = 0 },
    @{ Name = 'null-prefetch-abort'; NullType = 1 },
    @{ Name = 'null-undefined-instruction'; NullType = 2 }
)
$records = @()
foreach ($variant in $variants) {
    Invoke-Checked $Make @('clean')
    $arguments = @(
        'package',
        'UVDB_KERNEL_THREAD_CONTROL=1',
        'UVDB_HARDWARE_SAFETY_GATE=1',
        "KUBRIDGE_DIR=$KuBridgeDirectory",
        "KUBRIDGE_LIB_DIR=$kuBridgeBuild",
        "VITADEBUG_KERNEL_BUILD_DIR=$kernelBuild"
    )
    if ($null -ne $variant.NullType) {
        $arguments += "UVDB_SAFETY_GATE_NULL_PREDECESSOR_TYPE=$($variant.NullType)"
    }
    Invoke-Checked $Make $arguments

    $variantDirectory = Join-Path $output $variant.Name
    [void](New-Item -ItemType Directory -Path $variantDirectory)
    foreach ($name in @('test.elf', 'test.velf', 'eboot.bin', 'uvdb-test.vpk')) {
        Copy-Item -LiteralPath (Join-Path $root $name) `
            -Destination (Join-Path $variantDirectory $name)
    }
    $records += [ordered]@{
        name = $variant.Name
        null_predecessor_type = $variant.NullType
        files = @(
            Get-ChildItem -LiteralPath $variantDirectory -File |
                Sort-Object Name |
                ForEach-Object {
                    [ordered]@{
                        name = $_.Name
                        length = $_.Length
                        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
                    }
                }
        )
    }
}

$companions = Join-Path $output 'companions'
[void](New-Item -ItemType Directory -Path $companions)
Copy-Item -LiteralPath (Join-Path $kernelBuild 'vitadebug.skprx') `
    -Destination (Join-Path $companions 'vitadebug.skprx')
Copy-Item -LiteralPath (Join-Path $kuBridgeBuild 'kubridge.skprx') `
    -Destination (Join-Path $companions 'kubridge.skprx')

$manifest = [ordered]@{
    format = 'VITADEBUGGER-EXCEPTION-MEMORY-FILEIO-CANDIDATES-1'
    source_commit = $head
    required_base_commit = $expectedBase
    kubridge_commit = (& git -C $KuBridgeDirectory rev-parse HEAD).Trim()
    title_id = 'SLRS00001'
    kernel_abi = '0x0001000E'
    required_kernel_capabilities = @(
        'VD_KERNEL_CAP_THREAD_LIST',
        'VD_KERNEL_CAP_THREAD_CONTROL',
        'VD_KERNEL_CAP_THREAD_REGISTERS',
        'VD_KERNEL_CAP_STOP_RECONCILE'
    )
    variants = $records
    companions = @(
        Get-ChildItem -LiteralPath $companions -File |
            Sort-Object Name |
            ForEach-Object {
                [ordered]@{
                    name = $_.Name
                    length = $_.Length
                    sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
                }
            }
    )
}
$manifestPath = Join-Path $output 'manifest.json'
$manifestJson = $manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText(
    $manifestPath,
    $manifestJson + [Environment]::NewLine,
    [System.Text.UTF8Encoding]::new($false)
)
Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath |
    ForEach-Object {
        "$($_.Hash.ToLowerInvariant())  manifest.json"
    } |
    Set-Content -LiteralPath (Join-Path $output 'manifest.sha256') -Encoding ascii

Write-Host "Frozen candidates: $output"
Write-Host "Manifest: $manifestPath"

# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
param(
    [string]$Output,
    [string]$Scene,
    [string]$Manifest,
    [ValidateSet('default', 'dark', 'compact', 'dark-compact')][string]$Theme,
    [ValidateSet('ltr', 'rtl')][string]$Direction,
    [ValidateSet('1', '1.5', '2')][string]$Scale,
    [switch]$WithoutLayoutInspection,
    [switch]$SkipBuild
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$formRepository = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $formRepository
try {
    $sourcePaths = @(& rg --files src | Where-Object { $_.EndsWith('.rs') }) + @(
        'Cargo.toml', 'Cargo.lock', 'examples/form_visual.rs', 'fixtures/form/visual-cases.json'
    )
    $sourceHashes = [ordered]@{}
    foreach ($sourcePath in $sourcePaths) {
        $sourceHashes[$sourcePath.Replace('\', '/')] = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if (-not $SkipBuild) {
        $buildArguments = @('build', '--locked', '--offline', '--example', 'form_visual')
        if (-not $WithoutLayoutInspection) { $buildArguments += @('--features', 'test-support') }
        & cargo @buildArguments
        if ($LASTEXITCODE -ne 0) { throw 'Form capture example build failed.' }
        foreach ($sourcePath in $sourcePaths) {
            $actual = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $sourceHashes[$sourcePath.Replace('\', '/')]) {
                throw "Source changed during compilation; rebuild a coherent capture before proceeding: $sourcePath"
            }
        }
    }
    $metadata = (& cargo metadata --format-version 1 --no-deps --locked --offline | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot locate Cargo target directory.' }
    $executable = Join-Path $metadata.target_directory 'debug/examples/form_visual.exe'
    if (-not (Test-Path -LiteralPath $executable)) { throw "Missing native capture example: $executable" }
    if (-not $Output) { $Output = Join-Path $metadata.target_directory 'form-native' }
    $Output = [System.IO.Path]::GetFullPath($Output)
    $null = New-Item -ItemType Directory -Path $Output -Force
    # Freeze the executable so subsequent builds cannot change an ongoing run.
    $captureExecutable = Join-Path $Output 'form_visual-capture.exe'
    Copy-Item -LiteralPath $executable -Destination $captureExecutable -Force
    $buildInfo = (& $captureExecutable --build-info | Out-String | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0 -or $buildInfo.label_layout_inspection -eq [bool]$WithoutLayoutInspection) {
        throw 'The frozen native executable does not match the requested layout-inspection feature.'
    }
    $peImage = [System.IO.File]::ReadAllBytes($captureExecutable)
    $peHeader = [BitConverter]::ToInt32($peImage, 0x3c)
    if ([BitConverter]::ToUInt32($peImage, $peHeader) -ne 0x00004550 -or
        [BitConverter]::ToUInt16($peImage, $peHeader + 24) -ne 0x20b) {
        throw 'Expected a native PE32+ capture executable.'
    }
    $stackReserve = [BitConverter]::ToUInt64($peImage, $peHeader + 24 + 72)
    $captureContext = [ordered]@{
        schema_version = 1
        executable_sha256 = (Get-FileHash -LiteralPath $captureExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
        pe_stack_reserve_bytes = $stackReserve
        features = @($buildInfo.features)
        label_geometry_source = $(if ($buildInfo.label_layout_inspection) { 'logical-prepaint' } else { 'pixel-snapped-quad' })
        source_snapshot_matches_build = (-not $SkipBuild)
        source_hashes = $sourceHashes
        capture_script_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
        os = [System.Runtime.InteropServices.RuntimeInformation]::OSDescription
        architecture = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
        started_utc = [DateTime]::UtcNow.ToString('o')
    }
    if ($Manifest) {
        $captureContext['runtime_manifest'] = [ordered]@{
            path = [System.IO.Path]::GetFullPath($Manifest)
            sha256 = (Get-FileHash -LiteralPath $Manifest -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $captureContext | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Output 'capture-context.json') -Encoding utf8
    $arguments = @('--output', $Output)
    if ($Manifest) { $arguments += @('--manifest', [System.IO.Path]::GetFullPath($Manifest)) }
    foreach ($filter in @(@('scene', $Scene), @('theme', $Theme), @('direction', $Direction), @('scale', $Scale))) {
        if ($filter[1]) { $arguments += @("--$($filter[0])", $filter[1]) }
    }
    # The example creates only a hidden, unfocused native window and asserts
    # that the capture process never becomes the foreground application.
    & $captureExecutable @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Native Form capture failed.' }
} finally {
    Pop-Location
}

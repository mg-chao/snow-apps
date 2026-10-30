# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [string]$Output,
    [string]$Scene,
    [ValidateSet('', 'default', 'dark', 'compact', 'dark-compact')][string]$Theme = '',
    [ValidateSet('', 'ltr', 'rtl')][string]$Direction = '',
    [ValidateSet('', '1', '1.5', '2')][string]$Scale = ''
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $crateRoot
if (-not $Output) { $Output = Join-Path $repositoryRoot 'build/ant-design-gpui/color-picker-native-final' }
$Output = [IO.Path]::GetFullPath($Output)
. (Join-Path $PSScriptRoot 'color-picker-validation-support.ps1')
Push-Location $crateRoot
try {
    $taskSources = Get-ColorPickerSourceFingerprint $crateRoot
    & cargo build --locked --offline --features test-support --example color_picker_visual
    if ($LASTEXITCODE -ne 0) { throw 'ColorPicker visual fixture build failed.' }
    Assert-ColorPickerSourceFingerprint $taskSources $crateRoot
    $taskExecutable = Join-Path $repositoryRoot 'build/ant-design-gpui/debug/examples/color_picker_visual.exe'
    $taskBinaryHash = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
    New-Item -ItemType Directory -Path $Output -Force | Out-Null
    $taskArguments = @('--output', $Output)
    foreach ($taskFilter in @(@('--scene', $Scene), @('--theme', $Theme), @('--direction', $Direction), @('--scale', $Scale))) {
        if ($taskFilter[1]) { $taskArguments += $taskFilter }
    }
    & $taskExecutable @taskArguments
    if ($LASTEXITCODE -ne 0) { throw 'Native ColorPicker capture failed.' }
    Assert-ColorPickerSourceFingerprint $taskSources $crateRoot
    if ((Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -cne $taskBinaryHash) {
        throw 'Capture binary changed during validation.'
    }
    $taskCaptureContext = Get-Content -LiteralPath (Join-Path $Output 'capture-context.json') -Raw | ConvertFrom-Json -AsHashtable
    if ($taskCaptureContext.foreground_violations -ne 0 -or $taskCaptureContext.foreground_checks -lt $taskCaptureContext.cases * 6) {
        throw 'Native capture did not verify background focus at every checkpoint.'
    }
    $taskMetadata = [ordered]@{
        schema_version = 1
        completed_utc = [DateTime]::UtcNow.ToString('o')
        profile = 'dev'
        purpose = 'native GPU visual capture, not performance measurement'
        source_root = $crateRoot
        git_head = (& git rev-parse HEAD) -join ''
        toolchain = Get-ColorPickerToolchain
        capture_context = $taskCaptureContext
        source_sha256 = $taskSources
        executable = @{ path = $taskExecutable; sha256 = $taskBinaryHash }
        metrics_sha256 = (Get-FileHash -LiteralPath (Join-Path $Output 'native-metrics.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $taskMetadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $Output 'native-provenance.json') -Encoding utf8NoBOM
} finally { Pop-Location }

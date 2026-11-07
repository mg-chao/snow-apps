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
if (-not $Output) { $Output = Join-Path $repositoryRoot 'build/ant-design-gpui/date-time-picker-reference/native-capture' }
$Output = [IO.Path]::GetFullPath($Output)
. (Join-Path $PSScriptRoot 'date-time-picker-validation-support.ps1')
Push-Location $crateRoot
try {
    $taskSources = Get-DateTimePickerSourceFingerprint $crateRoot
    & cargo build --locked --offline --features test-support --example date_time_picker_visual
    if ($LASTEXITCODE -ne 0) { throw 'Date/time picker visual fixture build failed.' }
    Assert-DateTimePickerSourceFingerprint $taskSources $crateRoot
    $taskExecutable = Join-Path $repositoryRoot 'build/ant-design-gpui/debug/examples/date_time_picker_visual.exe'
    $taskBinaryHash = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
    New-Item -ItemType Directory -Path $Output -Force | Out-Null
    $taskArguments = @('--output', $Output)
    foreach ($taskFilter in @(@('--scene', $Scene), @('--theme', $Theme), @('--direction', $Direction), @('--scale', $Scale))) {
        if ($taskFilter[1]) { $taskArguments += $taskFilter }
    }
    & $taskExecutable @taskArguments
    if ($LASTEXITCODE -ne 0) { throw 'Native Date/time picker capture failed.' }
    Assert-DateTimePickerSourceFingerprint $taskSources $crateRoot
    if ((Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -cne $taskBinaryHash) { throw 'Capture binary changed during validation.' }
    $taskCaptureContext = Get-Content -LiteralPath (Join-Path $Output 'capture-context.json') -Raw | ConvertFrom-Json -AsHashtable
    if ($taskCaptureContext.foreground_violations -ne 0 -or $taskCaptureContext.foreground_checks -lt $taskCaptureContext.cases * 6) {
        throw 'Native capture did not verify foreground ownership at every checkpoint.'
    }
    $taskImages = @(Get-ChildItem -LiteralPath $Output -Filter '*.png' | Sort-Object Name | ForEach-Object {
        @{ file = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $taskImages | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Output 'image-manifest.json') -Encoding utf8NoBOM
    $taskMetadata = [ordered]@{
        schema_version = 1
        completed_utc = [DateTime]::UtcNow.ToString('o')
        profile = 'dev'
        purpose = 'native GPU visual capture, not performance measurement'
        source_root = $crateRoot
        git_head = (& git rev-parse HEAD) -join ''
        toolchain = Get-DateTimePickerToolchain
        capture_context = $taskCaptureContext
        source_sha256 = $taskSources
        executable = @{ path = $taskExecutable; sha256 = $taskBinaryHash }
        metrics_sha256 = (Get-FileHash -LiteralPath (Join-Path $Output 'native-metrics.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $taskMetadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $Output 'native-provenance.json') -Encoding utf8NoBOM
} finally { Pop-Location }

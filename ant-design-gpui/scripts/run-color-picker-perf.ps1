# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([ValidateSet('windows-msvc-performance')][string]$Preset = 'windows-msvc-performance')
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $crateRoot
$cachePath = Join-Path $repositoryRoot "build/$Preset/CMakeCache.txt"
$cache = Get-Content -LiteralPath $cachePath -Raw
if ($cache -notmatch '(?m)^SNOW_APPS_BUILD_BENCHMARKS:BOOL=ON\r?$' -or $cache -notmatch '(?m)^CMAKE_CONFIGURATION_TYPES:(?:STRING|UNINITIALIZED)=Release\r?$') {
    throw 'The performance preset must enable benchmarks and use Release.'
}
$output = Join-Path $repositoryRoot "build/$Preset/ant-design-gpui"
New-Item -ItemType Directory -Path $output -Force | Out-Null
. (Join-Path $PSScriptRoot 'color-picker-validation-support.ps1')
Push-Location $crateRoot
try {
    $taskSources = Get-ColorPickerSourceFingerprint $crateRoot
    $taskBuildOutput = @(& cargo bench --locked --offline --features test-support --bench color_picker --target-dir $output --no-run --message-format=json)
    if ($LASTEXITCODE -ne 0) {
        foreach ($taskLine in $taskBuildOutput) {
            try { $taskDiagnostic = $taskLine | ConvertFrom-Json -AsHashtable; if ($taskDiagnostic.reason -eq 'compiler-message') { Write-Host $taskDiagnostic.message.rendered } } catch {}
        }
        throw 'ColorPicker performance fixture build failed.'
    }
    $taskArtifacts = @($taskBuildOutput | ForEach-Object { try { $_ | ConvertFrom-Json -AsHashtable } catch {} })
    $taskBinaries = @($taskArtifacts | Where-Object { $_.reason -eq 'compiler-artifact' -and $_.target.name -eq 'color_picker' -and $_.executable } | ForEach-Object { $_.executable })
    if ($taskBinaries.Count -ne 1) { throw 'Expected exactly one ColorPicker performance executable.' }
    Assert-ColorPickerSourceFingerprint $taskSources $crateRoot
    $taskExecutable = $taskBinaries[0]
    $taskBinaryHash = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
    & $taskExecutable | Tee-Object -FilePath (Join-Path $output 'color-picker-results.jsonl')
    if ($LASTEXITCODE -ne 0) { throw 'ColorPicker performance measurements failed.' }
    Assert-ColorPickerSourceFingerprint $taskSources $crateRoot
    $taskMetadata = [ordered]@{
        schema_version = 1
        completed_utc = [DateTime]::UtcNow.ToString('o')
        preset = $Preset
        profile = 'Release'
        measurement = 'CPU and allocations with deterministic offscreen GPUI; not GPU presentation or physical input latency'
        source_root = $crateRoot
        git_head = (& git rev-parse HEAD) -join ''
        toolchain = Get-ColorPickerToolchain
        source_sha256 = $taskSources
        executable = @{ path = $taskExecutable; sha256 = $taskBinaryHash }
        cmake_cache_sha256 = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash.ToLowerInvariant()
        results_sha256 = (Get-FileHash -LiteralPath (Join-Path $output 'color-picker-results.jsonl') -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $taskMetadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $output 'color-picker-provenance.json') -Encoding utf8NoBOM
} finally { Pop-Location }

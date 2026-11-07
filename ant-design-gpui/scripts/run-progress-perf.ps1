# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateSet('windows-msvc-performance')][string]$Preset = 'windows-msvc-performance',
    [switch]$Online
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $crateRoot
$cachePath = Join-Path $repositoryRoot "build/$Preset/CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath)) {
    throw "Configure the repository's $Preset preset before running performance measurements."
}
$cache = Get-Content -LiteralPath $cachePath -Raw
if ($cache -notmatch '(?m)^SNOW_APPS_BUILD_BENCHMARKS:BOOL=ON\r?$' -or
    $cache -notmatch '(?m)^CMAKE_CONFIGURATION_TYPES:(?:STRING|UNINITIALIZED)=Release\r?$') {
    throw "The $Preset build tree must enable benchmarks and use Release."
}
$output = Join-Path $repositoryRoot "build/$Preset/ant-design-gpui"
$cargoArguments = @('bench', '--locked', '--bench', 'progress', '--target-dir', $output)
if (-not $Online) { $cargoArguments += '--offline' }
$env:PROGRESS_PERF_OUTPUT = Join-Path $output 'progress-perf.json'
Push-Location $crateRoot
try {
    & cargo @cargoArguments
    if ($LASTEXITCODE -ne 0) { throw 'Progress performance measurements failed.' }
    $report = Get-Content -LiteralPath $env:PROGRESS_PERF_OUTPUT -Raw | ConvertFrom-Json
    $hashes = [ordered]@{}
    foreach ($file in @('Cargo.toml','Cargo.lock','benches/progress.rs') + @(Get-ChildItem -LiteralPath 'src/progress' -Filter '*.rs' | ForEach-Object { 'src/progress/' + $_.Name })) {
        $hashes[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $binary = Get-ChildItem -LiteralPath (Join-Path $output 'release/deps') -Filter 'progress-*.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $report | Add-Member -NotePropertyName provenance -NotePropertyValue ([ordered]@{
        utc=[DateTime]::UtcNow.ToString('O');preset=$Preset;rustc=(& rustc --version);cpu=(Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name);
        binary_sha256=(Get-FileHash -LiteralPath $binary.FullName -Algorithm SHA256).Hash.ToLowerInvariant();source_sha256=$hashes;
        timing='Offscreen CPU draw; process allocation counter is enabled during each sample.'
    })
    $json=($report | ConvertTo-Json -Depth 12).Replace("`r`n","`n")+"`n"
    [IO.File]::WriteAllText($env:PROGRESS_PERF_OUTPUT,$json,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $crateRoot 'fixtures/progress/performance-windows.json'),$json,[Text.UTF8Encoding]::new($false))
} finally { Pop-Location }

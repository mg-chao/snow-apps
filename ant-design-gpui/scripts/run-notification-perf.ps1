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
$cargoArguments = @('bench', '--locked', '--bench', 'notification', '--target-dir', $output)
if (-not $Online) { $cargoArguments += '--offline' }
Push-Location $crateRoot
try {
    $hashes = [ordered]@{}
    $sources = @('benches/notification.rs', 'scripts/run-notification-perf.ps1', 'Cargo.toml', 'Cargo.lock', 'src/root.rs', 'src/lib.rs', 'src/config.rs', 'src/behavior/text.rs', 'src/behavior/notice.rs') +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src/notification') -Filter '*.rs' | ForEach-Object { 'src/notification/' + $_.Name })
    foreach ($source in $sources) {
        $hashes[$source] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $source) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    & cargo @cargoArguments
    if ($LASTEXITCODE -ne 0) { throw 'Notification performance measurements failed.' }
    foreach ($source in $sources) {
        if ($hashes[$source] -ne (Get-FileHash -LiteralPath (Join-Path $crateRoot $source) -Algorithm SHA256).Hash.ToLowerInvariant()) {
            throw "A benchmark input changed during the run: $source"
        }
    }
    $rawPath = Join-Path $output 'release/deps/notification-performance.json'
    $raw = Get-Content -LiteralPath $rawPath -Raw | ConvertFrom-Json
    $binary = Get-ChildItem -LiteralPath (Join-Path $output 'release/deps') -Filter 'notification-*.exe' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $binary) { throw 'Notification benchmark executable was not found.' }
    $record = [ordered]@{
        schema_version = 1
        platform = 'windows-x64'
        preset = $Preset
        profile = 'release'
        utc = [DateTime]::UtcNow.ToString('O')
        cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name).Trim()
        rustc = ((& rustc --version) -join '').Trim()
        measurement = $raw.measurement
        native_executable = [ordered]@{ name = $binary.Name; sha256 = (Get-FileHash -LiteralPath $binary.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        source_sha256 = $hashes
        assertions = @('empty and static hosts request zero frames', 'active progress requests exactly one callback per controller', 'eight forced redraws share one frame callback', 'warm icon parse and raster misses remain unchanged', 'keyed replacement preserves 100 retained notices', 'destruction drains continuous work')
        measurements = $raw.measurements
        limitations = @('CPU layout and scene preparation rather than GPU frame times.', 'Diagnostic timings on this machine; no hardware-independent timing threshold.')
    }
    $json = ($record | ConvertTo-Json -Depth 20).Replace("`r`n", "`n")
    $summary = Join-Path $crateRoot 'fixtures/notification/performance-windows.json'
    [IO.File]::WriteAllText($summary, $json + "`n", [Text.UTF8Encoding]::new($false))
} finally { Pop-Location }

# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateSet('windows-msvc-performance')][string]$Preset = 'windows-msvc-performance',
    [switch]$Online,
    [string]$RepositoryRoot,
    [string]$SummaryPath
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent $crateRoot
}
$repositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
if ([string]::IsNullOrWhiteSpace($SummaryPath)) {
    $SummaryPath = Join-Path $crateRoot 'fixtures/carousel/performance-windows.json'
}
$summary = [IO.Path]::GetFullPath($SummaryPath)
New-Item -ItemType Directory -Path (Split-Path -Parent $summary) -Force | Out-Null
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
$cargoArguments = @('bench', '--locked', '--bench', 'carousel', '--features', 'test-support', '--target-dir', $output)
if (-not $Online) { $cargoArguments += '--offline' }
Push-Location $crateRoot
try {
    $hashes = [ordered]@{}
    function Get-InputSources {
        @('benches/carousel.rs','scripts/run-carousel-perf.ps1','Cargo.toml','Cargo.lock','README.md','rust-toolchain.toml','.cargo/config.toml') +
            @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src') -Recurse -File -Filter '*.rs' | Sort-Object FullName | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot,$_.FullName).Replace('\','/') }) +
            @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'resources') -Recurse -File | Sort-Object FullName | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot,$_.FullName).Replace('\','/') })
    }
    $sources = @(Get-InputSources)
    foreach ($source in $sources) {
        $hashes[$source] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $source) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    & cargo @cargoArguments
    if ($LASTEXITCODE -ne 0) { throw 'Carousel performance measurements failed.' }
    if (@(Compare-Object $sources @(Get-InputSources)).Count -ne 0) { throw 'Carousel benchmark input manifest changed during validation.' }
    foreach ($source in $sources) {
        if ($hashes[$source] -ne (Get-FileHash -LiteralPath (Join-Path $crateRoot $source) -Algorithm SHA256).Hash.ToLowerInvariant()) {
            throw "A benchmark input changed during the run: $source"
        }
    }
    $rawPath = Join-Path $output 'release/deps/carousel-performance.json'
    $raw = Get-Content -LiteralPath $rawPath -Raw | ConvertFrom-Json
    $binary = Get-ChildItem -LiteralPath (Join-Path $output 'release/deps') -Filter 'carousel-*.exe' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $binary) { throw 'Carousel benchmark executable was not found.' }
    $record = [ordered]@{
        schema_version = 1
        platform = 'windows-x64'
        source_root = $crateRoot
        preset = $Preset
        profile = 'release'
        utc = [DateTime]::UtcNow.ToString('O')
        cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name).Trim()
        rustc = ((& rustc --version) -join '').Trim()
        measurement = $raw.measurement
        native_executable = [ordered]@{ name = $binary.Name; sha256 = (Get-FileHash -LiteralPath $binary.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        source_sha256 = $hashes
        assertions = $raw.assertions
        measurements = $raw.measurements
        limitations = @('CPU layout and scene preparation rather than GPU frame times.', 'Diagnostic timings on this machine; no hardware-independent timing threshold.')
    }
    $json = ($record | ConvertTo-Json -Depth 20).Replace("`r`n", "`n")
    [IO.File]::WriteAllText($summary, $json + "`n", [Text.UTF8Encoding]::new($false))
} finally { Pop-Location }

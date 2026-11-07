# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([ValidateSet('windows-msvc-performance')][string]$Preset = 'windows-msvc-performance')
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $crateRoot
$cachePath = Join-Path $repositoryRoot "build/$Preset/CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath)) { throw "Configure the repository's $Preset preset before measuring Form performance." }
$cache = Get-Content -LiteralPath $cachePath -Raw
if ($cache -notmatch '(?m)^SNOW_APPS_BUILD_BENCHMARKS:BOOL=ON\r?$' -or $cache -notmatch '(?m)^CMAKE_CONFIGURATION_TYPES:(?:STRING|UNINITIALIZED)=Release\r?$') {
    throw 'Form benchmarks require the benchmark-enabled performance preset and Release.'
}
$output = Join-Path $repositoryRoot "build/$Preset/ant-design-gpui"
New-Item -ItemType Directory -Path $output -Force | Out-Null
function Get-FormPerformanceSources {
    $paths = @('benches/form.rs', 'scripts/run-form-perf.ps1', 'Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml', '.cargo/config.toml') +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src') -Recurse -File -Filter '*.rs' | Sort-Object FullName | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') }) +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'resources') -Recurse -File | Sort-Object FullName | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') })
    $hashes = [ordered]@{}
    foreach ($path in $paths) { $hashes[$path] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $path) -Algorithm SHA256).Hash.ToLowerInvariant() }
    return $hashes
}
function Assert-FormPerformanceSources($Expected) {
    $actual = Get-FormPerformanceSources
    if (@(Compare-Object @($Expected.Keys) @($actual.Keys)).Count -ne 0) { throw 'Form benchmark source manifest changed during validation.' }
    foreach ($path in $Expected.Keys) { if ($Expected[$path] -ne $actual[$path]) { throw "A benchmark input changed during the run: $path" } }
}
Push-Location $crateRoot
try {
    $sources = Get-FormPerformanceSources
    $buildOutput = @(& cargo bench --locked --offline --features test-support --bench form --target-dir $output --no-run --message-format=json)
    if ($LASTEXITCODE -ne 0) {
        foreach ($line in $buildOutput) { try { $diagnostic = $line | ConvertFrom-Json -AsHashtable; if ($diagnostic.reason -eq 'compiler-message') { Write-Host $diagnostic.message.rendered } } catch {} }
        throw 'Form performance fixture build failed.'
    }
    $artifacts = @($buildOutput | ForEach-Object { try { $_ | ConvertFrom-Json -AsHashtable } catch {} })
    $benchmarkArtifacts = @($artifacts | Where-Object { $_.reason -eq 'compiler-artifact' -and $_.target.name -eq 'form' -and $_.executable })
    if ($benchmarkArtifacts.Count -ne 1) { throw 'Expected one Form performance executable.' }
    $benchmarkArtifact = $benchmarkArtifacts[0]
    if ($benchmarkArtifact.profile.debug_assertions -or $benchmarkArtifact.profile.opt_level -ne '3') { throw 'Form performance measurements require an optimized Release artifact.' }
    Assert-FormPerformanceSources $sources
    $executable = $benchmarkArtifact.executable
    $processorBefore = Get-CimInstance Win32_Processor | Select-Object -First 1
    & $executable | Tee-Object -FilePath (Join-Path $output 'form-results.jsonl')
    if ($LASTEXITCODE -ne 0) { throw 'Form performance measurements failed.' }
    $processorAfter = Get-CimInstance Win32_Processor | Select-Object -First 1
    Assert-FormPerformanceSources $sources
    [ordered]@{
        schema_version = 1
        completed_utc = [DateTime]::UtcNow.ToString('o')
        preset = $Preset
        profile = 'Release'
        cargo_profile = $benchmarkArtifact.profile
        features = $benchmarkArtifact.features
        source_root = $crateRoot
        rustc = ((& rustc --version) -join '').Trim()
        cpu = $processorBefore.Name.Trim()
        logical_processors = [Environment]::ProcessorCount
        system_cpu_load_percent = @{ before = $processorBefore.LoadPercentage; after = $processorAfter.LoadPercentage }
        source_sha256 = $sources
        executable = @{ path = $executable; sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant() }
        cmake_cache_sha256 = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash.ToLowerInvariant()
        results_sha256 = (Get-FileHash -LiteralPath (Join-Path $output 'form-results.jsonl') -Algorithm SHA256).Hash.ToLowerInvariant()
        limitations = @('CPU and allocations with deterministic offscreen GPUI; not GPU presentation or physical input latency.', 'Diagnostic distributions on this machine; background system load is sampled, not isolated. No hardware-independent timing threshold.')
    } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $output 'form-provenance.json') -Encoding utf8NoBOM
} finally { Pop-Location }

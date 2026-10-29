# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([ValidateSet('windows-msvc-performance')][string]$Preset = 'windows-msvc-performance', [switch]$Online)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'image_build_support.ps1')
$crateRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $crateRoot
$cachePath = Join-Path $repositoryRoot "build/$Preset/CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath)) { throw "Configure $Preset before running performance measurements." }
$cache = Get-Content -LiteralPath $cachePath -Raw
if ($cache -notmatch '(?m)^SNOW_APPS_BUILD_BENCHMARKS:BOOL=ON\r?$' -or $cache -notmatch '(?m)^CMAKE_CONFIGURATION_TYPES:(?:STRING|UNINITIALIZED)=Release\r?$') {
    throw "$Preset must enable benchmarks and use Release."
}
$output = Join-Path $repositoryRoot "build/$Preset/ant-design-gpui"
New-Item -ItemType Directory -Path $output -Force | Out-Null
$resultsPath = Join-Path $output 'image-results.txt'
$startedUtc = [DateTime]::UtcNow.ToString('O')
function Get-ImageSourceHashes {
    $paths = @('Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml', '.cargo/config.toml', 'benches/image.rs', 'scripts/run-image-perf.ps1', 'scripts/image_build_support.ps1', 'src/lib.rs', 'src/root.rs', 'src/locale.rs', 'src/config.rs', 'resources/image/manifest.json')
    foreach ($directory in @('src/image', 'src/modal', 'src/icons', 'src/behavior', 'src/theme')) {
        $paths += Get-ChildItem -LiteralPath (Join-Path $crateRoot $directory) -Recurse -File -Filter '*.rs' | Where-Object { $_.Name -notmatch '(?:^|_)tests\.rs$' } | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') }
    }
    $paths += Get-ChildItem -LiteralPath (Join-Path $crateRoot 'resources/image') -File -Filter '*.png' | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') }
    $hashes = [ordered]@{}
    foreach ($path in ($paths | Sort-Object -Unique)) {
        if (Test-Path -LiteralPath (Join-Path $crateRoot $path)) { $hashes[$path] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $path) -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    return $hashes
}
$before = Get-ImageSourceHashes
$arguments = @('bench', '--locked', '--manifest-path', (Join-Path $crateRoot 'Cargo.toml'), '--bench', 'image', '--target-dir', $output, '--verbose')
if (-not $Online) { $arguments += '--offline' }
Push-Location $crateRoot
try {
    Reset-ImageVerificationFingerprints -CrateRoot $crateRoot -TargetDirectory $output -BuildProfile release -Kind test-bench -Name image
    & cargo @arguments 2>&1 | Tee-Object -FilePath $resultsPath
    if ($LASTEXITCODE -ne 0) { throw 'Image performance measurements failed.' }
    $linked = Get-ImageLinkedLibraryProvenance -BuildLog $resultsPath -CrateRoot $crateRoot
    $after = Get-ImageSourceHashes
    $executable = Get-ChildItem -LiteralPath (Join-Path $output 'release/deps') -Filter 'image-*.exe' -File | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($null -eq $executable) { throw 'The measured Image benchmark executable was not found.' }
    $provenance = [ordered]@{
        status = 'passed'; started_utc = $startedUtc; completed_utc = [DateTime]::UtcNow.ToString('O'); preset = $Preset; profile = 'bench (Release)'
        rustc = ((& rustc --version) -join ' '); cargo = ((& cargo --version) -join ' ')
        cpu = ((Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name) -join '; ').Trim()
        executable_sha256 = (Get-FileHash -LiteralPath $executable.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        results = [IO.Path]::GetRelativePath($crateRoot, $resultsPath).Replace('\', '/'); results_sha256 = (Get-FileHash -LiteralPath $resultsPath -Algorithm SHA256).Hash.ToLowerInvariant()
        sources_unchanged_during_run = (($before | ConvertTo-Json -Compress) -eq ($after | ConvertTo-Json -Compress)); source_sha256 = $after
        linked_library = $linked
    }
    if (-not $provenance.sources_unchanged_during_run) { throw 'Image sources changed during the measurement; rerun against stable sources.' }
    $provenance | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'image-provenance.json') -Encoding utf8
} finally { Pop-Location }

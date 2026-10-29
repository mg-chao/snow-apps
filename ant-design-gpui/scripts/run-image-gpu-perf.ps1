# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([ValidateSet('windows-msvc-performance')][string]$Preset = 'windows-msvc-performance', [switch]$Online)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'image_build_support.ps1')
$crateRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $crateRoot
$cachePath = Join-Path $repositoryRoot "build/$Preset/CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath)) { throw "Configure $Preset before native GPU measurements." }
$cache = Get-Content -LiteralPath $cachePath -Raw
if ($cache -notmatch '(?m)^SNOW_APPS_BUILD_BENCHMARKS:BOOL=ON\r?$' -or $cache -notmatch '(?m)^CMAKE_CONFIGURATION_TYPES:(?:STRING|UNINITIALIZED)=Release\r?$') {
    throw "$Preset must enable benchmarks and use Release."
}
$output = Join-Path $repositoryRoot "build/$Preset/ant-design-gpui"
[IO.Directory]::CreateDirectory($output) | Out-Null
$result = Join-Path $output 'image-gpu-results.json'
function Get-ImageGpuSources {
    $paths = @('Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml', '.cargo/config.toml', 'examples/image_gpu_perf.rs', 'scripts/run-image-gpu-perf.ps1', 'scripts/image_build_support.ps1', 'src/lib.rs', 'src/root.rs', 'src/config.rs', 'src/locale.rs', 'resources/image/manifest.json')
    foreach ($directory in @('src/image', 'src/modal', 'src/icons', 'src/behavior', 'src/theme')) {
        $paths += Get-ChildItem -LiteralPath (Join-Path $crateRoot $directory) -Recurse -File -Filter '*.rs' | Where-Object { $_.Name -notmatch '(?:^|_)tests\.rs$' } | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') }
    }
    $paths += Get-ChildItem -LiteralPath (Join-Path $crateRoot 'resources/image') -File -Filter '*.png' | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') }
    $hashes = [ordered]@{}
    foreach ($path in ($paths | Sort-Object -Unique)) { $hashes[$path] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $path) -Algorithm SHA256).Hash.ToLowerInvariant() }
    return $hashes
}
$before = Get-ImageGpuSources
$startedUtc = [DateTime]::UtcNow.ToString('O')
$arguments = @('build', '--locked', '--release', '--manifest-path', (Join-Path $crateRoot 'Cargo.toml'), '--example', 'image_gpu_perf', '--target-dir', $output, '--verbose')
if (-not $Online) { $arguments += '--offline' }
Push-Location $crateRoot
try {
    Reset-ImageVerificationFingerprints -CrateRoot $crateRoot -TargetDirectory $output -BuildProfile release -Kind example -Name image_gpu_perf
    $buildLog = Join-Path $output 'image-gpu-build.log'
    & cargo @arguments 2>&1 | Tee-Object -FilePath $buildLog
    if ($LASTEXITCODE -ne 0) { throw 'Native Image GPU benchmark build failed.' }
    $linked = Get-ImageLinkedLibraryProvenance -BuildLog $buildLog -CrateRoot $crateRoot
    $executable = Join-Path $output 'release/examples/image_gpu_perf.exe'
    & $executable $result
    if ($LASTEXITCODE -ne 0) { throw 'Native Image GPU benchmark failed.' }
    $after = Get-ImageGpuSources
    $unchanged = (($before | ConvertTo-Json -Compress) -eq ($after | ConvertTo-Json -Compress))
    $report = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
    if ($report.status -ne 'passed') { throw 'Native Image GPU benchmark did not report success.' }
    $provenance = [ordered]@{
        status = $(if ($unchanged) { 'passed' } else { 'sources_changed_during_run' })
        preset = $Preset; profile = 'Release'; started_utc = $startedUtc; completed_utc = [DateTime]::UtcNow.ToString('O')
        rustc = ((& rustc --version) -join ' ')
        cpu = ((Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name) -join '; ').Trim()
        windows = (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber)
        executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
        results_sha256 = (Get-FileHash -LiteralPath $result -Algorithm SHA256).Hash.ToLowerInvariant()
        sources_unchanged_during_run = $unchanged; source_sha256 = $before
        linked_library = $linked
        measurements = $report
    }
    $provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'image-gpu-provenance.json') -Encoding utf8
    if (-not $unchanged) { throw 'Sources changed during GPU benchmark; repeat after implementation settles.' }
} finally { Pop-Location }

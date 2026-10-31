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
New-Item -ItemType Directory -Path $output -Force | Out-Null
$resultsPath = Join-Path $output 'menu-results.txt'
$startedUtc = [DateTime]::UtcNow.ToString('O')
Set-Content -LiteralPath $resultsPath -Value ("Menu focused CPU/layout measurements: " + $startedUtc) -Encoding utf8
function Get-MenuSourceHashes {
    $paths = @('Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml', '.cargo/config.toml',
        'benches/menu.rs', 'scripts/run-menu-perf.ps1', 'src/popup/element.rs',
        'src/popup/mod.rs', 'src/popup/motion.rs', 'src/popup/runtime.rs')
    $paths += Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src/menu') -Recurse -File -Filter '*.rs' |
        Where-Object { $_.Name -notmatch '(?:^|_)tests\.rs$' } |
        ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') }
    $hashes = [ordered]@{}
    foreach ($path in ($paths | Sort-Object -Unique)) {
        $hashes[$path] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $path) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $hashes
}
$sourcesBefore = Get-MenuSourceHashes
$cargoArguments = @('bench', '--locked', '--bench', 'menu', '--target-dir', $output)
if (-not $Online) { $cargoArguments += '--offline' }
Push-Location $crateRoot
try {
    & cargo @cargoArguments | Tee-Object -FilePath $resultsPath -Append
    if ($LASTEXITCODE -ne 0) { throw 'Menu performance measurements failed.' }
    $sourcesAfter = Get-MenuSourceHashes
    $executable = Get-ChildItem -LiteralPath (Join-Path $output 'release/deps') -Filter 'menu-*.exe' -File |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($null -eq $executable) { throw 'The measured Menu benchmark executable was not found.' }
    $provenance = [ordered]@{
        status = 'passed'
        started_utc = $startedUtc
        completed_utc = [DateTime]::UtcNow.ToString('O')
        preset = $Preset
        profile = 'bench (Release)'
        rustc = ((& rustc --version) -join ' ')
        cargo = ((& cargo --version) -join ' ')
        cpu = ((Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name) -join '; ').Trim()
        executable = [IO.Path]::GetRelativePath($crateRoot, $executable.FullName).Replace('\', '/')
        executable_sha256 = (Get-FileHash -LiteralPath $executable.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        results = [IO.Path]::GetRelativePath($crateRoot, $resultsPath).Replace('\', '/')
        results_sha256 = (Get-FileHash -LiteralPath $resultsPath -Algorithm SHA256).Hash.ToLowerInvariant()
        sources_unchanged_during_run = (($sourcesBefore | ConvertTo-Json -Compress) -eq ($sourcesAfter | ConvertTo-Json -Compress))
        source_sha256 = $sourcesAfter
    }
    $provenance | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'menu-provenance.json') -Encoding utf8
} finally { Pop-Location }

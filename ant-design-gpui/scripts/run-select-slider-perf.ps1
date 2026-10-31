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
Push-Location $crateRoot
try {
    & cargo bench --locked --offline --bench select_slider --target-dir $output | Tee-Object -FilePath (Join-Path $output 'select-slider-results.jsonl')
    if ($LASTEXITCODE -ne 0) { throw 'Select/Slider performance measurements failed.' }
} finally { Pop-Location }

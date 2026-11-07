# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [string]$Output = '../build/ant-design-gpui/progress-native',
    [string]$Scene,
    [ValidateSet('default','dark','compact','dark-compact')][string]$Theme,
    [ValidateSet('ltr','rtl')][string]$Direction,
    [ValidateSet('1','1.5','2')][string]$Scale,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
Push-Location $crateRoot
try {
    $outputPath = [IO.Path]::GetFullPath($Output, $crateRoot)
    New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
    if (-not $SkipBuild) {
        & cargo build --locked --offline --example progress_visual
        if ($LASTEXITCODE -ne 0) { throw 'Progress capture build failed' }
    }
    $exe = [IO.Path]::GetFullPath('../build/ant-design-gpui/debug/examples/progress_visual.exe', $crateRoot)
    $arguments = @('--output',$outputPath)
    foreach ($option in @(@('scene',$Scene),@('theme',$Theme),@('direction',$Direction),@('scale',$Scale))) {
        if ($option[1]) { $arguments += @(('--'+$option[0]),$option[1]) }
    }
    $hashes = [ordered]@{}
    $files = @('Cargo.toml','Cargo.lock','src/lib.rs','src/config.rs','examples/progress_visual.rs','examples/progress_support/mod.rs','fixtures/progress/visual-cases.json')
    $files += @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src/progress') -Filter '*.rs' | ForEach-Object { 'src/progress/' + $_.Name })
    foreach ($file in $files) { $hashes[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    $binaryHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Progress native capture failed' }
    foreach ($file in $files) {
        if ($hashes[$file] -ne (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()) { throw "Source changed during capture: $file" }
    }
    $metadata = [ordered]@{utc=[DateTime]::UtcNow.ToString('O'); binary_sha256=$binaryHash; source_sha256=$hashes; arguments=$arguments; platform='windows'; mode='hidden-native-window'}
    [IO.File]::WriteAllText((Join-Path $outputPath 'capture-info.json'), ($metadata | ConvertTo-Json -Depth 8).Replace("`r`n","`n")+"`n", [Text.UTF8Encoding]::new($false))
} finally { Pop-Location }

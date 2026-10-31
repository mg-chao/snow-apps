# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [string]$Output,
    [string]$TargetDirectory,
    [string]$Scene,
    [string]$Theme,
    [ValidateSet('ltr', 'rtl')][string]$Direction,
    [ValidateSet('1', '1.5', '2')][string]$Scale
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = if ([string]::IsNullOrWhiteSpace($TargetDirectory)) {
    Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui'
} else { [IO.Path]::GetFullPath($TargetDirectory) }
if (-not $Output) { $Output = Join-Path $buildRoot 'carousel-native-current' }
$Output = [IO.Path]::GetFullPath($Output)
function Get-InputSources {
    @('Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml', '.cargo/config.toml', 'examples/carousel_visual.rs', 'fixtures/carousel/visual-cases.json', 'scripts/capture-carousel-visual.ps1') +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src') -Filter '*.rs' -Recurse -File | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') }) +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'resources') -Recurse -File | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') })
}
$sources = @(Get-InputSources)
function Get-SourceHashes {
    $result = [ordered]@{}
    foreach ($source in $sources) {
        $result[$source] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $source) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $result
}
Push-Location $crateRoot
try {
    $hashes = Get-SourceHashes
    & cargo build --locked --offline --features test-support --example carousel_visual --target-dir $buildRoot
    if ($LASTEXITCODE -ne 0) { throw 'Carousel visual fixture build failed.' }
    $after = Get-SourceHashes
    if (@(Compare-Object $sources @(Get-InputSources)).Count -ne 0) { throw 'Native capture input file set changed during compilation.' }
    foreach ($source in $sources) {
        if ($hashes[$source] -ne $after[$source]) { throw "A native build input changed during compilation: $source. Rebuild to bind captures to exact source." }
    }
    $executable = Join-Path $buildRoot 'debug/examples/carousel_visual.exe'
    New-Item -ItemType Directory -Path $Output -Force | Out-Null
    # Keep subsequent builds free to relink the shared target on Windows.
    $captureExecutable = Join-Path $Output 'carousel_visual.exe'
    Copy-Item -LiteralPath $executable -Destination $captureExecutable -Force
    $manifest = [ordered]@{
        schema_version = 1
        utc = [DateTime]::UtcNow.ToString('O')
        source_root = $crateRoot
        command = @('cargo', 'build', '--locked', '--offline', '--features', 'test-support', '--example', 'carousel_visual', '--target-dir', $buildRoot)
        rustc = ((& rustc --version) -join '').Trim()
        source_sha256 = $hashes
        native_executable = [ordered]@{ name = 'carousel_visual.exe'; sha256 = (Get-FileHash -LiteralPath $captureExecutable -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $snapshotManifest = Join-Path $crateRoot 'source-snapshot-manifest.json'
    if (Test-Path -LiteralPath $snapshotManifest) {
        $manifest['source_snapshot_manifest'] = [ordered]@{ path = $snapshotManifest; sha256 = (Get-FileHash -LiteralPath $snapshotManifest -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $arguments = @('--output', $Output)
    if ($Scene) { $arguments += @('--scene', $Scene) }
    if ($Theme) { $arguments += @('--theme', $Theme) }
    if ($Direction) { $arguments += @('--direction', $Direction) }
    if ($Scale) { $arguments += @('--scale', $Scale) }
    & $captureExecutable @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Carousel native GPU capture failed.' }
    $json = ($manifest | ConvertTo-Json -Depth 10).Replace("`r`n", "`n") + "`n"
    [IO.File]::WriteAllText((Join-Path $Output 'build-manifest.json'), $json, [Text.UTF8Encoding]::new($false))
} finally { Pop-Location }

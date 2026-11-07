# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [string]$Output = (Join-Path $PSScriptRoot '../../build/ant-design-gpui/image-native'),
    [switch]$SkipBuild,
    [string]$Scene,
    [ValidateSet('default', 'dark', 'compact', 'dark-compact')][string]$Theme,
    [ValidateSet('ltr', 'rtl')][string]$Direction,
    [ValidateSet('1', '1.5', '2')][string]$Scale
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'image_build_support.ps1')
$crateRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $crateRoot '../build/ant-design-gpui/debug/examples/image_visual.exe'
$Output = [IO.Path]::GetFullPath($Output)
[IO.Directory]::CreateDirectory($Output) | Out-Null
function Get-ImageCaptureSources {
    $paths = @('Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml', '.cargo/config.toml', 'examples/image_visual.rs', 'scripts/capture-image-native.ps1', 'scripts/image_build_support.ps1', 'fixtures/image/visual-cases.json', 'src/lib.rs', 'src/config.rs', 'src/locale.rs', 'src/root.rs', 'resources/image/manifest.json')
    foreach ($directory in @('src/image', 'src/modal', 'src/icons', 'src/behavior', 'src/theme')) {
        $paths += Get-ChildItem -LiteralPath (Join-Path $crateRoot $directory) -Recurse -File -Filter '*.rs' |
            Where-Object { $_.Name -notmatch '(?:^|_)tests\.rs$' } |
            ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') }
    }
    $paths += Get-ChildItem -LiteralPath (Join-Path $crateRoot 'resources/image') -File -Filter '*.png' |
        ForEach-Object { [IO.Path]::GetRelativePath($crateRoot, $_.FullName).Replace('\', '/') }
    $hashes = [ordered]@{}
    foreach ($path in ($paths | Sort-Object -Unique)) {
        $hashes[$path] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $path) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $hashes
}
$before = Get-ImageCaptureSources
$startedUtc = [DateTime]::UtcNow.ToString('O')
$buildStamp = [IO.Path]::ChangeExtension([IO.Path]::GetFullPath($executable), '.build-provenance.json')
Push-Location $crateRoot
try {
    if (-not $SkipBuild) {
        Reset-ImageVerificationFingerprints -CrateRoot $crateRoot -TargetDirectory (Join-Path $crateRoot '../build/ant-design-gpui') -BuildProfile debug -Kind example -Name image_visual
        $buildLog = Join-Path $Output 'native-build.log'
        & cargo build --locked --offline --manifest-path (Join-Path $crateRoot 'Cargo.toml') --example image_visual --verbose 2>&1 | Tee-Object -FilePath $buildLog
        if ($LASTEXITCODE -ne 0) { throw 'Image native capture build failed.' }
        $linked = Get-ImageLinkedLibraryProvenance -BuildLog $buildLog -CrateRoot $crateRoot
        $built = Get-ImageCaptureSources
        if (($before | ConvertTo-Json -Compress) -ne ($built | ConvertTo-Json -Compress)) {
            throw 'Sources changed while compiling the native harness; repeat after implementation settles.'
        }
        [ordered]@{
            executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
            source_sha256 = $built
            linked_library = $linked
        } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $buildStamp -Encoding utf8
    }
    if (-not (Test-Path -LiteralPath $executable)) { throw "Missing native capture executable: $executable" }
    if (-not (Test-Path -LiteralPath $buildStamp)) { throw 'No verified native build stamp. Run this capture script without -SkipBuild first.' }
    $stamp = Get-Content -LiteralPath $buildStamp -Raw | ConvertFrom-Json -AsHashtable
    if ($stamp.executable_sha256 -ne (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant() -or
        ($stamp.source_sha256 | ConvertTo-Json -Compress) -ne ($before | ConvertTo-Json -Compress)) {
        throw 'The native executable/build stamp does not match current source hashes. Run without -SkipBuild.'
    }
    if (-not $stamp.linked_library -or
        [IO.Path]::GetFullPath($stamp.linked_library.manifest_directory) -ne [IO.Path]::GetFullPath($crateRoot) -or
        (Get-FileHash -LiteralPath $stamp.linked_library.dependency_info -Algorithm SHA256).Hash.ToLowerInvariant() -ne $stamp.linked_library.dependency_info_sha256) {
        throw 'Native library compiler dependency identity is missing or changed. Run without -SkipBuild.'
    }
    $arguments = @('--output', $Output)
    foreach ($filter in @(@('scene', $Scene), @('theme', $Theme), @('direction', $Direction), @('scale', $Scale))) {
        if ($filter[1]) { $arguments += @(('--' + $filter[0]), $filter[1]) }
    }
    & $executable @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Native Image capture failed.' }
    $after = Get-ImageCaptureSources
    $unchanged = (($before | ConvertTo-Json -Compress) -eq ($after | ConvertTo-Json -Compress))
    $provenance = [ordered]@{
        status = $(if ($unchanged) { 'passed' } else { 'sources_changed_during_capture' })
        started_utc = $startedUtc; completed_utc = [DateTime]::UtcNow.ToString('O')
        executable = [IO.Path]::GetFullPath($executable)
        executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
        build_verified = $true
        build_stamp_sha256 = (Get-FileHash -LiteralPath $buildStamp -Algorithm SHA256).Hash.ToLowerInvariant()
        linked_library = $stamp.linked_library
        metrics_sha256 = (Get-FileHash -LiteralPath (Join-Path $Output 'native-metrics.json') -Algorithm SHA256).Hash.ToLowerInvariant()
        sources_unchanged_during_run = $unchanged; source_sha256 = $before
        filters = @{ scene = $Scene; theme = $Theme; direction = $Direction; scale = $Scale }
        rustc = ((& rustc --version) -join ' ')
    }
    $provenance | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Output 'native-provenance.json') -Encoding utf8
    if (-not $unchanged) { throw 'Sources changed during native capture. Repeat after implementation settles.' }
} finally { Pop-Location }

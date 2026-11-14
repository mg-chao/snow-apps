# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
# Shared verification helpers for targets that may have cached copied checkouts.
function Reset-ImageVerificationFingerprints {
    param([string]$CrateRoot, [string]$TargetDirectory, [ValidateSet('debug', 'release')][string]$BuildProfile,
        [ValidateSet('example', 'test-bench')][string]$Kind, [string]$Name)
    if ($Name -notmatch '^[a-z][a-z0-9_]*$') { throw 'Invalid verification target name.' }
    $crateRoot = [IO.Path]::GetFullPath($CrateRoot)
    $targetRoot = [IO.Path]::GetFullPath($TargetDirectory)
    $fingerprints = Join-Path $targetRoot "$BuildProfile/.fingerprint"
    if (-not (Test-Path -LiteralPath $fingerprints)) { return }
    foreach ($directory in Get-ChildItem -LiteralPath $fingerprints -Directory -Filter 'ant-design-gpui-*') {
        $libraryMarker = Join-Path $directory.FullName 'lib-ant_design_gpui'
        $libraryInfo = $libraryMarker + '.json'
        $markers = @()
        if ((Test-Path -LiteralPath $libraryMarker) -and (Test-Path -LiteralPath $libraryInfo)) {
            $metadata = Get-Content -LiteralPath $libraryInfo -Raw | ConvertFrom-Json
            if ($metadata.features -eq '[]') {
                $hash = $directory.Name.Substring('ant-design-gpui-'.Length)
                $dep = Join-Path $targetRoot "$BuildProfile/deps/ant_design_gpui-$hash.d"
                $bound = if (Test-Path -LiteralPath $dep) {
                    Select-String -LiteralPath $dep -Pattern '^# env-dep:CARGO_MANIFEST_DIR=(.+)$' | Select-Object -First 1
                }
                if (-not $bound -or [IO.Path]::GetFullPath($bound.Matches[0].Groups[1].Value) -ne $crateRoot) {
                    $markers += $libraryMarker
                }
            }
        }
        # Force the owned executable to relink, even if a copied checkout left a
        # colliding Cargo target fingerprint. Other example markers stay intact.
        $owned = Join-Path $directory.FullName "$Kind-$Name"
        if (Test-Path -LiteralPath $owned) { $markers += $owned }
        foreach ($marker in $markers) {
            $resolved = [IO.Path]::GetFullPath($marker)
            if (-not $resolved.StartsWith($targetRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Verification cache marker escaped the explicit target directory.'
            }
            Remove-Item -LiteralPath $resolved -Force
        }
    }
}

function Get-ImageLinkedLibraryProvenance {
    param([string]$BuildLog, [string]$CrateRoot)
    $text = Get-Content -LiteralPath $BuildLog -Raw
    $externMatches = [regex]::Matches($text, '--extern ant_design_gpui=(?:"([^"]+)"|(\S+))')
    if ($externMatches.Count -eq 0) { throw 'Build log does not prove an actual native executable relink.' }
    $externMatch = $externMatches[$externMatches.Count - 1]
    $library = if ($externMatch.Groups[1].Success) { $externMatch.Groups[1].Value } else { $externMatch.Groups[2].Value }
    $library = [IO.Path]::GetFullPath($library)
    $filename = [IO.Path]::GetFileName($library)
    if ($filename -notmatch '^libant_design_gpui-([a-f0-9]+)\.rlib$') { throw "Unexpected linked Image library: $filename" }
    $dep = Join-Path (Split-Path -Parent $library) ('ant_design_gpui-' + $Matches[1] + '.d')
    if (-not (Test-Path -LiteralPath $dep)) { throw "Missing actual compiler dependency info: $dep" }
    $bound = Select-String -LiteralPath $dep -Pattern '^# env-dep:CARGO_MANIFEST_DIR=(.+)$' | Select-Object -First 1
    if (-not $bound) { throw 'Linked library has no compiler-recorded manifest directory; force a fresh library build.' }
    $manifestDirectory = [IO.Path]::GetFullPath($bound.Matches[0].Groups[1].Value)
    if ($manifestDirectory -ne [IO.Path]::GetFullPath($CrateRoot)) { throw "Linked library came from another checkout: $manifestDirectory" }
    return [ordered]@{
        library = $library
        library_sha256 = (Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash.ToLowerInvariant()
        dependency_info = [IO.Path]::GetFullPath($dep)
        dependency_info_sha256 = (Get-FileHash -LiteralPath $dep -Algorithm SHA256).Hash.ToLowerInvariant()
        manifest_directory = $manifestDirectory
        build_log_sha256 = (Get-FileHash -LiteralPath $BuildLog -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Build and run only Image and its related native library/gallery regressions.
.DESCRIPTION
Verifies Cargo's actual compiler dep-info before executing an artifact. A stale
artifact from a source snapshot is rejected and rebuilt once in a source-bound
target directory. Shared caches and other chats' artifacts are never removed.
#>
[CmdletBinding()]
param([string]$TargetDirectory, [string]$OutputDirectory, [switch]$Online)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$crateRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$buildRoot = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui'
if ([string]::IsNullOrWhiteSpace($TargetDirectory)) { $TargetDirectory = $buildRoot }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $buildRoot 'image-current-core-verification'
}
$TargetDirectory = [IO.Path]::GetFullPath($TargetDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$receiptPath = Join-Path $OutputDirectory 'focused-tests.json'

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Get-RelativePath([string]$Path) {
    [IO.Path]::GetRelativePath($crateRoot, [IO.Path]::GetFullPath($Path)).Replace('\', '/')
}
function Get-SourcePaths {
    @('Cargo.toml', 'Cargo.lock', 'README.md', 'rust-toolchain.toml', '.cargo/config.toml',
        'scripts/test-image.ps1') +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src') -Recurse -File |
            ForEach-Object { Get-RelativePath $_.FullName }) +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'examples/gallery') -Recurse -File |
            ForEach-Object { Get-RelativePath $_.FullName }) +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'resources') -Recurse -File |
            ForEach-Object { Get-RelativePath $_.FullName }) +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'fixtures') -Recurse -File |
            # Validation outputs may be refreshed by parallel native/Release
            # runs. Reference fixtures remain guarded compiler/test inputs.
            Where-Object {
                $_.Name -notmatch '(?i)(?:validation|provenance|report).*\.json$' -and
                (Get-RelativePath $_.FullName) -notmatch '^fixtures/image/(?:gpu-)?performance-windows\.json$'
            } |
            ForEach-Object { Get-RelativePath $_.FullName }) |
        Sort-Object -Unique
}
function Get-SourceHashes {
    $hashes = [ordered]@{}
    foreach ($path in (Get-SourcePaths)) { $hashes[$path] = Get-Sha256 (Join-Path $crateRoot $path) }
    return $hashes
}
function Assert-SourceHashes {
    $current = Get-SourceHashes
    if (($sourceHashes | ConvertTo-Json -Compress) -cne ($current | ConvertTo-Json -Compress)) {
        $allPaths = @($sourceHashes.Keys) + @($current.Keys) | Sort-Object -Unique
        $changed = @($allPaths | Where-Object {
            -not $sourceHashes.Contains($_) -or -not $current.Contains($_) -or $sourceHashes[$_] -cne $current[$_]
        })
        $result.source_sha256_after = $current
        $result.changed_inputs = $changed
        throw "Compiler/test inputs changed during Image validation: $($changed -join ', '). Repeat against stable sources."
    }
}
function Test-SamePath([string]$Left, [string]$Right) {
    [IO.Path]::GetFullPath($Left).TrimEnd('\', '/').Equals(
        [IO.Path]::GetFullPath($Right).TrimEnd('\', '/'), [StringComparison]::OrdinalIgnoreCase)
}
function Assert-Within([string]$Path, [string]$Root) {
    $relative = [IO.Path]::GetRelativePath([IO.Path]::GetFullPath($Root), [IO.Path]::GetFullPath($Path))
    if ([IO.Path]::IsPathRooted($relative) -or $relative -eq '..' -or
        $relative.StartsWith('../') -or $relative.StartsWith('..\')) {
        throw "Artifact/compiler input is outside its expected source or target directory: $Path"
    }
}
function Invoke-CheckedBuild([string]$Directory, [int]$Attempt) {
    $log = Join-Path $OutputDirectory "build-$Attempt.jsonl"
    $arguments = @('test', '--locked', '--no-run', '--lib', '--example', 'gallery',
        '--target-dir', $Directory, '--message-format', 'json-render-diagnostics')
    if (-not $Online) { $arguments += '--offline' }
    Write-Host "Building focused Image test artifacts (attempt $Attempt)."
    & cargo @arguments 2>&1 | Tee-Object -FilePath $log | ForEach-Object {
        if (-not ([string]$_).TrimStart().StartsWith('{')) { Write-Host $_ }
    }
    $exitCode = $LASTEXITCODE
    $messages = @(Get-Content -LiteralPath $log | Where-Object { $_.TrimStart().StartsWith('{') } |
        ForEach-Object { ConvertFrom-Json -InputObject $_ })
    if ($exitCode -ne 0) {
        foreach ($message in $messages) {
            if ($message.reason -eq 'compiler-message' -and $message.message.rendered) {
                Write-Host $message.message.rendered
            }
        }
        throw "Focused Image test build failed; see $log"
    }
    Assert-SourceHashes
    return @($messages | Where-Object { $_.reason -eq 'compiler-artifact' })
}
function Get-ArtifactDepInfo($Artifact, [bool]$Library) {
    if ($Artifact.executable) {
        $stem = [IO.Path]::GetFileNameWithoutExtension($Artifact.executable)
        return Join-Path (Split-Path -Parent $Artifact.executable) ($stem + '.d')
    }
    $libraryFile = @($Artifact.filenames | Where-Object { $_.EndsWith('.rlib') }) | Select-Object -First 1
    if (-not $Library -or -not $libraryFile) { throw 'Cargo did not report the expected native library artifact.' }
    $stem = [IO.Path]::GetFileNameWithoutExtension($libraryFile) -replace '^lib', ''
    return Join-Path (Split-Path -Parent $libraryFile) ($stem + '.d')
}
function Assert-CompilerProvenance($Artifact, [bool]$Library, [bool]$RequireManifest, [string]$Directory) {
    $expectedEntry = if ($Library) { 'src/lib.rs' } else { 'examples/gallery/main.rs' }
    if (-not (Test-SamePath $Artifact.target.src_path (Join-Path $crateRoot $expectedEntry))) {
        throw "Cargo artifact entry point belongs to another checkout: $($Artifact.target.src_path)"
    }
    foreach ($filename in $Artifact.filenames) { Assert-Within $filename $Directory }
    $depInfo = Get-ArtifactDepInfo $Artifact $Library
    if (-not (Test-Path -LiteralPath $depInfo -PathType Leaf)) { throw "Missing compiler dep-info: $depInfo" }
    $text = Get-Content -LiteralPath $depInfo -Raw
    $environment = [regex]::Match($text, '(?m)^# env-dep:CARGO_MANIFEST_DIR=(?<root>[^\r\n]+)\r?$')
    $manifestRoot = $null
    if ($environment.Success) {
        # Rust dep-info escapes backslashes in environment values, but not in
        # ordinary Windows Makefile paths. Decode this record separately.
        $manifestRoot = $environment.Groups['root'].Value.Replace('\\', '\')
        if (-not (Test-SamePath $manifestRoot $crateRoot)) {
            throw "Compiler CARGO_MANIFEST_DIR belongs to another checkout: $manifestRoot"
        }
    } elseif ($RequireManifest) {
        throw "Compiler dep-info has no source-root environment dependency: $depInfo"
    }
    $inputs = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $logical = $text -replace '\\\r?\n', ''
    foreach ($line in ($logical -split '\r?\n')) {
        if ($line.StartsWith('#')) { continue }
        $dependencyLine = [regex]::Match($line, '^.+?:\s+(?<inputs>.+)$')
        if (-not $dependencyLine.Success) { continue }
        foreach ($word in ([regex]::Split($dependencyLine.Groups['inputs'].Value.Trim(), '(?<!\\)\s+'))) {
            if ([string]::IsNullOrWhiteSpace($word)) { continue }
            $decoded = $word.Replace('\ ', ' ').Replace('\#', '#').Replace('$$', '$')
            $path = if ([IO.Path]::IsPathRooted($decoded)) { [IO.Path]::GetFullPath($decoded) }
                else { [IO.Path]::GetFullPath((Join-Path $crateRoot $decoded)) }
            Assert-Within $path $crateRoot
            $relative = Get-RelativePath $path
            if (-not $sourceHashes.Contains($relative)) {
                throw "Compiler input was not captured before building: $relative"
            }
            if ($sourceHashes[$relative] -ne (Get-Sha256 $path)) {
                throw "Compiler input changed since the guarded snapshot: $relative"
            }
            $null = $inputs.Add($relative)
        }
    }
    $required = if ($Library) {
        @('src/lib.rs', 'src/image/preview.rs', 'src/modal/host.rs', 'src/config.rs',
            'src/locale.rs', 'src/behavior/accessibility.rs')
    } else { @('examples/gallery/main.rs', 'examples/gallery/image.rs', 'examples/gallery/tests/image.rs') }
    foreach ($path in $required) {
        if (-not $inputs.Contains($path)) { throw "Compiler dep-info omits current required source $path : $depInfo" }
    }
    return [ordered]@{
        source_root = $crateRoot; cargo_manifest_dir = $manifestRoot
        entry_point = Get-RelativePath $Artifact.target.src_path
        dep_info = $depInfo; dep_info_sha256 = Get-Sha256 $depInfo
        compiler_inputs = @($inputs | Sort-Object); fresh = $Artifact.fresh
    }
}
function Invoke-Filter([string]$Executable, [string]$Filter, [string]$Name) {
    $log = Join-Path $OutputDirectory ($Name + '.log')
    Write-Host "Running only $Filter"
    & $Executable $Filter '--test-threads=1' 2>&1 | Tee-Object -FilePath $log | Out-Host
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) { throw "Image-related filter failed: $Filter (see $log)" }
    $summary = [regex]::Match((Get-Content -LiteralPath $log -Raw),
        'test result: ok\. (?<passed>\d+) passed; (?<failed>\d+) failed; (?<ignored>\d+) ignored; (?<measured>\d+) measured; (?<filtered>\d+) filtered out;')
    if (-not $summary.Success -or [int]$summary.Groups['passed'].Value -eq 0) {
        throw "The requested Image-related filter did not execute any tests: $Filter"
    }
    return [ordered]@{
        filter = $Filter; passed = [int]$summary.Groups['passed'].Value
        failed = [int]$summary.Groups['failed'].Value; ignored = [int]$summary.Groups['ignored'].Value
        filtered = [int]$summary.Groups['filtered'].Value; log = $log; log_sha256 = Get-Sha256 $log
    }
}

$sourceHashes = Get-SourceHashes
$result = [ordered]@{
    schema_version = 1; status = 'running'; started_utc = [DateTime]::UtcNow.ToString('O')
    source_root = $crateRoot; source_sha256 = $sourceHashes; provenance_rebuilds = @(); results = @()
}
Push-Location $crateRoot
try {
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        $artifacts = @(Invoke-CheckedBuild $TargetDirectory $attempt)
        $libraryTest = $artifacts | Where-Object { $_.target.name -eq 'ant_design_gpui' -and $_.profile.test -and $_.executable } | Select-Object -Last 1
        $normalLibrary = $artifacts | Where-Object { $_.target.name -eq 'ant_design_gpui' -and -not $_.profile.test } | Select-Object -Last 1
        $galleryTest = $artifacts | Where-Object { $_.target.name -eq 'gallery' -and $_.profile.test -and $_.executable } | Select-Object -Last 1
        if (-not $libraryTest -or -not $normalLibrary -or -not $galleryTest) { throw 'Cargo did not report all three requested library/gallery artifacts.' }
        try {
            $libraryCompiler = Assert-CompilerProvenance $libraryTest $true $true $TargetDirectory
            $normalCompiler = Assert-CompilerProvenance $normalLibrary $true $true $TargetDirectory
            $galleryCompiler = Assert-CompilerProvenance $galleryTest $false $false $TargetDirectory
            break
        } catch {
            $result.provenance_rebuilds += [ordered]@{ target_directory = $TargetDirectory; rejected_reason = $_.Exception.Message }
            if ($attempt -eq 2) { throw }
            $rootBytes = [Text.Encoding]::UTF8.GetBytes($crateRoot.ToLowerInvariant())
            $rootHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($rootBytes)).ToLowerInvariant().Substring(0, 12)
            $TargetDirectory = Join-Path $OutputDirectory ("source-bound-target-" + $rootHash)
            Write-Host 'Rejected stale compiler provenance. Rebuilding once in an isolated target; shared caches are preserved.'
        }
    }
    Assert-SourceHashes
    $libraryHash = Get-Sha256 $libraryTest.executable
    $galleryHash = Get-Sha256 $galleryTest.executable
    $result.artifacts = [ordered]@{
        library_test = [ordered]@{ executable = $libraryTest.executable; sha256 = $libraryHash; compiler = $libraryCompiler }
        library = [ordered]@{ files = @($normalLibrary.filenames); compiler = $normalCompiler }
        gallery_test = [ordered]@{ executable = $galleryTest.executable; sha256 = $galleryHash; compiler = $galleryCompiler }
    }
    foreach ($filter in @('image::', 'modal::tests::', 'config::', 'locale::', 'root::', 'behavior::', 'icons::')) {
        $result.results += Invoke-Filter $libraryTest.executable $filter (($filter -split '::')[0] + '-guarded-tests')
    }
    $result.results += Invoke-Filter $galleryTest.executable 'image_gallery' 'image-gallery-guarded-tests'
    Assert-SourceHashes
    if ($libraryHash -ne (Get-Sha256 $libraryTest.executable) -or $galleryHash -ne (Get-Sha256 $galleryTest.executable)) {
        throw 'A test executable was replaced during validation.'
    }
    foreach ($compiler in @($libraryCompiler, $normalCompiler, $galleryCompiler)) {
        if ($compiler.dep_info_sha256 -ne (Get-Sha256 $compiler.dep_info)) {
            throw "Compiler dep-info was replaced during validation: $($compiler.dep_info)"
        }
    }
    $result.sources_unchanged_during_run = $true
    $result.status = 'passed'
    Write-Host 'Focused Image validation passed with source-bound compiler evidence.'
} catch {
    $result.status = 'failed'; $result.error = $_.Exception.Message
    throw
} finally {
    $result.completed_utc = [DateTime]::UtcNow.ToString('O')
    $result.target_directory = $TargetDirectory
    $json = ($result | ConvertTo-Json -Depth 12).Replace("`r`n", "`n") + "`n"
    [IO.File]::WriteAllText($receiptPath, $json, [Text.UTF8Encoding]::new($false))
    Pop-Location
}

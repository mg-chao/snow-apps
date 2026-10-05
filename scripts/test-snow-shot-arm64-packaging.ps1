#Requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-ocr-release-runtime.ps1')
. (Join-Path $PSScriptRoot 'test-snow-shot-native-package.ps1') -FunctionsOnly
$repo = Split-Path -Parent $PSScriptRoot
$root = Join-Path $repo ("build/arm64-packaging-tests-" + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root

function New-TestPe([string]$Path, [int]$Machine) {
    $bytes = [byte[]]::new(128)
    [BitConverter]::GetBytes([uint16]0x5a4d).CopyTo($bytes, 0)
    [BitConverter]::GetBytes([uint32]64).CopyTo($bytes, 0x3c)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($bytes, 64)
    [BitConverter]::GetBytes([uint16]$Machine).CopyTo($bytes, 68)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

function New-TestRuntime([string]$Architecture, [string]$Case) {
    $stage = Join-Path $root "$Architecture-$Case"
    $null = New-Item -ItemType Directory -Path $stage
    $worker = "snow-ocr-process-1.0.9-windows-$Architecture.exe"
    $machine = if ($Architecture -eq 'arm64') { 0xaa64 } else { 0x8664 }
    if ($Case -eq 'wrong-machine') { $machine = if ($Architecture -eq 'arm64') { 0x8664 } else { 0xaa64 } }
    foreach ($name in @($worker, 'DirectML.dll')) { New-TestPe (Join-Path $stage $name) $machine }
    $files = @($worker, 'DirectML.dll') | ForEach-Object {
        $path = Join-Path $stage $_
        [ordered]@{ name = $_; size = (Get-Item -LiteralPath $path).Length
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    if ($Case -eq 'wrong-file-hash') { $files[0].sha256 = '0' * 64 }
    $platform = if ($Case -eq 'wrong-platform') { 'windows-unknown' } else { "windows-$Architecture" }
    [ordered]@{ schema = 1; version = '1.0.9'; platform = $platform; protocol = 5; files = $files } |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage 'runtime-manifest.json') -Encoding utf8NoBOM
    if ($Case -eq 'unexpected-file') { [IO.File]::WriteAllText((Join-Path $stage 'extra.dll'), 'unexpected') }
    $archive = "$stage.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive)
    return $archive
}

try {
    $original = Get-Content -LiteralPath (Join-Path $repo 'snow_shot/packaging/snow-shot-ocr-asset-manifest.json') -Raw | ConvertFrom-Json
    foreach ($architecture in @('x64', 'arm64')) {
        foreach ($case in @('valid', 'wrong-machine', 'wrong-file-hash', 'wrong-platform', 'unexpected-file')) {
            $archive = New-TestRuntime $architecture $case
            $output = Join-Path $root "$architecture-$case.json"
            $rejected = $false
            try { $null = New-SnowOcrAssetManifest -ArchivePath $archive -Architecture $architecture -OutputPath $output }
            catch { $rejected = $true }
            if ($case -eq 'valid') {
                if ($rejected) { throw "Valid $architecture runtime descriptor was rejected." }
                $manifest = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
                if ($manifest.runtime.platform -cne "windows-$architecture" -or
                    $manifest.runtime.archive.sha256 -cne (Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant() -or
                    ($manifest.models | ConvertTo-Json -Depth 10 -Compress) -cne ($original.models | ConvertTo-Json -Depth 10 -Compress)) {
                    throw 'Generated architecture descriptor changed trusted model hashes or misrepresented the actual runtime.'
                }
            } elseif (-not $rejected -or (Test-Path -LiteralPath $output)) {
                throw "Invalid OCR runtime produced a trusted descriptor: $architecture/$case"
            }
            Write-Output "PASS: $architecture/$case"
        }
        $pinned = Join-Path $root "$architecture-valid.json"
        $archive = Join-Path $root "$architecture-valid.zip"
        $output = Join-Path $root "$architecture-resolved.json"
        $missingPin = Join-Path $root "$architecture-not-prepared.json"
        $rejected = $false
        try { $null = Resolve-SnowOcrAssetManifest -ArchivePath $archive -Architecture $architecture `
            -PinnedManifestPath $missingPin -OutputPath $output }
        catch { $rejected = $true }
        if (-not $rejected -or (Test-Path -LiteralPath $output)) { throw 'Ordinary local import generated a new trust anchor.' }
        Copy-Item -LiteralPath $pinned -Destination $output
        $resolved = Resolve-SnowOcrAssetManifest -ArchivePath $archive -Architecture $architecture `
            -PinnedManifestPath $missingPin -OutputPath $output
        if ($resolved -cne $output) { throw 'Prepared immutable local runtime pins were not selected.' }
        Remove-Item -LiteralPath $output
        $resolved = Resolve-SnowOcrAssetManifest -ArchivePath $archive -Architecture $architecture `
            -PinnedManifestPath $pinned -OutputPath $output
        if ($resolved -cne $pinned -or (Test-Path -LiteralPath $output)) { throw 'Packaging replaced an existing trusted OCR descriptor.' }
        $oldHash = (Get-FileHash -LiteralPath $pinned).Hash
        $input = [IO.File]::Open($archive, [IO.FileMode]::Append)
        try { $input.WriteByte(1) } finally { $input.Dispose() }
        $rejected = $false
        try { $null = Resolve-SnowOcrAssetManifest -ArchivePath $archive -Architecture $architecture -PinnedManifestPath $pinned -OutputPath $output }
        catch { $rejected = $true }
        if (-not $rejected -or (Get-FileHash -LiteralPath $pinned).Hash -cne $oldHash -or (Test-Path -LiteralPath $output)) {
            throw 'A supplied runtime bypassed the existing architecture trust anchor.'
        }
        Write-Output "PASS: $architecture/pinned-archive-mismatch"
        Write-Output "PASS: $architecture/prepared-local-pins-required"
    }
    foreach ($case in @('valid', 'entry-hash', 'traversal', 'symlink', 'nonempty')) {
        $archivePath = Join-Path $root "native-$case.zip"
        $payload = [Text.Encoding]::UTF8.GetBytes('native archive fixture')
        $name = if ($case -eq 'traversal') { '../escape' } else { 'bin/fixture.txt' }
        $stream = [IO.File]::Open($archivePath, [IO.FileMode]::CreateNew)
        $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            $entry = $zip.CreateEntry($name)
            if ($case -eq 'symlink') { $entry.ExternalAttributes = -1577058304 }
            $output = $entry.Open()
            try { $output.Write($payload) } finally { $output.Dispose() }
        } finally { $zip.Dispose(); $stream.Dispose() }
        $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($payload)).ToLowerInvariant()
        if ($case -eq 'entry-hash') { $hash = '0' * 64 }
        $manifest = [pscustomobject]@{
            Archive = [pscustomobject]@{ Path = [IO.Path]::GetFileName($archivePath); Bytes = (Get-Item $archivePath).Length
                Sha256 = (Get-FileHash $archivePath).Hash.ToLowerInvariant() }
            InstallFiles = @([pscustomobject]@{ Path = $name; Bytes = $payload.Length; Sha256 = $hash })
        }
        $destination = Join-Path $root "native-$case"
        $null = New-Item -ItemType Directory -Path $destination
        if ($case -eq 'nonempty') { [IO.File]::WriteAllText((Join-Path $destination 'keep.txt'), 'preserved') }
        $rejected = $false
        try { Expand-SnowNativePackage -ArchivePath $archivePath -Manifest $manifest -Destination $destination }
        catch { $rejected = $true }
        if ($case -eq 'valid') {
            if ($rejected -or [IO.File]::ReadAllText((Join-Path $destination $name)) -cne 'native archive fixture') {
                throw 'Valid native package extraction failed.'
            }
        } else {
            $count = if ($case -eq 'nonempty') { 1 } else { 0 }
            if (-not $rejected -or @(Get-ChildItem -LiteralPath $destination -Recurse -File).Count -ne $count) {
                throw 'Native package extraction accepted unsafe bytes or changed files before completing validation.'
            }
        }
        Write-Output "PASS: native-extraction/$case"
    }
    $hostArchitecture = Get-SnowWindowsHostArchitecture
    $crossArchitecture = if ($hostArchitecture -ceq 'arm64') { 'x64' } else { 'arm64' }
    $crossWorker = Join-Path $root 'missing-foreign-worker.exe'
    $crossArguments = @{ Architecture = $crossArchitecture; Executable = $crossWorker
        Detector = 'not-downloaded-detector'; Recognizer = 'not-downloaded-recognizer'; Dictionary = 'not-downloaded-dictionary' }
    if (Invoke-SnowOcrModelSetValidation @crossArguments) { throw 'Cross model validation claimed native execution.' }
    $rejected = $false
    try { $null = Invoke-SnowOcrModelSetValidation @crossArguments -RequireNative } catch { $rejected = $true }
    if (-not $rejected) { throw 'Native-only model validation accepted a cross host.' }
    $rejected = $false
    try { Invoke-SnowNativePackageProbe -Stage (Join-Path $root 'missing-cross-stage') -Product snow-shot -TargetArchitecture $crossArchitecture -Version '1.2.3' }
    catch { $rejected = $_.Exception.Message -like 'Native OCR validation requires*' }
    if (-not $rejected) { throw 'Exact-package proof inspected or executed a target before rejecting the cross host.' }
    Write-Output 'PASS: cross-host model validation performs no target execution and cannot issue native proof'
    $null = Add-SnowMsvcToolsToPath -Architecture $hostArchitecture
    $probeStage = Join-Path $root ('native probe ' + [char]0x96ea)
    $bin = Join-Path $probeStage 'bin'
    $null = New-Item -ItemType Directory -Path $bin
    $fixture = Join-Path $bin 'snow_shot.exe'
    & cl /nologo /std:c++20 /W4 /WX /O2 /MT "/Fe:$fixture" "/Fo:$root/native-probe.obj" `
        (Join-Path $repo 'snow_shot/tests/native_package_probe_fixture.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Native probe fixture compilation failed.' }
    Copy-Item -LiteralPath $fixture -Destination (Join-Path $bin 'snow-shot-updater.exe')
    Copy-Item -LiteralPath $fixture -Destination (Join-Path $bin 'snow-ocr-process.exe')
    $assets = Join-Path $bin 'assets/ocr'
    $runtime = Join-Path $assets "runtimes/1.0.9/windows-$hostArchitecture"
    $model = Join-Path $assets 'models/fixture-small'
    $null = New-Item -ItemType Directory -Path $runtime, $model -Force
    Copy-Item -LiteralPath $fixture -Destination (Join-Path $runtime "snow-ocr-process-1.0.9-windows-$hostArchitecture.exe")
    foreach ($name in @('detector', 'recognizer', 'dictionary')) {
        [IO.File]::WriteAllText((Join-Path $model "$name.txt"), "valid $name", [Text.UTF8Encoding]::new($false))
    }
    $packagedAssets = [ordered]@{ schema = 2; default_model = 'small'
        runtime = @{ version = '1.0.9'; platform = "windows-$hostArchitecture" }
        models = @(@{ type = 'small'; id = 'fixture-small'; detector = 'detector.txt'
            recognizer = 'recognizer.txt'; dictionary = 'dictionary.txt'
            files = @(foreach ($name in @('detector', 'recognizer', 'dictionary')) {
                $path = Join-Path $model "$name.txt"
                @{ name = "$name.txt"; size = (Get-Item -LiteralPath $path).Length
                    sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
            }) }) }
    $assetManifest = Join-Path $assets 'asset-manifest.json'
    $packagedAssets | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $assetManifest -Encoding utf8NoBOM
    Invoke-SnowNativePackageProbe -Stage $probeStage -Product snow-shot -TargetArchitecture $hostArchitecture -Version '1.2.3'
    [IO.File]::WriteAllText((Join-Path $model 'dictionary.txt'), 'invalid dictionary')
    $rejected = $false
    try { Invoke-SnowNativePackageProbe -Stage $probeStage -Product snow-shot -TargetArchitecture $hostArchitecture -Version '1.2.3' }
    catch { $rejected = $_.Exception.Message -ceq 'Packaged OCR model bytes differ from the trusted asset manifest.' }
    if (-not $rejected) { throw 'Native package proof accepted model bytes that differ from packaged pins.' }
    $dictionaryPath = Join-Path $model 'dictionary.txt'
    $packagedAssets.models[0].files[2].size = (Get-Item -LiteralPath $dictionaryPath).Length
    $packagedAssets.models[0].files[2].sha256 = (Get-FileHash -LiteralPath $dictionaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $packagedAssets | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $assetManifest -Encoding utf8NoBOM
    $rejected = $false
    try { Invoke-SnowNativePackageProbe -Stage $probeStage -Product snow-shot -TargetArchitecture $hostArchitecture -Version '1.2.3' }
    catch { $rejected = $_.Exception.Message -like 'Native OCR model initialization failed*' }
    if (-not $rejected) { throw 'Native package proof accepted models rejected by the bundled worker.' }
    [IO.File]::WriteAllText((Join-Path $model 'dictionary.txt'), 'valid dictionary')
    $packagedAssets.models[0].files[2].size = (Get-Item -LiteralPath $dictionaryPath).Length
    $packagedAssets.models[0].files[2].sha256 = (Get-FileHash -LiteralPath $dictionaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $packagedAssets | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $assetManifest -Encoding utf8NoBOM
    Remove-Item -LiteralPath (Join-Path $model 'recognizer.txt')
    $rejected = $false
    try { Invoke-SnowNativePackageProbe -Stage $probeStage -Product snow-shot -TargetArchitecture $hostArchitecture -Version '1.2.3' }
    catch { $rejected = $_.Exception.Message -like 'OCR model validation input is missing*' }
    if (-not $rejected) { throw 'Native package proof accepted an incomplete model set.' }
    [IO.File]::WriteAllText((Join-Path $model 'recognizer.txt'), 'valid recognizer')
    Write-Output 'PASS: packaged model initialization and pinned/rejected/missing model failure gates'
    $rejected = $false
    try { Invoke-SnowNativePackageProbe -Stage $probeStage -Product snow-shot -TargetArchitecture $hostArchitecture -Version '9.9.9' }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'Native package validation accepted a failed executable startup probe.' }
    Write-Output 'PASS: native startup/helper/OCR probes and startup failure rejection'
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    $allowed = [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup directory.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

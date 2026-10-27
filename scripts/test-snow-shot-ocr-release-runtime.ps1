[CmdletBinding()]
param([ValidateSet('x64', 'arm64')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-ocr-release-runtime.ps1')
$testRoot = Join-Path (Split-Path -Parent $PSScriptRoot) "build/ocr-runtime-import-tests-$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $testRoot
$names = @("snow-ocr-process-1.2.0-windows-$Architecture.exe", 'runtime-manifest.json')
$payload = [Text.Encoding]::UTF8.GetBytes('immutable fixture')
$payloadHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($payload)).ToLowerInvariant()
foreach ($case in @('valid', 'archive-hash', 'archive-size', 'entry-hash', 'entry-size', 'duplicate', 'traversal', 'case-collision', 'symlink', 'directory', 'missing', 'nonempty')) {
    $archivePath = Join-Path $testRoot "$case.zip"
    $destination = Join-Path $testRoot $case
    $null = New-Item -ItemType Directory -Path $destination
    $stream = [IO.File]::Open($archivePath, [IO.FileMode]::CreateNew)
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        for ($index = 0; $index -lt 2; $index++) {
            if ($case -eq 'missing' -and $index -eq 1) { continue }
            $name = $names[$index]
            if ($index -eq 1) {
                switch ($case) {
                    'duplicate' { $name = $names[0] }
                    'traversal' { $name = '../escape' }
                    'case-collision' { $name = $names[0].ToUpperInvariant() }
                }
            }
            $entry = $archive.CreateEntry($name)
            if ($case -eq 'symlink' -and $index -eq 1) { $entry.ExternalAttributes = -1577058304 }
            if ($case -eq 'directory' -and $index -eq 1) { $entry.ExternalAttributes = 0x10 }
            $output = $entry.Open()
            try { $output.Write($payload) } finally { $output.Dispose() }
        }
    } finally { $archive.Dispose(); $stream.Dispose() }
    $runtime = [pscustomobject]@{
        version = '1.2.0'; platform = "windows-$Architecture"
        archive = [pscustomobject]@{ size = (Get-Item -LiteralPath $archivePath).Length; sha256 = (Get-FileHash -LiteralPath $archivePath).Hash.ToLowerInvariant() }
        files = @($names | ForEach-Object { [pscustomobject]@{ name = $_; size = $payload.Length; sha256 = $payloadHash } })
    }
    switch ($case) {
        'archive-hash' { $runtime.archive.sha256 = '0' * 64 }
        'archive-size' { $runtime.archive.size++ }
        'entry-hash' { $runtime.files[1].sha256 = '0' * 64 }
        'entry-size' { $runtime.files[1].size++ }
        'nonempty' { [IO.File]::WriteAllBytes((Join-Path $destination 'keep.txt'), $payload) }
    }
    $rejected = $false
    try { Expand-PinnedSnowOcrRuntime -ArchivePath $archivePath -Runtime $runtime -Destination $destination }
    catch { $rejected = $true }
    if ($case -eq 'valid') {
        if ($rejected -or @(Get-ChildItem -LiteralPath $destination -File).Count -ne 2) { throw 'Valid pinned archive was not imported.' }
        foreach ($name in $names) {
            if ((Get-FileHash -LiteralPath (Join-Path $destination $name)).Hash.ToLowerInvariant() -cne $payloadHash) {
                throw 'Import changed pinned file bytes.'
            }
        }
    } else {
        $expectedFiles = if ($case -eq 'nonempty') { 1 } else { 0 }
        if (-not $rejected -or @(Get-ChildItem -LiteralPath $destination -Force).Count -ne $expectedFiles) {
            throw "Unsafe import was accepted or wrote files before verification: $case"
        }
    }
    Write-Output "PASS: $case"
}
if (Test-Path -LiteralPath (Join-Path $testRoot 'escape')) { throw 'Archive escaped its destination.' }

# Exercise the CMake development/offline path with the same two-entry archive.
# In particular, restaging must not retain the retired three-file loop bounds.
$modelId = 'fixture-small'
$modelCache = Join-Path $testRoot "ocr-models-$modelId"
$null = New-Item -ItemType Directory -Path $modelCache
$modelFiles = @('detector.onnx', 'recognizer.onnx', 'dictionary.txt') | ForEach-Object {
    [IO.File]::WriteAllBytes((Join-Path $modelCache $_), $payload)
    [ordered]@{ name = $_; size = $payload.Length; sha256 = $payloadHash; url = 'https://invalid.invalid/unused' }
}
$validArchive = Join-Path $testRoot 'valid.zip'
$manifest = [ordered]@{
    schema = 2; default_model = 'small'
    runtime = [ordered]@{
        version = '1.2.0'; platform = "windows-$Architecture"
        archive = [ordered]@{ name = 'valid.zip'; size = (Get-Item -LiteralPath $validArchive).Length; sha256 = (Get-FileHash -LiteralPath $validArchive).Hash.ToLowerInvariant(); url = 'https://invalid.invalid/unused' }
        files = @($names | ForEach-Object { [ordered]@{ name = $_; size = $payload.Length; sha256 = $payloadHash } })
    }
    models = @('extra_small', 'small', 'medium') | ForEach-Object {
        [ordered]@{ type = $_; id = $modelId; files = @($modelFiles) }
    }
}
$manifestPath = Join-Path $testRoot 'asset-manifest.json'
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding utf8
$stage = Join-Path $testRoot 'offline-stage'
$runtimeStage = Join-Path $stage "runtimes/1.2.0/windows-$Architecture"
foreach ($pass in 1..2) {
    & cmake "-DSNOW_OCR_ASSET_MANIFEST=$manifestPath" "-DSNOW_OCR_ARTIFACTS_DIR=$testRoot" `
        "-DSNOW_OCR_DESTINATION=$stage" "-DSNOW_OCR_PLATFORM=windows-$Architecture" `
        -P (Join-Path $PSScriptRoot '../cmake/StageSnowShotOcrAssets.cmake')
    if ($LASTEXITCODE -ne 0) { throw 'Cached offline runtime staging failed.' }
    foreach ($name in $names) {
        if ((Get-FileHash -LiteralPath (Join-Path $runtimeStage $name)).Hash.ToLowerInvariant() -cne $payloadHash) {
            throw 'Offline staging changed pinned runtime bytes.'
        }
    }
    if (-not (Test-Path (Join-Path $runtimeStage '.complete.json')) -or
        -not (Test-Path (Join-Path $stage "models/$modelId/.complete.json")) -or
        @(Get-ChildItem -LiteralPath $runtimeStage -Force -File).Count -ne 3) {
        throw 'Offline staging must complete exactly two runtime files.'
    }
    if ($pass -eq 1) { [IO.File]::WriteAllText((Join-Path $runtimeStage $names[0]), 'corrupt') }
}
Write-Output 'PASS: cached offline staging and corrupt runtime restaging'

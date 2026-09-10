[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-ocr-release-runtime.ps1')
$testRoot = Join-Path (Split-Path -Parent $PSScriptRoot) "build/ocr-runtime-import-tests-$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $testRoot
$names = @('snow-ocr-process-1.0.5-windows-x64.exe', 'DirectML.dll', 'runtime-manifest.json')
$payload = [Text.Encoding]::UTF8.GetBytes('immutable fixture')
$payloadHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($payload)).ToLowerInvariant()
foreach ($case in @('valid', 'archive-hash', 'archive-size', 'entry-hash', 'entry-size', 'duplicate', 'traversal', 'case-collision', 'symlink', 'directory', 'missing', 'nonempty')) {
    $archivePath = Join-Path $testRoot "$case.zip"
    $destination = Join-Path $testRoot $case
    $null = New-Item -ItemType Directory -Path $destination
    $stream = [IO.File]::Open($archivePath, [IO.FileMode]::CreateNew)
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        for ($index = 0; $index -lt 3; $index++) {
            if ($case -eq 'missing' -and $index -eq 2) { continue }
            $name = $names[$index]
            if ($index -eq 2) {
                switch ($case) {
                    'duplicate' { $name = $names[0] }
                    'traversal' { $name = '../escape' }
                    'case-collision' { $name = $names[0].ToUpperInvariant() }
                }
            }
            $entry = $archive.CreateEntry($name)
            if ($case -eq 'symlink' -and $index -eq 2) { $entry.ExternalAttributes = -1577058304 }
            if ($case -eq 'directory' -and $index -eq 2) { $entry.ExternalAttributes = 0x10 }
            $output = $entry.Open()
            try { $output.Write($payload) } finally { $output.Dispose() }
        }
    } finally { $archive.Dispose(); $stream.Dispose() }
    $runtime = [pscustomobject]@{
        version = '1.0.5'; platform = 'windows-x64'
        archive = [pscustomobject]@{ size = (Get-Item -LiteralPath $archivePath).Length; sha256 = (Get-FileHash -LiteralPath $archivePath).Hash.ToLowerInvariant() }
        files = @($names | ForEach-Object { [pscustomobject]@{ name = $_; size = $payload.Length; sha256 = $payloadHash } })
    }
    switch ($case) {
        'archive-hash' { $runtime.archive.sha256 = '0' * 64 }
        'archive-size' { $runtime.archive.size++ }
        'entry-hash' { $runtime.files[2].sha256 = '0' * 64 }
        'entry-size' { $runtime.files[2].size++ }
        'nonempty' { [IO.File]::WriteAllBytes((Join-Path $destination 'keep.txt'), $payload) }
    }
    $rejected = $false
    try { Expand-PinnedSnowOcrRuntime -ArchivePath $archivePath -Runtime $runtime -Destination $destination }
    catch { $rejected = $true }
    if ($case -eq 'valid') {
        if ($rejected -or @(Get-ChildItem -LiteralPath $destination -File).Count -ne 3) { throw 'Valid pinned archive was not imported.' }
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

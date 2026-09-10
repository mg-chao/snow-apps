# Import an already published OCR runtime; application releases must not rebuild immutable assets.
function Expand-PinnedSnowOcrRuntime {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ArchivePath,
        [Parameter(Mandatory)]$Runtime,
        [Parameter(Mandatory)][string]$Destination
    )
    $ErrorActionPreference = 'Stop'
    if ($Runtime.version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or
        $Runtime.platform -cne 'windows-x64') { throw 'Invalid pinned OCR runtime identity.' }
    $expectedNames = @("snow-ocr-process-$($Runtime.version)-$($Runtime.platform).exe", 'DirectML.dll', 'runtime-manifest.json')
    $expected = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
    foreach ($file in $Runtime.files) {
        if ($file.name -cnotin $expectedNames -or $file.size -le 0 -or $file.size -gt 134217728 -or
            $file.sha256 -cnotmatch '^[0-9a-f]{64}$' -or -not $expected.TryAdd($file.name, $file)) {
            throw 'Invalid pinned OCR file inventory.'
        }
    }
    if ($expected.Count -ne 3) { throw 'The pinned OCR inventory must contain exactly three files.' }
    $directory = Get-Item -LiteralPath $Destination
    if (-not $directory.PSIsContainer -or @(Get-ChildItem -LiteralPath $Destination -Force).Count) {
        throw 'The OCR extraction destination must be an empty directory.'
    }
    for ($ancestor = $directory; $null -ne $ancestor; $ancestor = $ancestor.Parent) {
        if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'The OCR extraction destination must not traverse links.'
        }
    }
    $stream = [IO.File]::OpenRead($ArchivePath)
    try {
        $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant()
        if ($stream.Length -ne $Runtime.archive.size -or $hash -cne $Runtime.archive.sha256) {
            throw 'The published OCR archive differs from its pinned size/SHA-256.'
        }
        $stream.Position = 0
        $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Read, $true)
        try {
            if ($archive.Entries.Count -ne $expected.Count) { throw 'Unexpected OCR archive entries.' }
            $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
            foreach ($entry in $archive.Entries) {
                $unixType = ($entry.ExternalAttributes -shr 16) -band 0xf000
                if (-not $expected.ContainsKey($entry.FullName) -or -not $seen.Add($entry.FullName) -or
                    $unixType -notin @(0, 0x8000) -or ($entry.ExternalAttributes -band 0x410)) {
                    throw 'Unsafe or duplicate OCR archive entry.'
                }
                $file = $expected[$entry.FullName]
                if ($entry.Length -ne $file.size) { throw 'OCR archive entry size mismatch.' }
                $input = $entry.Open()
                try { $actual = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($input)).ToLowerInvariant() }
                finally { $input.Dispose() }
                if ($actual -cne $file.sha256) { throw 'OCR archive entry hash mismatch.' }
            }
            # No entry is written until the entire pinned archive inventory has passed.
            foreach ($entry in $archive.Entries) {
                $output = [IO.File]::Open((Join-Path $Destination $entry.FullName), [IO.FileMode]::CreateNew)
                $input = $entry.Open()
                try { $input.CopyTo($output) }
                finally { $input.Dispose(); $output.Dispose() }
            }
        }
        finally { $archive.Dispose() }
    }
    finally { $stream.Dispose() }
}

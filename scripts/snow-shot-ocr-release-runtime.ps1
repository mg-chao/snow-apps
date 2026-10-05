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
        $Runtime.platform -cnotin @('windows-x64', 'windows-arm64')) { throw 'Invalid pinned OCR runtime identity.' }
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

# Read PE headers directly: a host-native inspector can audit cross-built binaries,
# but executing them is never required to establish their architecture.
function Assert-SnowPeArchitecture {
    param([Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][ValidateSet('x64', 'arm64')][string]$Architecture)
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw "Invalid PE image: $Path" }
        $stream.Position = 0x3c
        $offset = $reader.ReadUInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length - 24) { throw "Invalid PE header: $Path" }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $Path" }
        $machine = $reader.ReadUInt16()
        $expected = if ($Architecture -eq 'arm64') { 0xaa64 } else { 0x8664 }
        if ($machine -ne $expected) {
            throw "Expected $Architecture PE machine $($expected.ToString('X4')); found $($machine.ToString('X4')): $Path"
        }
    } finally { $reader.Dispose(); $stream.Dispose() }
}

function New-SnowOcrAssetManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ArchivePath,
        [Parameter(Mandatory)][ValidateSet('x64', 'arm64')][string]$Architecture,
        [Parameter(Mandatory)][string]$OutputPath,
        [string]$TemplatePath = (Join-Path $PSScriptRoot '../snow_shot/packaging/snow-shot-ocr-asset-manifest.json')
    )
    $platform = "windows-$Architecture"
    $template = Get-Content -LiteralPath $TemplatePath -Raw | ConvertFrom-Json
    if ($template.schema -ne 2 -or $template.default_model -cne 'small' -or $template.models.Count -ne 7) {
        throw 'The trusted model template must describe schema 2 and all seven models.'
    }
    $archivePathResolved = (Resolve-Path -LiteralPath $ArchivePath).Path
    $zip = [IO.Compression.ZipFile]::OpenRead($archivePathResolved)
    try {
        $entry = $zip.GetEntry('runtime-manifest.json')
        if (-not $entry -or $entry.Length -le 0 -or $entry.Length -gt 1MB) { throw 'Missing or oversized OCR runtime manifest.' }
        $reader = [IO.StreamReader]::new($entry.Open())
        try { $manifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if ($manifest.schema -ne 1 -or $manifest.platform -cne $platform -or $manifest.protocol -ne 5 -or
            $manifest.version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
            throw 'The OCR runtime manifest has an unexpected platform, protocol, or version.'
        }
        $version = $manifest.version
        $names = @("snow-ocr-process-$version-$platform.exe", 'DirectML.dll', 'runtime-manifest.json')
        if ($zip.Entries.Count -ne 3 -or $manifest.files.Count -ne 2) { throw 'Unexpected OCR runtime inventory.' }
        $files = @($zip.Entries | ForEach-Object {
            $input = $_.Open()
            try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($input)).ToLowerInvariant() }
            finally { $input.Dispose() }
            [ordered]@{ name = $_.FullName; size = $_.Length; sha256 = $hash }
        })
        foreach ($name in $names[0..1]) {
            $declared = @($manifest.files | Where-Object { $_.name -ceq $name })
            $actual = @($files | Where-Object { $_.name -ceq $name })
            if ($declared.Count -ne 1 -or $actual.Count -ne 1 -or $declared[0].size -ne $actual[0].size -or
                $declared[0].sha256 -cne $actual[0].sha256) { throw 'OCR runtime files differ from their runtime manifest.' }
        }
        $files = @($names | ForEach-Object {
            $name = $_
            $files | Where-Object { $_.name -ceq $name }
        })
    } finally { $zip.Dispose() }
    $archiveName = "snow-ocr-runtime-$version-$platform.zip"
    $runtime = [pscustomobject][ordered]@{
        version = $version; platform = $platform
        archive = [ordered]@{ name = $archiveName; size = (Get-Item -LiteralPath $archivePathResolved).Length
            sha256 = (Get-FileHash -LiteralPath $archivePathResolved -Algorithm SHA256).Hash.ToLowerInvariant()
            url = "https://www.modelscope.cn/models/mgchao/SnowShotOCR/resolve/master/runtime/$version/$platform/$archiveName" }
        files = $files
    }
    # Reuse the strict importer before trusting any archive paths or attributes.
    $verification = Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($OutputPath))) `
        ("ocr-manifest-verify-" + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $verification -Force
    try {
        Expand-PinnedSnowOcrRuntime -ArchivePath $archivePathResolved -Runtime $runtime -Destination $verification
        foreach ($name in $names[0..1]) { Assert-SnowPeArchitecture -Path (Join-Path $verification $name) -Architecture $Architecture }
    } finally {
        $resolved = [IO.Path]::GetFullPath($verification)
        $parent = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($OutputPath)).TrimEnd('\') + '\'
        if (-not $resolved.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe OCR verification directory.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
    $template.runtime = $runtime
    $template | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $OutputPath -Encoding utf8NoBOM
    return [IO.Path]::GetFullPath($OutputPath)
}

function Resolve-SnowOcrAssetManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ArchivePath,
        [Parameter(Mandatory)][ValidateSet('x64', 'arm64')][string]$Architecture,
        [Parameter(Mandatory)][string]$PinnedManifestPath,
        [Parameter(Mandatory)][string]$OutputPath
    )
    if (Test-Path -LiteralPath $PinnedManifestPath -PathType Leaf) {
        $manifest = Get-Content -LiteralPath $PinnedManifestPath -Raw | ConvertFrom-Json
        if ($manifest.runtime.platform -cne "windows-$Architecture" -or
            $manifest.runtime.archive.size -ne (Get-Item -LiteralPath $ArchivePath).Length -or
            $manifest.runtime.archive.sha256 -cne (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()) {
            throw 'The supplied OCR runtime differs from the existing trusted architecture manifest. Prepare a separately versioned OCR release; do not replace its pins during application packaging.'
        }
        return (Resolve-Path -LiteralPath $PinnedManifestPath).Path
    }
    if (Test-Path -LiteralPath $OutputPath -PathType Leaf) {
        # A locally bootstrapped descriptor is also immutable on subsequent
        # packaging calls. Updating its pins belongs to explicit OCR preparation.
        return Resolve-SnowOcrAssetManifest -ArchivePath $ArchivePath -Architecture $Architecture `
            -PinnedManifestPath $OutputPath -OutputPath $OutputPath
    }
    throw 'A pinned architecture manifest is required before importing a local OCR archive. Run explicit OCR runtime preparation or new-snow-shot-ocr-asset-manifest.ps1 first.'
}

# Build-time archive creation only. The installer ships a separate native decoder.

function New-SnowShotInstallerPayload {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Archive,
        [Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string]$Architecture,
        [Parameter(Mandatory = $true)][string]$Encoder
    )
    $stagePath = (Resolve-Path -LiteralPath $Stage -ErrorAction Stop).Path
    $encoderPath = (Resolve-Path -LiteralPath $Encoder -ErrorAction Stop).Path
    $archivePath = [IO.Path]::GetFullPath($Archive)
    if (-not (Test-Path -LiteralPath $stagePath -PathType Container) -or
        -not (Test-Path -LiteralPath $encoderPath -PathType Leaf)) {
        throw 'The installer payload stage and pinned encoder must exist.'
    }
    $stagePrefix = $stagePath.TrimEnd([char[]]@('\', '/')) + [IO.Path]::DirectorySeparatorChar
    if ($archivePath.StartsWith($stagePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The installer payload archive must be outside its input stage.'
    }
    $encoderInfo = @(& $encoderPath i -sccUTF-8 2>&1)
    if ($LASTEXITCODE -ne 0 -or
        -not (($encoderInfo -join "`n") -match '(?m)^7-Zip(?: \(r\))? 26\.05(?: |$)')) {
        throw 'Installer payloads require the integrity-pinned LZMA SDK 26.05 encoder.'
    }
    $entries = @(Get-ChildItem -LiteralPath $stagePath -Recurse -Force -ErrorAction Stop)
    $fileCount = 0
    $unpackedBytes = 0L
    $expected = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
    $windowsNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $entries) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Installer payloads cannot contain reparse points: $($entry.FullName)"
        }
        # GetRelativePath normalizes Windows trailing dots/spaces. Preserve the
        # enumerated name so unsupported entries are rejected before encoding.
        if (-not $entry.FullName.StartsWith($stagePrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'An installer payload entry resolved outside its input stage.'
        }
        $relative = $entry.FullName.Substring($stagePrefix.Length).Replace('\', '/')
        $invalidComponents = @($relative.Split('/') | Where-Object {
            $_ -in @('', '.', '..') -or $_ -match '[. ]$' -or
                $_ -match '^(?i:CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\.|$)'
        })
        if ($relative -match '[\x00-\x1f\\:*?"<>|]' -or $invalidComponents.Count -ne 0) {
            throw "The installer payload contains an unsupported path: $relative"
        }
        if (-not $windowsNames.Add($relative)) {
            throw "Installer payload paths collide under Windows case rules: $relative"
        }
        $size = if ($entry.PSIsContainer) { 0L } else { [long]$entry.Length }
        if ($size -gt 256MB) {
            throw "An installer payload file exceeds the 256 MiB decoder block contract: $relative"
        }
        $expected.Add($relative, [pscustomobject]@{ Directory = $entry.PSIsContainer; Bytes = $size })
        if (-not $entry.PSIsContainer) {
            $fileCount++
            $unpackedBytes += $size
        }
    }
    if ($fileCount -eq 0 -or $entries.Count -gt 100000 -or $unpackedBytes -gt 1GB) {
        throw 'The installer payload must contain files and fit the decoder count/size limits.'
    }
    $archiveDirectory = Split-Path -Parent $archivePath
    $null = New-Item -ItemType Directory -Path $archiveDirectory -Force
    $listPath = Join-Path $archiveDirectory ('.snow-payload-list-' + [guid]::NewGuid().ToString('N') + '.txt')
    $roots = [string[]]@(Get-ChildItem -LiteralPath $stagePath -Force | ForEach-Object { $_.Name })
    [Array]::Sort($roots, [StringComparer]::Ordinal)
    [IO.File]::WriteAllLines($listPath, $roots, [Text.UTF8Encoding]::new($false))
    try {
        if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
            Remove-Item -LiteralPath $archivePath -Force
        }
        Push-Location -LiteralPath $stagePath
        try {
            $arguments = @('a', '-t7z', '-m0=LZMA2', '-mx=9', '-md=32m', '-ms=256m', '-mmt=2',
                '-mtc=off', '-mta=off', '-mtm=off', '-scsUTF-8', '-sccUTF-8', '-y', '-bso0', '-bsp0',
                $archivePath, "@$listPath")
            $creation = @(& $encoderPath @arguments 2>&1)
            if ($LASTEXITCODE -ne 0) {
                throw "Installer payload compression failed: $($creation -join "`n")"
            }
        }
        finally { Pop-Location }
        $integrity = @(& $encoderPath t -sccUTF-8 -bso0 -bsp0 $archivePath 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "Installer payload integrity test failed: $($integrity -join "`n")"
        }
        $listing = @(& $encoderPath l -slt -sccUTF-8 $archivePath 2>&1)
        if ($LASTEXITCODE -ne 0) { throw 'Unable to inspect the compressed installer payload.' }
        $header = @{}
        $records = [Collections.Generic.List[hashtable]]::new()
        $current = @{}
        $fileSection = $false
        foreach ($item in $listing) {
            $line = [string]$item
            if ($line -ceq '----------') {
                $fileSection = $true
                continue
            }
            if ($line -match '^([^=]+) = (.*)$') {
                if ($fileSection) { $current[$Matches[1].Trim()] = $Matches[2] }
                else { $header[$Matches[1].Trim()] = $Matches[2] }
            }
            elseif ([string]::IsNullOrWhiteSpace($line) -and $fileSection -and $current.ContainsKey('Path')) {
                $records.Add($current)
                $current = @{}
            }
        }
        if ($current.ContainsKey('Path')) { $records.Add($current) }
        if ($records.Count -ne $expected.Count) {
            throw 'Compressed installer inventory differs from the staged inventory.'
        }
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($record in $records) {
            $relative = ([string]$record.Path).Replace('\', '/')
            $directory = ([string]$record.Attributes).StartsWith('D', [StringComparison]::Ordinal)
            if (-not $seen.Add($relative) -or -not $expected.ContainsKey($relative) -or
                $expected[$relative].Directory -ne $directory -or
                $expected[$relative].Bytes -ne [long]$record.Size) {
                throw "Compressed installer inventory differs at: $relative"
            }
        }
        $methods = [string]$header.Method
        $filter = if ($Architecture -eq 'arm64') { '\bARM64\b' } else { '\bBCJ2\b' }
        if ($methods -notmatch '\bLZMA2\b' -or $methods -notmatch $filter) {
            throw "Installer payload did not use the expected executable filter for $Architecture`: $methods"
        }
        return [pscustomobject][ordered]@{
            Archive = $archivePath
            Bytes = (Get-Item -LiteralPath $archivePath).Length
            Sha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
            Encoder = $encoderPath
            EncoderVersion = '26.05'
            EncoderSha256 = (Get-FileHash -LiteralPath $encoderPath -Algorithm SHA256).Hash.ToLowerInvariant()
            Methods = $methods
            Blocks = [int]$header.Blocks
            FileCount = $fileCount
            UnpackedBytes = $unpackedBytes
            SolidBlockLimitBytes = 256MB
        }
    }
    catch {
        if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
            Remove-Item -LiteralPath $archivePath -Force
        }
        throw
    }
    finally {
        if (Test-Path -LiteralPath $listPath -PathType Leaf) {
            Remove-Item -LiteralPath $listPath -Force
        }
    }
}

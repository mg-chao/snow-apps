#Requires -Version 7.0
# Dot-sourcing defines helpers only; release downloads happen in the generator.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-SnowShotScoopVersion([string]$Tag) {
    if ($Tag -cnotmatch '^v(?<version>(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?)(?:_snow-shot)?\z') {
        throw "Not a Snow Shot release tag: $Tag"
    }
    $version = $Matches.version
    # SemanticVersion also rejects leading zeros in numeric prerelease identifiers.
    $null = [System.Management.Automation.SemanticVersion]::Parse($version)
    return $version
}

function Invoke-SnowShotScoopRelease([string]$Tag) {
    $headers = @{ Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
    if ($env:GH_TOKEN) { $headers.Authorization = "Bearer $env:GH_TOKEN" }
    Invoke-RestMethod -Uri "https://api.github.com/repos/mg-chao/snow-apps/releases/tags/$Tag" -Headers $headers
}

function Get-SnowShotScoopAsset($Release, [string]$Name, [string]$Tag, [switch]$Optional) {
    $assets = @($Release.assets | Where-Object { $_.name -ceq $Name })
    if ($Optional -and $assets.Count -eq 0) { return $null }
    if ($assets.Count -ne 1) { throw "Expected exactly one release asset named $Name." }
    $url = "https://github.com/mg-chao/snow-apps/releases/download/$Tag/$Name"
    if ($assets[0].browser_download_url -cne $url) { throw "Unexpected asset URL for $Name." }
    return $assets[0]
}

function Assert-SnowShotScoopArchive([string]$Path) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            if ($name.StartsWith('/') -or $name.Contains(':') -or
                $name.Split('/') -contains '..' -or -not $names.Add($name)) {
                throw "Unsafe or duplicate archive path: $name"
            }
        }
        $exe = $zip.GetEntry('bin/snow_shot.exe')
        $marker = $zip.GetEntry('bin/__data_directory')
        if (-not $exe -or $exe.Length -eq 0 -or -not $marker) {
            throw 'Portable archive must contain bin/snow_shot.exe and bin/__data_directory.'
        }
        $reader = [IO.StreamReader]::new($marker.Open())
        try { $value = $reader.ReadToEnd().Trim() } finally { $reader.Dispose() }
        if ($value -cne 'portable') { throw 'Unexpected portable data-directory marker.' }
    } finally { $zip.Dispose() }
}

function New-SnowShotScoopManifestObject([string]$Version, [string]$Url, [string]$Hash) {
    # Keep the install contract and upstream maintenance metadata in one place.
    # checkver scripts also run under Windows PowerShell 5.1 in Scoop's tooling.
    return [ordered]@{
        version = $Version
        description = 'A screenshot utility for capturing, annotating, pinning, and recognizing screen content.'
        homepage = 'https://snowshot.top'
        license = 'GPL-3.0-or-later'
        notes = 'Quit Snow Shot before upgrading. Use scoop update snowshot instead of the built-in updater.'
        architecture = [ordered]@{ '64bit' = [ordered]@{ url = $Url; hash = $Hash } }
        bin = ,@('bin\snow_shot.exe', 'snowshot')
        shortcuts = ,@('bin\snow_shot.exe', 'Snow Shot')
        persist = 'bin\portable'
        checkver = [ordered]@{
            url = 'https://api.github.com/repos/mg-chao/snow-apps/releases?per_page=100'
            script = @(
                '$releases = ($page | ConvertFrom-Json) | Where-Object { -not $_.draft -and -not $_.prerelease -and $_.tag_name -cmatch ''^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:_snow-shot)?$'' }'
                '$releases = $releases | Sort-Object { [version] ($_.tag_name -replace ''^v|_snow-shot$'', '''') } -Descending'
                'foreach ($release in $releases) {'
                '    $version = $release.tag_name -replace ''^v|_snow-shot$'', '''''
                '    $name = "snow-shot-$version-windows-x64-portable.zip"'
                '    $asset = $release.assets | Where-Object { $_.name -ceq $name -and $_.browser_download_url -ceq "https://github.com/mg-chao/snow-apps/releases/download/$($release.tag_name)/$name" } | Select-Object -First 1'
                '    if ($asset) { $asset.browser_download_url; break }'
                '}'
            )
            regex = '/download/(?<tag>v(?<version>\d+\.\d+\.\d+)(?:_snow-shot)?)/snow-shot-\k<version>-windows-x64-portable\.zip$'
        }
        autoupdate = [ordered]@{
            architecture = [ordered]@{
                '64bit' = [ordered]@{
                    url = 'https://github.com/mg-chao/snow-apps/releases/download/$matchTag/snow-shot-$version-windows-x64-portable.zip'
                    hash = [ordered]@{ url = '$url.sha256' }
                }
            }
        }
    }
}

function New-SnowShotScoopManifest([string]$Tag, [string]$OutputPath) {
    $version = Get-SnowShotScoopVersion $Tag
    if ($version.Contains('-')) { throw 'Scoop Extras requires a stable release version.' }
    $release = Invoke-SnowShotScoopRelease $Tag
    if ($release.draft -or $release.tag_name -cne $Tag) { throw 'Expected the requested published release.' }
    if ($release.prerelease) { throw 'Scoop Extras requires a stable published release.' }
    $name = "snow-shot-$version-windows-x64-portable.zip"
    $asset = Get-SnowShotScoopAsset $release $name $Tag
    $sidecar = Get-SnowShotScoopAsset $release "$name.sha256" $Tag -Optional
    $temp = Join-Path ([IO.Path]::GetTempPath()) "snow-shot-scoop-$([guid]::NewGuid().ToString('N')).zip"
    try {
        Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $temp -TimeoutSec 600
        if ($asset.size -le 0 -or (Get-Item -LiteralPath $temp).Length -ne $asset.size) {
            throw 'Downloaded ZIP size does not match the release asset.'
        }
        $hash = (Get-FileHash -LiteralPath $temp -Algorithm SHA256).Hash.ToLowerInvariant()
        $checksums = @()
        $digestProperty = $asset.PSObject.Properties['digest']
        if ($digestProperty -and $digestProperty.Value) {
            if ($digestProperty.Value -cnotmatch '^sha256:([a-fA-F0-9]{64})$') { throw 'Invalid GitHub asset digest.' }
            $checksums += $Matches[1].ToLowerInvariant()
        }
        if ($sidecar) {
            Invoke-WebRequest -Uri $sidecar.browser_download_url -OutFile "$temp.sha256" -TimeoutSec 60
            $checksumText = [IO.File]::ReadAllText("$temp.sha256").Trim()
            if ($checksumText -cnotmatch ('^([a-fA-F0-9]{64})[ \t]+\*?' + [regex]::Escape($name) + '$')) {
                throw 'Invalid checksum sidecar or filename.'
            }
            $checksums += $Matches[1].ToLowerInvariant()
        }
        if ($checksums.Count -eq 0) { throw 'At least one published SHA-256 checksum is required.' }
        foreach ($expected in $checksums) {
            if ($hash -cne $expected) { throw 'Portable ZIP SHA-256 mismatch.' }
        }
        Assert-SnowShotScoopArchive $temp
        $manifest = New-SnowShotScoopManifestObject $version $asset.browser_download_url $hash
        $output = [IO.Path]::GetFullPath($OutputPath)
        $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output))
        [IO.File]::WriteAllText($output, (($manifest | ConvertTo-Json -Depth 8).Replace("`r`n", "`n") + "`n"),
            [Text.UTF8Encoding]::new($false))
        return $output
    } finally {
        foreach ($file in @($temp, "$temp.sha256")) {
            if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
        }
    }
}

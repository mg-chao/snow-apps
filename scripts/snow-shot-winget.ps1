# Shared release/manifest operations. Dot-sourcing this file performs no network or disk writes.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-SnowShotWingetVersion([string]$Tag) {
    if ($Tag -cnotmatch '^v(?<version>(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?)(?:_snow-shot)?\z') {
        throw "Not a Snow Shot release tag: $Tag"
    }
    return $Matches.version
}

function Invoke-SnowShotWingetApi([string]$Path) {
    $headers = @{ Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
    if ($env:GH_TOKEN) { $headers.Authorization = "Bearer $env:GH_TOKEN" }
    Invoke-RestMethod -Uri "https://api.github.com/$Path" -Headers $headers
}

function New-SnowShotWingetManifest([string]$Tag, [string]$OutputDirectory) {
    $version = Get-SnowShotWingetVersion $Tag
    $repository = 'mg-chao/snow-apps'
    $release = Invoke-SnowShotWingetApi "repos/$repository/releases/tags/$Tag"
    if ($release.draft -or $release.tag_name -cne $Tag) { throw 'Expected the requested published release.' }
    $releaseDate = ([DateTimeOffset]$release.published_at).UtcDateTime.ToString('yyyy-MM-dd')
    $notes = ([string]$release.body).Replace("`r`n", "`n").Replace("`r", "`n").Trim()
    if ([string]::IsNullOrWhiteSpace($notes) -or $notes.Length -gt 10000) {
        throw 'Published release notes must contain 1 to 10000 characters for WinGet.'
    }
    # Explicit indentation keeps Markdown, YAML-looking text and leading spaces literal.
    $releaseNotes = ($notes.Split("`n") | ForEach-Object { "  $_" }) -join "`n"
    $name = "snow-shot-$version-windows-x64-offline.exe"
    $assets = @($release.assets | Where-Object { $_.name -ceq $name })
    if ($assets.Count -ne 1) { throw "Expected exactly one release asset named $name." }
    $url = "https://github.com/$repository/releases/download/$Tag/$name"
    if ($assets[0].browser_download_url -cne $url) { throw 'Unexpected installer asset URL.' }
    $null = New-Item -ItemType Directory -Force -Path $OutputDirectory
    $download = Join-Path $OutputDirectory "$([guid]::NewGuid().ToString('N')).exe"
    try {
        Invoke-WebRequest -Uri $url -OutFile $download -TimeoutSec 600
        if ((Get-Item -LiteralPath $download).Length -ne $assets[0].size -or $assets[0].size -le 0) {
            throw 'Downloaded installer size does not match the release asset.'
        }
        $hash = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash
    } finally {
        if (Test-Path -LiteralPath $download) { Remove-Item -LiteralPath $download }
    }
    $directory = Join-Path $OutputDirectory "manifests/m/mg-chao/snow-shot/$version"
    $null = New-Item -ItemType Directory -Force -Path $directory
    $common = "PackageIdentifier: mg-chao.snow-shot`nPackageVersion: '$version'"
    $documents = [ordered]@{
        'mg-chao.snow-shot.yaml' = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.1.12.0.schema.json
$common
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.12.0
"@
        'mg-chao.snow-shot.installer.yaml' = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.installer.1.12.0.schema.json
$common
InstallerLocale: en-US
InstallerType: nullsoft
Scope: machine
InstallModes:
  - interactive
  - silent
  - silentWithProgress
InstallerSwitches:
  Silent: /S
  SilentWithProgress: /S
UpgradeBehavior: install
ElevationRequirement: elevationRequired
ProductCode: SnowShot
ReleaseDate: $releaseDate
InstallationMetadata:
  DefaultInstallLocation: '%ProgramFiles%\SnowShot'
AppsAndFeaturesEntries:
  - DisplayName: Snow Shot
    Publisher: Snow Apps
    ProductCode: SnowShot
ExpectedReturnCodes:
  - InstallerReturnCode: 10
    ReturnResponse: packageInUse
Installers:
  - Architecture: x64
    InstallerUrl: $url
    InstallerSha256: $hash
ManifestType: installer
ManifestVersion: 1.12.0
"@
        'mg-chao.snow-shot.locale.en-US.yaml' = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.1.12.0.schema.json
$common
PackageLocale: en-US
Publisher: Snow Apps
PublisherUrl: https://github.com/mg-chao
PublisherSupportUrl: https://github.com/$repository/issues
PackageName: Snow Shot
PackageUrl: https://snowshot.top
License: GPL-3.0-or-later
LicenseUrl: https://github.com/$repository/blob/$Tag/snow_shot/COPYRIGHT
ShortDescription: A screenshot utility for capturing, annotating, pinning, and recognizing screen content.
Moniker: snowshot
Tags:
  - chatbot
  - screen-capture
  - screenshot
  - snowshot
  - translate
  - annotation
  - ocr
ReleaseNotes: |2-
$releaseNotes
ReleaseNotesUrl: https://github.com/$repository/releases/tag/$Tag
ManifestType: defaultLocale
ManifestVersion: 1.12.0
"@
    }
    foreach ($entry in $documents.GetEnumerator()) {
        [IO.File]::WriteAllText((Join-Path $directory $entry.Key),
            $entry.Value.Replace("`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
    }
    return [IO.Path]::GetFullPath($directory)
}

function Get-SnowShotWingetSubmission([string]$Version) {
    $path = "manifests/m/mg-chao/snow-shot/$Version"
    try {
        $null = Invoke-SnowShotWingetApi "repos/microsoft/winget-pkgs/contents/$path"
        return "Already merged: https://github.com/microsoft/winget-pkgs/tree/master/$path"
    } catch {
        # Authentication, rate limits and server failures must not masquerade as absence.
        if (-not $_.Exception.Response -or [int]$_.Exception.Response.StatusCode -ne 404) { throw }
    }
    $query = [uri]::EscapeDataString('repo:microsoft/winget-pkgs is:pr is:open in:title "mg-chao.snow-shot"')
    $page = 1
    do {
        $response = Invoke-SnowShotWingetApi "search/issues?q=$query&per_page=100&page=$page"
        if ($response.incomplete_results) { throw 'GitHub returned an incomplete submission search.' }
        foreach ($item in $response.items) {
            if ($item.title -match ('(?i)(?<![\w.])mg-chao\.snow-shot(?![\w.]).*?(?<![\w.+-])' +
                    [regex]::Escape($Version) + '(?![\w.+-])')) {
                return "Submission already open: $($item.html_url)"
            }
        }
        if ($page * 100 -ge $response.total_count) { break }
        if ($page -ge 10) { throw 'Too many open submissions to safely check for duplicates.' }
        $page++
    } while ($true)
    return $null
}

function Submit-SnowShotWingetManifest([string]$Tag, [string]$ManifestDirectory, [string]$WingetCreate) {
    $version = Get-SnowShotWingetVersion $Tag
    $existing = Get-SnowShotWingetSubmission $version
    if ($existing) { Write-Output $existing; return }
    if (-not $env:WINGET_CREATE_GITHUB_TOKEN) {
        throw 'Set the WINGET_CREATE_GITHUB_TOKEN repository secret (classic PAT, public_repo scope). Generated manifests remain available as workflow artifacts.'
    }
    & $WingetCreate submit --no-open --prtitle "New version: mg-chao.snow-shot version $version" $ManifestDirectory
    if ($LASTEXITCODE -ne 0) { throw "WinGetCreate submission failed with exit code $LASTEXITCODE." }
}

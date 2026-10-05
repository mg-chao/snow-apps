# Shared release/manifest operations. Dot-sourcing this file performs no network or disk writes.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-editions.ps1')

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

function New-SnowShotWingetManifest([string]$Tag, [string]$OutputDirectory, [string]$Edition = 'Full') {
    $product = Get-SnowShotEdition $Edition
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
    $variant = if ($Edition -eq 'Mini') { 'online' } else { 'offline' }
    $null = New-Item -ItemType Directory -Force -Path $OutputDirectory
    $installerEntries = @()
    foreach ($architecture in @(Get-SnowShotReleaseArchitectures $release $version)) {
    $name = "$($product.Product)-$version-windows-$architecture-$variant.exe"
    $assets = @($release.assets | Where-Object { $_.name -ceq $name })
    if ($assets.Count -ne 1) { throw "Expected exactly one release asset named $name." }
    $url = "https://github.com/$repository/releases/download/$Tag/$name"
    if ($assets[0].browser_download_url -cne $url) { throw 'Unexpected installer asset URL.' }
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
    $installerEntries += "  - Architecture: $architecture`n    InstallerUrl: $url`n    InstallerSha256: $hash"
    }
    $installers = $installerEntries -join "`n"
    $directory = Join-Path $OutputDirectory "manifests/m/mg-chao/$($product.Product)/$version"
    $null = New-Item -ItemType Directory -Force -Path $directory
    $common = "PackageIdentifier: $($product.Winget)`nPackageVersion: '$version'"
    $documents = [ordered]@{
        "$($product.Winget).yaml" = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.version.1.12.0.schema.json
$common
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.12.0
"@
        "$($product.Winget).installer.yaml" = @"
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
ProductCode: $($product.Registry)
ReleaseDate: $releaseDate
InstallationMetadata:
  DefaultInstallLocation: '%ProgramFiles%\$($product.Registry)'
AppsAndFeaturesEntries:
  - DisplayName: $($product.Name)
    Publisher: Snow Apps
    ProductCode: $($product.Registry)
ExpectedReturnCodes:
  - InstallerReturnCode: 10
    ReturnResponse: packageInUse
Installers:
$installers
ManifestType: installer
ManifestVersion: 1.12.0
"@
        "$($product.Winget).locale.en-US.yaml" = @"
# yaml-language-server: `$schema=https://aka.ms/winget-manifest.defaultLocale.1.12.0.schema.json
$common
PackageLocale: en-US
Publisher: Snow Apps
PublisherUrl: https://github.com/mg-chao
PublisherSupportUrl: https://github.com/$repository/issues
PackageName: $($product.Name)
PackageUrl: https://snowshot.top
License: GPL-3.0-or-later
LicenseUrl: https://github.com/$repository/blob/$Tag/snow_shot/COPYRIGHT
ShortDescription: A screenshot utility for capturing, annotating, pinning, and recognizing screen content.
Moniker: $($product.Scoop)
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
        if ($Edition -eq 'Mini') { $entry.Value = $entry.Value.Replace("  - chatbot`n", '').Replace("  - translate`n", '') }
        [IO.File]::WriteAllText((Join-Path $directory $entry.Key),
            $entry.Value.Replace("`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
    }
    return [IO.Path]::GetFullPath($directory)
}

function Get-SnowShotWingetSubmission([string]$Version, [string]$Edition = 'Full') {
    $product = Get-SnowShotEdition $Edition
    $path = "manifests/m/mg-chao/$($product.Product)/$Version"
    try {
        $null = Invoke-SnowShotWingetApi "repos/microsoft/winget-pkgs/contents/$path"
        return "Already merged: https://github.com/microsoft/winget-pkgs/tree/master/$path"
    } catch {
        # Authentication, rate limits and server failures must not masquerade as absence.
        if (-not $_.Exception.Response -or [int]$_.Exception.Response.StatusCode -ne 404) { throw }
    }
    $query = [uri]::EscapeDataString("repo:microsoft/winget-pkgs is:pr is:open in:title `"$($product.Winget)`"")
    $page = 1
    do {
        $response = Invoke-SnowShotWingetApi "search/issues?q=$query&per_page=100&page=$page"
        if ($response.incomplete_results) { throw 'GitHub returned an incomplete submission search.' }
        foreach ($item in $response.items) {
            if ($item.title -match ('(?i)(?<![\w.])' + [regex]::Escape($product.Winget) + '(?![\w.-]).*?(?<![\w.+-])' +
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

function Submit-SnowShotWingetManifest([string]$Tag, [string]$ManifestDirectory, [string]$WingetCreate, [string]$Edition = 'Full') {
    $product = Get-SnowShotEdition $Edition
    $version = Get-SnowShotWingetVersion $Tag
    $existing = Get-SnowShotWingetSubmission $version $Edition
    if ($existing) { Write-Output $existing; return }
    if (-not $env:WINGET_CREATE_GITHUB_TOKEN) {
        throw 'Set the WINGET_CREATE_GITHUB_TOKEN repository secret (classic PAT, public_repo scope). Generated manifests remain available as workflow artifacts.'
    }
    & $WingetCreate submit --no-open --prtitle "New version: $($product.Winget) version $version" $ManifestDirectory
    if ($LASTEXITCODE -ne 0) { throw "WinGetCreate submission failed with exit code $LASTEXITCODE." }
}

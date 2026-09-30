#Requires -Version 7.0
param([string]$SchemaPath)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-scoop.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) "snow-shot-scoop-tests-$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $root
function Require($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Expect-Failure([scriptblock]$Action, [string]$Pattern) {
    try { & $Action } catch {
        Require ($_.Exception.Message -match $Pattern) "Unexpected error: $_"
        return
    }
    throw "Expected failure matching $Pattern"
}
function New-FixtureArchive([string]$Marker = 'portable', [string]$Executable = 'bin/snow_shot.exe',
    [string]$Extra = '') {
    $script:zipPath = Join-Path $root "$([guid]::NewGuid().ToString('N')).zip"
    $zip = [IO.Compression.ZipFile]::Open($script:zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $files = [ordered]@{ $Executable = 'fixture executable'; 'bin/__data_directory' = $Marker }
        if ($Extra) { $files[$Extra] = 'extra' }
        foreach ($entry in $files.GetEnumerator()) {
            $writer = [IO.StreamWriter]::new($zip.CreateEntry($entry.Key).Open())
            try { $writer.Write($entry.Value) } finally { $writer.Dispose() }
        }
    } finally { $zip.Dispose() }
    $script:hash = (Get-FileHash $script:zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Set-FixtureRelease([string]$Tag = 'v1.1.8') {
    $version = Get-SnowShotScoopVersion $Tag
    $script:name = "snow-shot-$version-windows-x64-portable.zip"
    $script:release = [pscustomobject]@{
        tag_name = $Tag; draft = $false; prerelease = $false
        assets = @([pscustomobject]@{ name = $script:name; size = (Get-Item $script:zipPath).Length
            digest = "sha256:$script:hash"
            browser_download_url = "https://github.com/mg-chao/snow-apps/releases/download/$Tag/$script:name" })
    }
    $script:sidecarText = "$script:hash  $script:name`n"
}
function Add-FixtureSidecar {
    $script:release.assets += [pscustomobject]@{ name = "$script:name.sha256"
        browser_download_url = "$($script:release.assets[0].browser_download_url).sha256" }
}
function Invoke-SnowShotScoopRelease([string]$Tag) { return $script:release }
function Invoke-WebRequest([string]$Uri, [string]$OutFile, [int]$TimeoutSec) {
    if ($Uri.EndsWith('.sha256')) { [IO.File]::WriteAllText($OutFile, $script:sidecarText) }
    else { Copy-Item -LiteralPath $script:zipPath -Destination $OutFile }
}
function New-CheckverRelease([string]$Tag, [bool]$Prerelease = $false, [bool]$Draft = $false) {
    $version = $Tag -replace '^v|_snow-shot$|_snow-image$', ''
    $name = "snow-shot-$version-windows-x64-portable.zip"
    return [pscustomobject]@{
        tag_name = $Tag; draft = $Draft; prerelease = $Prerelease
        assets = @([pscustomobject]@{ name = $name
            browser_download_url = "https://github.com/mg-chao/snow-apps/releases/download/$Tag/$name" })
    }
}
function Get-FixtureCheckverMatch($Manifest, $Releases) {
    $page = ConvertTo-Json -InputObject @($Releases) -Depth 8
    $url = Invoke-Command ([scriptblock]::Create($Manifest.checkver.script -join "`n"))
    if ($IsWindows) {
        # Scoop's upstream tools can use 5.1, whose ConvertFrom-Json does not
        # enumerate arrays in a pipeline like PowerShell 7 does.
        $manifestPath = Join-Path $root 'checkver-manifest.json'
        $releasePath = Join-Path $root 'checkver-releases.json'
        $runnerPath = Join-Path $root 'checkver-runner.ps1'
        $Manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath
        Set-Content -LiteralPath $releasePath -Value $page
        @'
param([string]$ManifestPath, [string]$ReleasePath)
$ErrorActionPreference = 'Stop'
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$page = Get-Content -LiteralPath $ReleasePath -Raw
Invoke-Command ([scriptblock]::Create($manifest.checkver.script -join "`n"))
'@ | Set-Content -LiteralPath $runnerPath
        $legacyShell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
        $legacyUrl = & $legacyShell -NoProfile -ExecutionPolicy Bypass -File $runnerPath $manifestPath $releasePath
        Require ($LASTEXITCODE -eq 0) 'checkver failed under Windows PowerShell 5.1'
        Require (($legacyUrl -join "`n") -ceq ($url -join "`n")) 'checkver differs between PowerShell 5.1 and 7'
    }
    return [regex]::Match(($url -join "`n"), $Manifest.checkver.regex)
}
try {
    New-FixtureArchive
    $output = Join-Path $root 'snowshot.json'
    foreach ($tag in @('v1.1.8', 'v1.1.8_snow-shot', 'v2.0.0', 'v2.0.0_snow-shot')) {
        Set-FixtureRelease $tag
        $null = New-SnowShotScoopManifest $tag $output
        $text = Get-Content $output -Raw
        $manifest = $text | ConvertFrom-Json
        Require ($manifest.version -ceq (Get-SnowShotScoopVersion $tag)) 'Wrong version'
        Require ($manifest.architecture.'64bit'.hash -ceq $script:hash) 'Wrong hash'
        Require ($manifest.architecture.'64bit'.url -ceq $script:release.assets[0].browser_download_url) 'Wrong URL'
        Require ($manifest.bin.Count -eq 1 -and $manifest.bin[0][0] -ceq 'bin\snow_shot.exe' -and
            $manifest.bin[0][1] -ceq 'snowshot') 'Wrong shim'
        Require ($manifest.shortcuts.Count -eq 1 -and $manifest.shortcuts[0][0] -ceq 'bin\snow_shot.exe' -and
            $manifest.shortcuts[0][1] -ceq 'Snow Shot') 'Wrong shortcut'
        Require ($manifest.persist -ceq 'bin\portable') 'Wrong persistence'
        Require ($manifest.license -ceq 'GPL-3.0-or-later') 'Wrong license'
        Require ($manifest.checkver.url -ceq 'https://api.github.com/repos/mg-chao/snow-apps/releases?per_page=100') 'Wrong checkver endpoint'
        $match = Get-FixtureCheckverMatch $manifest @($script:release)
        Require ($match.Success -and $match.Groups['version'].Value -ceq $manifest.version -and
            $match.Groups['tag'].Value -ceq $tag) 'checkver lost version or release tag'
        $auto = $manifest.autoupdate.architecture.'64bit'
        $url = $auto.url.Replace('$matchTag', $match.Groups['tag'].Value).Replace('$version', $manifest.version)
        Require ($url -ceq $manifest.architecture.'64bit'.url) 'Autoupdate URL differs from verified release'
        Require ($auto.hash.url.Replace('$url', $url) -ceq "$url.sha256") 'Autoupdate checksum URL'
        Require (-not $text.Contains("`r")) 'Expected LF JSON'
        if ($SchemaPath) { Require (Test-Json -Json $text -SchemaFile $SchemaPath) 'Schema validation failed' }
    }
    foreach ($tag in @('v1.1.9-beta', 'v1.1.9-beta_snow-shot')) {
        Set-FixtureRelease $tag
        Expect-Failure { New-SnowShotScoopManifest $tag $output } 'stable release'
    }
    Set-FixtureRelease
    $release.prerelease = $true
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'stable published release'
    foreach ($tag in @('v1.1.7_snow-image', '1.1.7', 'v01.1.7', 'v1.2.3-beta.01', "v1.2.3`n")) {
        Expect-Failure { Get-SnowShotScoopVersion $tag } 'release tag|version|input string'
    }
    Set-FixtureRelease
    $release.draft = $true
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'published release'
    Set-FixtureRelease
    $release.tag_name = 'v1.1.6-beta'
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'published release'
    Set-FixtureRelease
    $release.assets = @()
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'exactly one'
    Set-FixtureRelease
    $release.assets += $release.assets[0]
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'exactly one'
    Set-FixtureRelease
    $release.assets[0].browser_download_url = 'https://example.com/package.zip'
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'Unexpected asset URL'
    Set-FixtureRelease
    $release.assets[0].size++
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'size'
    Set-FixtureRelease
    $release.assets[0].digest = 'sha256:' + ('0' * 64)
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'SHA-256 mismatch'
    Set-FixtureRelease
    $release.assets[0].digest = 'invalid'
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'Invalid GitHub'
    Set-FixtureRelease
    $release.assets[0].PSObject.Properties.Remove('digest')
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'At least one'
    Add-FixtureSidecar
    $null = New-SnowShotScoopManifest 'v1.1.8' $output
    Set-FixtureRelease
    Add-FixtureSidecar
    $null = New-SnowShotScoopManifest 'v1.1.8' $output
    $script:sidecarText = ('0' * 64) + "  $script:name"
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'SHA-256 mismatch'
    $script:sidecarText = "$script:hash  another.zip"
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'Invalid checksum'
    $release.assets[1].browser_download_url = 'https://example.com/checksum'
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'Unexpected asset URL'
    foreach ($case in @(@{ Marker = 'elsewhere' }, @{ Executable = 'snow_shot.exe' },
        @{ Extra = '../outside' }, @{ Extra = 'BIN/snow_shot.exe' })) {
        New-FixtureArchive @case
        Set-FixtureRelease
        Expect-Failure { New-SnowShotScoopManifest 'v1.1.8' $output } 'marker|must contain|Unsafe or duplicate'
    }
    New-FixtureArchive
    Set-FixtureRelease
    $null = New-SnowShotScoopManifest 'v1.1.8' $output
    $firstText = Get-Content $output -Raw
    $null = New-SnowShotScoopManifest 'v1.1.8' $output
    Require ((Get-Content $output -Raw) -ceq $firstText) 'Repeat generation changed manifest'
    $manifest = $firstText | ConvertFrom-Json
    $releases = @(
        (New-CheckverRelease 'v8.0.0_snow-image')
        (New-CheckverRelease 'v7.0.0' -Draft $true)
        (New-CheckverRelease 'v6.0.0' -Prerelease $true)
        (New-CheckverRelease 'v5.0.0-beta')
        (New-CheckverRelease 'v4.0.0_snow-shot')
        (New-CheckverRelease 'v3.0.0_snow-shot')
        (New-CheckverRelease 'v2.0.0_snow-shot')
        (New-CheckverRelease 'v1.1.9_snow-shot')
        (New-CheckverRelease 'v1.1.10_snow-shot')
        (New-CheckverRelease 'v1.1.8')
    )
    $releases[4].assets = @() # Stable release with no Windows portable package.
    $releases[5].assets[0].name = 'snow-shot-3.0.0-windows-x64-offline.exe'
    $releases[6].assets[0].browser_download_url = 'https://example.com/portable.zip'
    $match = Get-FixtureCheckverMatch $manifest $releases
    Require ($match.Success -and $match.Groups['version'].Value -ceq '1.1.10' -and
        $match.Groups['tag'].Value -ceq 'v1.1.10_snow-shot') 'checkver must choose newest eligible Snow Shot stable ZIP'
    $match = Get-FixtureCheckverMatch $manifest @($releases[0..6])
    Require (-not $match.Success) 'checkver accepted an ineligible release'
    $match = Get-FixtureCheckverMatch $manifest @()
    Require (-not $match.Success) 'checkver accepted an empty release list'
    Write-Output 'Snow Shot Scoop fixture tests passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolved -Leaf) -notlike 'snow-shot-scoop-tests-*') { throw 'Unsafe fixture cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

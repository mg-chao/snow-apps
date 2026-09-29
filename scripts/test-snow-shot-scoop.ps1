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
function Set-FixtureRelease([string]$Tag = 'v1.1.7-beta') {
    $version = Get-SnowShotScoopVersion $Tag
    $script:name = "snow-shot-$version-windows-x64-portable.zip"
    $script:release = [pscustomobject]@{
        tag_name = $Tag; draft = $false; prerelease = $true
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
try {
    New-FixtureArchive
    $output = Join-Path $root 'snowshot.json'
    foreach ($tag in @('v1.1.7-beta', 'v1.1.7-beta_snow-shot', 'v2.0.0', 'v2.0.0_snow-shot')) {
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
        Require (-not $text.Contains("`r")) 'Expected LF JSON'
        if ($SchemaPath) { Require (Test-Json -Json $text -SchemaFile $SchemaPath) 'Schema validation failed' }
    }
    foreach ($tag in @('v1.1.7_snow-image', '1.1.7', 'v01.1.7', 'v1.2.3-beta.01', "v1.2.3`n")) {
        Expect-Failure { Get-SnowShotScoopVersion $tag } 'release tag|version|input string'
    }
    Set-FixtureRelease
    $release.draft = $true
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'published release'
    Set-FixtureRelease
    $release.tag_name = 'v1.1.6-beta'
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'published release'
    Set-FixtureRelease
    $release.assets = @()
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'exactly one'
    Set-FixtureRelease
    $release.assets += $release.assets[0]
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'exactly one'
    Set-FixtureRelease
    $release.assets[0].browser_download_url = 'https://example.com/package.zip'
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'Unexpected asset URL'
    Set-FixtureRelease
    $release.assets[0].size++
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'size'
    Set-FixtureRelease
    $release.assets[0].digest = 'sha256:' + ('0' * 64)
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'SHA-256 mismatch'
    Set-FixtureRelease
    $release.assets[0].digest = 'invalid'
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'Invalid GitHub'
    Set-FixtureRelease
    $release.assets[0].PSObject.Properties.Remove('digest')
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'At least one'
    Add-FixtureSidecar
    $null = New-SnowShotScoopManifest 'v1.1.7-beta' $output
    Set-FixtureRelease
    Add-FixtureSidecar
    $null = New-SnowShotScoopManifest 'v1.1.7-beta' $output
    $script:sidecarText = ('0' * 64) + "  $script:name"
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'SHA-256 mismatch'
    $script:sidecarText = "$script:hash  another.zip"
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'Invalid checksum'
    $release.assets[1].browser_download_url = 'https://example.com/checksum'
    Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'Unexpected asset URL'
    foreach ($case in @(@{ Marker = 'elsewhere' }, @{ Executable = 'snow_shot.exe' },
        @{ Extra = '../outside' }, @{ Extra = 'BIN/snow_shot.exe' })) {
        New-FixtureArchive @case
        Set-FixtureRelease
        Expect-Failure { New-SnowShotScoopManifest 'v1.1.7-beta' $output } 'marker|must contain|Unsafe or duplicate'
    }
    New-FixtureArchive
    Set-FixtureRelease
    $null = New-SnowShotScoopManifest 'v1.1.7-beta' $output
    $existing = Join-Path $root 'existing.json'
    Require ((Get-SnowShotScoopUpdateAction $existing $output) -ceq 'update') 'Initial manifest'
    Copy-Item $output $existing
    Require ((Get-SnowShotScoopUpdateAction $existing $output) -ceq 'unchanged') 'Idempotence'
    [IO.File]::WriteAllText($existing, [IO.File]::ReadAllText($output).Replace("`n", "`r`n"))
    Require ((Get-SnowShotScoopUpdateAction $existing $output) -ceq 'unchanged') 'Windows checkout idempotence'
    foreach ($pair in @(@('1.1.6', 'update'), @('1.1.7', 'older'), @('1.1.7-beta.2', 'older'),
        @('1.1.7-alpha', 'update'), @('1.1.10-beta', 'older'))) {
        $manifest = Get-Content $output -Raw | ConvertFrom-Json
        $manifest.version = $pair[0]
        $manifest | ConvertTo-Json -Depth 8 | Set-Content $existing
        Require ((Get-SnowShotScoopUpdateAction $existing $output) -ceq $pair[1]) "Ordering: $($pair[0])"
    }
    Copy-Item $output $existing -Force
    [IO.File]::AppendAllText($existing, ' ')
    Expect-Failure { Get-SnowShotScoopUpdateAction $existing $output } 'different content'
    if ($SchemaPath) {
        Require (Test-Json -Json (Get-Content "$PSScriptRoot/../bucket/snowshot.json" -Raw) -SchemaFile $SchemaPath) 'Bucket schema'
    }
    Write-Output 'Snow Shot Scoop fixture tests passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolved -Leaf) -notlike 'snow-shot-scoop-tests-*') { throw 'Unsafe fixture cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

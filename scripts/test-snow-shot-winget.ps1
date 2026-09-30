#Requires -Version 7.0
# Deterministic fixtures only: no GitHub requests, installer execution, or PR submissions.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-winget.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) "snow-shot-winget-tests-$([guid]::NewGuid().ToString('N'))"
$originalToken = $env:WINGET_CREATE_GITHUB_TOKEN
$script:submitted = 0
$script:submitExitCode = 0
$script:requests = @()
$script:mode = 'missing'

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
function Invoke-SnowShotWingetApi([string]$Path) {
    $script:requests += $Path
    if ($Path -like 'repos/mg-chao/snow-apps/releases/tags/*') { return $script:release }
    if ($Path -like '*/contents/*') {
        if ($script:mode -eq 'merged') { return @(@{ name = 'manifest.yaml' }) }
        $status = if ($script:mode -eq 'rate-limit') { 403 } else { 404 }
        $response = [Net.Http.HttpResponseMessage]::new([Net.HttpStatusCode]$status)
        throw [Microsoft.PowerShell.Commands.HttpResponseException]::new('Fixture HTTP failure', $response)
    }
    if ($script:mode -eq 'paginated' -and $Path -like '*page=1') {
        return @{ incomplete_results = $false; total_count = 101; items = @() }
    }
    return @{
        incomplete_results = ($script:mode -eq 'incomplete')
        total_count = 1
        items = @(@{
            title = if ($script:mode -in @('pending', 'paginated')) { 'New version: mg-chao.snow-shot version 1.1.5-beta' }
                    else { 'New version: mg-chao.snow-shot version 1.1.5-beta.2' }
            html_url = 'https://github.com/microsoft/winget-pkgs/pull/123'
        })
    }
}
function Invoke-WebRequest([string]$Uri, [string]$OutFile, [int]$TimeoutSec) {
    Require ($Uri -ceq $script:release.assets[0].browser_download_url) 'Wrong download URL'
    [IO.File]::WriteAllBytes($OutFile, [byte[]](1, 2, 3, 4))
}
function Invoke-FixtureSubmit {
    $script:submitted++
    Require ($args -contains '--no-open') 'Submission must not open a browser'
    Require (-not ($args -contains '--token')) 'Token must not be passed on the command line'
    Require (-not ($args -contains '--replace')) 'Submission must not replace versions'
    $global:LASTEXITCODE = $script:submitExitCode
}
function Set-FixtureRelease([string]$Tag, [string]$Edition = 'Full') {
    $version = Get-SnowShotWingetVersion $Tag
    $name = if ($Edition -eq 'Mini') { "snow-shot-mini-$version-windows-x64-online.exe" } else { "snow-shot-$version-windows-x64-offline.exe" }
    $script:release = @{
        tag_name = $Tag; draft = $false; prerelease = $true
        published_at = '2026-09-28T01:30:00+02:00'
        body = "## Changes`r`n`r`n- Fix: screenshot capture`r`n  Nested text`r`n---`r`n雪`r`n"
        assets = @(@{ name = $name; size = 4
            browser_download_url = "https://github.com/mg-chao/snow-apps/releases/download/$Tag/$name" })
    }
}
try {
    Set-FixtureRelease 'v1.2.0_snow-shot' Mini
    $miniDirectory = New-SnowShotWingetManifest 'v1.2.0_snow-shot' $root Mini
    $miniInstaller = Get-Content (Join-Path $miniDirectory 'mg-chao.snow-shot-mini.installer.yaml') -Raw
    Require ($miniInstaller.Contains('ProductCode: SnowShotMini') -and $miniInstaller.Contains('snow-shot-mini-1.2.0-windows-x64-online.exe')) 'Mini WinGet has its own registration and online installer.'
    $miniLocale = Get-Content (Join-Path $miniDirectory 'mg-chao.snow-shot-mini.locale.en-US.yaml') -Raw
    Require ($miniLocale.Contains('PackageName: Snow Shot Mini') -and -not $miniLocale.Contains('  - translate')) 'Mini metadata must describe retained features.'
    foreach ($tag in @('v1.1.5-beta', 'v1.1.5-beta_snow-shot', 'v1.2.0', 'v1.2.0_snow-shot')) {
        Set-FixtureRelease $tag
        $directory = New-SnowShotWingetManifest $tag $root
        $files = @(Get-ChildItem -LiteralPath $directory -Filter '*.yaml')
        Require ($files.Count -eq 3) 'Expected three manifests'
        foreach ($file in $files) {
            $content = Get-Content -LiteralPath $file.FullName -Raw
            Require ($content.Contains("PackageVersion: '$(Get-SnowShotWingetVersion $tag)'")) 'Version mismatch'
            Require ($content.Contains('ManifestVersion: 1.12.0')) 'Schema mismatch'
            Require (-not $content.Contains("`r")) 'Manifests must use LF'
        }
        $installer = Get-Content -LiteralPath (Join-Path $directory 'mg-chao.snow-shot.installer.yaml') -Raw
        $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([byte[]](1, 2, 3, 4)))
        Require ($installer.Contains("InstallerSha256: $hash")) 'Incorrect SHA-256'
        Require ($installer.Contains("/$tag/")) 'Wrong release URL'
        Require ($installer.Contains('ReturnResponse: packageInUse')) 'Missing running-app response'
        Require ($installer.Contains('InstallerLocale: en-US')) 'Missing installer locale'
        Require ($installer.Contains('ReleaseDate: 2026-09-27')) 'Release date must use the publication date in UTC'
        Require ($installer.Contains("InstallationMetadata:`n  DefaultInstallLocation: '%ProgramFiles%\SnowShot'")) 'Incorrect default install location'
        $locale = Get-Content -LiteralPath (Join-Path $directory 'mg-chao.snow-shot.locale.en-US.yaml') -Raw
        foreach ($keyword in @('chatbot', 'ocr', 'screen-capture', 'screenshot', 'snowshot', 'translate', 'annotation')) {
            Require ($locale.Contains("  - $keyword`n")) "Missing package tag: $keyword"
        }
        Require ($locale.Contains("ReleaseNotes: |2-`n  ## Changes`n  `n  - Fix: screenshot capture`n    Nested text`n  ---`n  雪`nReleaseNotesUrl:")) 'Release notes must retain literal multiline content'
    }
    foreach ($body in @('', '   ', ('x' * 10001))) {
        Set-FixtureRelease 'v1.1.5-beta'
        $script:release.body = $body
        Expect-Failure { New-SnowShotWingetManifest 'v1.1.5-beta' $root } 'release notes must contain'
    }
    foreach ($tag in @('v1.2.0_viewer', '1.2.0', 'v01.2.0', 'v1.2.0-beta..1', "v1.2.0`nextra", "v1.2.0`n")) {
        Expect-Failure { Get-SnowShotWingetVersion $tag } 'Not a Snow Shot release'
    }
    Set-FixtureRelease 'v1.1.5-beta'
    $script:release.tag_name = 'v1.1.4-beta'
    Expect-Failure { New-SnowShotWingetManifest 'v1.1.5-beta' $root } 'published release'
    Set-FixtureRelease 'v1.1.5-beta'
    $script:release.draft = $true
    Expect-Failure { New-SnowShotWingetManifest 'v1.1.5-beta' $root } 'published release'
    Set-FixtureRelease 'v1.1.5-beta'
    $script:release.assets = @()
    Expect-Failure { New-SnowShotWingetManifest 'v1.1.5-beta' $root } 'exactly one'
    Set-FixtureRelease 'v1.1.5-beta'
    $script:release.assets += $script:release.assets[0]
    Expect-Failure { New-SnowShotWingetManifest 'v1.1.5-beta' $root } 'exactly one'
    Set-FixtureRelease 'v1.1.5-beta'
    $script:release.assets[0].size = 5
    Expect-Failure { New-SnowShotWingetManifest 'v1.1.5-beta' $root } 'size does not match'
    Set-FixtureRelease 'v1.1.5-beta'
    $script:release.assets[0].browser_download_url = 'https://snowshot.top/setup/latest.exe'
    Expect-Failure { New-SnowShotWingetManifest 'v1.1.5-beta' $root } 'Unexpected installer asset URL'
    Set-FixtureRelease 'v1.1.5-beta'
    $script:release.assets += @{ name = 'snow-shot-1.1.5-beta-windows-x64-online.exe' }
    $directory = New-SnowShotWingetManifest 'v1.1.5-beta' $root
    Require (@(Get-ChildItem -LiteralPath $root -Filter '*.exe').Count -eq 0) 'Installer downloads were not cleaned up'

    $env:WINGET_CREATE_GITHUB_TOKEN = ''
    foreach ($mode in @('merged', 'pending', 'paginated')) {
        $script:mode = $mode
        $result = Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit'
        Require ($result -match 'https://github.com/microsoft/winget-pkgs/') 'Missing duplicate link'
    }
    Require ($script:submitted -eq 0) 'Duplicate submitted'
    $script:mode = 'missing'
    Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'repository secret'
    $env:WINGET_CREATE_GITHUB_TOKEN = 'fixture-token'
    Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit'
    Require ($script:submitted -eq 1) 'New version not submitted'
    $script:mode = 'rate-limit'
    Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'Fixture HTTP failure'
    $script:mode = 'incomplete'
    Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'incomplete submission search'
    Require ($script:submitted -eq 1) 'Failed duplicate lookup must not submit'
    $script:mode = 'missing'
    $script:submitExitCode = 1
    Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'submission failed'
    $global:LASTEXITCODE = 0
    # Real installer checks and reputation-policy changes must never act on a developer PC.
    $runnerEnvironment = $env:RUNNER_ENVIRONMENT
    $runnerOs = $env:RUNNER_OS
    try {
        $env:RUNNER_ENVIRONMENT = ''
        $env:RUNNER_OS = 'Windows'
        $installTest = Join-Path $PSScriptRoot 'test-snow-shot-winget-install.ps1'
        Expect-Failure { & $installTest -AllowUnrecognizedRelease } 'require a disposable GitHub-hosted Windows runner'
    } finally {
        $env:RUNNER_ENVIRONMENT = $runnerEnvironment
        $env:RUNNER_OS = $runnerOs
    }
    # Provisioning must upgrade older clients using the requested official bundle.
    function Get-Command { param($Name, $ErrorAction); return @{ Source = 'Invoke-FixtureWinget' } }
    function Get-AppxPackage { param($Name); return @() }
    function Invoke-FixtureWinget { $global:LASTEXITCODE = 0; return "v$script:fixtureWingetVersion" }
    function Invoke-WebRequest {
        param($Uri, $OutFile)
        Require ($Uri.StartsWith('https://github.com/microsoft/winget-cli/releases/download/v1.29.380/')) 'Wrong client release'
        [IO.File]::WriteAllText($OutFile, 'fixture')
    }
    function Expand-Archive {
        param($LiteralPath, $DestinationPath, [switch]$Force)
        $null = New-Item -ItemType Directory -Force -Path (Join-Path $DestinationPath 'x64')
        [IO.File]::WriteAllText((Join-Path $DestinationPath 'x64/dependency.appx'), 'fixture')
    }
    function Add-AppxPackage {
        param($Path, $DependencyPath)
        Require ($DependencyPath.Count -gt 0) 'Client provisioning must install matching dependencies'
        $script:repairCalls++
        if (-not $script:repairBroken) { $script:fixtureWingetVersion = '1.29.380' }
    }
    $script:repairCalls = 0
    $script:repairBroken = $false
    $script:fixtureWingetVersion = '1.26.510'
    $null = . (Join-Path $PSScriptRoot 'initialize-snow-shot-winget.ps1') -ToolDirectory (Join-Path $root 'client')
    Require ($script:repairCalls -eq 1) 'Older client was not upgraded'
    $null = . (Join-Path $PSScriptRoot 'initialize-snow-shot-winget.ps1') -ToolDirectory (Join-Path $root 'client')
    Require ($script:repairCalls -eq 1) 'Compatible client was unnecessarily reinstalled'
    $script:repairBroken = $true
    $script:fixtureWingetVersion = '1.26.510'
    Expect-Failure { . (Join-Path $PSScriptRoot 'initialize-snow-shot-winget.ps1') -ToolDirectory (Join-Path $root 'client') } 'or newer is required'
    Write-Output 'PASS: Snow Shot WinGet generation and submission fixtures.'
} finally {
    $env:WINGET_CREATE_GITHUB_TOKEN = $originalToken
    # This exact, newly allocated test root is the only directory removed.
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}

#Requires -Version 7.0
# Deterministic fixtures only: no GitHub requests, installer execution, or PR submissions.
$ErrorActionPreference = 'Stop'
# Load the web exception types before replacing Invoke-WebRequest with a fixture.
Import-Module Microsoft.PowerShell.Utility
. (Join-Path $PSScriptRoot 'snow-shot-winget.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) "snow-shot-winget-tests-$([guid]::NewGuid().ToString('N'))"
$originalToken = $env:WINGET_CREATE_GITHUB_TOKEN
$script:submitted = 0
$script:submitExitCode = 0
$script:requests = @()
$script:mode = 'missing'
$script:downloadFailures = @()
$script:downloadAttempts = 0
$script:submissionScopes = @('public_repo', 'workflow')
$script:credentialRequests = 0
$script:credentialFailure = $null

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
    if ($Path -like 'repos/mg-chao/snow-shot/releases/tags/*') { return $script:release }
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
function Invoke-WebRequest([string]$Uri, [string]$OutFile, [int]$TimeoutSec, [hashtable]$Headers) {
    if ($Uri -ceq 'https://api.github.com/user') {
        Require ($Headers.Authorization -ceq 'Bearer fixture-token') 'Credential checks must use the submission token'
        $script:credentialRequests++
        if ($script:credentialFailure) { throw $script:credentialFailure }
        return @{
            Content = '{"login":"fixture-account"}'
            Headers = @{ 'X-OAuth-Scopes' = $script:submissionScopes -join ', ' }
        }
    }
    Require ($Uri -ceq $script:release.assets[0].browser_download_url) 'Wrong download URL'
    Require (-not (Test-Path -LiteralPath $OutFile)) 'A partial installer download was reused'
    $script:downloadAttempts++
    if ($script:downloadFailures.Count) {
        $failure = $script:downloadFailures[0]
        $script:downloadFailures = @($script:downloadFailures | Select-Object -Skip 1)
        [IO.File]::WriteAllBytes($OutFile, [byte[]](9))
        throw $failure
    }
    [IO.File]::WriteAllBytes($OutFile, [byte[]](1, 2, 3, 4))
}
function Invoke-FixtureSubmit {
    $script:submitted++
    Require ($args -contains '--no-open') 'Submission must not open a browser'
    Require (-not ($args -contains '--token')) 'Token must not be passed on the command line'
    Require (-not ($args -contains '--replace')) 'Submission must not replace versions'
    $global:LASTEXITCODE = $script:submitExitCode
}
function Set-FixtureRelease([string]$Tag) {
    $version = Get-SnowShotWingetVersion $Tag
    $name = "snow-shot-$version-windows-x64-offline.exe"
    $script:release = @{
        tag_name = $Tag; draft = $false; prerelease = $true
        published_at = '2026-09-28T01:30:00+02:00'
        body = "## Changes`r`n`r`n- Fix: screenshot capture`r`n  Nested text`r`n---`r`n雪`r`n"
        assets = @(@{ name = $name; size = 4
            browser_download_url = "https://github.com/mg-chao/snow-shot/releases/download/$Tag/$name" })
    }
}
try {
    Set-FixtureRelease 'v1.2.0_snow-shot'
    $requestsBeforeMini = $script:requests.Count
    Expect-Failure { New-SnowShotWingetManifest 'v1.2.0_snow-shot' $root Mini } 'Mini'
    Expect-Failure { Get-SnowShotWingetSubmission '1.2.0' Mini } 'Mini'
    Expect-Failure { Submit-SnowShotWingetManifest 'v1.2.0_snow-shot' $root 'Invoke-FixtureSubmit' Mini } 'Mini'
    Expect-Failure { & (Join-Path $PSScriptRoot 'new-snow-shot-winget-manifest.ps1') -Tag 'v1.2.0' -Edition Mini } 'Mini'
    Expect-Failure { & (Join-Path $PSScriptRoot 'submit-snow-shot-winget.ps1') -Tag 'v1.2.0' -Edition Mini -ManifestDirectory $root } 'Mini'
    Expect-Failure { & (Join-Path $PSScriptRoot 'test-snow-shot-winget-install.ps1') -Edition Mini } 'Mini'
    Require ($script:requests.Count -eq $requestsBeforeMini -and $script:submitted -eq 0) 'Unsupported Mini requests must fail before network or submission.'
    Require (-not (Test-Path -LiteralPath $root)) 'Unsupported Mini requests must not generate manifests.'
    # Only transient transport/HTTP failures may retry; partial bytes must never be hashed.
    & {
        function Start-Sleep([int]$Seconds) { $script:retrySleeps += $Seconds }
        $transientFailures = @(
            [Net.Http.HttpRequestException]::new('Fixture connection timeout',
                [Net.Sockets.SocketException]::new(10060))
            [Net.Http.HttpRequestException]::new('Fixture connection reset',
                [Net.Sockets.SocketException]::new(10054))
            [Threading.Tasks.TaskCanceledException]::new('Fixture request timeout',
                [TimeoutException]::new('Fixture timeout'))
        )
        foreach ($status in @(408, 429, 500, 502, 503, 504)) {
            $response = [Net.Http.HttpResponseMessage]::new([Net.HttpStatusCode]$status)
            $transientFailures += [Microsoft.PowerShell.Commands.HttpResponseException]::new('Fixture transient HTTP failure', $response)
        }
        foreach ($failure in $transientFailures) {
            Set-FixtureRelease 'v1.3.6_snow-shot'
            $script:downloadAttempts = 0
            $script:retrySleeps = @()
            $script:downloadFailures = @($failure, $failure)
            $directory = New-SnowShotWingetManifest 'v1.3.6_snow-shot' (Join-Path $root 'recovered')
            Require ($script:downloadAttempts -eq 3) 'Transient downloads must recover within the attempt limit'
            Require (($script:retrySleeps -join ',') -ceq '2,4') 'Retries must use bounded backoff'
            $installer = Get-Content -LiteralPath (Join-Path $directory 'mg-chao.snow-shot.installer.yaml') -Raw
            $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([byte[]](1, 2, 3, 4)))
            Require ($installer.Contains("InstallerSha256: $hash")) 'Recovered installer hash is incorrect'
            Require (@(Get-ChildItem -LiteralPath (Join-Path $root 'recovered') -Filter '*.exe').Count -eq 0) 'Recovered downloads must be cleaned up'
        }
        $permanentFailures = @(
            [IO.IOException]::new('Fixture disk failure')
            [Net.Http.HttpRequestException]::new('Fixture TLS failure',
                [Security.Authentication.AuthenticationException]::new('Fixture certificate failure'))
            [Net.Http.HttpRequestException]::new('Fixture unknown HTTP client failure')
            [Threading.Tasks.TaskCanceledException]::new('Fixture cancellation without timeout')
        )
        foreach ($status in @(400, 401, 403, 404, 501)) {
            $response = [Net.Http.HttpResponseMessage]::new([Net.HttpStatusCode]$status)
            $permanentFailures += [Microsoft.PowerShell.Commands.HttpResponseException]::new('Fixture permanent HTTP failure', $response)
        }
        foreach ($failure in @($transientFailures[0]) + $permanentFailures) {
            $script:downloadAttempts = 0
            $script:retrySleeps = @()
            $script:downloadFailures = @($failure, $failure, $failure)
            $failedOutput = Join-Path $root "failed-$([guid]::NewGuid().ToString('N'))"
            Expect-Failure { New-SnowShotWingetManifest 'v1.3.6_snow-shot' $failedOutput } ([regex]::Escape($failure.Message))
            $transient = $failure -eq $transientFailures[0]
            Require ($script:downloadAttempts -eq $(if ($transient) { 3 } else { 1 })) 'Permanent errors must fail immediately and transient retries must be bounded'
            Require ($script:retrySleeps.Count -eq $(if ($transient) { 2 } else { 0 })) 'Unexpected retry delay after failure'
            Require (@(Get-ChildItem -LiteralPath $failedOutput -Recurse -File).Count -eq 0) 'Failed downloads must leave no installer or manifest files'
        }
        $script:downloadFailures = @()
        $script:downloadAttempts = 0
        $script:retrySleeps = @()
        $script:release.assets[0].size = 5
        $invalidOutput = Join-Path $root 'invalid-size'
        Expect-Failure { New-SnowShotWingetManifest 'v1.3.6_snow-shot' $invalidOutput } 'size does not match'
        Require ($script:downloadAttempts -eq 1 -and $script:retrySleeps.Count -eq 0) 'Validation failures must not retry'
        Require (@(Get-ChildItem -LiteralPath $invalidOutput -Recurse -File).Count -eq 0) 'Invalid installers must be cleaned up'
    }
    # Here-string line endings follow the checkout, while generated YAML must always use LF.
    $source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'snow-shot-winget.ps1')).Replace("`r`n", "`n")
    $expectedManifests = @{}
    foreach ($checkout in @('LF', 'CRLF')) {
        $sourceDirectory = Join-Path $root $checkout
        $null = New-Item -ItemType Directory -Force -Path $sourceDirectory
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'snow-shot-editions.ps1') -Destination $sourceDirectory
        $newline = if ($checkout -eq 'CRLF') { "`r`n" } else { "`n" }
        $sourcePath = Join-Path $sourceDirectory 'snow-shot-winget.ps1'
        [IO.File]::WriteAllText($sourcePath, $source.Replace("`n", $newline), [Text.UTF8Encoding]::new($false))
        & {
            . $sourcePath
            function Invoke-SnowShotWingetApi([string]$Path) { return $script:release }
            Set-FixtureRelease 'v1.2.0_snow-shot'
            $directory = New-SnowShotWingetManifest 'v1.2.0_snow-shot' $sourceDirectory
            $locale = Get-Content (Join-Path $directory 'mg-chao.snow-shot.locale.en-US.yaml') -Raw
            Require ($locale.Contains('PackageName: Snow Shot')) "$checkout package name mismatch."
            foreach ($tag in @('chatbot', 'screen-capture', 'screenshot', 'snowshot', 'translate', 'annotation', 'ocr')) {
                Require ($locale.Contains("  - $tag`n")) "$checkout is missing retained tag: $tag"
            }
            foreach ($file in Get-ChildItem -LiteralPath $directory -Filter '*.yaml') {
                $content = [IO.File]::ReadAllText($file.FullName)
                Require (-not $content.Contains("`r")) "$checkout manifests must use LF."
                if ($checkout -eq 'LF') { $expectedManifests[$file.Name] = $content }
                else { Require ($content -ceq $expectedManifests[$file.Name]) "Checkout line endings changed $($file.Name)." }
            }
            $script:release.body = "- chatbot`n- translate"
            $directory = New-SnowShotWingetManifest 'v1.2.0_snow-shot' $sourceDirectory
            $locale = Get-Content (Join-Path $directory 'mg-chao.snow-shot.locale.en-US.yaml') -Raw
            Require ($locale.Contains("ReleaseNotes: |2-`n  - chatbot`n  - translate`nReleaseNotesUrl:")) "$checkout release notes must remain literal."
        }
    }
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
    Require ($script:credentialRequests -eq 0) 'Duplicate submissions must not require credentials'
    $script:mode = 'missing'
    Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'repository secret'
    Require ($script:credentialRequests -eq 0) 'Missing tokens must fail without authentication requests'
    $env:WINGET_CREATE_GITHUB_TOKEN = 'fixture-token'
    # A fork can be fast-forwarded through commits that change workflows only with workflow scope.
    foreach ($scopes in @('public_repo', 'repo')) {
        $script:submissionScopes = @($scopes)
        Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'workflow.*WINGET_CREATE_GITHUB_TOKEN'
        Require ($script:submitted -eq 0) 'Tokens without workflow scope must fail before WinGetCreate'
    }
    foreach ($scopes in @('', 'workflow')) {
        $script:submissionScopes = @($scopes)
        Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'classic token with public_repo'
        Require ($script:submitted -eq 0) 'Tokens without repository scope must fail before WinGetCreate'
    }
    foreach ($status in @(401, 403, 500)) {
        $response = [Net.Http.HttpResponseMessage]::new([Net.HttpStatusCode]$status)
        $script:credentialFailure = [Microsoft.PowerShell.Commands.HttpResponseException]::new('Fixture credential failure', $response)
        Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'Fixture credential failure'
        Require ($script:submitted -eq 0) 'Authentication failures must stop submission'
    }
    $script:credentialFailure = $null
    $script:submissionScopes = @('public_repo', 'workflow')
    Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit'
    Require ($script:submitted -eq 1) 'New version not submitted'
    $script:submissionScopes = @('repo', 'workflow')
    Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit'
    Require ($script:submitted -eq 2) 'Full repository scope with workflow must also submit'
    $requestsBeforeLookupFailure = $script:credentialRequests
    $script:mode = 'rate-limit'
    Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'Fixture HTTP failure'
    $script:mode = 'incomplete'
    Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'incomplete submission search'
    Require ($script:submitted -eq 2) 'Failed duplicate lookup must not submit'
    Require ($script:credentialRequests -eq $requestsBeforeLookupFailure) 'Failed duplicate lookup must not authenticate'
    $script:mode = 'missing'
    $script:submitExitCode = 1
    Expect-Failure { Submit-SnowShotWingetManifest 'v1.1.5-beta' $directory 'Invoke-FixtureSubmit' } 'submission failed'
    $global:LASTEXITCODE = 0
    # The standalone read-only check must enforce the same scopes and still validate fork access.
    & {
        $script:forkRequests = 0
        $script:fixtureFork = @{
            fork = $true
            parent = @{ full_name = 'microsoft/winget-pkgs' }
            permissions = @{ push = $true }
            full_name = 'fixture-account/winget-pkgs'
        }
        function Invoke-RestMethod([string]$Uri, [hashtable]$Headers, [string]$Method = 'Get') {
            Require ($Uri -ceq 'https://api.github.com/repos/fixture-account/winget-pkgs') 'Wrong submission account fork'
            Require ($Headers.Authorization -ceq 'Bearer fixture-token') 'Fork checks must use the submission token'
            Require ($Method -ceq 'Get') 'Credential checks must remain read-only'
            $script:forkRequests++
            return $script:fixtureFork
        }
        $credentialTest = Join-Path $PSScriptRoot 'test-snow-shot-winget-credentials.ps1'
        $script:submissionScopes = @('public_repo')
        Expect-Failure { . $credentialTest } 'workflow.*WINGET_CREATE_GITHUB_TOKEN'
        Require ($script:forkRequests -eq 0) 'Invalid scopes must fail before checking the fork'
        foreach ($repositoryScope in @('public_repo', 'repo')) {
            $script:submissionScopes = @($repositoryScope, 'workflow')
            $result = . $credentialTest
            Require ($result -match 'PASS:.*fixture-account.*repository and workflow scopes') 'Credential check must report verified scopes'
            Require (-not ($result -match 'fixture-token')) 'Credential output must not expose the token'
        }
        $script:fixtureFork.fork = $false
        Expect-Failure { . $credentialTest } 'writable fork'
        $script:fixtureFork.fork = $true
        $script:fixtureFork.parent.full_name = 'unrelated/winget-pkgs'
        Expect-Failure { . $credentialTest } 'writable fork'
        $script:fixtureFork.parent.full_name = 'microsoft/winget-pkgs'
        $script:fixtureFork.permissions.push = $false
        Expect-Failure { . $credentialTest } 'writable fork'
    }
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
    & (Join-Path $PSScriptRoot 'test-snow-shot-winget-desktop.ps1')
    & (Join-Path $PSScriptRoot 'test-snow-shot-winget-attachment.ps1')
    & (Join-Path $PSScriptRoot 'test-snow-shot-winget-windows.ps1')
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
        foreach ($nativeArchitecture in @('x64', 'arm64')) {
            $null = New-Item -ItemType Directory -Force -Path (Join-Path $DestinationPath $nativeArchitecture)
            [IO.File]::WriteAllText((Join-Path $DestinationPath "$nativeArchitecture/dependency.appx"), 'fixture')
        }
    }
    function Add-AppxPackage {
        param($Path, $DependencyPath, [switch]$ForceApplicationShutdown)
        Require ($DependencyPath.Count -gt 0) 'Client provisioning must install matching dependencies'
        $nativeArchitecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
        Require (@($DependencyPath | Where-Object { (Split-Path (Split-Path $_ -Parent) -Leaf) -cne $nativeArchitecture }).Count -eq 0) `
            'Client provisioning must select only the host architecture dependencies.'
        if ($script:dependenciesInUse -and -not $ForceApplicationShutdown) {
            throw 'Fixture dependency is in use (0x80073D02).'
        }
        Require ($ForceApplicationShutdown.IsPresent -eq $script:expectForceShutdown) 'Only disposable hosted Windows runners may close dependency applications.'
        $script:repairCalls++
        if (-not $script:repairBroken) { $script:fixtureWingetVersion = '1.29.380' }
    }
    $script:repairCalls = 0
    $script:repairBroken = $false
    try {
        $env:RUNNER_ENVIRONMENT = 'github-hosted'
        $env:RUNNER_OS = 'Windows'
        $script:expectForceShutdown = $true
        $script:dependenciesInUse = $true
        $script:fixtureWingetVersion = '1.26.510'
        $null = . (Join-Path $PSScriptRoot 'initialize-snow-shot-winget.ps1') -ToolDirectory (Join-Path $root 'client')
        Require ($script:repairCalls -eq 1) 'Older client was not upgraded with active dependencies'
        $null = . (Join-Path $PSScriptRoot 'initialize-snow-shot-winget.ps1') -ToolDirectory (Join-Path $root 'client')
        Require ($script:repairCalls -eq 1) 'Compatible client was unnecessarily reinstalled'
        $env:RUNNER_ENVIRONMENT = ''
        $script:expectForceShutdown = $false
        $script:dependenciesInUse = $false
        $script:fixtureWingetVersion = '1.26.510'
        $null = . (Join-Path $PSScriptRoot 'initialize-snow-shot-winget.ps1') -ToolDirectory (Join-Path $root 'client')
        Require ($script:repairCalls -eq 2) 'Local client provisioning must not force application shutdown'
        $script:repairBroken = $true
        $script:fixtureWingetVersion = '1.26.510'
        Expect-Failure { . (Join-Path $PSScriptRoot 'initialize-snow-shot-winget.ps1') -ToolDirectory (Join-Path $root 'client') } 'or newer is required'
    } finally {
        $env:RUNNER_ENVIRONMENT = $runnerEnvironment
        $env:RUNNER_OS = $runnerOs
    }
    Write-Output 'PASS: Snow Shot WinGet generation and submission fixtures.'
} finally {
    $env:WINGET_CREATE_GITHUB_TOKEN = $originalToken
    # This exact, newly allocated test root is the only directory removed.
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}

# Focused syntax/preview contracts. Never builds or connects to an SSH host.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
foreach ($name in @('publish-snow-shot-release.ps1', 'package-snow-shot-remote-macos.ps1',
    'publish-snow-shot-release.local.example.ps1')) {
    $tokens = $null
    $errors = $null
    $null = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
}
$publisher = Join-Path $PSScriptRoot 'publish-snow-shot-release.ps1'
$version = [regex]::Match((Get-Content -Raw (Join-Path (Split-Path -Parent $PSScriptRoot) 'CMakeLists.txt')), 'set\(SNOW_SHOT_VERSION "([^"]+)"\)').Groups[1].Value
$settings = @{ WhatIf = $true }
$windows = @(& $publisher @settings)
if ($windows.Count -ne 52 -or @($windows | Where-Object { $_ -like '*-macos-*.dmg*' }).Count) { throw 'Windows preview changed unexpectedly.' }
$combined = @(& $publisher @settings -MacHost 'mac.invalid' -MacUser 'test' -MacProjectDirectory '/Users/test/snow-apps')
if ($combined.Count -ne 59 -or $combined[-1] -cne 'install-snow-shot-macos.sh') { throw 'Combined preview has the wrong file order/count.' }
foreach ($name in @("snow-shot-$version-macos-arm64.dmg", "snow-shot-$version-macos-arm64.dmg.sha256", "snow-shot-mini-$version-macos-arm64.dmg", "snow-shot-mini-$version-macos-arm64.dmg.sha256", "snow-shot-$version-macos-x86_64.dmg", "snow-shot-$version-macos-x86_64.dmg.sha256", 'install-snow-shot-macos.sh')) {
    if ($combined -cnotcontains $name) { throw "Missing macOS artifact: $name" }
}
if (@($combined | Where-Object { $_ -like 'snow-shot-mini-*-macos-x86_64*' -or $_ -like '*-macos-x64*' }).Count) {
    throw 'Intel releases must contain Full only and use the x86_64 asset suffix.'
}
$armOnly = @(& $publisher @settings -MacHost 'mac.invalid' -MacUser 'test' -MacProjectDirectory '/Users/test/snow-apps' -MacArchitectures arm64)
if ($armOnly.Count -ne 57 -or @($armOnly | Where-Object { $_ -like '*-macos-x86_64*' }).Count) {
    throw 'Explicit ARM64 preview must preserve the paired macOS release.'
}
$intelOnly = @(& $publisher @settings -MacHost 'mac.invalid' -MacUser 'test' -MacProjectDirectory '/Users/test/snow-apps' -MacArchitectures x64)
if ($intelOnly.Count -ne 55 -or $intelOnly -cnotcontains "snow-shot-$version-macos-x86_64.dmg" -or
    @($intelOnly | Where-Object { $_ -like '*-macos-arm64*' -or $_ -like 'snow-shot-mini-*-macos-*' }).Count) {
    throw 'Explicit Intel preview must select only the Intel Full DMG.'
}
try {
    & $publisher @settings -MacArchitectures @('arm64', 'arm64')
    throw 'Expected duplicate architectures to be rejected.'
} catch {
    if ($_.Exception.Message -cne 'MacArchitectures must not contain duplicate architectures.') { throw }
}
try {
    & $publisher @settings -MacHost 'mac.invalid'
    throw 'Expected incomplete Mac settings to be rejected.'
} catch {
    if ($_.Exception.Message -cne 'MacProjectDirectory is required with MacHost.') { throw }
}
$fixture = Join-Path ([IO.Path]::GetTempPath()) "snow-remote-macos-tests-$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $fixture
try {
    # Run the real request construction without starting SSH. The existing
    # single-architecture entry point remains ARM64 Full+Mini by default.
    $wrapper = Get-Content -Raw (Join-Path $PSScriptRoot 'package-snow-shot-remote-macos.ps1')
    $requestBuilder = [scriptblock]::Create(($wrapper -split '\$encoded =', 2)[0] + "`n" + '$request | ConvertTo-Json -Compress')
    $parameters = @{ MacHost = 'mac.invalid'; MacUser = 'test'; MacProjectDirectory = '/Users/test/snow-apps';
        Version = $version; OutputDirectory = $fixture }
    $request = (& $requestBuilder @parameters) | ConvertFrom-Json
    if ($request.architecture -cne 'arm64' -or ($request.editions -join ',') -cne 'Full,Mini') {
        throw 'Default remote request must retain the paired ARM64 editions.'
    }
    $request = (& $requestBuilder @parameters -Architecture X64 -SkipBuild) | ConvertFrom-Json
    if ($request.architecture -cne 'x64' -or ($request.editions -join ',') -cne 'Full' -or -not $request.skipBuild) {
        throw 'Intel remote requests must contain only Full and preserve SkipBuild.'
    }

    # Exercise the publisher's actual job body using a local harmless worker.
    $tokens = $null
    $errors = $null
    $publisherAst = [Management.Automation.Language.Parser]::ParseFile($publisher, [ref]$tokens, [ref]$errors)
    $dispatch = $publisherAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Start-Job'
    }, $true)[0]
    $dispatchBlock = @($dispatch.CommandElements | Where-Object {
        $_ -is [Management.Automation.Language.ScriptBlockExpressionAst]
    })[0].ScriptBlock.GetScriptBlock()
    $worker = Join-Path $fixture 'worker.ps1'
    [IO.File]::WriteAllText($worker, @'
param([string]$Architecture, [string]$OutputDirectory, [switch]$ChangedSource)
$metadataBase = if ($Architecture -eq 'arm64') { 'macos-build' } else { 'macos-x64-build' }
$hash = if ($ChangedSource -and $Architecture -eq 'x64') { 'changed' } else { 'unchanged' }
@{ source = @{ commit = 'fixture'; workingTreeSha256 = $hash } } |
    ConvertTo-Json | Set-Content (Join-Path $OutputDirectory "$metadataBase.json")
Write-Output $Architecture
'@)
    $job = Start-Job -ScriptBlock $dispatchBlock -ArgumentList $worker, @{ OutputDirectory = $fixture }, @('arm64', 'x64')
    try {
        $dispatched = @($job | Wait-Job | Receive-Job -ErrorAction Stop)
        if ($job.State -ne 'Completed' -or ($dispatched -join ',') -cne 'arm64,x64') {
            throw 'Coordinated packaging must dispatch each selected macOS architecture once.'
        }
    } finally { Remove-Job $job }
    try {
        & $dispatchBlock $worker @{ OutputDirectory = $fixture; ChangedSource = $true } @('arm64', 'x64') | Out-Null
        throw 'Expected mixed-source macOS architectures to be rejected.'
    } catch {
        if ($_.Exception.Message -cne 'macOS architectures were packaged from different source checkouts.') { throw }
    }

    # Verify the real macOS checksum loop against local DMGs. Native Windows
    # auditors, SSH, GitHub and published releases are outside this fixture.
    $verification = $publisherAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.ForEachStatementAst] -and
        $node.Variable.VariablePath.UserPath -eq 'arch'
    }, $true)[0]
    $verifyPackages = [scriptblock]::Create($verification.Extent.Text)
    $global:SnowRemoteTestDownloads = [Collections.Generic.List[string]]::new()
    $global:SnowRemoteTestDirectory = $fixture
    function Get-SnowGitHubAsset($Repository, $Version, $Name, $Directory) {
        $global:SnowRemoteTestDownloads.Add($Name)
        return Join-Path $global:SnowRemoteTestDirectory $Name
    }
    $macNames = @("snow-shot-$version-macos-arm64.dmg", "snow-shot-mini-$version-macos-arm64.dmg",
        "snow-shot-$version-macos-x86_64.dmg")
    $release = [pscustomobject]@{ assets = @() }
    foreach ($name in $macNames) {
        $image = Join-Path $fixture $name
        [IO.File]::WriteAllText($image, "fixture: $name")
        $hash = (Get-FileHash $image -Algorithm SHA256).Hash.ToLowerInvariant()
        [IO.File]::WriteAllText("$image.sha256", "$hash  $name`n")
        $release.assets += [pscustomobject]@{ name = $name }
        $release.assets += [pscustomobject]@{ name = "$name.sha256" }
    }
    $verifiedMacPackages = @{}
    $product = [pscustomobject]@{ Product = 'snow-shot' }
    $verifyDirectory = $fixture
    $GitHubRepository = 'fixture/repository'
    & $verifyPackages
    & $verifyPackages
    $product = [pscustomobject]@{ Product = 'snow-shot-mini' }
    & $verifyPackages
    if ($global:SnowRemoteTestDownloads.Count -ne 6 -or
        -not $global:SnowRemoteTestDownloads.Contains("snow-shot-$version-macos-x86_64.dmg")) {
        throw 'Verification must audit Intel Full and both ARM64 editions without duplicate downloads.'
    }
    [IO.File]::WriteAllText((Join-Path $fixture "snow-shot-$version-macos-x86_64.dmg.sha256"), '0' * 64)
    $verifiedMacPackages = @{}
    $product = [pscustomobject]@{ Product = 'snow-shot' }
    try {
        & $verifyPackages
        throw 'Expected the Intel checksum mismatch to be rejected.'
    } catch {
        if ($_.Exception.Message -cne "GitHub macOS checksum failed: snow-shot-$version-macos-x86_64.dmg") { throw }
    }
} finally {
    Remove-Variable -Scope Global -Name 'SnowRemoteTest*' -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
Write-Output 'PASS: PowerShell syntax, Windows and architecture-selective macOS previews, metadata order, invalid configuration.'

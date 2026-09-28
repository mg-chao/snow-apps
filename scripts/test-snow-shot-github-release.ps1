[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'snow-shot-github-release.ps1')
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Must-Fail([scriptblock]$Action) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    Require $failed 'Expected operation to fail.'
}
$root = Join-Path ([IO.Path]::GetTempPath()) ("snow-github-tests-$([guid]::NewGuid().ToString('N'))")
$null = New-Item -ItemType Directory -Path $root
$originalPath = $env:PATH
$originalSshLog = $env:SNOW_GITHUB_TEST_SSH_LOG
$global:SnowGitHubTestFailDownload = $false
$global:SnowGitHubTestRelease = $null
$global:SnowGitHubTestBytes = @{}
$global:SnowGitHubTestEvents = [Collections.Generic.List[string]]::new()
function global:gh {
    $arguments = @($args)
    $global:LASTEXITCODE = 0
    if ($arguments[0] -eq 'api') {
        if ($arguments[1] -like '*/commits/*') { return ('a' * 40) }
        # --slurp returns one array per API page.
        if ($global:SnowGitHubTestRelease) { return ConvertTo-Json -InputObject @(@($global:SnowGitHubTestRelease)) -Depth 10 -Compress }
        return '[[]]'
    }
    $command = $arguments[1]
    $global:SnowGitHubTestEvents.Add($command)
    switch ($command) {
        create {
            Require (-not $global:SnowGitHubTestRelease) 'Do not create a duplicate release.'
            $global:SnowGitHubTestRelease = @{ tag_name = $arguments[2]; draft = $true; prerelease = ($arguments -contains '--prerelease'); assets = @() }
        }
        upload {
            $path = $arguments[3]
            $name = [IO.Path]::GetFileName($path)
            Require (-not $global:SnowGitHubTestBytes.ContainsKey($name)) 'Never overwrite an asset.'
            $global:SnowGitHubTestBytes[$name] = [IO.File]::ReadAllBytes($path)
            $global:SnowGitHubTestRelease.assets += @{ name = $name }
        }
        download {
            if ($global:SnowGitHubTestFailDownload) { throw 'Injected GitHub asset download failure.' }
            $name = $arguments[[array]::IndexOf($arguments, '--pattern') + 1]
            $directory = $arguments[[array]::IndexOf($arguments, '--dir') + 1]
            Require ($global:SnowGitHubTestBytes.ContainsKey($name)) "Asset missing: $name"
            [IO.File]::WriteAllBytes((Join-Path $directory $name), $global:SnowGitHubTestBytes[$name])
        }
        edit {
            Require ($global:SnowGitHubTestEvents.Contains('download')) 'Verify before publishing.'
            $global:SnowGitHubTestRelease.draft = $false
            $global:SnowGitHubTestLatest = $arguments -contains '--latest'
        }
        default { throw "Unexpected gh operation: $command" }
    }
}
try {
    $path = Join-Path $root 'latest-version.json'
    [IO.File]::WriteAllText($path, 'signed fixture')
    $assets = @{ 'latest-version.json' = $path }
    $verify = Join-Path $root 'verify'
    Publish-SnowGitHubRelease 'mg-chao/snow-apps' '2.0.0' $assets $verify
    Require (-not $global:SnowGitHubTestRelease.draft -and $global:SnowGitHubTestLatest) 'Stable release becomes public and latest.'
    Require (($global:SnowGitHubTestEvents -join ',') -eq 'create,upload,download,edit') 'Upload/verify/publish ordering.'
    $global:SnowGitHubTestEvents.Clear()
    Publish-SnowGitHubRelease 'mg-chao/snow-apps' '2.0.0' $assets $verify
    Require (($global:SnowGitHubTestEvents -join ',') -eq 'download,download') 'Identical retry makes no mutations.'
    $global:SnowGitHubTestEvents.Clear()
    Publish-SnowGitHubRelease 'mg-chao/snow-apps' '2.0.0' $assets $verify -VerifyOnly
    Require (($global:SnowGitHubTestEvents -join ',') -eq 'download') 'Verification never mutates releases.'
    [IO.File]::WriteAllText($path, 'different bytes')
    Must-Fail { Publish-SnowGitHubRelease 'mg-chao/snow-apps' '2.0.0' $assets $verify }
    Require (-not $global:SnowGitHubTestEvents.Contains('upload')) 'Conflicts rejected before upload.'
    $global:SnowGitHubTestRelease = $null
    $global:SnowGitHubTestBytes = @{}
    $global:SnowGitHubTestEvents.Clear()
    Publish-SnowGitHubRelease 'mg-chao/snow-apps' '2.0.0-beta' $assets $verify
    Require ($global:SnowGitHubTestRelease.prerelease -and -not $global:SnowGitHubTestLatest) 'Beta never becomes latest.'
    $global:SnowGitHubTestRelease.prerelease = $false
    Must-Fail { Publish-SnowGitHubRelease 'mg-chao/snow-apps' '2.0.0-beta' $assets $verify }

    $global:SnowGitHubTestRelease = $null
    $global:SnowGitHubTestBytes = @{}
    $global:SnowGitHubTestEvents.Clear()
    $global:SnowGitHubTestFailDownload = $true
    Must-Fail { Publish-SnowGitHubRelease 'mg-chao/snow-apps' '2.0.0' $assets $verify }
    Require ($global:SnowGitHubTestRelease.draft -and -not $global:SnowGitHubTestEvents.Contains('edit')) 'Failed verification leaves a draft.'
    $global:SnowGitHubTestFailDownload = $false
    Publish-SnowGitHubRelease 'mg-chao/snow-apps' '2.0.0' $assets $verify
    $missing = @{ 'missing.zip' = $path }
    Must-Fail { Publish-SnowGitHubRelease 'mg-chao/snow-apps' '2.0.0' $missing $verify }

    # Execute the real orchestration with isolated files, real RSA signing, and mocked
    # packaging audit/network/CLI boundaries. No production settings or server are used.
    $fixture = Join-Path $root 'repository'
    $fixtureScripts = Join-Path $fixture 'scripts'
    $resources = Join-Path $fixture 'snow_shot/resources'
    $packaging = Join-Path $fixture 'snow_shot/packaging'
    $build = Join-Path $fixture 'build'
    foreach ($directory in @($fixtureScripts, $resources, $packaging, $build)) { $null = New-Item -ItemType Directory -Force -Path $directory }
    foreach ($name in @('publish-snow-shot-release.ps1','snow-shot-github-release.ps1','snow-shot-publish-server.py')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $fixtureScripts
    }
    Set-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value 'set(SNOW_SHOT_VERSION "2.0.0")'
    $global:SnowGitHubTestRsa = [Security.Cryptography.RSA]::Create(3072)
    $keyPath = Join-Path $root 'test-key.pem'
    [IO.File]::WriteAllText($keyPath, $global:SnowGitHubTestRsa.ExportRSAPrivateKeyPem())
    $public = $global:SnowGitHubTestRsa.ExportParameters($false)
    @{ keys = @(@{ id = 'test'; modulus = [Convert]::ToBase64String($public.Modulus); exponent = [Convert]::ToBase64String($public.Exponent) }) } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $resources 'update-trusted-keys.json')
    $global:SnowGitHubTestOcr = Join-Path $root 'ocr.zip'
    [IO.File]::WriteAllText($global:SnowGitHubTestOcr, 'ocr')
    @{ runtime = @{ version = '1'; archive = @{ url = 'https://example.invalid/ocr'; size = 3; sha256 = (Get-FileHash $global:SnowGitHubTestOcr).Hash.ToLowerInvariant() } } } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $packaging 'snow-shot-ocr-asset-manifest.json')
    foreach ($variant in @('online','offline','portable')) {
        $kinds = if ($variant -eq 'portable') { @('portable') } else { @('installer','update') }
        foreach ($kind in $kinds) {
            $suffix = if ($kind -eq 'installer') { '.exe' } elseif ($kind -eq 'update') { '-update.zip' } else { '.zip' }
            $base = "snow-shot-2.0.0-windows-x64-$variant"
            $package = Join-Path $build "$base$suffix"
            [IO.File]::WriteAllText($package, 'package')
            $hash = (Get-FileHash $package).Hash.ToLowerInvariant()
            [IO.File]::WriteAllText("$package.sha256", "$hash  $base$suffix`n")
            $manifest = @{ PackageVersion = '2.0.0'; Variant = $variant; Preset = 'snow-shot-msvc-release'; StaticCrt = $true; StaticQt = $true;
                Architecture = 'x64'; Installer = @{ Sha256 = $hash; Bytes = 7 }; Archive = @{ Sha256 = $hash; Bytes = 7 }; InstallFiles = @() }
            $manifestSuffix = if ($kind -eq 'update') { '-update.manifest.json' } else { '.manifest.json' }
            $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $build "$base$manifestSuffix")
        }
    }
    function global:Invoke-WebRequest { param($Uri, $OutFile, $TimeoutSec, $MaximumRedirection)
        Require ($Uri.AbsoluteUri -eq 'https://example.invalid/ocr') 'Unexpected public network access.'
        Copy-Item -LiteralPath $global:SnowGitHubTestOcr -Destination $OutFile
    }
    function global:git { $global:LASTEXITCODE = 0; return ('a' * 40) }
    function global:Start-Process { param($FilePath, $ArgumentList, $WindowStyle, [switch]$PassThru, $RedirectStandardError)
        Require ($FilePath.EndsWith('snow-shot-updater.exe')) 'Only the local auditor can run.'
        Require (-not $global:SnowGitHubTestEvents.Contains('upload')) 'Audit must precede every upload.'
        $global:SnowGitHubTestEvents.Add('audit')
        $manifestPath = $ArgumentList[[array]::IndexOf($ArgumentList, '--manifest') + 1].Trim('"')
        $envelope = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
        Require ($global:SnowGitHubTestRsa.VerifyData([Convert]::FromBase64String($envelope.payload), [Convert]::FromBase64String($envelope.signature),
            [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pss)) 'Envelope must be authenticated.'
        $process = [pscustomobject]@{ ExitCode = 0; HasExited = $true }
        $process | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value { param($timeout) return $true }
        return $process
    }
    $global:SnowGitHubTestRelease = $null
    $global:SnowGitHubTestBytes = @{}
    $global:SnowGitHubTestEvents.Clear()
    & (Join-Path $fixtureScripts 'publish-snow-shot-release.ps1') -Destination GitHub -BuildDirectory $build -SigningKeyPath $keyPath -SkipBuild
    Require ($global:SnowGitHubTestEvents[0] -eq 'audit' -and $global:SnowGitHubTestEvents[-1] -eq 'edit') 'GitHub-only publication audits before publishing.'
    Require ($global:SnowGitHubTestBytes.ContainsKey('latest-version.json') -and $global:SnowGitHubTestBytes.Count -eq 16) 'Publish all five packages, sidecars, audit manifests, and signed feed.'
    $previousEnvelope = [Convert]::ToBase64String($global:SnowGitHubTestBytes['latest-version.json'])
    $global:SnowGitHubTestEvents.Clear()
    & (Join-Path $fixtureScripts 'publish-snow-shot-release.ps1') -Destination GitHub -BuildDirectory $build -SigningKeyPath $keyPath -SkipBuild -AuditOnly
    Require (-not $global:SnowGitHubTestEvents.Contains('upload') -and -not $global:SnowGitHubTestEvents.Contains('edit')) 'AuditOnly never mutates GitHub.'
    Require ([Convert]::ToBase64String($global:SnowGitHubTestBytes['latest-version.json']) -ceq $previousEnvelope) 'Retry preserves authenticated envelope.'
    $global:SnowGitHubTestEvents.Clear()
    & (Join-Path $fixtureScripts 'publish-snow-shot-release.ps1') -Destination GitHub -WhatIf
    Require ($global:SnowGitHubTestEvents.Count -eq 0) 'WhatIf does not build, sign, or contact GitHub.'
    Must-Fail { & (Join-Path $fixtureScripts 'publish-snow-shot-release.ps1') -Destination GitHub -Operation Rollback }
    # A tiny failing SSH executable exercises Both's actual process boundary without
    # a network connection. GitHub must remain published after website staging fails.
    $sshSource = Join-Path $root 'ssh.cs'
    $sshExecutable = Join-Path $root 'ssh.exe'
    @"
using System;
using System.IO;
using System.Text;
class FixtureSsh {
    static int Main(string[] args) {
        Console.In.ReadToEnd();
        string command = args[args.Length - 1];
        string request = Encoding.UTF8.GetString(Convert.FromBase64String(command.Substring(command.LastIndexOf(' ') + 1)));
        bool begin = request.Contains("\"operation\":\"begin\"");
        File.AppendAllText(Environment.GetEnvironmentVariable("SNOW_GITHUB_TEST_SSH_LOG"), begin ? "begin\n" : "verify\n");
        if (begin) { Console.Error.WriteLine("injected website staging failure"); return 1; }
        Console.WriteLine("{}"); return 0;
    }
}
"@ | Set-Content -LiteralPath $sshSource -Encoding utf8NoBOM
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    & $compiler /nologo /target:exe "/out:$sshExecutable" $sshSource
    if ($LASTEXITCODE -ne 0) { throw 'Failed to compile isolated SSH fixture.' }
    $env:PATH = "$root;$originalPath"
    $env:SNOW_GITHUB_TEST_SSH_LOG = Join-Path $root 'ssh.log'
    $global:SnowGitHubTestRelease = $null
    $global:SnowGitHubTestBytes = @{}
    $global:SnowGitHubTestEvents.Clear()
    Must-Fail {
        & (Join-Path $fixtureScripts 'publish-snow-shot-release.ps1') -Destination Both -BuildDirectory $build `
            -SigningKeyPath $keyPath -SkipBuild -ServerHost 'fixture.invalid' -PublicBaseUrl 'https://fixture.invalid'
    }
    Require ($global:SnowGitHubTestEvents.Contains('edit') -and -not $global:SnowGitHubTestRelease.draft) 'Website failure preserves published GitHub release.'
    Require ((Get-Content -Raw -LiteralPath $env:SNOW_GITHUB_TEST_SSH_LOG).Replace("`r", '') -ceq "verify`nbegin`n") 'Website begins only after GitHub publication.'
    $global:SnowGitHubTestRsa.Dispose()
    Write-Output 'PASS: GitHub release publication, retries, conflicts, classification, local signing, audit ordering, and dry runs.'
} finally {
    $env:PATH = $originalPath
    $env:SNOW_GITHUB_TEST_SSH_LOG = $originalSshLog
    foreach ($name in @('gh','git','Invoke-WebRequest','Start-Process')) { Remove-Item "Function:/$name" -ErrorAction SilentlyContinue }
    Remove-Variable -Scope Global -Name 'SnowGitHubTest*' -ErrorAction SilentlyContinue
    $resolved = [IO.Path]::GetFullPath($root)
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($temporary, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to remove fixture outside temp.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

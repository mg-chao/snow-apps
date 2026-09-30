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
            if ($arguments -contains '--notes-file') {
                $global:SnowGitHubTestRelease.body = [IO.File]::ReadAllText($arguments[[array]::IndexOf($arguments, '--notes-file') + 1])
            }
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
    foreach ($name in @('publish-snow-shot-release.ps1','snow-shot-github-release.ps1')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $fixtureScripts
    }
    @'
param($Version, $WebsiteDirectory)
Require ($Version -ceq '2.0.0' -and $WebsiteDirectory -ceq 'fixture-site') 'Forward the release website identity.'
Require ($global:SnowGitHubTestEvents.Contains('edit')) 'Deploy website after verified GitHub publication.'
$global:SnowGitHubTestEvents.Add('website')
'@ | Set-Content -LiteralPath (Join-Path $fixtureScripts 'publish-snow-shot-website.ps1')
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
    & (Join-Path $fixtureScripts 'publish-snow-shot-release.ps1') -BuildDirectory $build -SigningKeyPath $keyPath -SkipBuild -SkipGitee
    Require ($global:SnowGitHubTestEvents[0] -eq 'audit' -and $global:SnowGitHubTestEvents[-1] -eq 'edit') 'GitHub-only publication audits before publishing.'
    Require ($global:SnowGitHubTestBytes.ContainsKey('latest-version.json') -and $global:SnowGitHubTestBytes.Count -eq 16) 'Publish all five packages, sidecars, audit manifests, and signed feed.'
    $previousEnvelope = [Convert]::ToBase64String($global:SnowGitHubTestBytes['latest-version.json'])
    $global:SnowGitHubTestEvents.Clear()
    & (Join-Path $fixtureScripts 'publish-snow-shot-release.ps1') -BuildDirectory $build -SigningKeyPath $keyPath -SkipBuild -AuditOnly -DeployWebsite -WebsiteDirectory 'fixture-site'
    Require (-not $global:SnowGitHubTestEvents.Contains('upload') -and -not $global:SnowGitHubTestEvents.Contains('edit')) 'AuditOnly never mutates GitHub.'
    Require (-not $global:SnowGitHubTestEvents.Contains('website')) 'AuditOnly never deploys the website.'
    Require ([Convert]::ToBase64String($global:SnowGitHubTestBytes['latest-version.json']) -ceq $previousEnvelope) 'Retry preserves authenticated envelope.'
    # Exercise the default two-channel orchestration with isolated local files.
    $notesPath = Join-Path $root 'notes.md'
    [IO.File]::WriteAllText($notesPath, "Detailed release notes.`n")
    $global:SnowGitHubTestNotes = [IO.File]::ReadAllText($notesPath)
    function global:python {
        $arguments = @($args)
        $global:LASTEXITCODE = 0
        Require ($arguments[0].EndsWith('publish-snow-shot-gitee-release.py')) 'Use the direct local Gitee publisher.'
        if ($arguments -contains '--check-auth') {
            Require (-not $global:SnowGitHubTestEvents.Contains('audit')) 'Credentials must be checked before packaging.'
            $global:SnowGitHubTestEvents.Add('gitee-auth')
            return
        }
        $manifest = Get-Content -Raw -LiteralPath $arguments[[array]::IndexOf($arguments, '--manifest') + 1] | ConvertFrom-Json
        Require ($manifest.body -ceq $global:SnowGitHubTestNotes) 'Both destinations use identical local notes.'
        Require ($manifest.sourceCommit -ceq ('a' * 40) -and $manifest.assets.Count -eq 16) 'Preserve the complete audited source and asset contract.'
        foreach ($asset in $manifest.assets) {
            Require ((Get-FileHash -LiteralPath $asset.path).Hash.ToLowerInvariant() -ceq $asset.sha256) 'Gitee files must match local audited bytes.'
        }
        if ($arguments -contains '--prepare-homebrew') {
            Require ($global:SnowGitHubTestEvents.Contains('audit') -and -not $global:SnowGitHubTestEvents.Contains('upload')) 'Prepare distribution assets after auditing and before uploads.'
            $global:SnowGitHubTestEvents.Add('gitee-prepare')
        } else {
            Require ($global:SnowGitHubTestEvents.Contains('edit')) 'Publish Gitee after GitHub has been verified and published.'
            Require ($global:SnowGitHubTestRelease.body -ceq $manifest.body) 'GitHub notes must equal local Gitee notes.'
            $global:SnowGitHubTestEvents.Add('gitee-publish')
        }
    }
    $global:SnowGitHubTestEvents.Clear()
    $global:SnowGitHubTestRelease = $null
    $global:SnowGitHubTestBytes = @{}
    & (Join-Path $fixtureScripts 'publish-snow-shot-release.ps1') -BuildDirectory $build -SigningKeyPath $keyPath -SkipBuild -ReleaseNotesPath $notesPath -DeployWebsite -WebsiteDirectory 'fixture-site'
    Require ($global:SnowGitHubTestEvents[0] -eq 'gitee-auth' -and $global:SnowGitHubTestEvents[-2] -eq 'gitee-publish' -and
        $global:SnowGitHubTestEvents[-1] -eq 'website') 'Website deployment follows verified publication to both release destinations.'
    Must-Fail { Publish-SnowGitHubRelease 'mg-chao/snow-apps' '2.0.0' @{ 'latest-version.json' = $path } $verify -NotesPath $keyPath }
    $global:SnowGitHubTestEvents.Clear()
    & (Join-Path $fixtureScripts 'publish-snow-shot-release.ps1') -WhatIf -DeployWebsite -WebsiteDirectory 'fixture-site'
    Require ($global:SnowGitHubTestEvents.Count -eq 0) 'WhatIf does not build, sign, or contact GitHub.'
    Must-Fail { & (Join-Path $fixtureScripts 'publish-snow-shot-release.ps1') -Operation Rollback }
    $global:SnowGitHubTestRsa.Dispose()
    Write-Output 'PASS: GitHub release publication, retries, conflicts, classification, local signing, audit ordering, and dry runs.'
} finally {
    foreach ($name in @('gh','git','python','Invoke-WebRequest','Start-Process')) { Remove-Item "Function:/$name" -ErrorAction SilentlyContinue }
    Remove-Variable -Scope Global -Name 'SnowGitHubTest*' -ErrorAction SilentlyContinue
    $resolved = [IO.Path]::GetFullPath($root)
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($temporary, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to remove fixture outside temp.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

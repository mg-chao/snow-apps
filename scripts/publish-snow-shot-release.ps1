[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidateSet('Publish', 'Verify')][string]$Operation = 'Publish',
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$GitHubRepository = 'mg-chao/snow-apps',
    [string]$Version = '',
    [string]$BuildDirectory = 'build/snow-shot-msvc-release',
    [string]$SigningKeyPath,
    [switch]$SkipBuild,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]*$')][string]$MacHost,
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$MacUser,
    [ValidateRange(1, 65535)][int]$MacPort = 22,
    [string]$MacIdentityFile,
    [string]$MacKnownHostsFile,
    [string]$MacProjectDirectory,
    [switch]$AuditOnly,
    [string]$ReleaseNotesPath,
    [switch]$SkipGitee,
    [switch]$DeployWebsite,
    [string]$WebsiteDirectory = 'D:/snow-apps-site',
    [ValidateRange(1, 256)][int]$Parallelism = 4
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($AuditOnly -and $Operation -ne 'Publish') { throw 'AuditOnly can only be used with Publish.' }
. (Join-Path $PSScriptRoot 'snow-shot-github-release.ps1')
. (Join-Path $PSScriptRoot 'snow-shot-editions.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$publicKeys = Join-Path $repo 'snow_shot/resources/update-trusted-keys.json'
$sourceVersion = [regex]::Match((Get-Content -Raw (Join-Path $repo 'CMakeLists.txt')), 'set\(SNOW_SHOT_VERSION "([^"]+)"\)').Groups[1].Value
if (-not $Version) { $Version = $sourceVersion }
if ($Version -cne $sourceVersion) { throw 'The requested version must match SNOW_SHOT_VERSION in CMakeLists.txt.' }
if ($MacHost) {
    if (-not $MacProjectDirectory) { throw 'MacProjectDirectory is required with MacHost.' }
    if (-not $MacUser) { throw 'MacUser is required with MacHost.' }
    if ($MacProjectDirectory -notmatch '^/[A-Za-z0-9_./-]+$' -or $MacProjectDirectory.Contains('..')) {
        throw 'MacProjectDirectory must be an absolute POSIX path without spaces or traversal.'
    }
} elseif ($MacProjectDirectory) { throw 'MacHost is required with MacProjectDirectory.' }
$destinations = if ($SkipGitee -or $Operation -eq 'Verify') { 'GitHub' } else { 'GitHub and Gitee' }
if (-not $PSCmdlet.ShouldProcess("Snow Shot $Version on $destinations", $Operation)) {
        foreach ($edition in @('Full', 'Mini')) {
            $product = Get-SnowShotEdition $edition
            foreach ($variant in $product.Variants) {
                $base = "$($product.Product)-$Version-windows-x64-$variant"
                $suffixes = if ($variant -eq 'portable') { @('.zip', '.zip.sha256', '.manifest.json') }
                    else { @('.exe', '.exe.sha256', '.manifest.json', '-update.zip', '-update.zip.sha256', '-update.manifest.json') }
                $suffixes | ForEach-Object { Write-Output "$base$_" }
            }
            Write-Output $product.Feed
            if ($MacHost) {
                Write-Output "$($product.Product)-$Version-macos-arm64.dmg"
                Write-Output "$($product.Product)-$Version-macos-arm64.dmg.sha256"
            }
        }
        if ($MacHost) { Write-Output 'install-snow-shot-macos.sh' }
        if ($DeployWebsite -and $Operation -eq 'Publish' -and -not $AuditOnly) {
            Write-Output "Then commit/push, build and deploy website $Version from $WebsiteDirectory."
        }
    return
}
if ($Operation -eq 'Publish' -and -not $AuditOnly -and -not $SkipGitee) {
    if (-not $ReleaseNotesPath -or -not (Test-Path -LiteralPath $ReleaseNotesPath -PathType Leaf)) {
        throw 'ReleaseNotesPath is required to publish identical detailed notes on GitHub and Gitee.'
    }
    $ReleaseNotesPath = (Resolve-Path -LiteralPath $ReleaseNotesPath).Path
    & python (Join-Path $PSScriptRoot 'publish-snow-shot-gitee-release.py') --check-auth
    if ($LASTEXITCODE -ne 0) { throw 'Local Gitee authentication failed before packaging.' }
}
if ($Operation -eq 'Verify') {
        if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repo $BuildDirectory }
        $release = Get-SnowGitHubRelease $GitHubRepository $Version
        if (-not $release -or $release.draft) { throw 'Expected a published GitHub release.' }
        $verifyDirectory = Join-Path $repo "artifacts/github-verify-$([guid]::NewGuid().ToString('N'))"
        foreach ($edition in @(Get-SnowShotReleaseEditions $release $Version)) {
            $product = Get-SnowShotEdition $edition
            $manifestPath = Get-SnowGitHubAsset $GitHubRepository $Version $product.Feed $verifyDirectory
            $helperName = if ($edition -eq 'Mini') { 'snow-shot-mini-updater' } else { 'snow-shot-updater' }
            $auditor = Join-Path $BuildDirectory "$($product.Executable)/Release/$helperName.exe"
            & $auditor --verify-release --manifest $manifestPath
            if ($LASTEXITCODE -ne 0) { throw 'GitHub update signature verification failed.' }
            $envelope = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
            $payload = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($envelope.payload)) | ConvertFrom-Json
            if ($payload.version -cne $Version) { throw 'GitHub tag and signed version differ.' }
            foreach ($package in $payload.packages) {
                $name = $package.path.Replace("setup/$($product.Product)_", "$($product.Product)-$Version-")
                $download = Get-SnowGitHubAsset $GitHubRepository $Version $name $verifyDirectory
                if ((Get-Item $download).Length -ne $package.size -or
                    (Get-FileHash $download -Algorithm SHA256).Hash.ToLowerInvariant() -cne $package.sha256) {
                    throw "GitHub package failed signed verification: $name"
                }
            }
            foreach ($arch in @('arm64', 'x64')) {
                $name = "$($product.Product)-$Version-macos-$arch.dmg"
                if (@($release.assets | Where-Object { $_.name -ceq $name }).Count -eq 0) { continue }
                $image = Get-SnowGitHubAsset $GitHubRepository $Version $name $verifyDirectory
                $checksum = Get-SnowGitHubAsset $GitHubRepository $Version "$name.sha256" $verifyDirectory
                $expected = ((Get-Content -Raw $checksum).Trim() -split '\s+')[0]
                if ($expected -notmatch '^[a-fA-F0-9]{64}$' -or
                    (Get-FileHash $image -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected.ToLowerInvariant()) {
                    throw "GitHub macOS checksum failed: $name"
                }
            }
        }
        Write-Output "Verified signed GitHub packages for $Version."
    return
}
if (-not $SigningKeyPath -or -not (Test-Path -LiteralPath $SigningKeyPath -PathType Leaf)) { throw 'A local release signing key is required.' }
# Online installations must be able to obtain the immutable runtime advertised by this
# source release. Fail before an expensive build if it is unavailable.
$ocrManifest = Get-Content -Raw (Join-Path $repo 'snow_shot/packaging/snow-shot-ocr-asset-manifest.json') | ConvertFrom-Json
$runtimePreflight = Join-Path ([IO.Path]::GetTempPath()) ("snow-shot-ocr-preflight-$([guid]::NewGuid().ToString('N')).zip")
try {
    $runtimeUri = [uri]$ocrManifest.runtime.archive.url
    if ($runtimeUri.Scheme -ne 'https' -or $runtimeUri.UserInfo) { throw 'The pinned OCR runtime must use HTTPS without credentials.' }
    Invoke-WebRequest -Uri $runtimeUri -OutFile $runtimePreflight -TimeoutSec 180 -MaximumRedirection 5
    if ((Get-Item -LiteralPath $runtimePreflight).Length -ne $ocrManifest.runtime.archive.size -or
        (Get-FileHash -LiteralPath $runtimePreflight -Algorithm SHA256).Hash.ToLowerInvariant() -cne $ocrManifest.runtime.archive.sha256) {
        throw 'The public OCR runtime does not match its immutable size/SHA-256.'
    }
} catch {
    throw "Release preflight failed: OCR runtime $($ocrManifest.runtime.version) is unavailable or differs from the checked-in manifest. Restore the exact approved asset or complete a separately authorized OCR release; do not replace pinned hashes to bypass this check. $($_.Exception.Message)"
} finally {
    if (Test-Path -LiteralPath $runtimePreflight -PathType Leaf) { Remove-Item -LiteralPath $runtimePreflight }
}
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repo $BuildDirectory }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$transaction = [guid]::NewGuid().ToString('N')
$releaseDirectory = Join-Path $repo "artifacts/publish-$transaction"
$null = New-Item -ItemType Directory -Path (Join-Path $releaseDirectory 'setup')
$macJob = $null
try {
    if ($MacHost) {
        $macParameters = @{ MacHost = $MacHost; MacUser = $MacUser; MacPort = $MacPort;
            MacIdentityFile = $MacIdentityFile; MacKnownHostsFile = $MacKnownHostsFile;
            MacProjectDirectory = $MacProjectDirectory; Version = $Version; Parallelism = $Parallelism;
            OutputDirectory = (Join-Path $releaseDirectory 'setup'); SkipBuild = [bool]$SkipBuild }
        $macJob = Start-Job -ScriptBlock {
            param($script, $parameters)
            $ErrorActionPreference = 'Stop'
            & $script @parameters
        } -ArgumentList (Join-Path $PSScriptRoot 'package-snow-shot-remote-macos.ps1'), $macParameters
        Write-Output "macOS packaging runs alongside Windows. Log: $releaseDirectory/setup/macos-build.log"
    }
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot 'package-snow-shot.ps1') -BuildDirectory $BuildDirectory -Parallelism $Parallelism
        if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
    }
} finally {
    # Join even if Windows fails: do not leave an unattended SSH packaging process.
    if ($macJob) {
        try {
            $macJob | Wait-Job | Receive-Job -ErrorAction Stop
            if ($macJob.State -ne 'Completed') { throw "macOS packaging failed; see $releaseDirectory/setup/macos-build.log" }
        } finally { Remove-Job $macJob }
    }
}
if ($MacHost) {
    # Normalize Git's Windows line endings: this file is executed by Apple's Bash.
    $installer = (Get-Content -Raw (Join-Path $PSScriptRoot 'install-snow-shot-macos.sh')).Replace("`r`n", "`n")
    [IO.File]::WriteAllText((Join-Path $releaseDirectory 'setup/install-snow-shot-macos.sh'), $installer, [Text.UTF8Encoding]::new($false))
}
$githubAssets = @{}
foreach ($edition in @('Full', 'Mini')) {
    $product = Get-SnowShotEdition $edition
    $packages = @()
    foreach ($variant in $product.Variants) {
        $kinds = if ($variant -eq 'portable') { @('portable') } else { @('installer', 'update') }
        foreach ($kind in $kinds) {
            $suffix = if ($kind -eq 'installer') { '.exe' } elseif ($kind -eq 'update') { '-update.zip' } else { '.zip' }
            $base = "$($product.Product)-$Version-windows-x64-$variant"
            $source = Join-Path $BuildDirectory "$base$suffix"
            $manifestSuffix = if ($kind -eq 'update') { '-update.manifest.json' } else { '.manifest.json' }
            $manifest = Get-Content -Raw (Join-Path $BuildDirectory "$base$manifestSuffix") | ConvertFrom-Json
            if ($manifest.PackageVersion -cne $Version -or $manifest.Variant -cne $variant) { throw 'Mixed package versions or variants.' }
            if ($kind -eq 'installer' -and ($manifest.Preset -cne 'snow-shot-msvc-release' -or
                -not $manifest.StaticCrt -or -not $manifest.StaticQt -or $manifest.Architecture -cne 'x64')) {
                throw 'Only audited static Windows x64 release installers may be published.'
            }
            $descriptor = if ($kind -eq 'installer') { $manifest.Installer } else { $manifest.Archive }
            $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($hash -cne $descriptor.Sha256 -or (Get-Item -LiteralPath $source).Length -ne $descriptor.Bytes) { throw 'Package audit manifest mismatch.' }
            $path = "setup/$($product.Product)_windows-x64-$variant$suffix"
            Copy-Item -LiteralPath $source -Destination (Join-Path $releaseDirectory $path)
                $githubDirectory = Join-Path $releaseDirectory 'github-assets'
                $null = New-Item -ItemType Directory -Force -Path $githubDirectory
                $githubPackage = Join-Path $githubDirectory "$base$suffix"
                Copy-Item -LiteralPath (Join-Path $releaseDirectory $path) -Destination $githubPackage
                $githubAssets["$base$suffix"] = $githubPackage
                $manifestPath = Join-Path $BuildDirectory "$base$manifestSuffix"
                $githubManifest = Join-Path $githubDirectory "$base$manifestSuffix"
                Copy-Item -LiteralPath $manifestPath -Destination $githubManifest
                $githubAssets["$base$manifestSuffix"] = $githubManifest
                $sum = (Get-Content -Raw -LiteralPath "$source.sha256").Trim() -split '\s+'
                if ($sum[0].ToLowerInvariant() -cne $hash) { throw 'Package checksum sidecar mismatch.' }
                [IO.File]::WriteAllText("$githubPackage.sha256", "$hash  $base$suffix`n", [Text.UTF8Encoding]::new($false))
                $githubAssets["$base$suffix.sha256"] = "$githubPackage.sha256"
            $package = [ordered]@{ variant = $variant; kind = $kind; path = $path; size = $descriptor.Bytes; sha256 = $hash }
            if ($kind -ne 'installer') {
                $package.files = @($manifest.InstallFiles | ForEach-Object {
                    [ordered]@{ path = $_.Path.Replace('\', '/'); size = $_.Bytes; sha256 = $_.Sha256 }
                })
            }
            $packages += $package
        }
    }
    $payload = [Text.Encoding]::UTF8.GetBytes(([ordered]@{ schema = 1; product = $product.Product; version = $Version;
        publishedAt = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'); platform = 'windows-x64'; packages = $packages } |
        ConvertTo-Json -Depth 12 -Compress))
    $rsa = [Security.Cryptography.RSA]::Create()
    try {
        $rsa.ImportFromPem((Get-Content -Raw -LiteralPath $SigningKeyPath))
        $public = $rsa.ExportParameters($false)
        $modulus = [Convert]::ToBase64String($public.Modulus)
        $exponent = [Convert]::ToBase64String($public.Exponent)
        $key = @((Get-Content -Raw $publicKeys | ConvertFrom-Json).keys | Where-Object { $_.modulus -ceq $modulus -and $_.exponent -ceq $exponent })
        if ($rsa.KeySize -ne 3072 -or $key.Count -ne 1) { throw 'The signing key does not match an embedded public key.' }
        $signature = $rsa.SignData($payload, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pss)
        if (-not $rsa.VerifyData($payload, $signature, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pss)) { throw 'Signature self-verification failed.' }
        [ordered]@{ schema = 1; keyId = $key[0].id; payload = [Convert]::ToBase64String($payload); signature = [Convert]::ToBase64String($signature) } |
            ConvertTo-Json -Compress | Set-Content -LiteralPath (Join-Path $releaseDirectory $product.Feed) -Encoding utf8NoBOM
    } finally { $rsa.Dispose() }
    # PSS signatures contain random salt. Reuse the authenticated published envelope
    # when its version and complete package contract are identical, making retries idempotent.
    $published = $null
    # Prefer an existing GitHub envelope for retries; the compiled auditor below
    # authenticates it and the complete package contract before any upload.
    $githubRelease = Get-SnowGitHubRelease $GitHubRepository $Version
    if ($githubRelease -and @($githubRelease.assets | Where-Object { $_.name -ceq $product.Feed }).Count -eq 1) {
        $existing = Get-SnowGitHubAsset $GitHubRepository $Version $product.Feed (Join-Path $releaseDirectory "existing-github-$edition")
        $published = [IO.File]::ReadAllBytes($existing)
    }
    if ($published) {
            $previousEnvelope = [Text.Encoding]::UTF8.GetString($published) | ConvertFrom-Json
            $previousPayload = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($previousEnvelope.payload)) | ConvertFrom-Json
            if ($previousPayload.version -cne $Version -or
                ($previousPayload.packages | ConvertTo-Json -Depth 12 -Compress) -cne ($packages | ConvertTo-Json -Depth 12 -Compress)) {
                throw 'This version is already published with different packages. Increase SNOW_SHOT_VERSION.'
            }
            # The compiled auditor below verifies this envelope before anything is uploaded.
            [IO.File]::WriteAllBytes((Join-Path $releaseDirectory $product.Feed), $published)
    }
    $helperName = if ($edition -eq 'Mini') { 'snow-shot-mini-updater' } else { 'snow-shot-updater' }
    $auditor = Join-Path $BuildDirectory "$($product.Executable)/Release/$helperName.exe"
    $auditErrors = Join-Path $releaseDirectory "audit-errors-$edition.log"
    $audit = Start-Process -FilePath $auditor -ArgumentList @('--audit-release', '--directory', "`"$releaseDirectory`"",
        '--manifest', "`"$(Join-Path $releaseDirectory $product.Feed)`"") -WindowStyle Hidden -PassThru -RedirectStandardError $auditErrors
    if (-not $audit.WaitForExit(300000) -or $audit.ExitCode -ne 0) {
        if (-not $audit.HasExited) { $audit.Kill() }
        $detail = if (Test-Path -LiteralPath $auditErrors) { (Get-Content -LiteralPath $auditErrors -TotalCount 20) -join ' ' } else { '' }
        throw "The compiled updater rejected the release signature, archive, or startup probe. $detail"
    }
    $githubAssets[$product.Feed] = Join-Path $releaseDirectory $product.Feed
}
$auditor = Join-Path $BuildDirectory 'snow_shot/Release/snow-shot-updater.exe'
$miniAuditor = Join-Path $BuildDirectory 'snow_shot_mini/Release/snow-shot-mini-updater.exe'
if ($AuditOnly) {
    Write-Output "Audited Snow Shot $Version without staging or publishing. Signed release: $releaseDirectory"
    return
}
    if ($MacHost) {
        foreach ($prefix in @('snow-shot', 'snow-shot-mini')) {
            $name = "$prefix-$Version-macos-arm64.dmg"
            $path = Join-Path $releaseDirectory $name
            Copy-Item -LiteralPath (Join-Path $releaseDirectory "setup/${prefix}_macos-arm64.dmg") -Destination $path
            $hash = (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()
            [IO.File]::WriteAllText("$path.sha256", "$hash  $name`n", [Text.UTF8Encoding]::new($false))
            $githubAssets[$name] = $path
            $githubAssets["$name.sha256"] = "$path.sha256"
        }
        $githubAssets['install-snow-shot-macos.sh'] = Join-Path $releaseDirectory 'setup/install-snow-shot-macos.sh'
    }
    # The remote tag must refer to this checkout, including when reusing a CI draft.
    $head = (& git -C $repo rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve source commit.' }
    $tag = "v${Version}_snow-shot"
    $tagCommit = (Invoke-SnowGitHub @('api', "repos/$GitHubRepository/commits/$tag", '--jq', '.sha')).Trim()
    if ($tagCommit -cne $head) { throw 'GitHub release tag does not match the audited source checkout.' }
    $localManifestPath = Join-Path $releaseDirectory 'local-release.json'
    if (-not $SkipGitee) {
        # Both destinations receive the same local audit inputs. Gitee never downloads
        # packages from GitHub, including the reproducible Homebrew distribution archive.
        $sharedDirectory = Join-Path $releaseDirectory 'github-assets'
        $null = New-Item -ItemType Directory -Force -Path $sharedDirectory
        $descriptors = foreach ($name in $githubAssets.Keys) {
            $path = Join-Path $sharedDirectory $name
            if ([IO.Path]::GetFullPath($githubAssets[$name]) -cne [IO.Path]::GetFullPath($path)) {
                Copy-Item -LiteralPath $githubAssets[$name] -Destination $path
            }
            @{ name = $name; path = $path; size = (Get-Item -LiteralPath $path).Length;
                sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
        }
        @{ schema = 1; tag = $tag; sourceCommit = $head; title = "Snow Shot $Version";
            body = [IO.File]::ReadAllText($ReleaseNotesPath); assets = @($descriptors) } |
            ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $localManifestPath -Encoding utf8NoBOM
        & python (Join-Path $PSScriptRoot 'publish-snow-shot-gitee-release.py') --manifest $localManifestPath --auditor $auditor --mini-auditor $miniAuditor --prepare-homebrew
        if ($LASTEXITCODE -ne 0) { throw 'Local distribution preparation failed before publication.' }
        $localRelease = Get-Content -Raw -LiteralPath $localManifestPath | ConvertFrom-Json
        $githubAssets = @{}
        foreach ($asset in $localRelease.assets) { $githubAssets[$asset.name] = $asset.path }
    }
    Publish-SnowGitHubRelease $GitHubRepository $Version $githubAssets (Join-Path $releaseDirectory 'verify-github') -NotesPath $ReleaseNotesPath
    if (-not $SkipGitee) {
        & python (Join-Path $PSScriptRoot 'publish-snow-shot-gitee-release.py') --manifest $localManifestPath --auditor $auditor --mini-auditor $miniAuditor
        if ($LASTEXITCODE -ne 0) { throw "Local Gitee publication failed. Retry with the audited manifest: $localManifestPath" }
    }
Write-Output "Published and verified Snow Shot $Version on $destinations."
if ($DeployWebsite) {
    & (Join-Path $PSScriptRoot 'publish-snow-shot-website.ps1') -Version $Version -WebsiteDirectory $WebsiteDirectory
}

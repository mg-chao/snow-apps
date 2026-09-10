[CmdletBinding(SupportsShouldProcess)]
param(
    [ValidateSet('Publish', 'Verify', 'Rollback')][string]$Operation = 'Publish',
    [string]$Version = '',
    [string]$BuildDirectory = 'build/snow-shot-msvc-release',
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]*$')][string]$ServerHost,
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$ServerUser = 'root',
    [ValidateRange(1, 65535)][int]$ServerPort = 22,
    [string]$IdentityFile,
    [string]$KnownHostsFile,
    [string]$RemoteWebRoot = '/var/www/html',
    [Parameter(Mandatory)][uri]$PublicBaseUrl,
    [string]$SigningKeyPath,
    [switch]$SkipBuild,
    [switch]$AuditOnly,
    [ValidateRange(1, 256)][int]$Parallelism = 4
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($AuditOnly -and $Operation -ne 'Publish') { throw 'AuditOnly can only be used with Publish.' }
$repo = Split-Path -Parent $PSScriptRoot
$publicKeys = Join-Path $repo 'snow_shot/resources/update-trusted-keys.json'
$sourceVersion = [regex]::Match((Get-Content -Raw (Join-Path $repo 'CMakeLists.txt')), 'set\(SNOW_SHOT_VERSION "([^"]+)"\)').Groups[1].Value
if (-not $Version) { $Version = $sourceVersion }
if ($Version -cne $sourceVersion) { throw 'The requested version must match SNOW_SHOT_VERSION in CMakeLists.txt.' }
if ($PublicBaseUrl.Scheme -ne 'https' -or $PublicBaseUrl.UserInfo) { throw 'PublicBaseUrl must use HTTPS without credentials.' }
if ($RemoteWebRoot -notmatch '^/[A-Za-z0-9_./-]+$' -or $RemoteWebRoot.Contains('..')) { throw 'Invalid remote web root.' }
$allowed = @('setup/snow-shot_windows-x64-offline.exe', 'setup/snow-shot_windows-x64-online.exe',
    'setup/snow-shot_windows-x64-portable.zip', 'setup/snow-shot_windows-x64-offline-update.zip',
    'setup/snow-shot_windows-x64-online-update.zip', 'setup/SHA256SUMS', 'latest-version.json', 'latest-version.txt')
if (-not $PSCmdlet.ShouldProcess("Snow Shot $Version; fixed files under $RemoteWebRoot", $Operation)) {
    $allowed | ForEach-Object { Write-Output $_ }
    return
}
$sshArguments = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'UpdateHostKeys=no',
    '-o', 'ConnectTimeout=15', '-p', "$ServerPort")
if ($IdentityFile) { $sshArguments += @('-i', [IO.Path]::GetFullPath($IdentityFile), '-o', 'IdentitiesOnly=yes') }
if ($KnownHostsFile) { $sshArguments += @('-o', "UserKnownHostsFile=$([IO.Path]::GetFullPath($KnownHostsFile))") }
$destination = "$ServerUser@$ServerHost"
function Invoke-PublishRemote([hashtable]$Request) {
    $Request.webRoot = $RemoteWebRoot
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($Request | ConvertTo-Json -Depth 12 -Compress)))
    $info = [Diagnostics.ProcessStartInfo]::new('ssh')
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($arg in ($sshArguments + @($destination, "python3 - $encoded"))) { $info.ArgumentList.Add($arg) }
    $process = [Diagnostics.Process]::Start($info)
    $output = $process.StandardOutput.ReadToEndAsync()
    $errors = $process.StandardError.ReadToEndAsync()
    $process.StandardInput.Write((Get-Content -Raw (Join-Path $PSScriptRoot 'snow-shot-publish-server.py')))
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(600000)) { $process.Kill($true); throw 'Remote publish operation timed out.' }
    if ($process.ExitCode -ne 0) { throw "Remote publish operation failed: $($errors.GetAwaiter().GetResult())" }
    return $output.GetAwaiter().GetResult() | ConvertFrom-Json -AsHashtable
}
if ($Operation -eq 'Rollback') { Invoke-PublishRemote @{ operation = 'rollback' }; return }
if ($Operation -eq 'Verify') { Invoke-PublishRemote @{ operation = 'verify' }; return }
if (-not $SigningKeyPath -or -not (Test-Path -LiteralPath $SigningKeyPath -PathType Leaf)) { throw 'A local release signing key is required.' }
# Online installations must be able to obtain the immutable runtime advertised by this
# source release. Fail before an expensive build or any SSH staging if it is unavailable.
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
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'package-snow-shot.ps1') -BuildDirectory $BuildDirectory -Parallelism $Parallelism
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
}
$transaction = [guid]::NewGuid().ToString('N')
$releaseDirectory = Join-Path $repo "artifacts/publish-$transaction"
$null = New-Item -ItemType Directory -Path (Join-Path $releaseDirectory 'setup')
$packages = @()
foreach ($variant in @('online', 'offline', 'portable')) {
    $kinds = if ($variant -eq 'portable') { @('portable') } else { @('installer', 'update') }
    foreach ($kind in $kinds) {
        $suffix = if ($kind -eq 'installer') { '.exe' } elseif ($kind -eq 'update') { '-update.zip' } else { '.zip' }
        $base = "snow-shot-$Version-windows-x64-$variant"
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
        $path = "setup/snow-shot_windows-x64-$variant$suffix"
        Copy-Item -LiteralPath $source -Destination (Join-Path $releaseDirectory $path)
        $package = [ordered]@{ variant = $variant; kind = $kind; path = $path; size = $descriptor.Bytes; sha256 = $hash }
        if ($kind -ne 'installer') {
            $package.files = @($manifest.InstallFiles | ForEach-Object {
                [ordered]@{ path = $_.Path.Replace('\', '/'); size = $_.Bytes; sha256 = $_.Sha256 }
            })
        }
        $packages += $package
    }
}
$payload = [Text.Encoding]::UTF8.GetBytes(([ordered]@{ schema = 1; version = $Version;
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
        ConvertTo-Json -Compress | Set-Content -LiteralPath (Join-Path $releaseDirectory 'latest-version.json') -Encoding utf8NoBOM
} finally { $rsa.Dispose() }
# PSS signatures contain random salt. Reuse the authenticated published envelope
# when its version and complete package contract are identical, making retries idempotent.
$published = Invoke-PublishRemote @{ operation = 'verify' }
if ($published.ContainsKey('latest-version.txt') -and $published.ContainsKey('latest-version.json')) {
    $publishedVersion = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($published['latest-version.txt'].contentBase64)).Trim()
    if ($publishedVersion -ceq $Version) {
        $previousBytes = [Convert]::FromBase64String($published['latest-version.json'].contentBase64)
        $previousEnvelope = [Text.Encoding]::UTF8.GetString($previousBytes) | ConvertFrom-Json
        $previousPayload = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($previousEnvelope.payload)) | ConvertFrom-Json
        if ($previousPayload.version -cne $Version -or
            ($previousPayload.packages | ConvertTo-Json -Depth 12 -Compress) -cne ($packages | ConvertTo-Json -Depth 12 -Compress)) {
            throw 'This version is already published with different packages. Increase SNOW_SHOT_VERSION.'
        }
        # The compiled auditor below verifies this envelope before anything is uploaded.
        [IO.File]::WriteAllBytes((Join-Path $releaseDirectory 'latest-version.json'), $previousBytes)
    }
}
[IO.File]::WriteAllText((Join-Path $releaseDirectory 'latest-version.txt'), $Version, [Text.UTF8Encoding]::new($false))
($packages | ForEach-Object { "$($_.sha256)  $([IO.Path]::GetFileName($_.path))" }) -join "`n" |
    Set-Content -LiteralPath (Join-Path $releaseDirectory 'setup/SHA256SUMS') -Encoding ascii
$files = @{}
foreach ($name in $allowed) {
    $path = Join-Path $releaseDirectory $name
    $files[$name] = @{ size = (Get-Item -LiteralPath $path).Length; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$auditor = Join-Path $BuildDirectory 'snow_shot/Release/snow-shot-updater.exe'
$auditErrors = Join-Path $releaseDirectory 'audit-errors.log'
$audit = Start-Process -FilePath $auditor -ArgumentList @('--audit-release', '--directory', "`"$releaseDirectory`"",
    '--manifest', "`"$(Join-Path $releaseDirectory 'latest-version.json')`"") -WindowStyle Hidden -PassThru -RedirectStandardError $auditErrors
if (-not $audit.WaitForExit(300000) -or $audit.ExitCode -ne 0) {
    if (-not $audit.HasExited) { $audit.Kill() }
    $detail = if (Test-Path -LiteralPath $auditErrors) { (Get-Content -LiteralPath $auditErrors -TotalCount 20) -join ' ' } else { '' }
    throw "The compiled updater rejected the release signature, archive, or startup probe. $detail"
}
if ($AuditOnly) {
    Write-Output "Audited Snow Shot $Version without staging or publishing. Signed release: $releaseDirectory"
    return
}
$stage = Invoke-PublishRemote @{ operation = 'begin'; id = $transaction }
$scpArguments = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'UpdateHostKeys=no', '-P', "$ServerPort")
if ($IdentityFile) { $scpArguments += @('-i', [IO.Path]::GetFullPath($IdentityFile), '-o', 'IdentitiesOnly=yes') }
if ($KnownHostsFile) { $scpArguments += @('-o', "UserKnownHostsFile=$([IO.Path]::GetFullPath($KnownHostsFile))") }
foreach ($name in $allowed) {
    & scp @scpArguments (Join-Path $releaseDirectory $name) "$destination`:$($stage.uploadDirectory)/$name"
    if ($LASTEXITCODE -ne 0) { throw "Upload failed for $name. Public files have not been changed." }
}
$activated = $false
try {
    $result = Invoke-PublishRemote @{ operation = 'activate'; id = $transaction; version = $Version; files = $files }
    $activated = $true
    foreach ($name in $allowed) {
        $download = Join-Path $releaseDirectory ('verify-' + [IO.Path]::GetFileName($name))
        $uri = [uri]::new($PublicBaseUrl, "/$name")
        Invoke-WebRequest -Uri $uri -OutFile $download -TimeoutSec 600 -Headers @{ 'Cache-Control' = 'no-cache' }
        if ((Get-Item -LiteralPath $download).Length -ne $files[$name].size -or
            (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant() -cne $files[$name].sha256) {
            throw "Public HTTPS verification failed for $name"
        }
    }
    if (-not $result.ContainsKey('idempotent')) { Invoke-PublishRemote @{ operation = 'commit'; id = $transaction } }
    Write-Output "Published and verified Snow Shot $Version. Public filenames are unchanged."
} catch {
    if ($activated -and -not $result.ContainsKey('idempotent')) {
        Invoke-PublishRemote @{ operation = 'rollback'; id = $transaction } | Out-Null
    }
    throw
}

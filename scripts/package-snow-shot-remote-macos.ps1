[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]*$')][string]$MacHost,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_-]+$')][string]$MacUser,
    [ValidateRange(1, 65535)][int]$MacPort = 22,
    [string]$MacIdentityFile,
    [string]$MacKnownHostsFile,
    [Parameter(Mandatory)][string]$MacProjectDirectory,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(1, 256)][int]$Parallelism = 4,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Restrict scp's remote path grammar as well as the SSH destination.
if ($MacProjectDirectory -notmatch '^/[A-Za-z0-9_./-]+$' -or $MacProjectDirectory.Contains('..')) {
    throw 'MacProjectDirectory must be an absolute POSIX path without spaces or traversal.'
}
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$common = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'UpdateHostKeys=no', '-o', 'ConnectTimeout=15')
if ($MacIdentityFile) { $common += @('-i', [IO.Path]::GetFullPath($MacIdentityFile), '-o', 'IdentitiesOnly=yes') }
if ($MacKnownHostsFile) { $common += @('-o', "UserKnownHostsFile=$([IO.Path]::GetFullPath($MacKnownHostsFile))") }
$request = @{ projectDirectory = $MacProjectDirectory; version = $Version; parallelism = $Parallelism;
    skipBuild = [bool]$SkipBuild; id = [guid]::NewGuid().ToString('N') }
$encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($request | ConvertTo-Json -Compress)))
$destination = "$MacUser@$MacHost"
$info = [Diagnostics.ProcessStartInfo]::new('ssh')
$info.UseShellExecute = $false
$info.RedirectStandardInput = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
foreach ($arg in ($common + @('-p', "$MacPort", $destination, "python3 - $encoded"))) { $info.ArgumentList.Add($arg) }
$process = [Diagnostics.Process]::Start($info)
$output = $process.StandardOutput.ReadToEndAsync()
$logPath = Join-Path $OutputDirectory 'macos-build.log'
$log = [IO.File]::Create($logPath)
try {
    $errors = $process.StandardError.BaseStream.CopyToAsync($log)
    $process.StandardInput.Write((Get-Content -Raw (Join-Path $PSScriptRoot 'snow-shot-remote-macos.py')))
    $process.StandardInput.Close()
    $process.WaitForExit()
    $errors.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "Remote macOS packaging failed. See $logPath" }
} finally { $log.Dispose(); $process.Dispose() }
$result = $output.GetAwaiter().GetResult() | ConvertFrom-Json
$expectedPath = "$MacProjectDirectory/artifacts/remote-release-$($request.id)/snow-shot_macos-arm64.dmg"
if ($result.version -cne $Version -or $result.path -cne $expectedPath) { throw 'Unexpected remote package identity.' }
$image = Join-Path $OutputDirectory 'snow-shot_macos-arm64.dmg'
& scp @common -P $MacPort "$destination`:$expectedPath" $image
if ($LASTEXITCODE -ne 0) { throw 'Downloading the macOS package failed.' }
if ((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant() -cne $result.sha256 -or
    (Get-Item -LiteralPath $image).Length -ne $result.size) { throw 'Downloaded macOS package checksum mismatch.' }
[IO.File]::WriteAllText("$image.sha256", "$($result.sha256)  snow-shot_macos-arm64.dmg`n", [Text.UTF8Encoding]::new($false))
$result | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutputDirectory 'macos-build.json') -Encoding utf8NoBOM
Write-Output "Packaged and verified macOS ${Version}: $image"

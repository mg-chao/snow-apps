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
    [ValidateSet('arm64', 'x64')][string]$Architecture = 'arm64',
    [ValidateRange(1, 256)][int]$Parallelism = 4,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Architecture = $Architecture.ToLowerInvariant()
# Restrict scp's remote path grammar as well as the SSH destination.
if ($MacProjectDirectory -notmatch '^/[A-Za-z0-9_./-]+$' -or $MacProjectDirectory.Contains('..')) {
    throw 'MacProjectDirectory must be an absolute POSIX path without spaces or traversal.'
}
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$common = @('-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'UpdateHostKeys=no', '-o', 'ConnectTimeout=15')
if ($MacIdentityFile) { $common += @('-i', [IO.Path]::GetFullPath($MacIdentityFile), '-o', 'IdentitiesOnly=yes') }
if ($MacKnownHostsFile) { $common += @('-o', "UserKnownHostsFile=$([IO.Path]::GetFullPath($MacKnownHostsFile))") }
$products = if ($Architecture -eq 'arm64') { @('snow-shot', 'snow-shot-mini') } else { @('snow-shot') }
$editions = if ($Architecture -eq 'arm64') { @('Full', 'Mini') } else { @('Full') }
$assetArchitecture = if ($Architecture -eq 'arm64') { 'arm64' } else { 'x86_64' }
$request = @{ projectDirectory = $MacProjectDirectory; version = $Version; parallelism = $Parallelism;
    architecture = $Architecture; editions = @($editions); skipBuild = [bool]$SkipBuild; id = [guid]::NewGuid().ToString('N') }
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
$metadataBase = if ($Architecture -eq 'arm64') { 'macos-build' } else { 'macos-x64-build' }
$logPath = Join-Path $OutputDirectory "$metadataBase.log"
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
if ($result.version -cne $Version -or $result.architecture -cne $Architecture -or
    @($result.images).Count -ne @($products).Count) { throw 'Unexpected remote package identity.' }
foreach ($product in $products) {
    $item = @($result.images | Where-Object { $_.product -ceq $product })
    $expectedPath = "$MacProjectDirectory/artifacts/remote-release-$($request.id)/${product}_macos-$assetArchitecture.dmg"
    if ($item.Count -ne 1 -or $item[0].path -cne $expectedPath -or
        $item[0].sha256 -cnotmatch '^[a-f0-9]{64}$' -or $item[0].size -le 0) { throw 'Unexpected remote package identity.' }
    $image = Join-Path $OutputDirectory "${product}_macos-$assetArchitecture.dmg"
    & scp @common -P $MacPort "$destination`:$expectedPath" $image
    if ($LASTEXITCODE -ne 0) { throw 'Downloading the macOS package failed.' }
    if ((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant() -cne $item[0].sha256 -or
        (Get-Item -LiteralPath $image).Length -ne $item[0].size) { throw 'Downloaded macOS package checksum mismatch.' }
    [IO.File]::WriteAllText("$image.sha256", "$($item[0].sha256)  ${product}_macos-$assetArchitecture.dmg`n", [Text.UTF8Encoding]::new($false))
}
$result | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutputDirectory "$metadataBase.json") -Encoding utf8NoBOM
Write-Output "Packaged and verified macOS ${Version} (${Architecture}): $image"

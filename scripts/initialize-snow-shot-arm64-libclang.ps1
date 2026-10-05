#Requires -Version 7.0
[CmdletBinding()]
param([string]$ToolDirectory = (Join-Path $PSScriptRoot '../.tools/llvm-arm64'))
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
if ((Get-SnowWindowsHostArchitecture) -cne 'arm64') { throw 'Native ARM64 libclang provisioning requires an ARM64 Windows host.' }
$rustVersion = & rustc -vV
if ($LASTEXITCODE -ne 0 -or -not ($rustVersion | Select-String '^host: aarch64-pc-windows-msvc$')) {
    throw 'Native ARM64 CI requires an ARM64 Rust host before exporting ARM64 libclang. An emulated x64 Rust host requires x64 libclang.'
}
Add-SnowMsvcToolsToPath -Architecture arm64 | Out-Null
$clang = Get-Command clang.exe -ErrorAction SilentlyContinue
$candidates = @($env:LIBCLANG_PATH, (Join-Path $ToolDirectory 'bin'),
    (Join-Path $env:VCINSTALLDIR 'Tools/Llvm/ARM64/bin'),
    (Join-Path $env:VCINSTALLDIR 'Tools/Llvm/bin'),
    $(if ($clang) { Split-Path -Parent $clang.Source })) | Where-Object { $_ }
foreach ($candidate in $candidates) {
    $dll = Join-Path $candidate 'libclang.dll'
    if ((Test-Path -LiteralPath $dll -PathType Leaf) -and (Get-SnowPeMachine -Path $dll) -eq 0xaa64) {
        Write-Output ([IO.Path]::GetFullPath($candidate))
        return
    }
}
# Use LLVM's pinned Windows on Arm distribution when the hosted image lacks libclang.
$version = '22.1.8'
$assetName = "LLVM-$version-woa64.exe"
$headers = @{ Accept = 'application/vnd.github+json' }
if ($env:GH_TOKEN) { $headers.Authorization = "Bearer $env:GH_TOKEN" }
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/llvm/llvm-project/releases/tags/llvmorg-$version" -Headers $headers
$asset = @($release.assets | Where-Object { $_.name -ceq $assetName })
if ($asset.Count -ne 1 -or $asset[0].digest -cnotmatch '^sha256:[0-9a-f]{64}$') { throw 'The official ARM64 LLVM asset has no unique SHA-256 digest.' }
$installer = Join-Path ([IO.Path]::GetTempPath()) "$([guid]::NewGuid().ToString('N'))-$assetName"
try {
    Invoke-WebRequest -Uri $asset[0].browser_download_url -OutFile $installer
    if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant() -cne $asset[0].digest.Substring(7)) { throw 'LLVM ARM64 installer SHA-256 mismatch.' }
    $directory = [IO.Path]::GetFullPath($ToolDirectory)
    $process = Start-Process -FilePath $installer -ArgumentList '/S', "/D=$directory" -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(300000) -or $process.ExitCode -ne 0) { throw 'LLVM ARM64 installation failed.' }
    $bin = Join-Path $directory 'bin'
    if ((Get-SnowPeMachine -Path (Join-Path $bin 'libclang.dll')) -ne 0xaa64) { throw 'Provisioned libclang is not ARM64.' }
    Write-Output $bin
} finally {
    if (Test-Path -LiteralPath $installer -PathType Leaf) { Remove-Item -LiteralPath $installer }
}

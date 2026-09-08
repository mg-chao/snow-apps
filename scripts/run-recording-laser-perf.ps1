[CmdletBinding()]
param([string]$PreviewDirectory = "")

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$env:FFMPEG_DIR = Join-Path $repoRoot ".tools/vcpkg/installed/dynamic/x64-windows"
$env:VCPKGRS_TRIPLET = "x64-windows"
$env:VCPKGRS_DYNAMIC = "1"
$env:Path = "$(Join-Path $env:FFMPEG_DIR 'bin');$env:Path"
$benchmarkArguments = @()
if ($PreviewDirectory) {
    $benchmarkArguments += $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($PreviewDirectory)
}
Push-Location (Join-Path $repoRoot "snow-crates")
try {
    & cargo run -p snow-recording-runtime --release --target x86_64-pc-windows-msvc `
        --target-dir (Join-Path $repoRoot "build/windows-msvc-performance/cargo") `
        --example laser_trail_benchmark -- @benchmarkArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Recording laser benchmark failed."
    }
}
finally {
    Pop-Location
}

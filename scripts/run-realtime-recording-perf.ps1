[CmdletBinding()]
param(
    # Comma-separated compile-time metric groups; trim to measure one group in
    # isolation without the instrumentation overhead of the others.
    [string]$Metrics = "bench-stage-timing,bench-compositor-timing,bench-pipeline-timing",
    [int]$DurationSeconds = 12,
    [int]$WarmupSeconds = 2,
    [int]$Samples = 1,
    [string]$Scenario = "",
    [string]$Backend = "auto",
    [int]$Fps = 30,
    [string]$Clarity = "1080p",
    [switch]$PreferHardware,
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$env:FFMPEG_DIR = Join-Path $repoRoot ".tools/vcpkg/installed/dynamic/x64-windows"
$env:VCPKGRS_TRIPLET = "x64-windows"
$env:VCPKGRS_DYNAMIC = "1"
$env:Path = "$(Join-Path $env:FFMPEG_DIR 'bin');$env:Path"

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot ("build\realtime-recording-perf\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

$benchmarkArguments = @(
    "--duration-seconds", $DurationSeconds,
    "--warmup-seconds", $WarmupSeconds,
    "--samples", $Samples,
    "--backend", $Backend,
    "--fps", $Fps,
    "--clarity", $Clarity,
    "--output", $OutputDirectory
)
if (-not [string]::IsNullOrWhiteSpace($Scenario)) {
    $benchmarkArguments += @("--scenario", $Scenario)
}
if ($PreferHardware) {
    $benchmarkArguments += "--prefer-hardware"
}

Write-Host "Realtime recording benchmark report: $OutputDirectory"
Write-Warning "The benchmark takes over the primary monitor and simulates mouse and keyboard input."
for ($remaining = 5; $remaining -ge 1; $remaining--) {
    Write-Host "Starting in $remaining..."
    Start-Sleep -Seconds 1
}

Push-Location (Join-Path $repoRoot "snow-crates")
try {
    & cargo run -p snow-recording-runtime --release --target x86_64-pc-windows-msvc `
        --target-dir (Join-Path $repoRoot "build/windows-msvc-performance/cargo") `
        --features $Metrics `
        --example realtime_recording_benchmark -- @benchmarkArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Realtime recording benchmark failed."
    }
}
finally {
    Pop-Location
}

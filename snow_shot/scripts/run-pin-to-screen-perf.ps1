#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$QtBin = "",
    [string]$OutputDirectory = "",
    [int]$ScreenIndex = 0,
    [int]$Warmups = 3,
    [int]$Samples = 40,
    [int]$TimeoutMilliseconds = 30000,
    [string]$Scenarios = "all",
    [int]$ScrollSteps = 8,
    [int]$ScrollDistance = 96,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$shot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$workspace = (Resolve-Path (Join-Path $shot "..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $shot ("build\pin-to-screen-perf\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

& (Join-Path $shot "scripts/configure-msvc-perf.ps1") -Fresh -QtBin $QtBin
if ($LASTEXITCODE -ne 0) { throw "The performance configuration failed" }
& cmake --build (Join-Path $workspace "build\windows-msvc-performance") --config Release --target `
    snow_shot snow-shot-pin-to-screen-performance-benchmark --parallel
if ($LASTEXITCODE -ne 0) { throw "The pin-to-screen benchmark build failed" }

$release = Join-Path $workspace "build\windows-msvc-performance\snow_shot\test-bin\Release"
$benchmark = Join-Path $release "snow-shot-pin-to-screen-performance-benchmark.exe"
$application = Join-Path $release "snow_shot.exe"
if (!(Test-Path $benchmark)) {
    $benchmark = (Get-ChildItem -Path (Join-Path $workspace "build\windows-msvc-performance") -Recurse -Filter "snow-shot-pin-to-screen-performance-benchmark.exe" | Select-Object -First 1).FullName
}
if (!(Test-Path $application)) {
    $application = (Get-ChildItem -Path (Join-Path $workspace "build\windows-msvc-performance") -Recurse -Filter "snow_shot.exe" | Select-Object -First 1).FullName
}
if (!(Test-Path $benchmark) -or !(Test-Path $application)) { throw "Expected benchmark binaries were not produced" }

Add-Type -AssemblyName System.Windows.Forms
$qtRuntime = Set-SnowPerformanceQtRuntime
$savedTrace = $env:SNOW_SHOT_PIN_PERF_TRACE; $cursor = [System.Windows.Forms.Cursor]::Position
try {
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $arguments = @("--app", $application, "--output", $OutputDirectory, "--screen-index", $ScreenIndex.ToString(), "--warmups", $Warmups.ToString(), "--samples", $Samples.ToString(), "--timeout-ms", $TimeoutMilliseconds.ToString(), "--scenarios", $Scenarios, "--scroll-steps", $ScrollSteps.ToString(), "--scroll-distance", $ScrollDistance.ToString())
    if ($SelfTest) { $arguments += "--self-test" }
    & $benchmark @arguments; $exitCode = $LASTEXITCODE
}
finally {
    [System.Windows.Forms.Cursor]::Position = $cursor
    Restore-SnowPerformanceQtRuntime -Snapshot $qtRuntime
    $env:SNOW_SHOT_PIN_PERF_TRACE = $savedTrace
}
if (!$SelfTest) { Write-Host "Pin-to-screen performance JSON: $(Join-Path $OutputDirectory 'report.json')"; Write-Host "Pin-to-screen performance HTML: $(Join-Path $OutputDirectory 'report.html')" }
exit $exitCode

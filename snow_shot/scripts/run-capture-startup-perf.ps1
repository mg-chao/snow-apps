#requires -Version 7.2
[CmdletBinding()]
param(
    [string]$QtBin = "",
    [string]$OutputDirectory = "",
    [int]$ScreenIndex = 0,
    [int]$Captures = 12,
    [int]$SettleMilliseconds = 500,
    [int]$TimeoutMilliseconds = 30000,
    [ValidateSet("", "single-repaint", "posted-update", "native-update", "native-invalidate", "native-invalidate-suppressed")]
    [string]$RevealStrategy = "",
    [switch]$SkipBuild,
    [switch]$SelfTest,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
$shot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$workspace = (Resolve-Path (Join-Path $shot "..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$performanceTarget = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
$performanceBuildDirectory = Join-Path $workspace "build/$($performanceTarget.Preset)"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $shot ("build/capture-startup-perf/" + $(if ($Architecture -eq "arm64") { "windows-arm64/" } else { "" }) + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

if (!$SkipBuild) {
    & (Join-Path $shot "scripts/configure-msvc-perf.ps1") -Fresh -QtBin $QtBin -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) { throw "The performance configuration failed" }
    & cmake --build ($performanceBuildDirectory) --config Release --target `
        snow_shot snow-shot-capture-startup-performance-benchmark --parallel
    if ($LASTEXITCODE -ne 0) { throw "The capture startup benchmark build failed" }
}
else {
    Initialize-SnowPerformanceEnvironment -QtBin $QtBin -Architecture $Architecture
}

$release = Join-Path $performanceBuildDirectory "snow_shot/test-bin/Release"
$benchmark = Join-Path $release "snow-shot-capture-startup-performance-benchmark.exe"
$application = Join-Path $release "snow_shot.exe"
if (!(Test-Path $benchmark)) {
    $benchmark = (Get-ChildItem -Path ($performanceBuildDirectory) -Recurse -Filter "snow-shot-capture-startup-performance-benchmark.exe" | Select-Object -First 1).FullName
}
if (!(Test-Path $application)) {
    $application = (Get-ChildItem -Path ($performanceBuildDirectory) -Recurse -Filter "snow_shot.exe" | Select-Object -First 1).FullName
}
if (!(Test-Path $benchmark) -or !(Test-Path $application)) { throw "Expected benchmark binaries were not produced" }

Assert-SnowPerformanceExecutable -Path $benchmark -Architecture $Architecture
Assert-SnowPerformanceExecutable -Path $application -Architecture $Architecture
Add-Type -AssemblyName System.Windows.Forms
$qtRuntime = Set-SnowPerformanceQtRuntime -Architecture $Architecture
$savedTrace = $env:SNOW_SHOT_CAPTURE_PERF_TRACE; $cursor = [System.Windows.Forms.Cursor]::Position
try {
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $arguments = @("--app", $application, "--output", $OutputDirectory, "--screen-index", $ScreenIndex.ToString(), "--captures", $Captures.ToString(), "--settle-ms", $SettleMilliseconds.ToString(), "--timeout-ms", $TimeoutMilliseconds.ToString())
    if (![string]::IsNullOrWhiteSpace($RevealStrategy)) { $arguments += @("--reveal-strategy", $RevealStrategy) }
    if ($SelfTest) { $arguments += "--self-test" }
    Write-Host "The benchmark controls the mouse cursor; do not touch the machine while it runs."
    & $benchmark @arguments; $exitCode = $LASTEXITCODE
}
finally {
    [System.Windows.Forms.Cursor]::Position = $cursor
    Restore-SnowPerformanceQtRuntime -Snapshot $qtRuntime
    $env:SNOW_SHOT_CAPTURE_PERF_TRACE = $savedTrace
}
if (!$SelfTest) { Write-Host "Capture startup performance JSON: $(Join-Path $OutputDirectory 'report.json')"; Write-Host "Capture startup performance HTML: $(Join-Path $OutputDirectory 'report.html')" }
exit $exitCode

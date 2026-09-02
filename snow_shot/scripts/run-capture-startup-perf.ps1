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
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$shot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$workspace = (Resolve-Path (Join-Path $shot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $shot ("build\capture-startup-perf\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

if (!$SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $shot "scripts\configure-msvc-perf.ps1") -Fresh
    if ($LASTEXITCODE -ne 0) { throw "The performance configuration failed" }
    & cmake --build (Join-Path $workspace "build\windows-msvc-performance") --config Release --target `
        snow_shot snow-shot-capture-startup-performance-benchmark --parallel
    if ($LASTEXITCODE -ne 0) { throw "The capture startup benchmark build failed" }
}

$release = Join-Path $workspace "build\windows-msvc-performance\snow_shot\test-bin\Release"
$benchmark = Join-Path $release "snow-shot-capture-startup-performance-benchmark.exe"
$application = Join-Path $release "snow_shot.exe"
if (!(Test-Path $benchmark)) {
    $benchmark = (Get-ChildItem -Path (Join-Path $workspace "build\windows-msvc-performance") -Recurse -Filter "snow-shot-capture-startup-performance-benchmark.exe" | Select-Object -First 1).FullName
}
if (!(Test-Path $application)) {
    $application = (Get-ChildItem -Path (Join-Path $workspace "build\windows-msvc-performance") -Recurse -Filter "snow_shot.exe" | Select-Object -First 1).FullName
}
if (!(Test-Path $benchmark) -or !(Test-Path $application)) { throw "Expected benchmark binaries were not produced" }

Add-Type -AssemblyName System.Windows.Forms
$savedPath = $env:PATH; $savedPlatform = $env:QT_QPA_PLATFORM; $savedPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH; $savedTrace = $env:SNOW_SHOT_CAPTURE_PERF_TRACE; $cursor = [System.Windows.Forms.Cursor]::Position
try {
    if (![string]::IsNullOrWhiteSpace($QtBin)) {
        $qtRoot = Split-Path $QtBin -Parent
        $env:PATH = "$QtBin;$env:PATH"; $env:QT_QPA_PLATFORM = "windows"; $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtRoot "plugins\platforms"
    }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $arguments = @("--app", $application, "--output", $OutputDirectory, "--screen-index", $ScreenIndex.ToString(), "--captures", $Captures.ToString(), "--settle-ms", $SettleMilliseconds.ToString(), "--timeout-ms", $TimeoutMilliseconds.ToString())
    if (![string]::IsNullOrWhiteSpace($RevealStrategy)) { $arguments += @("--reveal-strategy", $RevealStrategy) }
    if ($SelfTest) { $arguments += "--self-test" }
    Write-Host "The benchmark controls the mouse cursor; do not touch the machine while it runs."
    & $benchmark @arguments; $exitCode = $LASTEXITCODE
}
finally {
    [System.Windows.Forms.Cursor]::Position = $cursor; $env:PATH = $savedPath; $env:QT_QPA_PLATFORM = $savedPlatform; $env:QT_QPA_PLATFORM_PLUGIN_PATH = $savedPluginPath; $env:SNOW_SHOT_CAPTURE_PERF_TRACE = $savedTrace
}
if (!$SelfTest) { Write-Host "Capture startup performance JSON: $(Join-Path $OutputDirectory 'report.json')"; Write-Host "Capture startup performance HTML: $(Join-Path $OutputDirectory 'report.html')" }
exit $exitCode

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
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $shot ("build\pin-to-screen-perf\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $shot "scripts\configure-msvc-perf.ps1") -Fresh
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
if (!(Test-Path (Join-Path $QtBin "Qt6Core.dll"))) { throw "Qt runtime not found in QtBin: $QtBin" }

Add-Type -AssemblyName System.Windows.Forms
$savedPath = $env:PATH; $savedPlatform = $env:QT_QPA_PLATFORM; $savedPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH; $savedTrace = $env:SNOW_SHOT_PIN_PERF_TRACE; $cursor = [System.Windows.Forms.Cursor]::Position
try {
    $qtRoot = Split-Path $QtBin -Parent
    $env:PATH = "$QtBin;$env:PATH"; $env:QT_QPA_PLATFORM = "windows"; $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtRoot "plugins\platforms"
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $arguments = @("--app", $application, "--output", $OutputDirectory, "--screen-index", $ScreenIndex.ToString(), "--warmups", $Warmups.ToString(), "--samples", $Samples.ToString(), "--timeout-ms", $TimeoutMilliseconds.ToString(), "--scenarios", $Scenarios, "--scroll-steps", $ScrollSteps.ToString(), "--scroll-distance", $ScrollDistance.ToString())
    if ($SelfTest) { $arguments += "--self-test" }
    & $benchmark @arguments; $exitCode = $LASTEXITCODE
}
finally {
    [System.Windows.Forms.Cursor]::Position = $cursor; $env:PATH = $savedPath; $env:QT_QPA_PLATFORM = $savedPlatform; $env:QT_QPA_PLATFORM_PLUGIN_PATH = $savedPluginPath; $env:SNOW_SHOT_PIN_PERF_TRACE = $savedTrace
}
if (!$SelfTest) { Write-Host "Pin-to-screen performance JSON: $(Join-Path $OutputDirectory 'report.json')"; Write-Host "Pin-to-screen performance HTML: $(Join-Path $OutputDirectory 'report.html')" }
exit $exitCode

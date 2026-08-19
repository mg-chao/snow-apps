[CmdletBinding()]
param(
    [string]$QtBin = "",
    [string]$OutputDirectory = "",
    [int]$Samples = 10,
    [int]$ScreenIndex = 0,
    [int]$PollMilliseconds = 250,
    [int]$BaselineMinimumWaitMilliseconds = 20000,
    [int]$PostEndMinimumWaitMilliseconds = 15000,
    [int]$StabilityWindow = 20,
    [int]$StabilityRangeKiB = 1024,
    [int]$TimeoutMilliseconds = 90000,
    [int]$CountdownSeconds = 5,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$shot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$workspace = (Resolve-Path (Join-Path $shot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $shot (
        "build\screenshot-lifecycle-perf\" + (Get-Date -Format "yyyyMMdd-HHmmss")
    )
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

if ($Samples -le 0) { throw "Samples must be positive" }
if ($ScreenIndex -lt 0) { throw "ScreenIndex must be nonnegative" }
if ($PollMilliseconds -le 0) { throw "PollMilliseconds must be positive" }
if ($BaselineMinimumWaitMilliseconds -lt 0) {
    throw "BaselineMinimumWaitMilliseconds must be nonnegative"
}
if ($PostEndMinimumWaitMilliseconds -lt 0) {
    throw "PostEndMinimumWaitMilliseconds must be nonnegative"
}
if ($StabilityWindow -le 1) { throw "StabilityWindow must be greater than one" }
if ($StabilityRangeKiB -le 0) { throw "StabilityRangeKiB must be positive" }
if ($TimeoutMilliseconds -le $BaselineMinimumWaitMilliseconds -or
    $TimeoutMilliseconds -le $PostEndMinimumWaitMilliseconds) {
    throw "TimeoutMilliseconds must exceed both minimum memory waits"
}
if (!(Test-Path (Join-Path $QtBin "Qt6Core.dll"))) {
    throw "Qt runtime not found in QtBin: $QtBin"
}

& powershell -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $shot "scripts\configure-msvc-perf.ps1") -Fresh
if ($LASTEXITCODE -ne 0) { throw "The performance configuration failed" }

& cmake --build (Join-Path $workspace "build\windows-msvc-performance") `
    --config Release --target snow_shot `
    snow-shot-screenshot-lifecycle-performance-benchmark --parallel
if ($LASTEXITCODE -ne 0) { throw "The screenshot lifecycle benchmark build failed" }

$buildRoot = Join-Path $workspace "build\windows-msvc-performance"
$release = Join-Path $buildRoot "snow_shot\test-bin\Release"
$benchmark = Join-Path $release "snow-shot-screenshot-lifecycle-performance-benchmark.exe"
$application = Join-Path $release "snow_shot.exe"
if (!(Test-Path $benchmark)) {
    $benchmark = (Get-ChildItem -Path $buildRoot -Recurse `
        -Filter "snow-shot-screenshot-lifecycle-performance-benchmark.exe" |
        Select-Object -First 1).FullName
}
if (!(Test-Path $application)) {
    $application = (Get-ChildItem -Path $buildRoot -Recurse -Filter "snow_shot.exe" |
        Select-Object -First 1).FullName
}
if (!(Test-Path $benchmark) -or !(Test-Path $application)) {
    throw "Expected screenshot lifecycle benchmark binaries were not produced"
}

Add-Type -AssemblyName System.Windows.Forms
$savedPath = $env:PATH
$savedPlatform = $env:QT_QPA_PLATFORM
$savedPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
$savedCommit = $env:SNOW_SHOT_PERF_GIT_COMMIT
$savedDirty = $env:SNOW_SHOT_PERF_GIT_DIRTY
$cursor = [System.Windows.Forms.Cursor]::Position
try {
    $qtRoot = Split-Path $QtBin -Parent
    $env:PATH = "$QtBin;$env:PATH"
    $env:QT_QPA_PLATFORM = "windows"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtRoot "plugins\platforms"
    $env:SNOW_SHOT_PERF_GIT_COMMIT = (& git -C $workspace rev-parse HEAD).Trim()
    $env:SNOW_SHOT_PERF_GIT_DIRTY = if (
        [string]::IsNullOrWhiteSpace((& git -C $workspace status --porcelain))
    ) { "0" } else { "1" }

    if (!$SelfTest) {
        Write-Warning (
            "This benchmark repeatedly launches Snow Shot, displays full-screen screenshot " +
            "overlays, and controls the real mouse pointer. Do not use the workstation until it finishes."
        )
        for ($remaining = $CountdownSeconds; $remaining -gt 0; --$remaining) {
            Write-Host "Starting screenshot lifecycle benchmark in $remaining"
            Start-Sleep -Seconds 1
        }
    }

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $arguments = @(
        "--app", $application,
        "--output", $OutputDirectory,
        "--samples", $Samples.ToString(),
        "--screen-index", $ScreenIndex.ToString(),
        "--poll-ms", $PollMilliseconds.ToString(),
        "--baseline-min-wait-ms", $BaselineMinimumWaitMilliseconds.ToString(),
        "--post-end-min-wait-ms", $PostEndMinimumWaitMilliseconds.ToString(),
        "--stability-window", $StabilityWindow.ToString(),
        "--stability-range-kib", $StabilityRangeKiB.ToString(),
        "--timeout-ms", $TimeoutMilliseconds.ToString()
    )
    if ($SelfTest) { $arguments += "--self-test" }
    & $benchmark @arguments
    $benchmarkExitCode = $LASTEXITCODE
}
finally {
    [System.Windows.Forms.Cursor]::Position = $cursor
    $env:PATH = $savedPath
    $env:QT_QPA_PLATFORM = $savedPlatform
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $savedPluginPath
    $env:SNOW_SHOT_PERF_GIT_COMMIT = $savedCommit
    $env:SNOW_SHOT_PERF_GIT_DIRTY = $savedDirty
}

if (!$SelfTest) {
    Write-Host "Screenshot lifecycle raw data: $(Join-Path $OutputDirectory 'raw.jsonl')"
    Write-Host "Screenshot lifecycle JSON: $(Join-Path $OutputDirectory 'report.json')"
    Write-Host "Screenshot lifecycle HTML: $(Join-Path $OutputDirectory 'report.html')"
}
exit $benchmarkExitCode

[CmdletBinding()]
param(
    [string]$QtBin = "",
    [string]$OutputDirectory = "",
    [int]$Samples = 10,
    [int]$ScreenIndex = 0,
    [int]$PollMilliseconds = 250,
    [int]$ColdStartMinimumWaitMilliseconds = 20000,
    [int]$StageMinimumWaitMilliseconds = 5000,
    [int]$StabilityWindow = 20,
    [int]$StabilityRangeKiB = 1024,
    [int]$FinalIdleToleranceKiB = 3072,
    [int]$TimeoutMilliseconds = 90000,
    [int]$CountdownSeconds = 5,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$shot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$workspace = (Resolve-Path (Join-Path $shot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $shot (
        "build\screenshot-memory-footprint\" + (Get-Date -Format "yyyyMMdd-HHmmss")
    )
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

if ($Samples -le 0) { throw "Samples must be positive" }
if ($ScreenIndex -lt 0) { throw "ScreenIndex must be nonnegative" }
if ($PollMilliseconds -le 0) { throw "PollMilliseconds must be positive" }
if ($ColdStartMinimumWaitMilliseconds -lt 0) {
    throw "ColdStartMinimumWaitMilliseconds must be nonnegative"
}
if ($StageMinimumWaitMilliseconds -lt 0) {
    throw "StageMinimumWaitMilliseconds must be nonnegative"
}
if ($StabilityWindow -le 1) { throw "StabilityWindow must be greater than one" }
if ($StabilityRangeKiB -le 0) { throw "StabilityRangeKiB must be positive" }
if ($FinalIdleToleranceKiB -lt 0) { throw "FinalIdleToleranceKiB must be nonnegative" }
if ($TimeoutMilliseconds -le $ColdStartMinimumWaitMilliseconds -or
    $TimeoutMilliseconds -le $StageMinimumWaitMilliseconds) {
    throw "TimeoutMilliseconds must exceed both minimum waits"
}
if (![string]::IsNullOrWhiteSpace($QtBin)) {
    $QtBin = (Resolve-Path -LiteralPath $QtBin).Path
}

& powershell -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $shot "scripts\configure-msvc-perf.ps1") -Fresh
if ($LASTEXITCODE -ne 0) { throw "The performance configuration failed" }

& cmake --build (Join-Path $workspace "build\windows-msvc-performance") `
    --config Release --target snow_shot `
    snow-shot-screenshot-memory-footprint-benchmark --parallel
if ($LASTEXITCODE -ne 0) { throw "The screenshot memory footprint benchmark build failed" }

$buildRoot = Join-Path $workspace "build\windows-msvc-performance"
$release = Join-Path $buildRoot "snow_shot\Release"
$benchmark = Join-Path $release "snow-shot-screenshot-memory-footprint-benchmark.exe"
$application = Join-Path $release "snow_shot.exe"
if (!(Test-Path -LiteralPath $benchmark) -or !(Test-Path -LiteralPath $application)) {
    throw "Expected screenshot memory footprint benchmark binaries were not produced"
}

Add-Type -AssemblyName System.Windows.Forms
$savedPath = $env:PATH
$savedPlatform = $env:QT_QPA_PLATFORM
$savedPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
$savedCommit = $env:SNOW_SHOT_PERF_GIT_COMMIT
$savedDirty = $env:SNOW_SHOT_PERF_GIT_DIRTY
$cursor = [System.Windows.Forms.Cursor]::Position
$qtBinPath = $null
$qtPlatformPluginPath = $null
try {
    if (![string]::IsNullOrWhiteSpace($QtBin)) {
        if (Test-Path -LiteralPath (Join-Path $QtBin "Qt6Core.dll")) {
            $qtBinPath = $QtBin
        } else {
            Write-Warning (
                "QtBin does not contain Qt6Core.dll; assuming a static Qt build and continuing: " +
                $QtBin
            )
        }
    }
    if ($null -eq $qtBinPath) {
        $stagedQtCore = Join-Path (Split-Path $application -Parent) "Qt6Core.dll"
        if (Test-Path -LiteralPath $stagedQtCore) {
            $qtBinPath = Split-Path $application -Parent
        }
    }
    if ($null -ne $qtBinPath) {
        $env:PATH = "$qtBinPath;$env:PATH"
        $qtRoot = Split-Path $qtBinPath -Parent
        $candidatePluginPath = Join-Path $qtRoot "plugins\platforms"
        if (Test-Path -LiteralPath $candidatePluginPath) {
            $qtPlatformPluginPath = $candidatePluginPath
        }
    }
    $env:QT_QPA_PLATFORM = "windows"
    if ($null -ne $qtPlatformPluginPath) {
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = $qtPlatformPluginPath
    } else {
        Remove-Item Env:QT_QPA_PLATFORM_PLUGIN_PATH -ErrorAction SilentlyContinue
    }
    $env:SNOW_SHOT_PERF_GIT_COMMIT = (& git -C $workspace rev-parse HEAD).Trim()
    $env:SNOW_SHOT_PERF_GIT_DIRTY = if (
        [string]::IsNullOrWhiteSpace((& git -C $workspace status --porcelain))
    ) { "0" } else { "1" }

    if (!$SelfTest) {
        Write-Warning (
            "This benchmark repeatedly launches Snow Shot, opens full-screen screenshot overlays, " +
            "and controls the real mouse pointer. Do not use the workstation until it finishes."
        )
        for ($remaining = $CountdownSeconds; $remaining -gt 0; --$remaining) {
            Write-Host "Starting screenshot memory footprint benchmark in $remaining"
            Start-Sleep -Seconds 1
        }
    }

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $rawPath = Join-Path $OutputDirectory "raw.jsonl"
    $jsonReportPath = Join-Path $OutputDirectory "report.json"
    $htmlReportPath = Join-Path $OutputDirectory "report.html"
    foreach ($artifactPath in @($rawPath, $jsonReportPath, $htmlReportPath)) {
        if (Test-Path -LiteralPath $artifactPath) {
            Remove-Item -LiteralPath $artifactPath -Force
        }
    }
    $arguments = @(
        "--app", $application,
        "--output", $OutputDirectory,
        "--samples", $Samples.ToString(),
        "--screen-index", $ScreenIndex.ToString(),
        "--poll-ms", $PollMilliseconds.ToString(),
        "--cold-start-min-wait-ms", $ColdStartMinimumWaitMilliseconds.ToString(),
        "--stage-min-wait-ms", $StageMinimumWaitMilliseconds.ToString(),
        "--stability-window", $StabilityWindow.ToString(),
        "--stability-range-kib", $StabilityRangeKiB.ToString(),
        "--final-idle-tolerance-kib", $FinalIdleToleranceKiB.ToString(),
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
    if (Test-Path -LiteralPath $rawPath) {
        Write-Host "Screenshot memory footprint raw data: $rawPath"
    }
    if (Test-Path -LiteralPath $jsonReportPath) {
        Write-Host "Screenshot memory footprint JSON: $jsonReportPath"
    } else {
        Write-Warning "The benchmark did not produce report.json"
    }
    if (Test-Path -LiteralPath $htmlReportPath) {
        Write-Host "Screenshot memory footprint HTML: $htmlReportPath"
    } else {
        Write-Warning "The benchmark did not produce report.html"
    }
}
exit $benchmarkExitCode

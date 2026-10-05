#requires -Version 7.2
param(
    [string]$QtBin = "",
    [string]$OutputDirectory = "",
    [switch]$IncludeLiveCapture,
    [Nullable[int]]$RegionX = $null,
    [Nullable[int]]$RegionY = $null,
    [int]$RegionWidth = 800,
    [int]$RegionHeight = 600,
    [int]$LiveWarmups = 30,
    [int]$LiveSamples = 240,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"

function Invoke-Checked {
    param(
        [string]$Executable,
        [string[]]$Arguments
    )
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable exited with code $LASTEXITCODE"
    }
}

$workspace = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$performanceTarget = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
$performanceBuildDirectory = Join-Path $workspace "build/$($performanceTarget.Preset)"
Initialize-SnowPerformanceEnvironment -QtBin $QtBin -Architecture $Architecture
$crates = Join-Path $workspace "snow-crates"
$shot = Join-Path $workspace "snow_shot"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $crates "target/scrolling-perf/$(if ($Architecture -eq 'arm64') { 'windows-arm64/' })replay"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

Push-Location $crates
try {
    Invoke-Checked "cargo" @(
        "run", "--release", "--target", $performanceTarget.RustTarget, "-p", "snow-stitch-images",
        "--features", "bench-internals", "--example", "scrolling_perf", "--",
        "--output", (Join-Path $OutputDirectory "stitch.json"),
        "--frames", "180", "--warmups", "2", "--rounds", "7"
    )

    if ($IncludeLiveCapture) {
        if ($RegionWidth -le 0 -or $RegionHeight -le 0 -or
            $LiveWarmups -lt 0 -or $LiveSamples -le 0) {
            throw "Live capture dimensions and sample counts are invalid"
        }
        if (($null -ne $RegionX) -ne ($null -ne $RegionY)) {
            throw "RegionX and RegionY must be supplied together"
        }
        $liveArguments = @(
            "run", "--release", "--target", $performanceTarget.RustTarget, "-p", "snow-capture-c",
            "--example", "scroll_region_benchmark", "--",
            "--output", (Join-Path $OutputDirectory "live-region.json"),
            "--width", $RegionWidth.ToString(),
            "--height", $RegionHeight.ToString(),
            "--warmups", $LiveWarmups.ToString(),
            "--samples", $LiveSamples.ToString()
        )
        if ($null -ne $RegionX) {
            $liveArguments += @(
                "--x", $RegionX.ToString(),
                "--y", $RegionY.ToString()
            )
        }
        Invoke-Checked "cargo" $liveArguments
    }
}
finally {
    Pop-Location
}

Push-Location $workspace
try {
    & (Join-Path $shot "scripts/configure-msvc-perf.ps1") -Fresh -QtBin $QtBin -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) {
        throw "The performance configuration failed"
    }

    Invoke-Checked "cmake" @(
        "--build", $performanceBuildDirectory, "--config", "Release", "--target",
        "snow-shot-scrolling-result-async-benchmark",
        "snow-shot-scrolling-preview-benchmark",
        "snow-shot-latest-bridge-mailbox-tests",
        "--parallel"
    )

    $release = Join-Path $performanceBuildDirectory "snow_shot/test-bin/Release"

    foreach ($name in @("snow-shot-scrolling-result-async-benchmark.exe", "snow-shot-scrolling-preview-benchmark.exe", "snow-shot-latest-bridge-mailbox-tests.exe")) {
        Assert-SnowPerformanceExecutable -Path (Join-Path $release $name) -Architecture $Architecture
    }
    $qtRuntime = Set-SnowPerformanceQtRuntime -Platform "offscreen" -Architecture $Architecture
    $savedOutput = $env:SNOW_SCROLLING_PERF_OUTPUT
    try {
        $env:SNOW_SCROLLING_PERF_OUTPUT =
            Join-Path $OutputDirectory "async-result.json"
        Invoke-Checked (Join-Path $release "snow-shot-scrolling-result-async-benchmark.exe") @()

        $env:SNOW_SCROLLING_PERF_OUTPUT =
            Join-Path $OutputDirectory "tiled-preview.json"
        Invoke-Checked (Join-Path $release "snow-shot-scrolling-preview-benchmark.exe") @()

        $env:SNOW_SCROLLING_PERF_OUTPUT =
            Join-Path $OutputDirectory "backpressure.json"
        Invoke-Checked (Join-Path $release "snow-shot-latest-bridge-mailbox-tests.exe") @()
    }
    finally {
        Restore-SnowPerformanceQtRuntime -Snapshot $qtRuntime
        $env:SNOW_SCROLLING_PERF_OUTPUT = $savedOutput
    }
}
finally {
    Pop-Location
}

Write-Output "Scrolling performance artifacts: $OutputDirectory"

# For two side-by-side 3840x2160 monitors whose virtual desktop starts at 0,0:
# .\scripts\run-scrolling-perf.ps1 -IncludeLiveCapture -RegionX 0 -RegionY 0 `
#     -RegionWidth 7680 -RegionHeight 2160 -LiveWarmups 10 -LiveSamples 60

param(
    [string]$QtBin = "",
    [string]$OutputDirectory = "",
    [switch]$IncludeLiveCapture,
    [Nullable[int]]$RegionX = $null,
    [Nullable[int]]$RegionY = $null,
    [int]$RegionWidth = 800,
    [int]$RegionHeight = 600,
    [int]$LiveWarmups = 30,
    [int]$LiveSamples = 240
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
$crates = Join-Path $workspace "snow-crates"
$shot = Join-Path $workspace "snow_shot"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $crates "target\scrolling-perf\replay"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

Push-Location $crates
try {
    Invoke-Checked "cargo" @(
        "run", "--release", "-p", "snow-stitch-images",
        "--features", "bench-internals", "--example", "scrolling_perf", "--",
        "--output", (Join-Path $OutputDirectory "stitch.json"),
        "--frames", "180", "--warmups", "2", "--rounds", "7"
    )

    if ($IncludeLiveCapture) {
        if ($RegionWidth -le 0 -or $RegionHeight -le 0 -or
            $LiveWarmups -lt 0 -or $LiveSamples -le 0) {
            throw "Live capture dimensions and sample counts are invalid"
        }
        if ($RegionX.HasValue -ne $RegionY.HasValue) {
            throw "RegionX and RegionY must be supplied together"
        }
        $liveArguments = @(
            "run", "--release", "-p", "snow-capture-c",
            "--example", "scroll_region_benchmark", "--",
            "--output", (Join-Path $OutputDirectory "live-region.json"),
            "--width", $RegionWidth.ToString(),
            "--height", $RegionHeight.ToString(),
            "--warmups", $LiveWarmups.ToString(),
            "--samples", $LiveSamples.ToString()
        )
        if ($RegionX.HasValue) {
            $liveArguments += @(
                "--x", $RegionX.Value.ToString(),
                "--y", $RegionY.Value.ToString()
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
    Invoke-Checked "powershell" @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $shot "scripts\configure-msvc-perf.ps1"),
        "-Fresh"
    )

    Invoke-Checked "cmake" @(
        "--build", "build/windows-msvc-performance", "--config", "Release", "--target",
        "snow-shot-scrolling-result-async-benchmark",
        "snow-shot-scrolling-preview-benchmark",
        "snow-shot-latest-bridge-mailbox-tests",
        "--parallel"
    )

    if (!(Test-Path (Join-Path $QtBin "Qt6Core.dll"))) {
        throw "Qt 6.11.1 runtime was not found in QtBin: $QtBin"
    }
    $env:PATH = "$QtBin;$env:PATH"
    $qtRoot = Split-Path $QtBin -Parent
    $env:QT_QPA_PLATFORM = "offscreen"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtRoot "plugins\platforms"
    $release = Join-Path $workspace "build\windows-msvc-performance\snow_shot\test-bin\Release"

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
    Remove-Item Env:SNOW_SCROLLING_PERF_OUTPUT -ErrorAction SilentlyContinue
    Pop-Location
}

Write-Output "Scrolling performance artifacts: $OutputDirectory"

# For two side-by-side 3840x2160 monitors whose virtual desktop starts at 0,0:
# .\scripts\run-scrolling-perf.ps1 -IncludeLiveCapture -RegionX 0 -RegionY 0 `
#     -RegionWidth 7680 -RegionHeight 2160 -LiveWarmups 10 -LiveSamples 60

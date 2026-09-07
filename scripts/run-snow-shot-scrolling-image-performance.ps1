[CmdletBinding()]
param(
    [string]$Image = "",
    [int]$ViewportHeight = 1600,
    [int]$StepPx = 25,
    [double]$ScrollFps = 30,
    [int]$MaxSteps = -1,
    [string]$OutputDirectory = "",
    [switch]$DetailedTiming
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$previousEnvironment = [Environment]::GetEnvironmentVariables("Process")
Push-Location $repoRoot
try {
    . (Join-Path $PSScriptRoot "snow-build-environment.ps1")
    Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
    $detail = if ($DetailedTiming) { "ON" } else { "OFF" }
    & cmake --preset windows-msvc-performance "-DSNOW_SHOT_SCROLLING_PERF_DETAIL=$detail"
    if ($LASTEXITCODE -ne 0) { throw "Scrolling benchmark configuration failed." }
    & cmake --build --preset build-windows-msvc-performance --target `
        snow-shot-scrolling-image-performance-benchmark --parallel
    if ($LASTEXITCODE -ne 0) { throw "Scrolling benchmark build failed." }

    if ([string]::IsNullOrWhiteSpace($Image)) {
        $Image = Join-Path $repoRoot "snow_shot/test-imgs/scrollscreenshot-test.png"
    }
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        $mode = if ($DetailedTiming) { "detailed" } else { "coarse" }
        $OutputDirectory = Join-Path $repoRoot "build/windows-msvc-performance/scrolling-image-performance/$mode"
    }
    $env:QT_QPA_PLATFORM = "offscreen"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $env:QTDIR "plugins/platforms"
    $runtime = Join-Path $repoRoot ".tools/vcpkg/installed/dynamic/bin"
    $qtBin = Join-Path $env:QTDIR "bin"
    $env:PATH = "$qtBin;$runtime;$env:PATH"
    $executable = Join-Path $repoRoot `
        "build/windows-msvc-performance/snow_shot/test-bin/Release/snow-shot-scrolling-image-performance-benchmark.exe"
    $arguments = @(
        "--image", $Image, "--viewport-height", $ViewportHeight.ToString(),
        "--step-px", $StepPx.ToString(),
        "--scroll-fps", $ScrollFps.ToString([Globalization.CultureInfo]::InvariantCulture),
        "--max-steps", $MaxSteps.ToString(), "--output-dir", $OutputDirectory
    )
    & $executable @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Scrolling benchmark failed. Inspect $OutputDirectory/results.json."
    }
}
finally {
    Pop-Location
    $currentEnvironment = [Environment]::GetEnvironmentVariables("Process")
    foreach ($key in $currentEnvironment.Keys) {
        if (-not $previousEnvironment.Contains($key)) {
            [Environment]::SetEnvironmentVariable($key, $null, "Process")
        }
    }
    foreach ($key in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($key, $previousEnvironment[$key], "Process")
    }
}

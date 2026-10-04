#requires -Version 7.2
param(
    [string]$QtBin = "",
    [string]$OutputDirectory = "",
    [string]$Scenario = "*",
    [int]$Warmups = 6,
    [int]$Samples = 25,
    [int]$CycleSweeps = 12,
    [int]$ColdSamples = 8,
    [int]$CountdownSeconds = 3,
    [switch]$ListScenarios,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

# One-command runner for the screenshot toolbar display benchmark
# (creation -> first displayed frame, and per-drawing-tool sub-toolbar display).
# Configures the matching performance preset, builds only the benchmark
# target, stamps environment metadata, and runs it with the native windows QPA.

$ErrorActionPreference = "Stop"

$shot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$workspace = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$performanceTarget = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
$performanceBuildDirectory = Join-Path $workspace "build/$($performanceTarget.Preset)"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputDirectory = Join-Path $shot "build/toolbar-display-perf/$(if ($Architecture -eq 'arm64') { 'windows-arm64/' })$stamp"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

if ($Warmups -lt 0 -or $Samples -le 0 -or $CycleSweeps -le 0 -or $ColdSamples -le 0) {
    throw "Warmups must be nonnegative; Samples, CycleSweeps, and ColdSamples must be positive"
}

Push-Location $workspace
try {
    & (Join-Path $PSScriptRoot "configure-msvc-perf.ps1") -QtBin $QtBin -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) {
        throw "The $($performanceTarget.Preset) configuration failed"
    }

    & cmake --build $performanceBuildDirectory --config Release --target `
        snow-shot-screenshot-toolbar-display-benchmark --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "The toolbar display benchmark target failed to build"
    }

    $release = Join-Path $performanceBuildDirectory "snow_shot/Release"
    $executable = Join-Path $release "snow-shot-screenshot-toolbar-display-benchmark.exe"
    if (!(Test-Path $executable)) {
        throw "Benchmark executable was not produced: $executable"
    }

    Assert-SnowPerformanceExecutable -Path $executable -Architecture $Architecture
    $qtRuntime = Set-SnowPerformanceQtRuntime -Architecture $Architecture
    $savedCommit = $env:SNOW_SHOT_PERF_GIT_COMMIT
    $savedGpuDriver = $env:SNOW_SHOT_PERF_GPU_DRIVER
    $savedPowerPlan = $env:SNOW_SHOT_PERF_POWER_PLAN
    try {
        $env:SNOW_SHOT_PERF_GIT_COMMIT = (& git rev-parse HEAD).Trim()
    }
    catch {
        $env:SNOW_SHOT_PERF_GIT_COMMIT = "unavailable"
    }
    try {
        $gpuDrivers = Get-CimInstance Win32_VideoController |
            ForEach-Object { "$($_.Name):$($_.DriverVersion)" }
        $env:SNOW_SHOT_PERF_GPU_DRIVER = $gpuDrivers -join " | "
    }
    catch {
        $env:SNOW_SHOT_PERF_GPU_DRIVER = "unavailable"
    }
    try {
        $env:SNOW_SHOT_PERF_POWER_PLAN = (& powercfg /getactivescheme) -join " "
    }
    catch {
        $env:SNOW_SHOT_PERF_POWER_PLAN = "unavailable"
    }

    try {
        $arguments = @(
            "--output", $OutputDirectory,
            "--scenario", $Scenario,
            "--warmups", $Warmups.ToString(),
            "--samples", $Samples.ToString(),
            "--cycle-sweeps", $CycleSweeps.ToString(),
            "--cold-samples", $ColdSamples.ToString()
        )
        if ($ListScenarios) { $arguments += "--list-scenarios" }

        if (!$ListScenarios) {
            Write-Warning "This benchmark creates always-on-top toolbar windows and parks the mouse pointer for about a minute; leave the workstation idle until it finishes"
            for ($remaining = $CountdownSeconds; $remaining -gt 0; --$remaining) {
                Write-Host "Starting toolbar display benchmark in $remaining"
                Start-Sleep -Seconds 1
            }
        }

        New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
        & $executable @arguments
        $benchmarkExitCode = $LASTEXITCODE
    }
    finally {
        Restore-SnowPerformanceQtRuntime -Snapshot $qtRuntime
        $env:SNOW_SHOT_PERF_GIT_COMMIT = $savedCommit
        $env:SNOW_SHOT_PERF_GPU_DRIVER = $savedGpuDriver
        $env:SNOW_SHOT_PERF_POWER_PLAN = $savedPowerPlan
    }
}
finally {
    Pop-Location
}

if (!$ListScenarios) {
    Write-Host "Toolbar display JSON: $(Join-Path $OutputDirectory 'report.json')"
}
exit $benchmarkExitCode

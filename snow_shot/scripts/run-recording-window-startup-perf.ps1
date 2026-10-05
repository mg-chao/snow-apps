#requires -Version 7.2
[CmdletBinding()]
param(
    [int]$Warmups = 3,
    [int]$Samples = 15,
    [string]$JsonReport = "",
    [string[]]$ExtraArguments = @(),
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$previousEnvironment = [Environment]::GetEnvironmentVariables("Process")
Push-Location $repoRoot
try {
    . (Join-Path $PSScriptRoot "performance-environment.ps1")
    $performanceTarget = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
    $performanceBuildDirectory = Join-Path $repoRoot "build/$($performanceTarget.Preset)"
    Initialize-SnowPerformanceEnvironment -Architecture $Architecture
    $configureArguments = Get-SnowConfigureArguments -Preset $performanceTarget.Preset -BuildDirectory $performanceBuildDirectory
    & cmake @configureArguments
    if ($LASTEXITCODE -ne 0) { throw "Recording window startup benchmark configuration failed." }
    & cmake --build --preset "build-$($performanceTarget.Preset)" --target `
        snow-shot-screen-recording-controller-tests --parallel
    if ($LASTEXITCODE -ne 0) { throw "Recording window startup benchmark build failed." }

    if ([string]::IsNullOrWhiteSpace($JsonReport)) {
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $JsonReport = Join-Path $repoRoot "build/recording-window-startup-perf/$(if ($Architecture -eq 'arm64') { 'windows-arm64/' })report-$stamp.json"
    }
    $JsonReport = [System.IO.Path]::GetFullPath($JsonReport)
    New-Item -ItemType Directory -Force -Path (Split-Path $JsonReport -Parent) | Out-Null

    $null = Set-SnowPerformanceQtRuntime -Platform offscreen -Architecture $Architecture
    $env:QT_QPA_FONTDIR = Join-Path $repoRoot "test-support/fonts"
    $executable = Join-Path $repoRoot `
        "build/$($performanceTarget.Preset)/snow_shot/test-bin/Release/snow-shot-screen-recording-controller-tests.exe"
    $arguments = @(
        "--recording-window-startup-performance",
        "--warmups", $Warmups.ToString(),
        "--samples", $Samples.ToString(),
        "--json", $JsonReport
    ) + $ExtraArguments
    Assert-SnowPerformanceExecutable -Path $executable -Architecture $Architecture
    & $executable @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Recording window startup benchmark failed. Inspect $JsonReport."
    }
    Write-Host "Recording window startup performance JSON: $JsonReport"
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

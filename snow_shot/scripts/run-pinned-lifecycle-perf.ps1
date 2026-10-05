#requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateRange(5, 200)]
    [int]$Samples = 40,
    [string]$OutputPath = "",
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
$workspace = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$performanceTarget = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
$performanceBuildDirectory = Join-Path $workspace "build/$($performanceTarget.Preset)"
Initialize-SnowPerformanceEnvironment -Architecture $Architecture
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = $(if ($Architecture -eq "arm64") { Join-Path $performanceBuildDirectory "pinned-lifecycle-performance.json" } else { Join-Path $workspace "build/pinned-lifecycle-performance.json" })
}
$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null

Push-Location $workspace
try {
    & (Join-Path $PSScriptRoot "configure-msvc-perf.ps1") -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) { throw "Performance configuration failed" }
    & cmake --build --preset "build-$($performanceTarget.Preset)" --target snow-shot-file-pin-batch-performance-benchmark --parallel
    if ($LASTEXITCODE -ne 0) { throw "Lifecycle benchmark build failed" }
    $benchmark = Join-Path $performanceBuildDirectory "snow_shot/Release/snow-shot-file-pin-batch-performance-benchmark.exe"
    Assert-SnowPerformanceExecutable -Path $benchmark -Architecture $Architecture
    $qtRuntime = Set-SnowPerformanceQtRuntime -Architecture $Architecture
    try {
        & $benchmark --lifecycle --samples $Samples --output $OutputPath -platform windows
        if ($LASTEXITCODE -ne 0) { throw "Lifecycle benchmark failed" }
    } finally { Restore-SnowPerformanceQtRuntime -Snapshot $qtRuntime }
    Write-Host "Lifecycle performance report: $OutputPath"
}
finally {
    Pop-Location
}

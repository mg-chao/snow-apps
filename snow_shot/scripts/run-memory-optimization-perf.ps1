#requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateRange(2, 100)][int]$Pairs = 20,
    [ValidateRange(1, 10)][int]$Warmups = 2,
    [string]$OutputPath = "",
    [ValidateSet("offscreen", "windows")][string]$Platform = "offscreen",
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$workspace = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$performanceTarget = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
$performanceBuildDirectory = Join-Path $workspace "build/$($performanceTarget.Preset)"
Initialize-SnowPerformanceEnvironment -Architecture $Architecture
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $performanceBuildDirectory "memory-optimization-performance.json"
}
$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null

Push-Location $workspace
try {
    if (!$SkipBuild) {
        & (Join-Path $PSScriptRoot "configure-msvc-perf.ps1") -Architecture $Architecture
        if ($LASTEXITCODE -ne 0) { throw "Performance configuration failed" }
        & cmake --build --preset "build-$($performanceTarget.Preset)" --target `
            snow-shot-memory-optimization-native-smoke-tests `
            snow-shot-memory-optimization-performance-benchmark --parallel
        if ($LASTEXITCODE -ne 0) { throw "Memory optimization fixture build failed" }
    }
    $benchmarkName = "snow-shot-memory-optimization-performance-benchmark.exe"
    $smokeName = "snow-shot-memory-optimization-native-smoke-tests.exe"
    $benchmark = Get-ChildItem -LiteralPath $performanceBuildDirectory -Recurse -File -Filter $benchmarkName |
        Where-Object { $_.Directory.Name -eq "Release" } | Select-Object -First 1 -ExpandProperty FullName
    $smoke = Get-ChildItem -LiteralPath $performanceBuildDirectory -Recurse -File -Filter $smokeName |
        Where-Object { $_.Directory.Name -eq "Release" } | Select-Object -First 1 -ExpandProperty FullName
    if (!$benchmark -or !$smoke) { throw "Expected Release memory optimization fixtures were not produced" }
    Assert-SnowPerformanceExecutable -Path $benchmark -Architecture $Architecture
    Assert-SnowPerformanceExecutable -Path $smoke -Architecture $Architecture
    $qtRuntime = Set-SnowPerformanceQtRuntime -Platform $Platform -Architecture $Architecture
    try {
        & $smoke
        if ($LASTEXITCODE -ne 0) { throw "The isolated native memory smoke test failed" }
        & $benchmark --pairs $Pairs --warmups $Warmups --output $OutputPath -platform $Platform
        if ($LASTEXITCODE -ne 0) { throw "The controlled memory optimization benchmark failed" }
    }
    finally { Restore-SnowPerformanceQtRuntime -Snapshot $qtRuntime }
    Write-Host "Memory optimization performance report: $OutputPath"
}
finally { Pop-Location }

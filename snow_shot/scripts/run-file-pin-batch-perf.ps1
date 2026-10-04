#requires -Version 7.2
[CmdletBinding()]
param(
    [int]$Samples = 5,
    [switch]$Fresh,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
$shot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$workspace = (Resolve-Path (Join-Path $shot "..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$performanceTarget = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
$performanceBuildDirectory = Join-Path $workspace "build/$($performanceTarget.Preset)"

& (Join-Path $shot "scripts/configure-msvc-perf.ps1") -Fresh:$Fresh -Architecture $Architecture
if ($LASTEXITCODE -ne 0) { throw "The performance configuration failed" }
& cmake --build ($performanceBuildDirectory) --config Release --target `
    snow-shot-file-pin-batch-performance-benchmark --parallel
if ($LASTEXITCODE -ne 0) { throw "The file-pin batch benchmark build failed" }

$release = Join-Path $performanceBuildDirectory "snow_shot/test-bin/Release"
$benchmark = Join-Path $release "snow-shot-file-pin-batch-performance-benchmark.exe"
if (!(Test-Path $benchmark)) {
    $benchmark = (Get-ChildItem -Path ($performanceBuildDirectory) -Recurse -Filter "snow-shot-file-pin-batch-performance-benchmark.exe" | Select-Object -First 1).FullName
}
if (!(Test-Path $benchmark)) { throw "Expected benchmark binary was not produced" }

Assert-SnowPerformanceExecutable -Path $benchmark -Architecture $Architecture
$qtRuntime = Set-SnowPerformanceQtRuntime -Architecture $Architecture
$exitCode = 1
try {
    & $benchmark @("--samples", $Samples.ToString()); $exitCode = $LASTEXITCODE
}
finally {
    Restore-SnowPerformanceQtRuntime -Snapshot $qtRuntime
}
exit $exitCode

[CmdletBinding()]
param(
    [int]$Samples = 5,
    [switch]$Fresh
)

$ErrorActionPreference = "Stop"
$shot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$workspace = (Resolve-Path (Join-Path $shot "..")).Path

$configureArguments = @()
if ($Fresh) { $configureArguments += "-Fresh" }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $shot "scripts\configure-msvc-perf.ps1") @configureArguments
if ($LASTEXITCODE -ne 0) { throw "The performance configuration failed" }
& cmake --build (Join-Path $workspace "build\windows-msvc-performance") --config Release --target `
    snow-shot-file-pin-batch-performance-benchmark --parallel
if ($LASTEXITCODE -ne 0) { throw "The file-pin batch benchmark build failed" }

$release = Join-Path $workspace "build\windows-msvc-performance\snow_shot\test-bin\Release"
$benchmark = Join-Path $release "snow-shot-file-pin-batch-performance-benchmark.exe"
if (!(Test-Path $benchmark)) {
    $benchmark = (Get-ChildItem -Path (Join-Path $workspace "build\windows-msvc-performance") -Recurse -Filter "snow-shot-file-pin-batch-performance-benchmark.exe" | Select-Object -First 1).FullName
}
if (!(Test-Path $benchmark)) { throw "Expected benchmark binary was not produced" }

$savedPlatform = $env:QT_QPA_PLATFORM
$exitCode = 1
try {
    $env:QT_QPA_PLATFORM = "windows"
    & $benchmark @("--samples", $Samples.ToString()); $exitCode = $LASTEXITCODE
}
finally {
    $env:QT_QPA_PLATFORM = $savedPlatform
}
exit $exitCode

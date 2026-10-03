[CmdletBinding()]
param(
    [ValidateRange(5, 200)]
    [int]$Samples = 40,
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
$workspace = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
. (Join-Path $workspace "scripts/snow-build-environment.ps1")
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $workspace "build/pinned-lifecycle-performance.json"
}
$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null

Push-Location $workspace
try {
    & cmake --preset windows-msvc-performance
    if ($LASTEXITCODE -ne 0) { throw "Performance configuration failed" }
    & cmake --build --preset build-windows-msvc-performance --target snow-shot-file-pin-batch-performance-benchmark --parallel
    if ($LASTEXITCODE -ne 0) { throw "Lifecycle benchmark build failed" }
    $benchmark = Join-Path $workspace "build/windows-msvc-performance/snow_shot/Release/snow-shot-file-pin-batch-performance-benchmark.exe"
    & $benchmark --lifecycle --samples $Samples --output $OutputPath -platform windows
    if ($LASTEXITCODE -ne 0) { throw "Lifecycle benchmark failed" }
    Write-Host "Lifecycle performance report: $OutputPath"
}
finally {
    Pop-Location
}

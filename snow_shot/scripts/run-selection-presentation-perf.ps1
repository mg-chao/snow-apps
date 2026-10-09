#requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateRange(1, 100000)][int]$Iterations = 240,
    [ValidateRange(1, 10000)][int]$Warmup = 30,
    [ValidateRange(81, 16384)][int]$Width = 1800,
    [ValidateRange(81, 16384)][int]$Height = 975,
    [string]$Scenario = "all",
    [string]$OutputDirectory = "",
    [switch]$Native,
    [switch]$SkipBuild,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
$workspace = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$target = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
Initialize-SnowPerformanceEnvironment -Architecture $Architecture
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $workspace "build/selection-presentation-perf/$(Get-Date -Format yyyyMMdd-HHmmss)"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

Push-Location $workspace
try {
    if (!$SkipBuild) {
        & cmake --preset $target.Preset
        if ($LASTEXITCODE -ne 0) { throw "Performance configuration failed" }
        & cmake --build (Join-Path $workspace "build/$($target.Preset)") --config Release `
            --target snow-shot-selection-presentation-benchmark --parallel
        if ($LASTEXITCODE -ne 0) { throw "Presentation benchmark build failed" }
    }
    $executable = Join-Path $workspace "build/$($target.Preset)/snow_shot/test-bin/Release/snow-shot-selection-presentation-benchmark.exe"
    Assert-SnowPerformanceExecutable -Path $executable -Architecture $Architecture
    $platform = if ($Native) { "windows" } else { "offscreen" }
    $qtRuntime = Set-SnowPerformanceQtRuntime -Platform $platform -Architecture $Architecture
    try {
        $arguments = @("--iterations", "$Iterations", "--warmup", "$Warmup",
            "--width", "$Width", "--height", "$Height", "--scenario", $Scenario,
            "--output", (Join-Path $OutputDirectory "report.json"))
        $arguments += if ($Native) { "--native" } else { "--offscreen" }
        & $executable @arguments
        if ($LASTEXITCODE -ne 0) { throw "Presentation benchmark failed" }
    }
    finally { Restore-SnowPerformanceQtRuntime -Snapshot $qtRuntime }
}
finally { Pop-Location }

[CmdletBinding()]
param(
    [int]$Warmups = 3,
    [int]$Samples = 15,
    [string]$JsonReport = "",
    [string[]]$ExtraArguments = @()
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$previousEnvironment = [Environment]::GetEnvironmentVariables("Process")
Push-Location $repoRoot
try {
    . (Join-Path $repoRoot "scripts\snow-build-environment.ps1")
    Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
    & cmake --preset windows-msvc-performance
    if ($LASTEXITCODE -ne 0) { throw "Recording window startup benchmark configuration failed." }
    & cmake --build --preset build-windows-msvc-performance --target `
        snow-shot-screen-recording-controller-tests --parallel
    if ($LASTEXITCODE -ne 0) { throw "Recording window startup benchmark build failed." }

    if ([string]::IsNullOrWhiteSpace($JsonReport)) {
        $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $JsonReport = Join-Path $repoRoot "build\recording-window-startup-perf\report-$stamp.json"
    }
    $JsonReport = [System.IO.Path]::GetFullPath($JsonReport)
    New-Item -ItemType Directory -Force -Path (Split-Path $JsonReport -Parent) | Out-Null

    $env:QT_QPA_PLATFORM = "offscreen"
    # A static Qt links the offscreen plugin into the executable and has no
    # plugins\platforms directory; pointing QT_QPA_PLATFORM_PLUGIN_PATH at the
    # missing directory makes the platform plugin fail to load. This mirrors
    # snow_shot_offscreen_qpa_environment() in cmake/SnowQtOffscreenTest.cmake.
    $platformPlugins = Join-Path $env:QTDIR "plugins\platforms"
    if (Test-Path $platformPlugins) {
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = $platformPlugins
    }
    $runtime = Join-Path $repoRoot ".tools/vcpkg/installed/dynamic/bin"
    $qtBin = Join-Path $env:QTDIR "bin"
    $env:PATH = "$qtBin;$runtime;$env:PATH"
    $executable = Join-Path $repoRoot `
        "build/windows-msvc-performance/snow_shot/test-bin/Release/snow-shot-screen-recording-controller-tests.exe"
    $arguments = @(
        "--recording-window-startup-performance",
        "--warmups", $Warmups.ToString(),
        "--samples", $Samples.ToString(),
        "--json", $JsonReport
    ) + $ExtraArguments
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

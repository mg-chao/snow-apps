#requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateRange(1, 20)][int]$Samples = 7,
    [string]$OutputDirectory = "",
    [ValidateSet("windows", "offscreen")][string]$Platform = "windows",
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
    [switch]$SkipBuild,
    [switch]$RequireForwardOnly
)

$ErrorActionPreference = "Stop"
$workspace = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$target = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
Initialize-SnowPerformanceEnvironment -Architecture $Architecture
$buildDirectory = Join-Path $workspace "build/$($target.Preset)"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $buildDirectory "shell-image-launch-performance"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Push-Location $workspace
try {
    if (!$SkipBuild) {
        & cmake --build --preset "build-$($target.Preset)" --target snow_shot `
            snow-shot-shell-launch-performance-benchmark `
            snow-shot-file-pin-batch-performance-benchmark --parallel
        if ($LASTEXITCODE -ne 0) { throw "Shell launch benchmark build failed" }
    }
    $binaryDirectory = Join-Path $buildDirectory "snow_shot/Release"
    $application = Join-Path $binaryDirectory "snow_shot.exe"
    $launcher = Join-Path $binaryDirectory "snow-shot-shell-launch-performance-benchmark.exe"
    $files = Join-Path $binaryDirectory "snow-shot-file-pin-batch-performance-benchmark.exe"
    foreach ($executable in @($application, $launcher, $files)) {
        Assert-SnowPerformanceExecutable -Path $executable -Architecture $Architecture
    }
    $runtime = Set-SnowPerformanceQtRuntime -Platform $Platform -Architecture $Architecture
    try {
        $arguments = @("--app", $application, "--output", (Join-Path $OutputDirectory "launch.json"),
            "--samples", "$Samples")
        if ($Platform -eq "offscreen") { $arguments += "--offscreen" }
        if ($RequireForwardOnly) { $arguments += "--require-forward-only" }
        & $launcher @arguments
        if ($LASTEXITCODE -ne 0) { throw "Shell launch benchmark failed" }
        & $files --shell-queue --samples $Samples -platform $Platform |
            Tee-Object -FilePath (Join-Path $OutputDirectory "file-presentation.txt")
        if ($LASTEXITCODE -ne 0) { throw "File presentation benchmark failed" }
    }
    finally { Restore-SnowPerformanceQtRuntime -Snapshot $runtime }
}
finally { Pop-Location }

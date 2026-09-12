[CmdletBinding()]
param(
    # Comma-separated compile-time metric groups; trim to measure one group in
    # isolation without the instrumentation overhead of the others.
    [string]$Metrics = "bench-stage-timing,bench-compositor-timing,bench-pipeline-timing",
    [int]$DurationSeconds = 12,
    [int]$WarmupSeconds = 2,
    [int]$Samples = 1,
    [string]$Scenario = "",
    [string]$Backend = "auto",
    [int]$Fps = 60,
    [string]$Clarity = "1080p",
    [switch]$PreferHardware,
    [string]$OutputDirectory = "",
    [ValidateSet("continuous", "static", "sparse")][string]$Workload = "continuous",
    [switch]$Audio,
    [ValidateRange(0, 64)][int]$EncodeThreads = 0,
    [ValidateRange(0, 4)][int]$ResizeThreads = 0,
    [switch]$AlignCapture,
    [switch]$FreeRunningCapture,
    [switch]$BuildOnly,
    [string]$Executable = ""
)

$ErrorActionPreference = "Stop"
if ($AlignCapture -and $FreeRunningCapture) { throw 'Choose one capture pacing override.' }
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$env:FFMPEG_DIR = Join-Path $repoRoot ".tools/vcpkg/installed/dynamic/x64-windows"
$env:VCPKGRS_TRIPLET = "x64-windows"
$env:VCPKGRS_DYNAMIC = "1"
$env:Path = "$(Join-Path $env:FFMPEG_DIR 'bin');$env:Path"

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot ("build\realtime-recording-perf\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

$benchmarkArguments = @(
    "--duration-seconds", $DurationSeconds,
    "--warmup-seconds", $WarmupSeconds,
    "--samples", $Samples,
    "--backend", $Backend,
    "--fps", $Fps,
    "--clarity", $Clarity,
    "--workload", $Workload,
    "--output", $OutputDirectory
)
if (-not [string]::IsNullOrWhiteSpace($Scenario)) {
    $benchmarkArguments += @("--scenario", $Scenario)
}
if ($Audio) { $benchmarkArguments += "--audio" }
if ($EncodeThreads -gt 0) { $benchmarkArguments += @("--encode-threads", $EncodeThreads) }
if ($ResizeThreads -gt 0) { $benchmarkArguments += @("--resize-threads", $ResizeThreads) }
if ($AlignCapture) { $benchmarkArguments += "--align-capture" }
if ($FreeRunningCapture) { $benchmarkArguments += "--free-running-capture" }
if ($PreferHardware) {
    $benchmarkArguments += "--prefer-hardware"
}

$env:SNOW_BENCH_REVISION = (& git -C $repoRoot rev-parse HEAD).Trim()
# Synthetic overlay input is always required: the benchmark never injects OS
# mouse or keyboard events, so the example target only builds with this feature.
$features = @($Metrics -split ',' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$features += 'bench-synthetic-input'
$features = @($features | Select-Object -Unique)
$featureArguments = @("--features", ($features -join ','))
Push-Location (Join-Path $repoRoot "snow-crates")
try {
    if ([string]::IsNullOrWhiteSpace($Executable)) {
        & cargo build -p snow-recording-runtime --release --target x86_64-pc-windows-msvc `
            --target-dir (Join-Path $repoRoot "build/windows-msvc-performance/cargo") `
            @featureArguments --example realtime_recording_benchmark
        if ($LASTEXITCODE -ne 0) { throw "Realtime recording benchmark build failed." }
        $Executable = Join-Path $repoRoot "build/windows-msvc-performance/cargo/x86_64-pc-windows-msvc/release/examples/realtime_recording_benchmark.exe"
    }
    if ($BuildOnly) { Write-Output $Executable; return }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    [ordered]@{
        schema_version = 1
        executable = [System.IO.Path]::GetFullPath($Executable)
        executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
        preset = 'windows-msvc-performance'
        arguments = $benchmarkArguments
    } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $OutputDirectory 'build-identity.json')
    Write-Host "Realtime recording benchmark report: $OutputDirectory"
    Write-Warning "The benchmark covers the leftmost monitor with a fullscreen workload window. It never injects mouse or keyboard input."
    for ($remaining = 5; $remaining -ge 1; $remaining--) {
        Write-Host "Starting in $remaining..."
        Start-Sleep -Seconds 1
    }
    & $Executable @benchmarkArguments
    if ($LASTEXITCODE -ne 0) { throw "Realtime recording benchmark failed." }
}
finally { Pop-Location }

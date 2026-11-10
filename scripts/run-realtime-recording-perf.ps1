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
    [ValidateSet("h264", "h265")][string]$Codec = "h264",
    [string]$OutputDirectory = "",
    [ValidateSet("continuous", "static", "sparse")][string]$Workload = "continuous",
    [switch]$Audio,
    [ValidateRange(0, 64)][int]$EncodeThreads = 0,
    [ValidateRange(0, 4)][int]$ResizeThreads = 0,
    [switch]$AlignCapture,
    [switch]$FreeRunningCapture,
    [ValidateRange(0, 4)][int]$ConversionThreads = 0,
    [switch]$ForceHardwareFailure,
    [switch]$AsyncEncoder,
    [switch]$AutomaticPolicies,
    [switch]$PartialComposition,
    [switch]$RestorationOnly,
    [switch]$HalfResize,
    [switch]$DirectOutput,
    [switch]$SkipUnneededCursor,
    [string]$RegionSize = "",
    [ValidateSet("off", "stationary", "pan", "boundary")][string]$FollowMouse = "off",
    [switch]$BuildOnly,
    [string]$Executable = "",
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
if ($AlignCapture -and $FreeRunningCapture) { throw 'Choose one capture pacing override.' }
. (Join-Path $PSScriptRoot "../snow_shot/scripts/performance-environment.ps1")
$target = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative:(-not $BuildOnly)
Set-SnowBuildEnvironment -Preset $target.Preset | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$env:FFMPEG_DIR = Join-Path $target.InstalledRoot $target.Triplet
$env:VCPKGRS_TRIPLET = $target.Triplet
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
    "--codec", $Codec,
    "--fps", $Fps,
    "--clarity", $Clarity,
    "--workload", $Workload,
    "--output", $OutputDirectory
)
if (-not [string]::IsNullOrWhiteSpace($Scenario)) {
    $benchmarkArguments += @("--scenario", $Scenario)
}
if ($Audio) { $benchmarkArguments += "--audio" }
if ($PSBoundParameters.ContainsKey('EncodeThreads')) { $benchmarkArguments += @("--encode-threads", $EncodeThreads) }
if ($PSBoundParameters.ContainsKey('ResizeThreads')) { $benchmarkArguments += @("--resize-threads", $ResizeThreads) }
if ($AlignCapture) { $benchmarkArguments += "--align-capture" }
if ($FreeRunningCapture) { $benchmarkArguments += "--free-running-capture" }
if ($ConversionThreads -gt 0) { $benchmarkArguments += @("--conversion-threads", $ConversionThreads) }
if ($ForceHardwareFailure) { $benchmarkArguments += "--force-hardware-failure" }
if ($AsyncEncoder) { $benchmarkArguments += "--async-encoder" }
if ($AutomaticPolicies) { $benchmarkArguments += "--automatic-policies" }
if ($PartialComposition) { $benchmarkArguments += "--partial-composition" }
if ($RestorationOnly) { $benchmarkArguments += "--restoration-only" }
if ($HalfResize) { $benchmarkArguments += "--half-resize" }
if ($DirectOutput) { $benchmarkArguments += "--direct-output" }
if ($SkipUnneededCursor) { $benchmarkArguments += "--skip-unneeded-cursor" }
if ($RegionSize) { $benchmarkArguments += @("--region-size", $RegionSize) }
if ($FollowMouse -ne 'off') { $benchmarkArguments += @('--follow-mouse', $FollowMouse) }
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
        & cargo build -p snow-recording-runtime --release --target $target.RustTarget `
            --target-dir (Join-Path $repoRoot "build/$($target.Preset)/cargo") `
            @featureArguments --example realtime_recording_benchmark
        if ($LASTEXITCODE -ne 0) { throw "Realtime recording benchmark build failed." }
        $Executable = Join-Path $repoRoot "build/$($target.Preset)/cargo/$($target.RustTarget)/release/examples/realtime_recording_benchmark.exe"
    }
    if ($BuildOnly) { Write-Output $Executable; return }
    Assert-SnowPerformanceExecutable -Path $Executable -Architecture $Architecture
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    [ordered]@{
        schema_version = 1
        executable = [System.IO.Path]::GetFullPath($Executable)
        executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
        preset = $target.Preset
        architecture = $target.Architecture
        arguments = $benchmarkArguments
    } | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $OutputDirectory 'build-identity.json')
    Write-Host "Realtime recording benchmark report: $OutputDirectory"
    if ($FollowMouse -eq 'boundary') {
        Write-Warning "The boundary benchmark covers every monitor with a workload window. It uses synthetic desktop positions and never injects OS input."
    } else {
        Write-Warning "The benchmark covers the leftmost monitor with a workload window. It never injects mouse or keyboard input."
    }
    for ($remaining = 5; $remaining -ge 1; $remaining--) {
        Write-Host "Starting in $remaining..."
        Start-Sleep -Seconds 1
    }
    & $Executable @benchmarkArguments
    if ($LASTEXITCODE -ne 0) { throw "Realtime recording benchmark failed." }
}
finally { Pop-Location }

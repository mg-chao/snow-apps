[CmdletBinding()]
param(
    [int]$Seconds = 12,
    [int]$Fps = 60,
    [string]$OutputDirectory = '',
    [switch]$BuildOnly,
    [switch]$SkipCpu
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$env:FFMPEG_DIR = Join-Path $repoRoot '.tools/vcpkg/installed/dynamic/x64-windows'
$env:VCPKGRS_TRIPLET = 'x64-windows'
$env:VCPKGRS_DYNAMIC = '1'
$env:Path = "$(Join-Path $env:FFMPEG_DIR 'bin');$env:Path"

& cargo build --manifest-path "$repoRoot/snow-crates/Cargo.toml" --release --target x86_64-pc-windows-msvc `
    --target-dir "$repoRoot/build/windows-msvc-performance/cargo" -p snow-recording-runtime `
    --features recording-benchmark --example recording_gpu_benchmark
if ($LASTEXITCODE -ne 0) { throw 'GPU benchmark build failed' }
$benchmark = "$repoRoot/build/windows-msvc-performance/cargo/x86_64-pc-windows-msvc/release/examples/recording_gpu_benchmark.exe"
if ($BuildOnly) { Write-Output $benchmark; return }

if (-not $OutputDirectory) {
    $OutputDirectory = "$repoRoot/build/windows-msvc-performance/recording-gpu-$(Get-Date -Format yyyyMMdd-HHmmss)"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path

@{
    revision = (& git -C $repoRoot rev-parse HEAD)
    preset = 'windows-msvc-performance'; profile = 'release'
    seconds = $Seconds; fps = $Fps
} | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'environment.json')

Write-Output "=== GPU zero-copy lane ($Seconds s) ==="
& $benchmark --seconds $Seconds --fps $Fps --output $OutputDirectory |
    Tee-Object -FilePath (Join-Path $OutputDirectory 'gpu.csv')
if ($LASTEXITCODE -ne 0) { throw 'GPU benchmark run failed' }

if (-not $SkipCpu) {
    Write-Output "=== CPU lane, same harness ($Seconds s) ==="
    & $benchmark --cpu --seconds $Seconds --fps $Fps --output $OutputDirectory |
        Tee-Object -FilePath (Join-Path $OutputDirectory 'cpu.csv')
    if ($LASTEXITCODE -ne 0) { throw 'CPU benchmark run failed' }
}

Write-Output "Artifacts: $OutputDirectory"

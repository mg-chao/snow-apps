[CmdletBinding()]
param(
    [string]$OutputDirectory = "",
    [ValidateRange(1, 100)][int]$Samples = 5,
    [ValidateRange(2, 3600)][int]$Frames = 60,
    [ValidateRange(1, 120)][int]$LiveSeconds = 12,
    [switch]$SkipLive
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "build/windows-msvc-performance/recording-perf-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
}
$OutputDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$env:FFMPEG_DIR = Join-Path $repoRoot '.tools/vcpkg/installed/dynamic/x64-windows'
$env:VCPKGRS_TRIPLET = 'x64-windows'
$env:VCPKGRS_DYNAMIC = '1'
$env:Path = "$(Join-Path $env:FFMPEG_DIR 'bin');$env:Path"
$targetDirectory = Join-Path $repoRoot 'build/windows-msvc-performance/cargo'
Push-Location $repoRoot
try {
    & cargo build --manifest-path snow-crates/Cargo.toml --release `
        --target x86_64-pc-windows-msvc --target-dir $targetDirectory `
        -p snow-capture -p snow-recording-export -p snow-recording-runtime `
        --features snow-capture/stage-timing `
        --example recording_export_benchmark --example streaming_copy_benchmark `
        --example recording_resize_benchmark --example recording_conversion_benchmark
    if ($LASTEXITCODE -ne 0) { throw 'Recording benchmark build failed.' }
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $examples = Join-Path $targetDirectory 'x86_64-pc-windows-msvc/release/examples'
    $runs = @(
        @{ Name = 'resize'; Executable = 'recording_resize_benchmark'; Arguments = @((Join-Path $OutputDirectory 'resize')) },
        @{ Name = 'conversion'; Executable = 'recording_conversion_benchmark'; Arguments = @((Join-Path $OutputDirectory 'conversion')) },
        @{ Name = 'software'; Executable = 'streaming_copy_benchmark'; Arguments = @((Join-Path $OutputDirectory 'software'), $Samples, $Frames) },
        @{ Name = 'hardware'; Executable = 'streaming_copy_benchmark'; Arguments = @((Join-Path $OutputDirectory 'hardware'), $Samples, $Frames, '--hardware') }
    )
    if (-not $SkipLive) {
        foreach ($backend in @('wgc', 'dxgi')) {
            $runs += @{ Name = "live-$backend"; Executable = 'recording_export_benchmark'; Arguments = @('--backend', $backend, '--seconds', $LiveSeconds, '--output', (Join-Path $OutputDirectory "live-$backend.mp4")) }
        }
    }
    foreach ($run in $runs) {
        $runArguments = $run.Arguments
        & (Join-Path $examples "$($run.Executable).exe") @runArguments 2>&1 |
            Tee-Object -FilePath (Join-Path $OutputDirectory "$($run.Name).log")
        if ($LASTEXITCODE -ne 0) { throw "Recording benchmark $($run.Name) failed." }
    }
    Write-Host "Recording benchmark results: $OutputDirectory"
}
finally { Pop-Location }

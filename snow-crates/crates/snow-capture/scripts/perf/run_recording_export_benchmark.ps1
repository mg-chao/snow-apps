param(
    [ValidateSet('dxgi','wgc','gdi')][string]$Backend = 'wgc',
    [int]$Seconds = 10,
    [int]$Fps = 60,
    [string]$Output = 'build/windows-msvc-performance/recording-export.mp4',
    [switch]$Software
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..\..')).Path
. (Join-Path $root 'scripts/snow-build-environment.ps1')
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
$env:FFMPEG_DIR = Join-Path $root '.tools/vcpkg/installed/dynamic/x64-windows'
$env:VCPKGRS_TRIPLET = 'x64-windows'
$env:VCPKGRS_DYNAMIC = '1'
$env:Path = "$(Join-Path $env:FFMPEG_DIR 'bin');$env:Path"
Push-Location $root
try {
    $benchmarkArgs = @('run','--manifest-path','snow-crates/Cargo.toml','-p','snow-capture','--release','--features','stage-timing','--target','x86_64-pc-windows-msvc','--target-dir','build/windows-msvc-performance/cargo','--example','recording_export_benchmark','--','--backend',$Backend,'--seconds',$Seconds,'--fps',$Fps,'--output',$Output)
    if ($Software) { $benchmarkArgs += '--software' }
    & cargo @benchmarkArgs
    if ($LASTEXITCODE -ne 0) { throw "recording/export benchmark failed ($LASTEXITCODE)" }
} finally { Pop-Location }

[CmdletBinding()]
param(
    [switch]$Fix
)

$ErrorActionPreference = "Stop"
$workspaceRoot = Split-Path -Parent $PSScriptRoot

# cargo check/clippy execute the ffmpeg-sys-next build script, which locates
# FFmpeg through FFMPEG_DIR, vcpkg, or pkg-config. The CMake build always sets
# FFMPEG_DIR explicitly; when linting standalone, fall back to the
# repository-managed vcpkg installation so the documented command works without
# a configured build tree.
if ([string]::IsNullOrWhiteSpace($env:FFMPEG_DIR)) {
    $ffmpegCandidates = @(
        ".tools\vcpkg\installed\dynamic\x64-windows",
        ".tools\vcpkg\installed\static\x64-windows",
        ".tools\vcpkg\installed\static\x64-windows-static"
    )
    $ffmpegRoot = $ffmpegCandidates |
        ForEach-Object { Join-Path $workspaceRoot $_ } |
        Where-Object { Test-Path (Join-Path $_ "include\libavutil") } |
        Select-Object -First 1
    if ($ffmpegRoot) {
        $env:FFMPEG_DIR = $ffmpegRoot
    }
}

$rustWorkspaces = @(
    (Join-Path $workspaceRoot "snow-crates"),
    (Join-Path $workspaceRoot "snow_draw_engine_qt")
)

foreach ($rustWorkspace in $rustWorkspaces) {
    Push-Location $rustWorkspace
    try {
        if ($Fix) {
            cargo fmt --all
        } else {
            cargo fmt --all -- --check
        }
        if ($LASTEXITCODE -ne 0) {
            throw "rustfmt failed in $rustWorkspace"
        }

        cargo check --workspace --all-targets --all-features
        if ($LASTEXITCODE -ne 0) {
            throw "cargo check failed in $rustWorkspace"
        }

        cargo clippy --workspace --all-targets --all-features -- -D warnings
        if ($LASTEXITCODE -ne 0) {
            throw "Clippy failed in $rustWorkspace"
        }
    } finally {
        Pop-Location
    }
}

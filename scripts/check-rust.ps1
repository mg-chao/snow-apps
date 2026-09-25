[CmdletBinding()]
param(
    [switch]$Fix,
    [string[]]$Workspace = @(),
    [string[]]$Package = @()
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
    (Join-Path $workspaceRoot "snow_draw_engine_qt"),
    (Join-Path $workspaceRoot "snow_shot\rust\snow-shot-updater"),
    (Join-Path $workspaceRoot "snow_shot\rust\snow-shot-mcp")
)

if ($Workspace.Count -gt 0) {
    $rustWorkspaces = @($Workspace | ForEach-Object { (Resolve-Path (Join-Path $workspaceRoot $_)).Path })
}
$cargoSelection = @('--workspace')
if ($Package.Count -gt 0) {
    $cargoSelection = @()
    foreach ($name in $Package) { $cargoSelection += @('-p', $name) }
}
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

        cargo check @cargoSelection --all-targets --all-features
        if ($LASTEXITCODE -ne 0) {
            throw "cargo check failed in $rustWorkspace"
        }

        cargo clippy @cargoSelection --all-targets --all-features -- -D warnings
        if ($LASTEXITCODE -ne 0) {
            throw "Clippy failed in $rustWorkspace"
        }
    } finally {
        Pop-Location
    }
}

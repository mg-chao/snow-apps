[CmdletBinding()]
param(
    [switch]$Fix
)

$ErrorActionPreference = "Stop"
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$projects = @(
    "ant_design_qt",
    "snow_draw_engine_qt",
    "snow_image",
    "snow_image_viewer",
    "snow_shot",
    "snow-crates/crates/snow-macos"
)
$extensions = @(".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm")
$files = foreach ($project in $projects) {
    git -C $workspaceRoot ls-files --cached --others --exclude-standard $project |
        Where-Object { $extensions -contains [System.IO.Path]::GetExtension($_).ToLowerInvariant() } |
        ForEach-Object { Join-Path $workspaceRoot $_ }
}

if ($LASTEXITCODE -ne 0) {
    throw "Unable to enumerate tracked C and C++ files."
}

foreach ($file in $files) {
    if ($Fix) {
        clang-format -i --style=file $file
    } else {
        clang-format --dry-run --Werror --style=file $file
    }
    if ($LASTEXITCODE -ne 0) {
        throw "clang-format failed for $file"
    }
}

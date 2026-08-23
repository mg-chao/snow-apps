[CmdletBinding()]
param(
    [ValidateSet("windows-msvc-debug", "windows-msvc-performance")]
    [string]$Preset = "windows-msvc-debug",
    [switch]$Clean,
    [switch]$NoBuild,
    [switch]$Detached
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDirectory = Join-Path $repoRoot "build/$Preset"
if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Preset $Preset -Target snow_image_viewer -Clean:$Clean
    if ($LASTEXITCODE -ne 0) { throw "Snow Image Viewer build failed." }
}
$configuration = if ($Preset -eq "windows-msvc-debug") { "Debug" } else { "Release" }
$executable = Get-ChildItem -LiteralPath $buildDirectory -Filter "snow_image_viewer.exe" -File -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\$configuration\\" } | Select-Object -First 1
if (-not $executable) { throw "snow_image_viewer.exe was not found under $buildDirectory." }
if ($Detached) { Start-Process -FilePath $executable.FullName -WorkingDirectory $executable.DirectoryName | Out-Null }
else { Push-Location $executable.DirectoryName; try { & $executable.FullName } finally { Pop-Location } }

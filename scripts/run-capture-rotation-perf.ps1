[CmdletBinding()]
param([switch]$Hdr)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location (Join-Path $repoRoot "snow-crates")
try {
    $benchmark = if ($Hdr) { "hdr_rotation_performance_benchmark" } else { "rotation::tests::rotation_performance_benchmark" }
    & cargo test -p snow-capture --release --target x86_64-pc-windows-msvc `
        --target-dir (Join-Path $repoRoot "build/windows-msvc-performance/cargo") `
        --lib $benchmark -- --ignored --nocapture
    if ($LASTEXITCODE -ne 0) {
        throw "Capture rotation benchmark failed."
    }
}
finally {
    Pop-Location
}

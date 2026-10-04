#requires -Version 7.2
[CmdletBinding()]
param([switch]$Fresh, [string]$QtBin = "",
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64")

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$target = Get-SnowPerformanceTarget -Architecture $Architecture
Initialize-SnowPerformanceEnvironment -QtBin $QtBin -Architecture $Architecture

Push-Location $repoRoot
try {
    $buildDirectory = Join-Path $repoRoot "build/$($target.Preset)"
    $arguments = @(Get-SnowConfigureArguments -Preset $target.Preset `
        -BuildDirectory $buildDirectory)
    if ($Fresh -and $arguments -notcontains "--fresh") {
        $arguments = @("--fresh") + $arguments
    }
    & cmake @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "The $($target.Preset) configuration failed."
    }
}
finally {
    Pop-Location
}

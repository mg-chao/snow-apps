#requires -Version 7.2
[CmdletBinding()]
param([switch]$Fresh, [string]$QtBin = "")

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
Initialize-SnowPerformanceEnvironment -QtBin $QtBin

Push-Location $repoRoot
try {
    $buildDirectory = Join-Path $repoRoot "build/windows-msvc-performance"
    $arguments = @(Get-SnowConfigureArguments -Preset "windows-msvc-performance" `
        -BuildDirectory $buildDirectory)
    if ($Fresh -and $arguments -notcontains "--fresh") {
        $arguments = @("--fresh") + $arguments
    }
    & cmake @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "The windows-msvc-performance configuration failed."
    }
}
finally {
    Pop-Location
}

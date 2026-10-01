# Focused syntax/preview contracts. Never builds or connects to an SSH host.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
foreach ($name in @('publish-snow-shot-release.ps1', 'package-snow-shot-remote-macos.ps1',
    'publish-snow-shot-release.local.example.ps1')) {
    $tokens = $null
    $errors = $null
    $null = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw ($errors | Out-String) }
}
$publisher = Join-Path $PSScriptRoot 'publish-snow-shot-release.ps1'
$version = [regex]::Match((Get-Content -Raw (Join-Path (Split-Path -Parent $PSScriptRoot) 'CMakeLists.txt')), 'set\(SNOW_SHOT_VERSION "([^"]+)"\)').Groups[1].Value
$settings = @{ WhatIf = $true }
$windows = @(& $publisher @settings)
if ($windows.Count -ne 26 -or $windows -contains "snow-shot-$version-macos-arm64.dmg") { throw 'Windows preview changed unexpectedly.' }
$combined = @(& $publisher @settings -MacHost 'mac.invalid' -MacUser 'test' -MacProjectDirectory '/Users/test/snow-apps')
if ($combined.Count -ne 31 -or $combined[-1] -cne 'install-snow-shot-macos.sh') { throw 'Combined preview has the wrong file order/count.' }
foreach ($name in @("snow-shot-$version-macos-arm64.dmg", "snow-shot-$version-macos-arm64.dmg.sha256", "snow-shot-mini-$version-macos-arm64.dmg", "snow-shot-mini-$version-macos-arm64.dmg.sha256", 'install-snow-shot-macos.sh')) {
    if ($combined -cnotcontains $name) { throw "Missing macOS artifact: $name" }
}
try {
    & $publisher @settings -MacHost 'mac.invalid'
    throw 'Expected incomplete Mac settings to be rejected.'
} catch {
    if ($_.Exception.Message -cne 'MacProjectDirectory is required with MacHost.') { throw }
}
Write-Output 'PASS: PowerShell syntax, Windows/combined previews, metadata order, incomplete Mac configuration.'

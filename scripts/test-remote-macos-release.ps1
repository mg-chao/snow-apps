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
$settings = @{ ServerHost = 'production.invalid'; PublicBaseUrl = 'https://production.invalid'; WhatIf = $true }
$windows = @(& $publisher @settings)
if ($windows.Count -ne 8 -or $windows -contains 'setup/snow-shot_macos-arm64.dmg') { throw 'Windows preview changed unexpectedly.' }
$combined = @(& $publisher @settings -MacHost 'mac.invalid' -MacUser 'test' -MacProjectDirectory '/Users/test/snow-apps')
if ($combined.Count -ne 11 -or $combined[-1] -cne 'latest-version.txt') { throw 'Combined preview has the wrong file order/count.' }
foreach ($name in @('setup/snow-shot_macos-arm64.dmg', 'setup/snow-shot_macos-arm64.dmg.sha256', 'setup/install-snow-shot-macos.sh')) {
    if ($combined -cnotcontains $name) { throw "Missing macOS artifact: $name" }
}
try {
    & $publisher @settings -MacHost 'mac.invalid'
    throw 'Expected incomplete Mac settings to be rejected.'
} catch {
    if ($_.Exception.Message -cne 'MacProjectDirectory is required with MacHost.') { throw }
}
Write-Output 'PASS: PowerShell syntax, Windows/combined previews, metadata order, incomplete Mac configuration.'

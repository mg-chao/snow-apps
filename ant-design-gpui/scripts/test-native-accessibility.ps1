# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Opt-in Windows UI Automation smoke test. Requires an interactive desktop.
#>
[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $crateRoot
$outputRoot = Join-Path $repositoryRoot 'build/ant-design-gpui'
$statePath = Join-Path $outputRoot 'native-accessibility-state.txt'
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example foundation
        if ($LASTEXITCODE -ne 0) { throw 'Foundation example build failed.' }
    } finally { Pop-Location }
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$executable = Join-Path $outputRoot 'debug/examples/foundation.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw "Missing example: $executable" }
if (Test-Path -LiteralPath $statePath) { Remove-Item -LiteralPath $statePath }
# Native focus/UIA checks require a visible interactive window. The startup
# Hidden flag suppresses GPUI's first ShowWindow and prevents a desktop UIA root.
$process = Start-Process -FilePath $executable -ArgumentList @('--uia-fixture', ('"' + $statePath + '"')) -PassThru -WindowStyle Normal
function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Find-Probe($Root, [string]$Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Wait-Condition([scriptblock]$Check, [string]$Message) {
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $Check) { return }
        Start-Sleep -Milliseconds 25
    }
    throw $Message
}
function Invoke-Probe($Root, [string]$Id) {
    $probe = Find-Probe $Root $Id
    Require ($null -ne $probe) "Missing native probe: $Id"
    $pattern = $probe.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $pattern.Invoke()
}
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $window = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) { throw "Foundation example exited: $($process.ExitCode)" }
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $window = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        if ($null -ne $window -and $null -ne (Find-Probe $window 'foundation.primary')) { break }
        Start-Sleep -Milliseconds 100
    }
    Require ($null -ne $window) 'Native foundation window did not appear.'
    $primary = Find-Probe $window 'foundation.primary'
    $disabled = Find-Probe $window 'foundation.disabled'
    $busy = Find-Probe $window 'foundation.busy'
    Require ($null -ne $primary -and $null -ne $disabled -and $null -ne $busy) 'Semantic probes are missing.'
    Require ($primary.Current.Name -eq 'Invoke probe') 'Accessible name differs from visible intent.'
    Require ($primary.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'Probe role is not Button.'
    Require $primary.Current.IsEnabled 'Primary probe is not enabled.'
    Require $primary.Current.IsKeyboardFocusable 'Primary probe is not keyboard focusable.'
    Require (-not $disabled.Current.IsEnabled) 'Disabled metadata did not reach native UIA.'
    Require (-not $disabled.Current.IsKeyboardFocusable) 'Disabled probe remains focusable.'
    Require $busy.Current.IsKeyboardFocusable 'Busy probe lost keyboard navigation.'
    $primary.SetFocus()
    $focusDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $primary.Current.HasKeyboardFocus -and [DateTime]::UtcNow -lt $focusDeadline) { Start-Sleep -Milliseconds 50 }
    Require $primary.Current.HasKeyboardFocus 'Programmatic native focus did not reach the probe.'
    $invoke = $primary.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $invoke.Invoke()
    $invokeDeadline = [DateTime]::UtcNow.AddSeconds(5)
    $count = 0
    while ([DateTime]::UtcNow -lt $invokeDeadline) {
        if (Test-Path -LiteralPath $statePath) { $count = [int](Get-Content -LiteralPath $statePath -Raw).Trim() }
        if ($count -eq 1) { break }
        Start-Sleep -Milliseconds 50
    }
    Require ($count -eq 1) 'Native Invoke did not dispatch exactly once.'
    $pattern = $null
    Require (-not $disabled.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) 'Disabled probe advertises Invoke.'
    $pattern = $null
    Require (-not $busy.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) 'Busy probe advertises Invoke.'
    Invoke-Probe $window 'foundation.toggle-disabled'
    Wait-Condition { -not $primary.Current.IsEnabled } 'Runtime disabled state did not reach UIA.'
    Require (-not $primary.Current.IsKeyboardFocusable) 'Runtime disabled probe remains focusable.'
    Invoke-Probe $window 'foundation.toggle-disabled'
    Wait-Condition { $primary.Current.IsEnabled } 'Runtime enabled state did not recover.'
    Invoke-Probe $window 'foundation.toggle-busy'
    Wait-Condition {
        $candidate = $null
        -not $primary.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$candidate)
    } 'Runtime busy state did not remove Invoke.'
    Require $primary.Current.IsKeyboardFocusable 'Runtime busy state removed keyboard navigation.'
    Invoke-Probe $window 'foundation.toggle-busy'
    Wait-Condition {
        $candidate = $null
        $primary.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$candidate)
    } 'Runtime ready state did not restore Invoke.'
    Invoke-Probe $window 'foundation.locale'
    Wait-Condition { $primary.Current.Name -eq '触发测试' } 'Simplified Chinese accessible name did not update.'
    Invoke-Probe $window 'foundation.locale'
    Wait-Condition { $primary.Current.Name -eq '觸發測試' } 'Traditional Chinese accessible name did not update.'
    Invoke-Probe $window 'foundation.locale'
    Wait-Condition { $primary.Current.Name -eq 'Invoke probe' } 'English accessible name did not recover.'
    Require ([int](Get-Content -LiteralPath $statePath -Raw).Trim() -eq 1) 'Configuration changes spuriously invoked a control.'
    $windowPattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    $windowPattern.Close()
    Require ($process.WaitForExit(5000)) 'Native example did not exit after its window closed.'
    Require ($process.ExitCode -eq 0) 'Native example failed during normal teardown.'
    Write-Output 'Native UIA checks passed: names, roles, focus, invocation, live availability, three-locale updates, and normal teardown.'
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Opt-in native Button UI Automation regression checks. Requires an interactive Windows desktop.
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$Executable
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $crateRoot
$outputRoot = Join-Path $repositoryRoot 'build/ant-design-gpui'
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example button
        if ($LASTEXITCODE -ne 0) { throw 'Button gallery build failed.' }
    } finally { Pop-Location }
}
if (-not $Executable) { $Executable = Join-Path $outputRoot 'debug/examples/button.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "Missing Button gallery: $Executable" }
$statePath = Join-Path $outputRoot ("button-uia-{0}-{1}.txt" -f $PID, [Guid]::NewGuid().ToString('N'))
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
if (-not ('AntButtonUiAFixture' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class AntButtonUiAFixture {
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
}
'@
}

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Wait-Condition([scriptblock]$Check, [string]$Message, [int]$TimeoutMilliseconds = 5000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $Check) { return }
        Start-Sleep -Milliseconds 25
    }
    throw $Message
}
function Find-Button($Root, [string]$Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Get-Button($Root, [string]$Id) {
    $button = Find-Button $Root $Id
    Require ($null -ne $button) "Missing native Button: $Id"
    Require ($button.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) "$Id role is not Button."
    return $button
}
function Has-Invoke($Button) {
    $pattern = $null
    return $Button.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)
}
function Invoke-Button($Root, [string]$Id) {
    $button = Get-Button $Root $Id
    $pattern = $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $pattern.Invoke()
}
function Read-Count {
    if (-not (Test-Path -LiteralPath $statePath)) { return -1 }
    $value = ([string](Get-Content -LiteralPath $statePath -Raw)).Trim()
    if ($value -match '^\d+$') { return [int]$value }
    return -1
}
function Wait-Count([int]$Expected) {
    Wait-Condition { (Read-Count) -eq $Expected } "Expected $Expected activations, got $(Read-Count)."
    # Catch duplicate deliveries queued behind the first accessible/keyboard action.
    Start-Sleep -Milliseconds 120
    Require ((Read-Count) -eq $Expected) "Activation was delivered more than once; expected $Expected."
}
function Focus-Button($Button) {
    # UIA child focus updates GPUI's retained focus handle. Activate the top-level
    # native window first so keyboard input belongs to this fixture's process.
    $nativeWindow = [IntPtr]::new($window.Current.NativeWindowHandle)
    [AntButtonUiAFixture]::SetForegroundWindow($nativeWindow) | Out-Null
    Wait-Condition {
        [AntButtonUiAFixture]::GetForegroundWindow() -eq $nativeWindow
    } 'Could not activate this gallery window for native keyboard validation.'
    $Button.SetFocus()
    Wait-Condition {
        $Button.Current.HasKeyboardFocus -and
        [System.Windows.Automation.AutomationElement]::FocusedElement.Current.ProcessId -eq $process.Id
    } 'Native keyboard focus did not reach this Button gallery.'
}

# This is an explicitly interactive UIA fixture, matching test-native-accessibility.ps1.
# A hidden startup suppresses GPUI's first ShowWindow and prevents a native UIA root.
$process = Start-Process -FilePath $Executable -ArgumentList @('--fixture-state', ('"' + $statePath + '"'), '--reduced-motion') -PassThru -WindowStyle Normal
try {
    $script:buttonWindow = $null
    Wait-Condition {
        $process.Refresh()
        if ($process.HasExited) { throw "Button gallery exited early: $($process.ExitCode)" }
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $script:buttonWindow = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        $null -ne $script:buttonWindow -and $null -ne (Find-Button $script:buttonWindow 'primary')
    } 'Native Button gallery did not expose its UIA tree.' 30000
    $window = $script:buttonWindow
    Wait-Count 0
    $primary = Get-Button $window 'primary'
    Require ($primary.Current.Name -eq 'Primary') 'Primary accessible name differs from visible text.'
    Require $primary.Current.IsEnabled 'Primary should be enabled.'
    Require $primary.Current.IsKeyboardFocusable 'Primary should participate in keyboard navigation.'
    foreach ($id in @('shape-circle', 'shape-square', 'icon-search')) {
        Require ((Get-Button $window $id).Current.Name -eq 'Search') "$id lacks its accessible Search name."
    }
    Require ((Get-Button $window 'icon-only').Current.Name -eq 'More actions') 'Icon-only accessible label is missing.'
    foreach ($id in @('disabled-primary', 'disabled-default', 'disabled-dashed', 'disabled-text', 'disabled-link')) {
        $disabled = Get-Button $window $id
        Require (-not $disabled.Current.IsEnabled) "$id is enabled in native accessibility."
        Require (-not $disabled.Current.IsKeyboardFocusable) "$id remains keyboard focusable."
        Require (-not (Has-Invoke $disabled)) "$id advertises Invoke while disabled."
    }
    $loading = Get-Button $window 'loading'
    Require $loading.Current.IsKeyboardFocusable 'Loading should retain its keyboard tab stop.'
    Require (-not (Has-Invoke $loading)) 'Loading should not advertise Invoke.'

    Invoke-Button $window 'primary'
    Wait-Count 1
    Focus-Button $primary
    [System.Windows.Forms.SendKeys]::SendWait('{TAB}')
    Wait-Condition { (Get-Button $window 'default').Current.HasKeyboardFocus } 'Tab did not reach the next Button.'
    [System.Windows.Forms.SendKeys]::SendWait('+{TAB}')
    Wait-Condition { $primary.Current.HasKeyboardFocus } 'Shift+Tab did not return to Primary.'
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    Wait-Count 2
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Wait-Count 3
    # Busy controls remain focusable while both activation keys are suppressed.
    Focus-Button $loading
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Wait-Count 3

    Focus-Button $primary
    Invoke-Button $window 'gallery.disabled'
    Wait-Condition { -not $primary.Current.IsEnabled } 'Provider disabled state did not reach native Button.'
    Require (-not $primary.Current.IsKeyboardFocusable) 'Provider-disabled Button remains focusable.'
    Require (-not $primary.Current.HasKeyboardFocus) 'Provider-disabled Button kept native keyboard focus.'
    Require (-not (Has-Invoke $primary)) 'Provider-disabled Button advertises Invoke.'
    Invoke-Button $window 'gallery.disabled'
    Wait-Condition { $primary.Current.IsEnabled -and (Has-Invoke $primary) } 'Provider enable did not restore Button activation.'
    Wait-Count 3

    $startLoading = Get-Button $window 'start-loading'
    Focus-Button $startLoading
    Invoke-Button $window 'start-loading'
    Wait-Count 4
    Wait-Condition { -not (Has-Invoke $startLoading) } 'Immediate loading did not remove Invoke.'
    Require $startLoading.Current.IsKeyboardFocusable 'Immediate loading lost keyboard navigation.'
    Require $startLoading.Current.HasKeyboardFocus 'Immediate loading lost existing focus.'
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Wait-Count 4
    Wait-Condition { Has-Invoke $startLoading } 'Loading Button did not recover when work completed.'

    $delayed = Get-Button $window 'delayed-loading'
    Invoke-Button $window 'delayed-loading'
    Wait-Count 5
    Wait-Condition { -not (Has-Invoke $delayed) } 'Delayed loading did not enter its busy state.'
    Require $delayed.Current.IsKeyboardFocusable 'Delayed loading lost keyboard navigation.'
    Wait-Condition { Has-Invoke $delayed } 'Delayed loading did not recover.'
    Wait-Count 5

    Invoke-Button $window 'gallery.theme'
    Wait-Condition { (Get-Button $window 'gallery.theme').Current.Name -eq 'Light theme' } 'Dark-theme toggle did not update its accessible name.'
    $height = $primary.Current.BoundingRectangle.Height
    Invoke-Button $window 'gallery.density'
    Wait-Condition { (Get-Button $window 'gallery.density').Current.Name -eq 'Standard density' } 'Compact-density toggle did not update.'
    Wait-Condition { $primary.Current.BoundingRectangle.Height -lt $height } 'Compact density did not reduce Button height.'
    Invoke-Button $window 'gallery.direction'
    Wait-Condition { (Get-Button $window 'gallery.direction').Current.Name -eq 'Left to right' } 'RTL toggle did not update.'
    Require ($primary.Current.Name -eq 'Primary') 'Theme/density/direction changes lost the accessible name.'
    Require (Has-Invoke $primary) 'Theme/density/direction changes lost activation.'
    Invoke-Button $window 'gallery.direction'
    Invoke-Button $window 'gallery.density'
    Invoke-Button $window 'gallery.theme'
    Wait-Condition { (Get-Button $window 'gallery.theme').Current.Name -eq 'Dark theme' } 'Theme did not recover to light.'
    Wait-Count 5

    $windowPattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    $windowPattern.Close()
    Require ($process.WaitForExit(5000)) 'Button gallery did not exit after its window closed.'
    Require ($process.ExitCode -eq 0) 'Button gallery failed during normal teardown.'
    Write-Output 'Button native UIA passed: labels/roles, focus, exactly-once Invoke/Enter/Space, disabled/loading guards, delayed loading, live theme/density/RTL, and normal teardown.'
    Write-Output "Activation fixture: $statePath (expected final count: 5)"
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

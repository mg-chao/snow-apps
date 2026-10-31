# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Opt-in native Checkbox UI Automation checks. Requires an interactive Windows desktop.
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
        & cargo build --locked --offline --example gallery
        if ($LASTEXITCODE -ne 0) { throw 'Checkbox gallery build failed.' }
    } finally { Pop-Location }
}
if (-not $Executable) { $Executable = Join-Path $outputRoot 'debug/examples/gallery.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "Missing component gallery: $Executable" }

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
if (-not ('AntCheckboxUiaFixture' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AntCheckboxUiaFixture {
    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    public static IntPtr FindGalleryWindow(int processId) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != processId) return true;
            var title = new StringBuilder(256);
            GetWindowText(window, title, title.Capacity);
            if (!title.ToString().StartsWith("Ant Design GPUI")) return true;
            result = window;
            return false;
        }, IntPtr.Zero);
        return result;
    }
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
function Find-ById($Root, [string]$Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Find-ByName($Root, [string]$Name, $ControlType) {
    $condition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, $Name),
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $ControlType))
    return $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Get-Checkbox($Root, [string]$Id) {
    $checkbox = Find-ById $Root $Id
    Require ($null -ne $checkbox) "Missing native Checkbox: $Id"
    Require ($checkbox.Current.ControlType -eq [System.Windows.Automation.ControlType]::CheckBox) "$Id role is not CheckBox."
    return $checkbox
}
function Get-ToggleState($Checkbox) {
    $pattern = $Checkbox.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    return $pattern.Current.ToggleState
}
function Toggle-Checkbox($Checkbox) {
    $pattern = $Checkbox.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    $pattern.Toggle()
}
function Wait-ToggleState($Checkbox, [System.Windows.Automation.ToggleState]$Expected) {
    Wait-Condition { (Get-ToggleState $Checkbox) -eq $Expected } "Expected native toggle state $Expected for $($Checkbox.Current.Name)."
    # Detect a second queued toggle reversing the first accessible action.
    Start-Sleep -Milliseconds 120
    Require ((Get-ToggleState $Checkbox) -eq $Expected) 'Native Checkbox state changed again after one activation.'
}
function Invoke-ButtonByName($Root, [string]$Name) {
    $button = Find-ByName $Root $Name ([System.Windows.Automation.ControlType]::Button)
    Require ($null -ne $button) "Missing native Button: $Name"
    $pattern = $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $pattern.Invoke()
}
function Invoke-ButtonById($Root, [string]$Id) {
    $button = Find-ById $Root $Id
    Require ($null -ne $button) "Missing native Button: $Id"
    $pattern = $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $pattern.Invoke()
}
function Focus-Checkbox($Checkbox) {
    [AntCheckboxUiaFixture]::SetForegroundWindow($script:nativeWindow) | Out-Null
    Wait-Condition {
        [AntCheckboxUiaFixture]::GetForegroundWindow() -eq $script:nativeWindow
    } 'Could not activate the Checkbox gallery for native keyboard validation.'
    $Checkbox.SetFocus()
    Wait-Condition {
        $Checkbox.Current.HasKeyboardFocus -and
        [System.Windows.Automation.AutomationElement]::FocusedElement.Current.ProcessId -eq $process.Id
    } 'Native keyboard focus did not reach the Checkbox.'
}

$process = Start-Process -FilePath $Executable -ArgumentList @('--component', 'checkbox', '--reduced-motion') -PassThru -WindowStyle Hidden
try {
    $script:nativeWindow = [IntPtr]::Zero
    Wait-Condition {
        $process.Refresh()
        if ($process.HasExited) { throw "Checkbox gallery exited early: $($process.ExitCode)" }
        $script:nativeWindow = [AntCheckboxUiaFixture]::FindGalleryWindow($process.Id)
        $script:nativeWindow -ne [IntPtr]::Zero
    } 'Native Checkbox gallery window was not created.' 30000
    # Start hidden; this explicit interactive check reveals only its own fixture
    # after the titled GPUI window exists, so native focus and UIA can be exercised.
    [AntCheckboxUiaFixture]::ShowWindow($script:nativeWindow, 1) | Out-Null
    $script:checkboxWindow = $null
    Wait-Condition {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $script:checkboxWindow = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        $null -ne $script:checkboxWindow -and
        $null -ne (Find-ById $script:checkboxWindow 'checkbox.basic')
    } 'Native Checkbox gallery did not expose its UIA tree.' 30000
    $window = $script:checkboxWindow
    $basic = Get-Checkbox $window 'checkbox.basic'
    Require ($basic.Current.Name -eq 'Checkbox') 'Checkbox accessible name differs from its visible label.'
    Require $basic.Current.IsEnabled 'Basic Checkbox should be enabled.'
    Require $basic.Current.IsKeyboardFocusable 'Basic Checkbox should participate in keyboard navigation.'
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::Off)

    foreach ($entry in @(
        @('checkbox.disabled.unchecked', [System.Windows.Automation.ToggleState]::Off),
        @('checkbox.disabled.checked', [System.Windows.Automation.ToggleState]::On)
    )) {
        $disabled = Get-Checkbox $window $entry[0]
        Require (-not $disabled.Current.IsEnabled) "$($entry[0]) should be disabled."
        Require (-not $disabled.Current.IsKeyboardFocusable) "$($entry[0]) remains keyboard focusable."
        # TogglePattern remains available for reading native checked state.
        try { Toggle-Checkbox $disabled } catch {
            if ($_.Exception.InnerException -isnot [System.Windows.Automation.ElementNotEnabledException]) { throw }
        }
        Wait-ToggleState $disabled $entry[1]
    }

    Toggle-Checkbox $basic
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::On)
    Toggle-Checkbox $basic
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::Off)
    Focus-Checkbox $basic
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::On)
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::On)
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::Off)
    [System.Windows.Forms.SendKeys]::SendWait('{TAB}')
    Wait-Condition { (Get-Checkbox $window 'checkbox.controller').Current.HasKeyboardFocus } 'Tab did not skip disabled Checkboxes and reach the controlled Checkbox.'
    [System.Windows.Forms.SendKeys]::SendWait('+{TAB}')
    Wait-Condition { $basic.Current.HasKeyboardFocus } 'Shift+Tab did not return to Basic.'

    Invoke-ButtonByName $window 'Controlled Checkbox'
    $controlled = Get-Checkbox $window 'checkbox.controller'
    Wait-ToggleState $controlled ([System.Windows.Automation.ToggleState]::On)
    Toggle-Checkbox $controlled
    Wait-ToggleState $controlled ([System.Windows.Automation.ToggleState]::Off)
    Require ($controlled.Current.Name -eq 'Unchecked-Enabled') 'Controlled callback did not update its accessible label.'
    Focus-Checkbox $controlled
    Invoke-ButtonByName $window 'Disable'
    Wait-Condition { -not $controlled.Current.IsEnabled } 'Controlled disabled state did not reach native UIA.'
    Require (-not $controlled.Current.IsKeyboardFocusable) 'Disabled controlled Checkbox remains focusable.'
    Require (-not $controlled.Current.HasKeyboardFocus) 'Disabled controlled Checkbox kept focus.'
    Invoke-ButtonByName $window 'Enable'
    Wait-Condition { $controlled.Current.IsEnabled } 'Controlled Checkbox did not recover after enabling.'

    Invoke-ButtonByName $window 'Check all'
    $mixed = Get-Checkbox $window 'checkbox.check-all.control'
    Require ($mixed.Current.Name -eq 'Check all') 'Mixed Checkbox lost its accessible name.'
    Wait-ToggleState $mixed ([System.Windows.Automation.ToggleState]::Indeterminate)
    Toggle-Checkbox $mixed
    Wait-ToggleState $mixed ([System.Windows.Automation.ToggleState]::On)
    Toggle-Checkbox $mixed
    Wait-ToggleState $mixed ([System.Windows.Automation.ToggleState]::Off)
    $fruit = Find-ByName $window 'Select fruit' ([System.Windows.Automation.ControlType]::Group)
    Require ($null -ne $fruit) 'CheckboxGroup did not expose its native Group role and name.'
    foreach ($name in @('Apple', 'Pear', 'Orange')) {
        $option = Find-ByName $fruit $name ([System.Windows.Automation.ControlType]::CheckBox)
        Require ($null -ne $option) "Missing native group option: $name"
        Wait-ToggleState $option ([System.Windows.Automation.ToggleState]::Off)
    }

    Invoke-ButtonById $window 'gallery.control.theme'
    Invoke-ButtonById $window 'gallery.control.density'
    Invoke-ButtonById $window 'gallery.control.direction'
    Wait-ToggleState $mixed ([System.Windows.Automation.ToggleState]::Off)
    Require ($mixed.Current.Name -eq 'Check all') 'Theme/density/RTL changes lost the Checkbox name.'
    Toggle-Checkbox $mixed
    Wait-ToggleState $mixed ([System.Windows.Automation.ToggleState]::On)

    $windowPattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    $windowPattern.Close()
    Require ($process.WaitForExit(5000)) 'Checkbox gallery did not exit after its window closed.'
    Require ($process.ExitCode -eq 0) 'Checkbox gallery failed during normal teardown.'
    Write-Output 'Checkbox native UIA passed: names/roles, unchecked/checked/mixed state, TogglePattern transitions, disabled guards, Tab/Space/Enter, controlled updates, group semantics, live theme/density/RTL, and normal teardown.'
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

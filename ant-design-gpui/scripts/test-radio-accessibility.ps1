# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Opt-in native Radio UI Automation checks. Requires an interactive Windows desktop.
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
        if ($LASTEXITCODE -ne 0) { throw 'Radio gallery build failed.' }
    } finally { Pop-Location }
}
if (-not $Executable) { $Executable = Join-Path $outputRoot 'debug/examples/gallery.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "Missing component gallery: $Executable" }

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
if (-not ('AntRadioUiaFixture' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AntRadioUiaFixture {
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
function Get-Radio($Root, [string]$Id) {
    $radio = Find-ById $Root $Id
    Require ($null -ne $radio) "Missing native Radio: $Id"
    Require ($radio.Current.ControlType -eq [System.Windows.Automation.ControlType]::RadioButton) "$Id role is not RadioButton."
    return $radio
}
function Get-Selected($Radio) {
    # AccessKit maps RadioButton + checked state to SelectionItemPattern and
    # deliberately excludes TogglePattern, following the native UIA contract.
    $pattern = $Radio.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
    return $pattern.Current.IsSelected
}
function Select-Radio($Radio) {
    $pattern = $Radio.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
    $pattern.Select()
}
function Wait-Selected($Radio, [bool]$Expected) {
    Wait-Condition { (Get-Selected $Radio) -eq $Expected } "Unexpected native selection for $($Radio.Current.Name)."
    Start-Sleep -Milliseconds 120
    Require ((Get-Selected $Radio) -eq $Expected) 'Radio selection changed again after a single action.'
}
function Wait-GroupSelection($Radios, [int]$SelectedIndex) {
    Wait-Condition {
        for ($index = 0; $index -lt $Radios.Count; $index++) {
            if ((Get-Selected $Radios[$index]) -ne ($index -eq $SelectedIndex)) { return $false }
        }
        return $true
    } "The group did not select only option $SelectedIndex."
}
function Invoke-Button($Root, [string]$Value, [switch]$ById) {
    $button = if ($ById) { Find-ById $Root $Value } else {
        Find-ByName $Root $Value ([System.Windows.Automation.ControlType]::Button)
    }
    Require ($null -ne $button) "Missing native Button: $Value"
    $pattern = $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $pattern.Invoke()
}
function Focus-Radio($Radio) {
    [AntRadioUiaFixture]::SetForegroundWindow($script:nativeWindow) | Out-Null
    Wait-Condition {
        [AntRadioUiaFixture]::GetForegroundWindow() -eq $script:nativeWindow
    } 'Could not activate the Radio gallery for native keyboard validation.'
    $Radio.SetFocus()
    Wait-Condition {
        $Radio.Current.HasKeyboardFocus -and
        [System.Windows.Automation.AutomationElement]::FocusedElement.Current.ProcessId -eq $process.Id
    } 'Native keyboard focus did not reach the Radio.'
}

$process = Start-Process -FilePath $Executable -ArgumentList @('--component', 'radio', '--reduced-motion') -PassThru -WindowStyle Hidden
try {
    $script:nativeWindow = [IntPtr]::Zero
    Wait-Condition {
        $process.Refresh()
        if ($process.HasExited) { throw "Radio gallery exited early: $($process.ExitCode)" }
        $script:nativeWindow = [AntRadioUiaFixture]::FindGalleryWindow($process.Id)
        $script:nativeWindow -ne [IntPtr]::Zero
    } 'Native Radio gallery window was not created.' 30000
    # Reveal only the fixture window after hidden startup, matching Checkbox UIA.
    [AntRadioUiaFixture]::ShowWindow($script:nativeWindow, 1) | Out-Null
    $script:radioWindow = $null
    Wait-Condition {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $script:radioWindow = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        $null -ne $script:radioWindow -and $null -ne (Find-ById $script:radioWindow 'radio.basic')
    } 'Native Radio gallery did not expose its UIA tree.' 30000
    $window = $script:radioWindow
    $basic = Get-Radio $window 'radio.basic'
    Require ($basic.Current.Name -eq 'Radio') 'Radio accessible name differs from its label.'
    Require $basic.Current.IsEnabled 'Basic Radio should be enabled.'
    Require $basic.Current.IsKeyboardFocusable 'Basic Radio should be keyboard focusable.'
    Wait-Selected $basic $false

    foreach ($entry in @(@('radio.disabled.empty', $false), @('radio.disabled.checked', $true))) {
        $disabled = Get-Radio $window $entry[0]
        Require (-not $disabled.Current.IsEnabled) "$($entry[0]) should be disabled."
        Require (-not $disabled.Current.IsKeyboardFocusable) "$($entry[0]) remains keyboard focusable."
        try { Select-Radio $disabled } catch {
            $exception = $_.Exception
            while ($exception.InnerException) { $exception = $exception.InnerException }
            if ($exception -isnot [System.Windows.Automation.ElementNotEnabledException]) { throw }
        }
        Wait-Selected $disabled $entry[1]
    }

    Select-Radio $basic
    Wait-Selected $basic $true
    Select-Radio $basic
    Wait-Selected $basic $true
    Focus-Radio $basic
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Wait-Selected $basic $true
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    Wait-Selected $basic $true

    Invoke-Button $window 'Toggle disabled'
    $availability = Get-Radio $window 'radio.disabled.empty'
    Wait-Condition { $availability.Current.IsEnabled } 'Radio did not recover after enabling.'
    Select-Radio $availability
    Wait-Selected $availability $true
    Focus-Radio $availability
    Invoke-Button $window 'Toggle disabled'
    Wait-Condition { -not $availability.Current.IsEnabled } 'Disabled state did not reach UIA.'
    Require (-not $availability.Current.IsKeyboardFocusable) 'Disabled Radio remains keyboard focusable.'
    Require (-not $availability.Current.HasKeyboardFocus) 'Disabled Radio retained keyboard focus.'

    Invoke-Button $window 'Radio Group'
    $group = Find-ByName $window 'Choose an option' ([System.Windows.Automation.ControlType]::Group)
    Require ($null -ne $group) 'RadioGroup did not expose its native Group role and name.'
    $options = @(1..4 | ForEach-Object { Get-Radio $group "radio.controlled.$_" })
    Wait-GroupSelection $options 0
    Select-Radio $options[1]
    Wait-GroupSelection $options 1
    Focus-Radio $options[1]
    [System.Windows.Forms.SendKeys]::SendWait('{RIGHT}')
    Wait-GroupSelection $options 2
    Require $options[2].Current.HasKeyboardFocus 'Arrow selection did not move keyboard focus.'
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Wait-GroupSelection $options 2
    [System.Windows.Forms.SendKeys]::SendWait('{DOWN}{RIGHT}')
    Wait-GroupSelection $options 0
    [System.Windows.Forms.SendKeys]::SendWait('{LEFT}')
    Wait-GroupSelection $options 3

    Focus-Radio $options[2]
    Wait-GroupSelection $options 3
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    Wait-GroupSelection $options 3
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Wait-GroupSelection $options 2
    [System.Windows.Forms.SendKeys]::SendWait('{TAB}')
    Wait-Condition { -not $options[2].Current.HasKeyboardFocus } 'Tab did not leave the RadioGroup.'
    $focusedId = [System.Windows.Automation.AutomationElement]::FocusedElement.Current.AutomationId
    Require ($focusedId -notin @('radio.controlled.1', 'radio.controlled.2', 'radio.controlled.4')) 'Tab entered a non-selected option in the same RadioGroup.'
    [System.Windows.Forms.SendKeys]::SendWait('+{TAB}')
    Wait-Condition { $options[2].Current.HasKeyboardFocus } 'Shift+Tab did not return to the selected option.'

    Invoke-Button $window 'gallery.control.theme' -ById
    Invoke-Button $window 'gallery.control.density' -ById
    Invoke-Button $window 'gallery.control.direction' -ById
    Wait-GroupSelection $options 2
    Focus-Radio $options[2]
    [System.Windows.Forms.SendKeys]::SendWait('{LEFT}')
    Wait-GroupSelection $options 3
    Require ($options[3].Current.Name -eq 'D') 'Theme/density/RTL changes lost the Radio label.'

    $windowPattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    $windowPattern.Close()
    Require ($process.WaitForExit(5000)) 'Radio gallery did not exit after its window closed.'
    Require ($process.ExitCode -eq 0) 'Radio gallery failed during normal teardown.'
    Write-Output 'Radio native UIA passed: roles/names, SelectionItemPattern, exclusive selection, disabled guards, Space/Enter, roving Tab, arrow wrapping, live theme/density/RTL, and normal teardown.'
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

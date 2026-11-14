# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Opt-in native Segmented UI Automation checks. Requires an interactive Windows desktop.
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
        if ($LASTEXITCODE -ne 0) { throw 'Segmented gallery build failed.' }
    } finally { Pop-Location }
}
if (-not $Executable) { $Executable = Join-Path $outputRoot 'debug/examples/gallery.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "Missing component gallery: $Executable" }

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
if (-not ('AntSegmentedUiaFixture' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AntSegmentedUiaFixture {
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
function Get-Group($Root, [string]$Id, [string]$Name) {
    $group = Find-ById $Root $Id
    Require ($null -ne $group) "Missing native Segmented group: $Id"
    Require ($group.Current.ControlType -eq [System.Windows.Automation.ControlType]::Group) "$Id role is not Group."
    Require ($group.Current.Name -eq $Name) "$Id lost its accessible group name."
    return $group
}
function Get-Option($Group, [string]$Name) {
    $option = Find-ByName $Group $Name ([System.Windows.Automation.ControlType]::RadioButton)
    Require ($null -ne $option) "Missing RadioButton named $Name in $($Group.Current.Name)."
    return $option
}
function Get-RadioOptions($Group) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::RadioButton)
    return $Group.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Get-IsSelected($Option) {
    $pattern = $Option.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
    return $pattern.Current.IsSelected
}
function Select-Option($Option) {
    $pattern = $Option.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
    $pattern.Select()
}
function Require-Selection($Group, [string]$Name) {
    $options = @(Get-RadioOptions $Group)
    Require ($options.Count -gt 0) "No radio options in $($Group.Current.Name)."
    $selected = @($options | Where-Object { Get-IsSelected $_ })
    Require ($selected.Count -eq 1) "$($Group.Current.Name) must expose exactly one selected radio."
    Require ($selected[0].Current.Name -eq $Name) "Expected $Name in $($Group.Current.Name); got $($selected[0].Current.Name)."
    $selectionPattern = $Group.GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern)
    $groupSelection = @($selectionPattern.Current.GetSelection())
    Require ($groupSelection.Count -eq 1 -and $groupSelection[0].Current.Name -eq $Name) 'Radio group selection disagrees with its selected option.'
}
function Wait-Selection($Group, [string]$Name) {
    Wait-Condition { Get-IsSelected (Get-Option $Group $Name) } "Expected selected radio $Name in $($Group.Current.Name)."
    # Allow queued native actions and controlled callbacks to settle; selecting
    # an already selected radio must never deselect it or select a second item.
    Start-Sleep -Milliseconds 120
    Require-Selection $Group $Name
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
function Focus-Option($Option) {
    [AntSegmentedUiaFixture]::SetForegroundWindow($script:nativeWindow) | Out-Null
    Wait-Condition {
        [AntSegmentedUiaFixture]::GetForegroundWindow() -eq $script:nativeWindow
    } 'Could not activate the Segmented gallery for native keyboard validation.'
    $Option.SetFocus()
    Wait-Condition {
        $Option.Current.HasKeyboardFocus -and
        [System.Windows.Automation.AutomationElement]::FocusedElement.Current.ProcessId -eq $process.Id
    } 'Native keyboard focus did not reach the Segmented option.'
}
function Wait-Focus($Option, [string]$Message) {
    Wait-Condition { $Option.Current.HasKeyboardFocus } $Message
}
function Navigate-Demo($Root, [string]$Title, [string]$GroupId, [string]$GroupName) {
    Invoke-ButtonByName $Root $Title
    Wait-Condition {
        $group = Find-ById $Root $GroupId
        $null -ne $group -and -not $group.Current.IsOffscreen
    } "The $Title example did not reveal its Segmented group."
    return Get-Group $Root $GroupId $GroupName
}

$process = Start-Process -FilePath $Executable -ArgumentList @('--component', 'segmented', '--reduced-motion') -PassThru -WindowStyle Hidden
try {
    $script:nativeWindow = [IntPtr]::Zero
    Wait-Condition {
        $process.Refresh()
        if ($process.HasExited) { throw "Segmented gallery exited early: $($process.ExitCode)" }
        $script:nativeWindow = [AntSegmentedUiaFixture]::FindGalleryWindow($process.Id)
        $script:nativeWindow -ne [IntPtr]::Zero
    } 'Native Segmented gallery window was not created.' 30000
    # This opt-in fixture needs native focus. Reveal only its own titled GPUI
    # window after hidden startup, matching the Checkbox accessibility fixture.
    [AntSegmentedUiaFixture]::ShowWindow($script:nativeWindow, 1) | Out-Null
    $script:segmentedWindow = $null
    Wait-Condition {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $script:segmentedWindow = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        $null -ne $script:segmentedWindow -and
        $null -ne (Find-ById $script:segmentedWindow 'segmented.basic')
    } 'Native Segmented gallery did not expose its UIA tree.' 30000
    $window = $script:segmentedWindow
    $basic = Get-Group $window 'segmented.basic' 'basic'
    $periods = @('Daily', 'Weekly', 'Monthly', 'Quarterly', 'Yearly')
    Require (@(Get-RadioOptions $basic).Count -eq $periods.Count) 'Basic lost a native radio option.'
    foreach ($name in $periods) {
        $option = Get-Option $basic $name
        Require $option.Current.IsEnabled "$name should be enabled."
        Require $option.Current.IsKeyboardFocusable "$name should permit programmatic and arrow-key focus."
    }
    Wait-Selection $basic 'Daily'
    $weekly = Get-Option $basic 'Weekly'
    Select-Option $weekly
    Wait-Selection $basic 'Weekly'
    Select-Option $weekly
    Wait-Selection $basic 'Weekly'
    Focus-Option $weekly
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Wait-Selection $basic 'Weekly'
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    Wait-Selection $basic 'Weekly'
    [System.Windows.Forms.SendKeys]::SendWait('{RIGHT}')
    Wait-Selection $basic 'Monthly'
    Wait-Focus (Get-Option $basic 'Monthly') 'Right arrow changed selection without moving focus.'
    [System.Windows.Forms.SendKeys]::SendWait('{HOME}')
    Wait-Selection $basic 'Daily'
    [System.Windows.Forms.SendKeys]::SendWait('{LEFT}')
    Wait-Selection $basic 'Yearly'
    [System.Windows.Forms.SendKeys]::SendWait('{RIGHT}')
    Wait-Selection $basic 'Daily'
    [System.Windows.Forms.SendKeys]::SendWait('{END}')
    Wait-Selection $basic 'Yearly'

    # Only the selected option is a Tab stop. Tab must leave Basic directly for
    # the following vertical group's selected radio, and reverse traversal must
    # return to the currently selected Basic option.
    [System.Windows.Forms.SendKeys]::SendWait('{TAB}')
    $vertical = Get-Group $window 'segmented.vertical' 'vertical'
    Wait-Focus (Get-Option $vertical 'List') 'Tab stopped inside Basic instead of entering the next group.'
    [System.Windows.Forms.SendKeys]::SendWait('+{TAB}')
    Wait-Focus (Get-Option $basic 'Yearly') 'Shift+Tab did not return to the selected Basic option.'
    Focus-Option (Get-Option $vertical 'List')
    [System.Windows.Forms.SendKeys]::SendWait('{DOWN}')
    Wait-Selection $vertical 'Kanban'
    [System.Windows.Forms.SendKeys]::SendWait('{DOWN}')
    Wait-Selection $vertical 'List'

    $disabled = Navigate-Demo $window 'Disabled' 'segmented.disabled.all' 'disabled all'
    Require (-not $disabled.Current.IsEnabled) 'Disabled group should be disabled in UIA.'
    Wait-Selection $disabled 'Map'
    foreach ($name in @('Map', 'Transit', 'Satellite')) {
        $option = Get-Option $disabled $name
        Require (-not $option.Current.IsEnabled) "$name remains enabled in the disabled group."
        Require (-not $option.Current.IsKeyboardFocusable) "$name remains focusable in the disabled group."
        try { Select-Option $option } catch {
            if ($_.Exception.InnerException -isnot [System.Windows.Automation.ElementNotEnabledException]) { throw }
        }
        Wait-Selection $disabled 'Map'
    }
    $partial = Get-Group $window 'segmented.disabled.options' 'disabled options'
    foreach ($name in @('Weekly', 'Quarterly')) {
        $option = Get-Option $partial $name
        Require (-not $option.Current.IsEnabled) "$name should be individually disabled."
        Require (-not $option.Current.IsKeyboardFocusable) "$name should leave native keyboard navigation."
        try { Select-Option $option } catch {
            if ($_.Exception.InnerException -isnot [System.Windows.Automation.ElementNotEnabledException]) { throw }
        }
        Wait-Selection $partial 'Daily'
    }
    Focus-Option (Get-Option $partial 'Daily')
    [System.Windows.Forms.SendKeys]::SendWait('{RIGHT}')
    Wait-Selection $partial 'Monthly'
    [System.Windows.Forms.SendKeys]::SendWait('{RIGHT}')
    Wait-Selection $partial 'Yearly'

    $controlled = Navigate-Demo $window 'Controlled mode' 'segmented.controlled.group' 'controlled group'
    Wait-Selection $controlled 'Map'
    Select-Option (Get-Option $controlled 'Transit')
    Wait-Selection $controlled 'Transit'
    Select-Option (Get-Option $controlled 'Satellite')
    Wait-Selection $controlled 'Satellite'
    Invoke-ButtonById $window 'gallery.control.theme'
    Wait-Condition { (Find-ById $window 'gallery.control.theme').Current.Name -eq 'Dark' } 'Theme toggle did not update the gallery.'
    Invoke-ButtonById $window 'gallery.control.density'
    Wait-Condition { (Find-ById $window 'gallery.control.density').Current.Name -eq 'Compact' } 'Density toggle did not update the gallery.'
    Invoke-ButtonById $window 'gallery.control.direction'
    Wait-Condition { (Find-ById $window 'gallery.control.direction').Current.Name -eq 'RTL' } 'Direction toggle did not update the gallery.'
    $controlled = Get-Group $window 'segmented.controlled.group' 'controlled group'
    Wait-Selection $controlled 'Satellite'
    Focus-Option (Get-Option $controlled 'Satellite')
    [System.Windows.Forms.SendKeys]::SendWait('{RIGHT}')
    Wait-Selection $controlled 'Transit'
    [System.Windows.Forms.SendKeys]::SendWait('{LEFT}')
    Wait-Selection $controlled 'Satellite'

    $icons = Navigate-Demo $window 'With Icon only' 'segmented.icon-only' 'icon-only'
    Require (@(Get-RadioOptions $icons).Count -eq 2) 'Icon-only group lost a radio option.'
    Wait-Selection $icons 'List'
    Select-Option (Get-Option $icons 'Kanban')
    Wait-Selection $icons 'Kanban'
    $named = Navigate-Demo $window 'With name' 'segmented.with-name' 'Period choices'
    Wait-Selection $named 'Daily'
    Select-Option (Get-Option $named 'Monthly')
    Wait-Selection $named 'Monthly'

    $windowPattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    $windowPattern.Close()
    Require ($process.WaitForExit(5000)) 'Segmented gallery did not exit after its window closed.'
    Require ($process.ExitCode -eq 0) 'Segmented gallery failed during normal teardown.'
    Write-Output 'Segmented native UIA passed: named Group/RadioButton roles, SelectionItemPattern single selection, disabled guards, Space/Enter, arrows/Home/End, one-stop Tab traversal, controlled updates, icon-only names, live theme/density/RTL, and normal teardown.'
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

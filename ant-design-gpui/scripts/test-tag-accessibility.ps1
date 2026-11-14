# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Opt-in native Tag UI Automation checks. Requires an interactive Windows desktop.
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
        if ($LASTEXITCODE -ne 0) { throw 'Tag gallery build failed.' }
    } finally { Pop-Location }
}
if (-not $Executable) { $Executable = Join-Path $outputRoot 'debug/examples/gallery.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "Missing component gallery: $Executable" }

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('AntTagUiaFixture' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AntTagUiaFixture {
    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr window, int command);
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
function Get-Control($Root, [string]$Id, $ControlType) {
    $control = Find-ById $Root $Id
    Require ($null -ne $control) "Missing native Tag control: $Id"
    Require ($control.Current.ControlType -eq $ControlType) "$Id has an unexpected native role."
    return $control
}
function Invoke-Control($Control) {
    $pattern = $Control.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $pattern.Invoke()
}
function Invoke-ButtonByName($Root, [string]$Name) {
    $button = Find-ByName $Root $Name ([System.Windows.Automation.ControlType]::Button)
    Require ($null -ne $button) "Missing native navigation Button: $Name"
    Invoke-Control $button
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
    # Catch a second queued accessible activation reversing the first update.
    Start-Sleep -Milliseconds 120
    Require ((Get-ToggleState $Checkbox) -eq $Expected) 'CheckableTag changed again after one native activation.'
}
function Require-Disabled($Control, [string]$Id) {
    Require (-not $Control.Current.IsEnabled) "$Id should be disabled."
    Require (-not $Control.Current.IsKeyboardFocusable) "$Id remains keyboard focusable."
}
function Invoke-Disabled($Control) {
    $pattern = $null
    if (-not $Control.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) {
        return
    }
    try { $pattern.Invoke() } catch {
        $cause = $_.Exception
        while ($null -ne $cause.InnerException) { $cause = $cause.InnerException }
        if ($cause -isnot [System.Windows.Automation.ElementNotEnabledException]) { throw }
    }
}

$process = Start-Process -FilePath $Executable -ArgumentList @('--component', 'tag', '--reduced-motion') -PassThru -WindowStyle Hidden
try {
    $script:nativeWindow = [IntPtr]::Zero
    Wait-Condition {
        $process.Refresh()
        if ($process.HasExited) { throw "Tag gallery exited early: $($process.ExitCode)" }
        $script:nativeWindow = [AntTagUiaFixture]::FindGalleryWindow($process.Id)
        $script:nativeWindow -ne [IntPtr]::Zero
    } 'Native Tag gallery window was not created.' 30000
    # UIA needs a visible native root. Show only this fixture without activating
    # it; every interaction below uses UIA and leaves desktop keyboard focus alone.
    [AntTagUiaFixture]::ShowWindow($script:nativeWindow, 4) | Out-Null
    $script:tagWindow = $null
    Wait-Condition {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $script:tagWindow = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        $null -ne $script:tagWindow -and $null -ne (Find-ById $script:tagWindow 'tag.basic.link')
    } 'Native Tag gallery did not expose its UIA tree.' 30000
    $window = $script:tagWindow
    $link = Get-Control $window 'tag.basic.link' ([System.Windows.Automation.ControlType]::Hyperlink)
    Require ($link.Current.Name -eq 'Link') 'Tag link accessible name differs from its visible label.'
    Require $link.Current.IsEnabled 'Basic Tag link should be enabled.'
    Require $link.Current.IsKeyboardFocusable 'Basic Tag link should participate in keyboard navigation.'

    $prevent = Get-Control $window 'tag.basic.prevent.close' ([System.Windows.Automation.ControlType]::Button)
    Require ($prevent.Current.Name -eq 'Close') 'Default close button should use the English Close catalog message.'
    Require $prevent.Current.IsKeyboardFocusable 'Enabled close button should participate in keyboard navigation.'
    Invoke-Control $prevent
    Start-Sleep -Milliseconds 120
    Require ($null -ne (Find-ById $window 'tag.basic.prevent.close')) 'Prevent Default incorrectly dismissed its tag.'

    $close = Get-Control $window 'tag.basic.two.close' ([System.Windows.Automation.ControlType]::Button)
    Require ($close.Current.Name -eq 'Close') 'Custom close artwork lost its native accessible name.'
    Invoke-Control $close
    Wait-Condition { $null -eq (Find-ById $window 'tag.basic.two.close') } 'Closing Tag 2 did not remove its close control.'
    Require ($null -ne (Find-ById $window 'tag.basic.link')) 'Closing Tag 2 removed the neighboring link.'
    Require ($null -ne (Find-ById $window 'tag.basic.prevent.close')) 'Closing Tag 2 removed Prevent Default.'
    Require ($null -ne (Find-ById $window 'tag.basic.three.close')) 'Closing Tag 2 removed Tag 3.'

    Invoke-ButtonByName $window 'Checkable'
    Wait-Condition { $null -ne (Find-ById $window 'tag.checkable.yes') } 'Checkable preview did not become accessible.'
    $checkable = Get-Control $window 'tag.checkable.yes' ([System.Windows.Automation.ControlType]::CheckBox)
    Require ($checkable.Current.Name -eq 'Yes') 'CheckableTag lost its visible label.'
    Require $checkable.Current.IsEnabled 'CheckableTag should be enabled.'
    Require $checkable.Current.IsKeyboardFocusable 'CheckableTag should participate in keyboard navigation.'
    Wait-ToggleState $checkable ([System.Windows.Automation.ToggleState]::On)
    Toggle-Checkbox $checkable
    Wait-ToggleState $checkable ([System.Windows.Automation.ToggleState]::Off)
    Toggle-Checkbox $checkable
    Wait-ToggleState $checkable ([System.Windows.Automation.ToggleState]::On)

    $single = Get-Control $window 'tag.single' ([System.Windows.Automation.ControlType]::Group)
    Require ($single.Current.Name -eq 'Single category') 'Single CheckableTagGroup lost its native Group name.'
    $books = Get-Control $single 'tag.single.1' ([System.Windows.Automation.ControlType]::CheckBox)
    Wait-ToggleState $books ([System.Windows.Automation.ToggleState]::On)
    Toggle-Checkbox $books
    Wait-ToggleState $books ([System.Windows.Automation.ToggleState]::Off)
    $movies = Get-Control $single 'tag.single.0' ([System.Windows.Automation.ControlType]::CheckBox)
    Toggle-Checkbox $movies
    Wait-ToggleState $movies ([System.Windows.Automation.ToggleState]::On)
    Wait-ToggleState $books ([System.Windows.Automation.ToggleState]::Off)
    $multiple = Get-Control $window 'tag.multiple' ([System.Windows.Automation.ControlType]::Group)
    Require ($multiple.Current.Name -eq 'Multiple categories') 'Multiple CheckableTagGroup lost its native Group name.'
    foreach ($index in @(0, 2)) {
        $option = Get-Control $multiple "tag.multiple.$index" ([System.Windows.Automation.ControlType]::CheckBox)
        Wait-ToggleState $option ([System.Windows.Automation.ToggleState]::On)
    }

    foreach ($id in @('gallery.control.theme', 'gallery.control.density', 'gallery.control.direction')) {
        Invoke-Control (Get-Control $window $id ([System.Windows.Automation.ControlType]::Button))
    }
    Wait-ToggleState $checkable ([System.Windows.Automation.ToggleState]::On)
    Require ($checkable.Current.Name -eq 'Yes') 'Theme/density/RTL changes lost the CheckableTag name.'

    Invoke-ButtonByName $window 'Disabled'
    Wait-Condition { $null -ne (Find-ById $window 'tag.disabled.link') } 'Disabled preview did not become accessible.'
    $disabledLink = Get-Control $window 'tag.disabled.link' ([System.Windows.Automation.ControlType]::Hyperlink)
    Require ($disabledLink.Current.Name -eq 'Link Tag') 'Disabled link lost its native name.'
    Require-Disabled $disabledLink 'tag.disabled.link'
    $disabledClose = Get-Control $window 'tag.disabled.close.close' ([System.Windows.Automation.ControlType]::Button)
    Require ($disabledClose.Current.Name -eq 'Close') 'Disabled close button lost its catalog label.'
    Require-Disabled $disabledClose 'tag.disabled.close.close'
    Invoke-Disabled $disabledClose
    Start-Sleep -Milliseconds 120
    Require ($null -ne (Find-ById $window 'tag.disabled.close.close')) 'Disabled close action removed its tag.'
    foreach ($index in @(0, 1)) {
        $disabled = Get-Control $window "tag.disabled.checkable.$index" ([System.Windows.Automation.ControlType]::CheckBox)
        Require-Disabled $disabled "tag.disabled.checkable.$index"
        $expected = if ($index -eq 1) { [System.Windows.Automation.ToggleState]::On } else { [System.Windows.Automation.ToggleState]::Off }
        Wait-ToggleState $disabled $expected
    }

    $windowPattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    $windowPattern.Close()
    Require ($process.WaitForExit(5000)) 'Tag gallery did not exit after its window closed.'
    Require ($process.ExitCode -eq 0) 'Tag gallery failed during normal teardown.'
    Write-Output 'Tag native UIA passed: link/close/checkbox/group names and roles, controlled TogglePattern updates, isolated and prevented dismissal, disabled controls, theme/density/RTL retention, and normal teardown.'
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

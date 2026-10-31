# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Opt-in native Switch UI Automation checks. Requires an interactive Windows desktop.
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
        if ($LASTEXITCODE -ne 0) { throw 'Switch gallery build failed.' }
    } finally { Pop-Location }
}
if (-not $Executable) { $Executable = Join-Path $outputRoot 'debug/examples/gallery.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "Missing component gallery: $Executable" }

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
if (-not ('AntSwitchUiaFixture' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AntSwitchUiaFixture {
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
function Get-Switch($Root, [string]$Id) {
    $switch = Find-ById $Root $Id
    Require ($null -ne $switch) "Missing native Switch: $Id"
    Require ($switch.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) "$Id does not expose the native Button control type used for Switch."
    $togglePattern = $null
    Require ($switch.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$togglePattern)) "$Id lost its native TogglePattern."
    return $switch
}
function Get-ToggleState($Switch) {
    $pattern = $Switch.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    return $pattern.Current.ToggleState
}
function Toggle-Switch($Switch) {
    $pattern = $Switch.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    $pattern.Toggle()
}
function Wait-ToggleState($Switch, [System.Windows.Automation.ToggleState]$Expected) {
    Wait-Condition { (Get-ToggleState $Switch) -eq $Expected } "Expected native toggle state $Expected for $($Switch.Current.Name)."
    # Detect a second queued toggle reversing the first accessible action.
    Start-Sleep -Milliseconds 120
    Require ((Get-ToggleState $Switch) -eq $Expected) 'Native Switch state changed again after one activation.'
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
function Focus-Switch($Switch) {
    [AntSwitchUiaFixture]::SetForegroundWindow($script:nativeWindow) | Out-Null
    Wait-Condition {
        [AntSwitchUiaFixture]::GetForegroundWindow() -eq $script:nativeWindow
    } 'Could not activate the Switch gallery for native keyboard validation.'
    $Switch.SetFocus()
    Wait-Condition {
        $Switch.Current.HasKeyboardFocus -and
        [System.Windows.Automation.AutomationElement]::FocusedElement.Current.ProcessId -eq $process.Id
    } 'Native keyboard focus did not reach the Switch.'
}

# Role::Switch maps to a native Button control with TogglePattern in AccessKit.
# LocalizedControlType follows the Windows language and is not an English role ID.
function Check-DisabledSwitch($Switch, [System.Windows.Automation.ToggleState]$Expected) {
    Require (-not $Switch.Current.IsEnabled) "$($Switch.Current.AutomationId) should be disabled."
    Require (-not $Switch.Current.IsKeyboardFocusable) "$($Switch.Current.AutomationId) remains focusable."
    try { Toggle-Switch $Switch } catch {
        if ($_.Exception.GetBaseException() -isnot [System.Windows.Automation.ElementNotEnabledException]) { throw }
    }
    Wait-ToggleState $Switch $Expected
}

$process = Start-Process -FilePath $Executable -ArgumentList @('--component', 'switch', '--reduced-motion') -PassThru -WindowStyle Hidden
try {
    $script:nativeWindow = [IntPtr]::Zero
    Wait-Condition {
        $process.Refresh()
        if ($process.HasExited) { throw "Switch gallery exited early: $($process.ExitCode)" }
        $script:nativeWindow = [AntSwitchUiaFixture]::FindGalleryWindow($process.Id)
        $script:nativeWindow -ne [IntPtr]::Zero
    } 'Native Switch gallery window was not created.' 30000
    [AntSwitchUiaFixture]::ShowWindow($script:nativeWindow, 1) | Out-Null
    $script:switchWindow = $null
    Wait-Condition {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $script:switchWindow = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        $null -ne $script:switchWindow -and
        $null -ne (Find-ById $script:switchWindow 'switch.basic')
    } 'Native Switch gallery did not expose its UIA tree.' 30000
    $window = $script:switchWindow
    $basic = Get-Switch $window 'switch.basic'
    Require ($basic.Current.Name -eq 'Basic switch') 'Switch accessible name differs from its explicit label.'
    Require $basic.Current.IsEnabled 'Basic Switch should be enabled.'
    Require $basic.Current.IsKeyboardFocusable 'Basic Switch should be a keyboard navigation stop.'
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::On)
    Toggle-Switch $basic
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::Off)
    Toggle-Switch $basic
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::On)

    $disabled = Get-Switch $window 'switch.disabled'
    Check-DisabledSwitch $disabled ([System.Windows.Automation.ToggleState]::On)
    Focus-Switch $basic
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::Off)
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::On)
    [System.Windows.Forms.SendKeys]::SendWait('{LEFT}')
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::Off)
    [System.Windows.Forms.SendKeys]::SendWait('{LEFT}')
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::Off)
    [System.Windows.Forms.SendKeys]::SendWait('{RIGHT}')
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::On)
    [System.Windows.Forms.SendKeys]::SendWait('{TAB}')
    Wait-Condition {
        (Find-ById $window 'switch.disabled.toggle').Current.HasKeyboardFocus
    } 'Tab did not skip the disabled Switch and reach its enabled Toggle disabled button.'
    [System.Windows.Forms.SendKeys]::SendWait('+{TAB}')
    Wait-Condition { $basic.Current.HasKeyboardFocus } 'Shift+Tab did not return to Basic Switch.'

    Invoke-ButtonById $window 'switch.disabled.toggle'
    Wait-Condition { $disabled.Current.IsEnabled } 'Switch did not recover when disabled was cleared.'
    Require $disabled.Current.IsKeyboardFocusable 'Enabled Switch did not recover its keyboard stop.'
    Toggle-Switch $disabled
    Wait-ToggleState $disabled ([System.Windows.Automation.ToggleState]::Off)
    Focus-Switch $disabled
    Invoke-ButtonById $window 'switch.disabled.toggle'
    Wait-Condition { -not $disabled.Current.IsEnabled } 'Switch did not expose its live disabled state.'
    Require (-not $disabled.Current.HasKeyboardFocus) 'Switch kept keyboard focus after disabling.'
    Check-DisabledSwitch $disabled ([System.Windows.Automation.ToggleState]::Off)

    Invoke-ButtonByName $window 'Text & icon'
    foreach ($entry in @(
        @('switch.text.words', 'Text switch'),
        @('switch.text.numbers', 'Numeric switch'),
        @('switch.text.icons', 'Icon switch'),
        @('switch.text.combined', 'Mood switch')
    )) {
        $textSwitch = Get-Switch $window $entry[0]
        Require ($textSwitch.Current.Name -eq $entry[1]) "$($entry[0]) lost its stable accessible name."
        Wait-ToggleState $textSwitch ([System.Windows.Automation.ToggleState]::On)
        Toggle-Switch $textSwitch
        Wait-ToggleState $textSwitch ([System.Windows.Automation.ToggleState]::Off)
        Require ($textSwitch.Current.Name -eq $entry[1]) 'Decorative state content changed the accessible name.'
    }

    Invoke-ButtonByName $window 'Loading'
    $loading = Get-Switch $window 'switch.loading.normal'
    $smallLoading = Get-Switch $window 'switch.loading.small'
    Check-DisabledSwitch $loading ([System.Windows.Automation.ToggleState]::On)
    Check-DisabledSwitch $smallLoading ([System.Windows.Automation.ToggleState]::Off)
    Invoke-ButtonById $window 'switch.loading.toggle'
    Wait-Condition { $loading.Current.IsEnabled -and $smallLoading.Current.IsEnabled } 'Switches did not recover when loading ended.'
    Require $loading.Current.IsKeyboardFocusable 'Switch lost its keyboard stop after loading.'
    Toggle-Switch $loading
    Wait-ToggleState $loading ([System.Windows.Automation.ToggleState]::Off)
    Focus-Switch $loading
    Invoke-ButtonById $window 'switch.loading.toggle'
    Wait-Condition { -not $loading.Current.IsEnabled } 'Loading did not disable the Switch.'
    Require (-not $loading.Current.HasKeyboardFocus) 'Loading Switch kept keyboard focus.'
    Check-DisabledSwitch $loading ([System.Windows.Automation.ToggleState]::Off)

    Invoke-ButtonById $window 'gallery.control.theme'
    Invoke-ButtonById $window 'gallery.control.density'
    Invoke-ButtonById $window 'gallery.control.direction'
    Check-DisabledSwitch $loading ([System.Windows.Automation.ToggleState]::Off)
    Invoke-ButtonById $window 'gallery.control.top'
    $basic = Get-Switch $window 'switch.basic'
    Require ($basic.Current.Name -eq 'Basic switch') 'Presentation changes lost the accessible name.'
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::On)
    Toggle-Switch $basic
    Wait-ToggleState $basic ([System.Windows.Automation.ToggleState]::Off)

    $windowPattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    $windowPattern.Close()
    Require ($process.WaitForExit(5000)) 'Switch gallery did not exit after its window closed.'
    Require ($process.ExitCode -eq 0) 'Switch gallery failed during normal teardown.'
    Write-Output 'Switch native UIA passed: names/roles, TogglePattern, exactly-once changes, Space/Enter/arrows/Tab, live disabled/loading focus removal and recovery, decorative content, theme/density/RTL, and normal teardown.'
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}


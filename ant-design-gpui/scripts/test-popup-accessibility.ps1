# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<# Native popup UI Automation checks. Requires an interactive Windows desktop. #>
[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui'
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example popup_accessibility
        if ($LASTEXITCODE -ne 0) { throw 'Popup accessibility fixture build failed.' }
    } finally { Pop-Location }
}
$statePath = Join-Path $output ("popup-uia-{0}.json" -f [Guid]::NewGuid().ToString('N'))
$stderr = "$statePath.stderr.txt"
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
if (-not ('AntPopupUiA' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class AntPopupUiA {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    // The .NET Framework UIA wrapper predates FullDescription (30159). Read it
    // through the documented native COM interface, as modern screen readers do.
    public static object FocusedProperty(int id) {
        var automation = (IPopupAutomation)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        IPopupElement element = null;
        try { element = automation.GetFocusedElement(); return element.GetCurrentPropertyValue(id); }
        finally { if (element != null) Marshal.ReleaseComObject(element); Marshal.ReleaseComObject(automation); }
    }
}
[ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPopupAutomation {
    void CompareElements(); void CompareRuntimeIds(); void GetRootElement();
    void ElementFromHandle(); void ElementFromPoint();
    [return: MarshalAs(UnmanagedType.Interface)] IPopupElement GetFocusedElement();
}
[ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPopupElement {
    void SetFocus(); void GetRuntimeId(); void FindFirst(); void FindAll();
    void FindFirstBuildCache(); void FindAllBuildCache(); void BuildUpdatedCache();
    [return: MarshalAs(UnmanagedType.Struct)] object GetCurrentPropertyValue(int id);
}
'@
}
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Wait-Condition([scriptblock]$Check, [string]$Message, [int]$Timeout = 5000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Timeout)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $Check) { return }
        Start-Sleep -Milliseconds 25
    }
    throw $Message
}
function Find-Node($Root, [string]$Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Get-Node([string]$Id) {
    $node = Find-Node $script:window $Id
    Require ($null -ne $node) "Missing accessible node: $Id"
    return $node
}
function Find-Action([string]$Name) {
    $surface = Find-Node $script:window 'confirm-surface'
    if ($null -eq $surface) { return $null }
    $condition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Name),
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button))
    return $surface.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Has-Invoke($Node) {
    $pattern = $null
    return $Node.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)
}
function Invoke-Node($Node) { $Node.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
function Expand-Pattern($Node) { return $Node.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern) }
function Read-State { return Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json }
function Count([string]$Field, [int]$Expected) {
    Wait-Condition { (Read-State).$Field -eq $Expected } "Expected $Field=$Expected"
    Start-Sleep -Milliseconds 100
    Require ((Read-State).$Field -eq $Expected) "$Field delivered more than once"
}
function Focus-Node($Node) {
    $handle = [IntPtr]::new($script:window.Current.NativeWindowHandle)
    $activated = [AntPopupUiA]::SetForegroundWindow($handle)
    $Node.SetFocus()
    try { Wait-Condition { $Node.Current.HasKeyboardFocus } "Native focus did not reach '$($Node.Current.Name)' (focusable=$($Node.Current.IsKeyboardFocusable), window=$handle, foreground=$([AntPopupUiA]::GetForegroundWindow()), activated=$activated)" }
    catch { throw "$_ Native focused name='$([AntPopupUiA]::FocusedProperty(30005))'; description='$([AntPopupUiA]::FocusedProperty(30159))'" }
}
# This is an interactive native UIA test, not a background helper. Windows
# suppresses GPUI's initial ShowWindow under hidden startup, including its tree.
$process = Start-Process -FilePath (Join-Path $output 'debug/examples/popup_accessibility.exe') -ArgumentList @($statePath) -WindowStyle Normal -PassThru -RedirectStandardError $stderr
try {
    Wait-Condition {
        $condition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $script:window = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst([System.Windows.Automation.TreeScope]::Children, $condition)
        $null -ne $script:window -and $null -ne (Find-Node $script:window 'confirm-trigger')
    } 'Popup native tree did not appear' 30000
    $fixtureHandle = [IntPtr]::new($script:window.Current.NativeWindowHandle)
    [AntPopupUiA]::ShowWindow($fixtureHandle, 6) | Out-Null
    [AntPopupUiA]::ShowWindow($fixtureHandle, 9) | Out-Null
    # Complete an observable native action round trip before keyboard focus:
    # the initial UIA tree is delivered asynchronously during window creation.
    Invoke-Node (Get-Node 'toggle-disabled')
    Wait-Condition { -not (Get-Node 'popover-trigger').Current.IsEnabled } 'Initial native action was not delivered'
    Invoke-Node (Get-Node 'toggle-disabled')
    Wait-Condition { (Get-Node 'popover-trigger').Current.IsEnabled } 'Initial native action did not recover'
    $hint = Get-Node 'hint-trigger'
    Require ($hint.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'Tooltip duplicated or changed the trigger role'
    Focus-Node $hint
    $description = [AntPopupUiA]::FocusedProperty(30159)
    Require ($description -eq "Existing description`nExplains the existing control") "Tooltip did not preserve and extend FullDescription: '$description'"
    $pattern = $null
    Require (-not $hint.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$pattern)) 'Tooltip advertises a dialog expansion action'
    $popover = Get-Node 'popover-trigger'
    $expand = Expand-Pattern $popover
    Require ($expand.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Collapsed) 'Closed popover is not collapsed'
    $expand.Expand()
    Count 'trigger_clicks' 0
    Wait-Condition { $null -ne (Find-Node $script:window 'popover-surface') } 'Expand did not open controlled popover'
    Wait-Condition { (Expand-Pattern (Get-Node 'popover-trigger')).Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded } 'Expanded state did not reach trigger'
    (Expand-Pattern (Get-Node 'popover-trigger')).Collapse()
    Wait-Condition { $null -eq (Find-Node $script:window 'popover-surface') } 'Collapse left outgoing nodes alive'
    (Expand-Pattern (Get-Node 'popover-trigger')).Expand()
    Wait-Condition { $null -ne (Find-Node $script:window 'popover-surface') } 'Expand did not open popover'
    Count 'trigger_clicks' 0
    (Expand-Pattern (Get-Node 'popover-trigger')).Collapse()
    Wait-Condition { $null -eq (Find-Node $script:window 'popover-surface') } 'Popover did not collapse'
    Focus-Node (Get-Node 'popover-trigger')
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    Count 'trigger_clicks' 1
    Wait-Condition { $null -ne (Find-Node $script:window 'popover-surface') } 'Enter did not open popover'
    [System.Windows.Forms.SendKeys]::SendWait(' ')
    Count 'trigger_clicks' 2
    Wait-Condition { $null -eq (Find-Node $script:window 'popover-surface') } 'Space did not close popover'
    Invoke-Node (Get-Node 'toggle-disabled')
    Wait-Condition { -not (Get-Node 'popover-trigger').Current.IsEnabled } 'Disabled state did not reach trigger'
    Require (-not (Has-Invoke (Get-Node 'popover-trigger'))) 'Disabled popup trigger advertises Invoke'
    Invoke-Node (Get-Node 'toggle-disabled')
    Wait-Condition { (Get-Node 'popover-trigger').Current.IsEnabled } 'Trigger did not recover'
    $confirm = Get-Node 'confirm-trigger'
    Focus-Node $confirm
    (Expand-Pattern $confirm).Expand()
    Wait-Condition { $null -ne (Find-Action 'Cancel') -and (Find-Action 'Cancel').Current.HasKeyboardFocus } 'Accessible confirmation did not focus safe Cancel action'
    Require ((Get-Node 'confirm-surface').Current.Name -eq 'Delete the item?') 'Alert dialog label is missing'
    [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
    Wait-Condition { $null -eq (Find-Node $script:window 'confirm-surface') -and (Get-Node 'confirm-trigger').Current.HasKeyboardFocus } 'Escape did not remove dialog and restore trigger focus'
    Count 'cancels' 0
    # Expand without focusing the trigger also restores the actual trigger on Cancel.
    Focus-Node (Get-Node 'outside')
    (Expand-Pattern (Get-Node 'confirm-trigger')).Expand()
    Wait-Condition { $null -ne (Find-Action 'OK') } 'Confirmation did not expand'
    Invoke-Node (Find-Action 'OK')
    Count 'confirms' 1
    Wait-Condition { -not (Has-Invoke (Find-Action 'OK')) } 'Pending confirmation still advertises Invoke'
    Require (Find-Action 'OK').Current.IsKeyboardFocusable 'Pending action lost its keyboard tab stop'
    Count 'errors' 1
    Wait-Condition { Has-Invoke (Find-Action 'OK') } 'Rejected confirmation did not allow retry'
    Invoke-Node (Find-Action 'OK')
    Count 'confirms' 2
    Invoke-Node (Find-Action 'Cancel')
    Count 'cancels' 1
    Wait-Condition { $null -eq (Find-Node $script:window 'confirm-surface') -and (Get-Node 'confirm-trigger').Current.HasKeyboardFocus } 'Cancel did not restore trigger focus'
    (Expand-Pattern (Get-Node 'confirm-trigger')).Expand()
    Wait-Condition { $null -ne (Find-Action 'OK') } 'New confirmation did not open'
    Start-Sleep -Milliseconds 2200
    Require ($null -ne (Find-Action 'OK')) 'Stale async completion closed a later opening'
    Invoke-Node (Find-Action 'OK')
    Count 'confirms' 3
    Wait-Condition { $null -eq (Find-Node $script:window 'confirm-surface') } 'Successful confirmation did not close'
    $script:window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
    Require ($process.WaitForExit(5000)) 'Native fixture did not exit normally'
    Require ($process.ExitCode -eq 0) 'Native fixture failed during teardown'
    Write-Output 'Popup native UIA passed: existing identity/FullDescription, controlled Expand/Collapse, exactly-once Enter/Space callbacks, disabled controls, safe initial focus, Escape/Cancel focus return, async rejection/retry/cancellation/stale results, immediate removal, normal teardown.'
    Write-Output "Evidence: $statePath"
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}

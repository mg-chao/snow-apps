# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Opt-in native Alert UI Automation checks. Requires an interactive Windows desktop.
#>
[CmdletBinding()]
param([switch]$SkipBuild, [string]$Executable)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/alert-uia'
New-Item -ItemType Directory -Path $output -Force | Out-Null
[ordered]@{ status = 'running'; utc = [DateTime]::UtcNow.ToString('O') } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example gallery
        if ($LASTEXITCODE -ne 0) { throw 'Alert gallery build failed.' }
    } finally { Pop-Location }
}
if (-not $Executable) {
    $Executable = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/gallery.exe'
}
if (-not (Test-Path -LiteralPath $Executable)) { throw "Missing component gallery: $Executable" }
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('AntAlertUia' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AntAlertUia {
    private delegate bool Callback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr handle, StringBuilder title, int capacity);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr handle, int command);
    public static IntPtr Find(int pid) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((handle, parameter) => {
            uint owner; GetWindowThreadProcessId(handle, out owner);
            if (owner == pid) {
                var title = new StringBuilder(256); GetWindowText(handle, title, title.Capacity);
                if (title.ToString().StartsWith("Ant Design GPUI")) { result = handle; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
    // The managed UIA API omits ARIA and live-region properties. Query them
    // through the same native COM interface used by Windows screen readers.
    public static object Property(IntPtr handle, string automationId, int propertyId) {
        var automation = (IAlertAutomation)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        IAlertElement root = null, element = null; object condition = null;
        try {
            root = automation.ElementFromHandle(handle);
            condition = automation.CreatePropertyCondition(30011, automationId);
            element = root.FindFirst(4, condition);
            if (element == null) throw new InvalidOperationException("Missing native Alert node: " + automationId);
            return element.GetCurrentPropertyValue(propertyId);
        } finally {
            if (element != null) Marshal.ReleaseComObject(element);
            if (condition != null) Marshal.ReleaseComObject(condition);
            if (root != null) Marshal.ReleaseComObject(root);
            Marshal.ReleaseComObject(automation);
        }
    }
}
[ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAlertAutomation {
    void CompareElements(); void CompareRuntimeIds(); void GetRootElement();
    [return: MarshalAs(UnmanagedType.Interface)] IAlertElement ElementFromHandle(IntPtr handle);
    void ElementFromPoint(); void GetFocusedElement(); void GetRootElementBuildCache();
    void ElementFromHandleBuildCache(); void ElementFromPointBuildCache(); void GetFocusedElementBuildCache();
    void CreateTreeWalker(); void GetControlViewWalker(); void GetContentViewWalker(); void GetRawViewWalker();
    void GetRawViewCondition(); void GetControlViewCondition(); void GetContentViewCondition();
    void CreateCacheRequest(); void CreateTrueCondition(); void CreateFalseCondition();
    [return: MarshalAs(UnmanagedType.Interface)] object CreatePropertyCondition(int id, [MarshalAs(UnmanagedType.Struct)] object value);
}
[ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAlertElement {
    void SetFocus(); void GetRuntimeId();
    [return: MarshalAs(UnmanagedType.Interface)] IAlertElement FindFirst(int scope, [MarshalAs(UnmanagedType.Interface)] object condition);
    void FindAll(); void FindFirstBuildCache(); void FindAllBuildCache(); void BuildUpdatedCache();
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
function Find-Node([string]$Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Node([string]$Id) { $node = Find-Node $Id; Require ($null -ne $node) "Missing Alert node: $Id"; return $node }
function Invoke-Node([string]$Id) { (Node $Id).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
function Jump([string]$Name) {
    $condition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Name),
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button))
    $button = $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    Require ($null -ne $button) "Missing Alert example navigation: $Name"
    $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
function Toggle-State($Control) {
    return $Control.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState
}
$process = Start-Process -FilePath $Executable -ArgumentList @('--component', 'alert', '--reduced-motion') -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output 'stderr.txt')
try {
    $script:handle = [IntPtr]::Zero
    Wait-Condition {
        $process.Refresh()
        if ($process.HasExited) { throw "Alert gallery exited early: $($process.ExitCode)" }
        $script:handle = [AntAlertUia]::Find($process.Id)
        $script:handle -ne [IntPtr]::Zero
    } 'Native Alert window was not created.' 30000
    # Show this fixture without activating it. Every interaction uses UIA;
    # the script never sends input to the user's foreground application.
    [AntAlertUia]::ShowWindow($script:handle, 4) | Out-Null
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    Wait-Condition { $null -ne (Find-Node 'alert.basic.one') } 'Native Alert tree did not appear.' 30000
    $basic = Node 'alert.basic.one'
    Require ($basic.Current.ControlType -eq [System.Windows.Automation.ControlType]::Text) 'Alert did not map to its native text role.'
    Require (-not $basic.Current.IsKeyboardFocusable) 'Static Alert should not be a keyboard tab stop.'
    $role = [string][AntAlertUia]::Property($script:handle, 'alert.basic.one', 30101)
    $live = [int][AntAlertUia]::Property($script:handle, 'alert.basic.one', 30135)
    Require ($role -eq 'alert') 'Alert lost its ARIA alert role.'
    Require ($live -eq 2) 'Alert should expose an assertive native live region.'

    Jump 'Closable'
    Wait-Condition { $null -ne (Find-Node 'alert.closable.Warning.close') } 'Closable alerts did not become accessible.'
    $close = Node 'alert.closable.Warning.close'
    Require ($close.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'Close control is not a native button.'
    Require ($close.Current.Name -eq 'Close') 'Close control lost its localized English label.'
    Require ($close.Current.IsEnabled -and $close.Current.IsKeyboardFocusable) 'Enabled close control should accept keyboard focus.'
    $close.SetFocus()
    Wait-Condition { $close.Current.HasKeyboardFocus } 'Native focus action did not focus the close control.'
    Invoke-Node 'alert.closable.Warning.close'
    Wait-Condition { $null -eq (Find-Node 'alert.closable.Warning') } 'Native Invoke failed to dismiss its alert.'
    Require ($null -ne (Find-Node 'alert.closable.Success.close')) 'Closing one alert removed its neighbor.'

    Jump 'Smoothly Unmount'
    Wait-Condition { $null -ne (Find-Node 'alert.smooth.item.close') } 'Smooth close example did not become accessible.'
    $toggle = Node 'alert.smooth.toggle'
    Require (-not $toggle.Current.IsEnabled) 'Visibility switch should be disabled while the alert is visible.'
    Require ((Toggle-State $toggle) -eq [System.Windows.Automation.ToggleState]::On) 'Visibility switch should initially be checked.'
    Invoke-Node 'alert.smooth.item.close'
    Wait-Condition {
        $null -eq (Find-Node 'alert.smooth.item') -and (Node 'alert.smooth.toggle').Current.IsEnabled
    } 'Close completion did not unmount the alert and enable its switch.'
    $toggle = Node 'alert.smooth.toggle'
    Require ((Toggle-State $toggle) -eq [System.Windows.Automation.ToggleState]::Off) 'Owner visibility did not follow close completion.'
    $toggle.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
    Wait-Condition { $null -ne (Find-Node 'alert.smooth.item.close') } 'Visibility switch did not mount a fresh alert.'
    Invoke-Node 'alert.smooth.item.close'
    Wait-Condition { $null -eq (Find-Node 'alert.smooth.item') } 'Remounted alert failed to close.'

    Jump 'Custom action'
    Wait-Condition { $null -ne (Find-Node 'alert.action.UNDO') } 'Action example did not become accessible.'
    Invoke-Node 'alert.action.UNDO'
    Require ($null -ne (Find-Node 'alert.action.success.close')) 'An action incorrectly dismissed its alert.'
    foreach ($id in @('gallery.control.theme', 'gallery.control.density', 'gallery.control.direction')) { Invoke-Node $id }
    Jump 'Smoothly Unmount'
    Wait-Condition { $null -ne (Find-Node 'alert.smooth.toggle') } 'Presentation change lost the visibility control.'
    Require ($null -eq (Find-Node 'alert.smooth.item')) 'Presentation changes restored a dismissed alert.'
    Require ((Toggle-State (Node 'alert.smooth.toggle')) -eq [System.Windows.Automation.ToggleState]::Off) 'Presentation changes lost retained owner state.'

    $script:window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
    Require ($process.WaitForExit(5000)) 'Alert gallery did not exit normally.'
    Require ($process.ExitCode -eq 0) 'Alert gallery failed during teardown.'
    [ordered]@{
        status = 'passed'; runtime = $PSVersionTable.PSVersion.ToString(); utc = [DateTime]::UtcNow.ToString('O')
        executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
        script_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
        aria_role = $role; live_setting = $live
        checks = @('alert-role', 'assertive-live-region', 'static-not-focusable', 'close-role-and-name', 'native-focus', 'native-invoke', 'isolated-close', 'completion-unmount', 'switch-remount', 'action-isolation', 'theme-density-rtl-retention')
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
    Write-Output "Alert native UIA passed. Evidence: $output"
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    $process.Dispose()
}

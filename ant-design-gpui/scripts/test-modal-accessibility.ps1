# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Opt-in native Modal UI Automation checks. Requires an interactive Windows desktop.
#>
[CmdletBinding()]
param([switch]$SkipBuild, [string]$Executable)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/modal-uia'
New-Item -ItemType Directory -Path $output -Force | Out-Null
[ordered]@{ status = 'running'; utc = [DateTime]::UtcNow.ToString('O') } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example gallery
        if ($LASTEXITCODE -ne 0) { throw 'Modal gallery build failed.' }
    } finally { Pop-Location }
}
if (-not $Executable) {
    $Executable = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/gallery.exe'
}
if (-not (Test-Path -LiteralPath $Executable)) { throw "Missing component gallery: $Executable" }
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('AntModalUia' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AntModalUia {
    private delegate bool Callback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr handle, StringBuilder title, int capacity);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr handle, int command);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr handle, uint msg, IntPtr key, IntPtr data);
    public static void Tab(IntPtr handle) {
        PostMessage(handle, 0x0100, new IntPtr(9), IntPtr.Zero);
        PostMessage(handle, 0x0101, new IntPtr(9), IntPtr.Zero);
    }
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
        var automation = (IModalAutomation)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        IModalElement root = null, element = null; object condition = null;
        try {
            root = automation.ElementFromHandle(handle);
            condition = automation.CreatePropertyCondition(30011, automationId);
            element = root.FindFirst(4, condition);
            if (element == null) throw new InvalidOperationException("Missing native Modal node: " + automationId);
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
interface IModalAutomation {
    void CompareElements(); void CompareRuntimeIds(); void GetRootElement();
    [return: MarshalAs(UnmanagedType.Interface)] IModalElement ElementFromHandle(IntPtr handle);
    void ElementFromPoint(); void GetFocusedElement(); void GetRootElementBuildCache();
    void ElementFromHandleBuildCache(); void ElementFromPointBuildCache(); void GetFocusedElementBuildCache();
    void CreateTreeWalker(); void GetControlViewWalker(); void GetContentViewWalker(); void GetRawViewWalker();
    void GetRawViewCondition(); void GetControlViewCondition(); void GetContentViewCondition();
    void CreateCacheRequest(); void CreateTrueCondition(); void CreateFalseCondition();
    [return: MarshalAs(UnmanagedType.Interface)] object CreatePropertyCondition(int id, [MarshalAs(UnmanagedType.Struct)] object value);
}
[ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IModalElement {
    void SetFocus(); void GetRuntimeId();
    [return: MarshalAs(UnmanagedType.Interface)] IModalElement FindFirst(int scope, [MarshalAs(UnmanagedType.Interface)] object condition);
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
function Node([string]$Id) { $node = Find-Node $Id; Require ($null -ne $node) "Missing Modal node: $Id"; return $node }
function Invoke-Node([string]$Id) { (Node $Id).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
function Jump([string]$Name) {
    $condition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Name),
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button))
    $button = $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    Require ($null -ne $button) "Missing Modal example navigation: $Name"
    $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
function Toggle-State($Control) {
    return $Control.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState
}
$process = Start-Process -FilePath $Executable -ArgumentList @('--component', 'modal', '--reduced-motion') -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output 'stderr.txt')
$checks = [Collections.Generic.List[string]]::new()
try {
    $script:handle = [IntPtr]::Zero
    Wait-Condition {
        $process.Refresh()
        if ($process.HasExited) { throw "Modal gallery exited: $($process.ExitCode)" }
        $script:handle = [AntModalUia]::Find($process.Id)
        $script:handle -ne [IntPtr]::Zero
    } 'Native Modal window was not created.' 30000
    [AntModalUia]::ShowWindow($script:handle, 4) | Out-Null
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    Wait-Condition { $null -ne (Find-Node 'modal-basic.0') } 'Modal gallery tree was not exposed.' 30000
    $trigger = Node 'modal-basic.0'
    $trigger.SetFocus()
    Invoke-Node 'modal-basic.0'
    Wait-Condition { $null -ne (Find-Node 'modal.dialog') } 'Modal did not open.'
    Require ((Node 'modal.dialog').Current.Name -eq 'Basic Modal') 'Dialog lost its accessible title.'
    Require ([string][AntModalUia]::Property($script:handle, 'modal.dialog', 30101) -eq 'dialog') 'Dialog lost its native dialog role.'
    Require ($null -eq (Find-Node 'modal-basic.0')) 'Background actions remain exposed during a modal dialog.'
    $checks.Add('dialog-title-role-and-background-isolation')
    (Node 'modal.cancel').SetFocus()
    for ($i=0; $i -lt 6; $i++) {
        [AntModalUia]::Tab($script:handle)
        Start-Sleep -Milliseconds 50
        $focused = [System.Windows.Automation.AutomationElement]::FocusedElement
        Require ($focused.Current.AutomationId -in @('modal.cancel','modal.ok','modal.close','modal.dialog')) 'Native Tab escaped the modal branch.'
    }
    $checks.Add('native-tab-containment')
    Invoke-Node 'modal.cancel'
    Wait-Condition { $null -eq (Find-Node 'modal.dialog') } 'Cancel failed to close the dialog.'
    Wait-Condition { (Node 'modal-basic.0').Current.HasKeyboardFocus } 'Dialog did not return focus to its trigger.'
    $checks.Add('cancel-and-focus-return')
    Invoke-Node 'modal-basic.0'
    Wait-Condition { $null -ne (Find-Node 'modal.close') } 'Close action missing.'
    Require ((Node 'modal.close').Current.Name -eq 'Close') 'Close action has the wrong accessible name.'
    Invoke-Node 'modal.close'
    Wait-Condition { $null -eq (Find-Node 'modal.dialog') } 'Close action failed.'
    $checks.Add('named-close-action')
    Jump 'Asynchronously close'
    Invoke-Node 'modal-async.0'
    Wait-Condition { $null -ne (Find-Node 'modal.ok') } 'Async dialog missing.'
    Invoke-Node 'modal.ok'
    Wait-Condition { $pattern = $null; -not (Node 'modal.ok').TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern) } 'Pending OK still exposes Invoke.' 900
    Wait-Condition { $null -eq (Find-Node 'modal.dialog') } 'Async completion did not close the dialog.'
    $checks.Add('async-busy-and-completion')
    Jump 'Internationalization'
    Invoke-Node 'modal-locale.2'
    Wait-Condition { $null -ne (Find-Node 'modal.ok') } 'Localized dialog missing.'
    Require ((Node 'modal.ok').Current.Name -eq '確定') 'Traditional Chinese OK text was not inherited.'
    Require ([string][AntModalUia]::Property($script:handle, 'modal.dialog', 30101) -eq 'alertdialog') 'Confirmation lost alertdialog semantics.'
    Invoke-Node 'modal.cancel'
    Wait-Condition { $null -eq (Find-Node 'modal.dialog') } 'Localized cancellation failed.'
    $checks.Add('locale-and-confirm-role')
    Jump 'Customize footer buttons'
    Invoke-Node 'modal-button-props.0'
    Wait-Condition { $null -ne (Find-Node 'modal.cancel') } 'Footer customization missing.'
    Require (-not (Node 'modal.cancel').Current.IsEnabled) 'Disabled Cancel is enabled.'
    Invoke-Node 'modal.ok'
    Wait-Condition { $null -eq (Find-Node 'modal.dialog') } 'Customized OK failed.'
    $checks.Add('disabled-footer-button')
    $script:window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
    Require ($process.WaitForExit(5000)) 'Modal gallery did not exit normally.'
    Require ($process.ExitCode -eq 0) 'Modal teardown failed.'
    $checks.Add('window-teardown')
    [ordered]@{status='passed';utc=[DateTime]::UtcNow.ToString('O');checks=@($checks);executable_sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant();script_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
    Write-Output "Modal native UIA passed. Evidence: $output"
} catch {
    [ordered]@{status='failed';utc=[DateTime]::UtcNow.ToString('O');checks=@($checks);error=$_.Exception.Message} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
    throw
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    $process.Dispose()
}

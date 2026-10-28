# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/menu-uia'
New-Item -ItemType Directory -Path $output -Force | Out-Null
[ordered]@{ status = 'running'; utc = [DateTime]::UtcNow.ToString('O') } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try { & cargo build --locked --offline --example menu_accessibility; if ($LASTEXITCODE -ne 0) { throw 'Menu UIA fixture build failed.' } }
    finally { Pop-Location }
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
if (-not ('AntMenuUia' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AntMenuUia {
    private delegate bool Callback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr handle, StringBuilder title, int capacity);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr handle, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr handle);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    // The managed wrapper omits the ARIA property identifiers. Read the native
    // properties through the same COM interface used by Windows screen readers.
    public static object FocusedProperty(int id) {
        var automation = (IMenuAutomation)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        IMenuElement element = null;
        try { element = automation.GetFocusedElement(); return element.GetCurrentPropertyValue(id); }
        finally { if (element != null) Marshal.ReleaseComObject(element); Marshal.ReleaseComObject(automation); }
    }
    public static IntPtr Find(int pid) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((handle, parameter) => { uint owner; GetWindowThreadProcessId(handle, out owner);
            if (owner == pid) { var title = new StringBuilder(256); GetWindowText(handle, title, title.Capacity);
                if (title.ToString() == "Menu accessibility fixture") { result = handle; return false; } }
            return true; }, IntPtr.Zero);
        return result;
    }
}
[ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMenuAutomation {
    void CompareElements(); void CompareRuntimeIds(); void GetRootElement();
    void ElementFromHandle(); void ElementFromPoint();
    [return: MarshalAs(UnmanagedType.Interface)] IMenuElement GetFocusedElement();
}
[ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMenuElement {
    void SetFocus(); void GetRuntimeId(); void FindFirst(); void FindAll();
    void FindFirstBuildCache(); void FindAllBuildCache(); void BuildUpdatedCache();
    [return: MarshalAs(UnmanagedType.Struct)] object GetCurrentPropertyValue(int id);
}
'@
}
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Wait-Condition([scriptblock]$Check, [string]$Message, [int]$Timeout = 5000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Timeout)
    while ([DateTime]::UtcNow -lt $deadline) { if (& $Check) { return }; Start-Sleep -Milliseconds 25 }
    throw $Message
}
function Find-Node([string]$Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Node([string]$Id) { $node = Find-Node $Id; Require ($null -ne $node) "Missing menu node: $Id"; return $node }
function Send-Keys([string]$Keys) {
    Require ([AntMenuUia]::GetForegroundWindow() -eq $script:handle) 'Fixture lost foreground; refusing to send keys'
    [System.Windows.Forms.SendKeys]::SendWait($Keys)
}
function Read-State {
    if (-not (Test-Path -LiteralPath $script:statePath)) { return $null }
    $stream = [IO.FileStream]::new($script:statePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { $state = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    if ($state.process_id -eq $process.Id) { return $state }
    return $null
}
function Invoke-Node([string]$Id) { (Node $Id).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
$script:statePath = Join-Path $output 'state.json'
$exe = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/menu_accessibility.exe'
$process = Start-Process -FilePath $exe -ArgumentList @($script:statePath) -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output 'stderr.txt')
try {
    $script:handle = [IntPtr]::Zero
    Wait-Condition { $script:handle = [AntMenuUia]::Find($process.Id); $script:handle -ne [IntPtr]::Zero } 'Menu fixture window was not created' 30000
    [AntMenuUia]::ShowWindow($script:handle, 9) | Out-Null
    [AntMenuUia]::SetForegroundWindow($script:handle) | Out-Null
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    Wait-Condition { $null -ne (Find-Node 'inline.item.home') -and $null -ne (Read-State) } 'Native menu tree did not appear' 30000
    Require ((Node 'inline').Current.ControlType -eq [System.Windows.Automation.ControlType]::Menu) 'Inline navigation is not a Menu'
    Require ((Node 'horizontal').Current.ControlType -eq [System.Windows.Automation.ControlType]::MenuBar) 'Horizontal navigation is not a MenuBar'
    Require ((Node 'inline.item.home').Current.ControlType -eq [System.Windows.Automation.ControlType]::MenuItem) 'Navigation row is not a MenuItem'
    Require (-not (Node 'inline.item.disabled').Current.IsEnabled) 'Disabled menu item is enabled'
    $projects = (Node 'inline.item.projects').GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $projects.Expand()
    Wait-Condition { (Read-State).inline_open -contains 'projects' } 'Native Expand did not open the inline submenu'
    Wait-Condition { $projects.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded } 'Native submenu metadata did not become Expanded'
    $projects.Collapse()
    Wait-Condition { (Read-State).inline_open.Count -eq 0 } 'Native Collapse did not close the inline submenu'
    Wait-Condition { $projects.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Collapsed } 'Native submenu metadata did not become Collapsed'
    (Node 'inline.item.home').SetFocus()
    Send-Keys '{DOWN}'
    Wait-Condition { [System.Windows.Automation.AutomationElement]::FocusedElement.Current.Name -eq 'Projects' } 'Keyboard navigation did not skip disabled item'
    Send-Keys '{ENTER}'
    Wait-Condition { (Read-State).inline_open -contains 'projects' } 'Inline submenu did not expand'
    Send-Keys '{DOWN}'
    Wait-Condition { [System.Windows.Automation.AutomationElement]::FocusedElement.Current.Name -eq 'Alpha project' } 'Inline submenu did not expose child keyboard focus'
    Send-Keys '{ENTER}'
    Wait-Condition { (Read-State).inline_selected -contains 'alpha' } 'Keyboard activation did not select Alpha project'
    Require ((Read-State).last_source -eq 'Keyboard') 'Keyboard activation lost its input source'
    Invoke-Node 'inline.item.gamma'
    Wait-Condition { (Read-State).inline_selected -contains 'gamma' } 'Native Invoke did not select Gamma project'
    Require ((Read-State).last_source -eq 'Accessibility') 'Native activation lost its input source'
    (Read-State) | ConvertTo-Json -Depth 25 | Set-Content -LiteralPath (Join-Path $output 'inline-expanded.json') -Encoding utf8
    (Node 'popup.item.more').GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    Wait-Condition { (Read-State).popup_open -contains 'more' } 'Native Expand did not open popup submenu'
    Invoke-Node 'popup.item.archive'
    Wait-Condition { (Read-State).popup_selected -contains 'archive' -and (Read-State).popup_open.Count -eq 0 } 'Popup activation did not select and close'
    (Node 'horizontal.item.top-home').SetFocus()
    Send-Keys '{RIGHT}'
    Wait-Condition { [System.Windows.Automation.AutomationElement]::FocusedElement.Current.Name -eq 'Help' } 'Horizontal arrow key did not focus the submenu'
    Send-Keys '{DOWN}'
    Wait-Condition { [System.Windows.Automation.AutomationElement]::FocusedElement.Current.Name -eq 'Documentation' } 'Horizontal submenu did not expose child keyboard focus'
    Send-Keys '{ENTER}'
    Wait-Condition { (Read-State).horizontal_selected -contains 'docs' } 'Horizontal submenu keyboard navigation failed'
    Require ((Read-State).activations -eq 4) 'Menu activation did not occur exactly once per action'
    $multipleA = Node 'multiple.item.choice-a'
    $multipleB = Node 'multiple.item.choice-b'
    $multipleC = Node 'multiple.item.choice-c'
    $multipleA.SetFocus()
    Wait-Condition { [System.Windows.Automation.AutomationElement]::FocusedElement.Current.Name -eq 'Choice A' } 'Multiple selection item did not accept native focus'
    $multipleRole = [string][AntMenuUia]::FocusedProperty(30101) # UIA_AriaRolePropertyId
    $multipleProperties = [string][AntMenuUia]::FocusedProperty(30102) # UIA_AriaPropertiesPropertyId
    Require ($multipleRole -eq 'menuitemcheckbox') 'Multiple selection does not expose the menuitemcheckbox ARIA role'
    Require ($multipleProperties -notmatch 'current=(page|true)') 'Multiple selection exposes a current-page marker'
    $toggleA = $multipleA.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    $toggleB = $multipleB.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    $toggleC = $multipleC.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    Require ($toggleA.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On) 'Initially selected Choice A is not checked'
    Require ($toggleB.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On) 'Initially selected Choice B is not checked'
    Require ($toggleC.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::Off) 'Unselected Choice C is checked'
    $beforeToggle = Read-State
    $toggleA.Toggle()
    Wait-Condition { (Read-State).multiple_selected.Count -eq 1 -and (Read-State).multiple_selected -contains 'choice-b' } 'Native Toggle did not deselect Choice A independently'
    Wait-Condition { $toggleA.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::Off } 'Native checked metadata did not update after deselection'
    Require ((Read-State).last_source -eq 'Accessibility') 'Native Toggle lost its accessibility source'
    Require ((Read-State).activations -eq $beforeToggle.activations + 1 -and (Read-State).deselections -eq $beforeToggle.deselections + 1 -and (Read-State).selections -eq $beforeToggle.selections) 'Native deselection did not emit exactly one activation and deselection'
    $toggleA.Toggle()
    Wait-Condition { (Read-State).multiple_selected.Count -eq 2 -and (Read-State).multiple_selected -contains 'choice-a' -and (Read-State).multiple_selected -contains 'choice-b' } 'Native Toggle did not reselect Choice A independently'
    Wait-Condition { $toggleA.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On } 'Native checked metadata did not update after selection'
    Require ((Read-State).activations -eq $beforeToggle.activations + 2 -and (Read-State).deselections -eq $beforeToggle.deselections + 1 -and (Read-State).selections -eq $beforeToggle.selections + 1) 'Native selection did not emit exactly one activation and selection'
    [ordered]@{ aria_role = $multipleRole; aria_properties = $multipleProperties; state = (Read-State) } | ConvertTo-Json -Depth 25 | Set-Content -LiteralPath (Join-Path $output 'multiple-toggle.json') -Encoding utf8
    Require ((Read-State).activations -eq 6) 'Native menu actions emitted duplicate activations'
    [ordered]@{
        status = 'passed'
        runtime = $PSVersionTable.PSVersion.ToString()
        utc = [DateTime]::UtcNow.ToString('O')
        executable_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
        fixture_source_sha256 = (Get-FileHash -LiteralPath (Join-Path $crateRoot 'examples/menu_accessibility.rs') -Algorithm SHA256).Hash.ToLowerInvariant()
        script_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
        checks = @('menu-role', 'menubar-role', 'menuitem-role', 'disabled', 'keyboard-focus', 'expand-collapse', 'inline-expand', 'selection', 'native-invoke', 'activation-source', 'popup-open-close', 'horizontal-keyboard', 'multiple-menuitemcheckbox', 'multiple-toggle-metadata', 'native-toggle', 'independent-selection', 'exactly-once')
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
    Write-Output "Menu native UIA checks passed. Evidence: $output"
} finally { if (-not $process.HasExited) { Stop-Process -Id $process.Id }; $process.Dispose() }

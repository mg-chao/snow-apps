# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/select-slider-uia'
New-Item -ItemType Directory -Path $output -Force | Out-Null
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try { & cargo build --locked --offline --example select_slider_accessibility; if ($LASTEXITCODE -ne 0) { throw 'UIA fixture build failed' } }
    finally { Pop-Location }
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
if (-not ('SelectSliderUia' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SelectSliderUia {
    private delegate bool Callback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr handle, StringBuilder title, int capacity);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr handle, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr handle);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    public static IntPtr Find(int pid) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((handle, parameter) => { uint owner; GetWindowThreadProcessId(handle, out owner);
            if (owner == pid) {
                var title = new StringBuilder(256); GetWindowText(handle, title, title.Capacity);
                if (title.ToString() == "Select and Slider accessibility fixture") { result = handle; return false; }
            } return true; }, IntPtr.Zero);
        return result;
    }
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
function Node([string]$Id) { $node = Find-Node $Id; Require ($null -ne $node) "Missing node: $Id"; return $node }
function Send-Keys([string]$Keys) {
    Require ([SelectSliderUia]::GetForegroundWindow() -eq $script:handle) 'Fixture lost foreground; refusing to send keys'
    [System.Windows.Forms.SendKeys]::SendWait($Keys)
}
function Read-State {
    if (-not (Test-Path -LiteralPath $script:statePath)) { return $null }
    $stream = [IO.FileStream]::new($script:statePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { $state = $reader.ReadToEnd() | ConvertFrom-Json }
    finally { $reader.Dispose() }
    if ($state.process_id -eq $process.Id) { return $state }
    return $null
}
function Active-Label($Tree) {
    if ($null -eq $Tree -or $null -eq $Tree.active_descendant_focus) { return '' }
    return $Tree.nodes.PSObject.Properties[$Tree.active_descendant_focus].Value.aria.label
}
function Invoke-Name([string]$Name) {
    $condition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Name)
    $node = $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    Require ($null -ne $node) "Missing action: $Name"
    $node.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
$script:statePath = Join-Path $output 'state.json'
$exe = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/select_slider_accessibility.exe'
$process = Start-Process -FilePath $exe -ArgumentList @($script:statePath) -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output 'stderr.txt')
try {
    $script:handle = [IntPtr]::Zero
    Wait-Condition { $script:handle = [SelectSliderUia]::Find($process.Id); $script:handle -ne [IntPtr]::Zero } 'Fixture window was not created' 30000
    # Only this test window is shown and activated, so native focus can be exercised.
    [SelectSliderUia]::ShowWindow($script:handle, 6) | Out-Null
    [SelectSliderUia]::ShowWindow($script:handle, 9) | Out-Null
    [SelectSliderUia]::SetForegroundWindow($script:handle) | Out-Null
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    Wait-Condition { $null -ne (Find-Node 'single') } 'Native tree did not appear' 30000
    Wait-Condition { $null -ne (Read-State) } 'Current fixture did not publish a snapshot'
    $single = Node 'single'
    Require ($single.Current.ControlType -eq [System.Windows.Automation.ControlType]::ComboBox) 'Select does not expose ComboBox'
    $single.SetFocus()
    $expand = $single.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $expand.Expand()
    Wait-Condition { $expand.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded } 'Select did not expand'
    Send-Keys '{DOWN}'
    Wait-Condition { [System.Windows.Automation.AutomationElement]::FocusedElement.Current.Name -eq 'Lucy' } 'Active option was not exposed as accessibility focus'
    Wait-Condition { (Active-Label (Read-State).tree) -eq 'Lucy' } 'Active option did not reach the evidence tree'
    Read-State | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $output 'expanded.json') -Encoding utf8
    $tree = (Read-State).tree
    $active = $tree.nodes.PSObject.Properties[$tree.active_descendant_focus].Value.aria
    Require ($active.position_in_set -eq 2 -and $active.size_of_set -eq 10003) 'Group headings changed option ordinals'
    $listItems = $script:window.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem))
    Require ($listItems.Count -lt 30) 'Virtualized Select materialized the entire accessibility list'
    Send-Keys '{ENTER}'
    Wait-Condition { (Read-State).single[0] -eq 'Lucy' } 'Keyboard selection failed'
    $single.SetFocus()
    $expand.Expand()
    $single.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('User 9999')
    Wait-Condition { (Active-Label (Read-State).tree) -eq 'User 9999' } 'Search did not expose a distant virtual option'
    $farOption = Node 'single.option.1'
    $farOption.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Wait-Condition { (Read-State).single[0] -eq 'User 9999' } 'Native SelectionItem selection failed'
    $multi = Node 'multiple'
    $multi.SetFocus()
    Invoke-Name 'Remove Jack'
    Wait-Condition { ((Read-State).multiple -join ',') -eq 'Lucy' } 'Accessible tag removal failed'
    Invoke-Name 'Clear selection'
    Wait-Condition { (Read-State).single.Count -eq 0 } 'Accessible clear failed'
    $middle = Node 'slider.handle.1'
    $range = $middle.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
    Require ($range.Current.Minimum -eq 0 -and $range.Current.Maximum -eq 100 -and $range.Current.SmallChange -eq 1) 'Slider range metadata differs'
    $range.SetValue(62)
    Wait-Condition { (Read-State).slider[1] -eq 62 } 'RangeValue SetValue failed'
    Require (-not (Node 'slider.handle.0').Current.IsEnabled) 'Disabled handle is enabled'
    $middle.SetFocus()
    Send-Keys '{RIGHT}'
    Wait-Condition { (Read-State).slider[1] -eq 63 } 'Slider keyboard editing failed after native SetValue'
    [ordered]@{ status = 'passed'; runtime = $PSVersionTable.PSVersion.ToString(); utc = [DateTime]::UtcNow.ToString('O'); checks = @('combobox', 'expand-collapse', 'active-option', 'group-ordinals', 'bounded-virtualization', 'distant-option-search', 'selection-item', 'selection', 'remove', 'clear', 'range-value', 'per-handle-disabled', 'keyboard') } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
    Write-Output "Select/Slider native UIA checks passed. Evidence: $output"
} finally { if (-not $process.HasExited) { Stop-Process -Id $process.Id }; $process.Dispose() }

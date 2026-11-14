# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Checks native Pagination UI Automation, with optional interactive keyboard checks.
.DESCRIPTION
The default launch stays hidden and never sends global keyboard input. Some GPUI
Windows backends do not initialize a native accessibility tree until ShowWindow;
that limitation is reported as unavailable, never passed. -Interactive explicitly
shows this fixture to test native focus, Tab, Shift+Tab, Enter, Space and blur.
#>
[CmdletBinding()]
param([switch]$SkipBuild, [switch]$Interactive)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/pagination-uia'
New-Item -ItemType Directory -Path $output -Force | Out-Null
[ordered]@{ status = 'running'; mode = $(if ($Interactive) { 'interactive' } else { 'hidden' })
    utc = [DateTime]::UtcNow.ToString('O')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example pagination_accessibility
        if ($LASTEXITCODE -ne 0) { throw 'Pagination UIA fixture build failed' }
    } finally { Pop-Location }
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if ($Interactive) { Add-Type -AssemblyName System.Windows.Forms }
if (-not ('PaginationUia' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class PaginationUia {
    // COM access also supports properties absent from .NET Framework's older UIA registry.
    [ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface Automation {
        void CompareElements(IntPtr a, IntPtr b, out bool same);
        void CompareRuntimeIds(IntPtr a, IntPtr b, out bool same);
        void GetRootElement(out Element element);
        void ElementFromHandle(IntPtr handle, out Element element);
        void ElementFromPoint(Int64 point, out Element element);
        void GetFocusedElement(out Element element);
        void GetRootElementBuildCache(IntPtr cache, out Element element);
        void ElementFromHandleBuildCache(IntPtr handle, IntPtr cache, out Element element);
        void ElementFromPointBuildCache(Int64 point, IntPtr cache, out Element element);
        void GetFocusedElementBuildCache(IntPtr cache, out Element element);
        void CreateTreeWalker(IntPtr condition, out IntPtr walker);
        void ControlViewWalker(out IntPtr walker);
        void ContentViewWalker(out IntPtr walker);
        void RawViewWalker(out IntPtr walker);
        void RawViewCondition(out IntPtr condition);
        void ControlViewCondition(out IntPtr condition);
        void ContentViewCondition(out IntPtr condition);
        void CreateCacheRequest(out IntPtr cache);
        void CreateTrueCondition(out IntPtr condition);
        void CreateFalseCondition(out IntPtr condition);
        void CreatePropertyCondition(int property, [MarshalAs(UnmanagedType.Struct)] object value,
            [MarshalAs(UnmanagedType.Interface)] out object condition);
    }
    [ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface Element {
        void SetFocus();
        void GetRuntimeId(out IntPtr runtimeId);
        void FindFirst(int scope, [MarshalAs(UnmanagedType.Interface)] object condition, out Element element);
        void FindAll(int scope, IntPtr condition, out IntPtr elements);
        void FindFirstBuildCache(int scope, IntPtr condition, IntPtr cache, out Element element);
        void FindAllBuildCache(int scope, IntPtr condition, IntPtr cache, out IntPtr elements);
        void BuildUpdatedCache(IntPtr cache, out Element element);
        void GetCurrentPropertyValue(int property, [MarshalAs(UnmanagedType.Struct)] out object value);
    }
    public static string AriaProperties(IntPtr handle, string automationId) {
        var automation = (Automation)Activator.CreateInstance(Type.GetTypeFromCLSID(
            new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        Element root = null, element = null;
        object condition = null;
        try {
            automation.ElementFromHandle(handle, out root);
            automation.CreatePropertyCondition(30011, automationId, out condition);
            root.FindFirst(4, condition, out element);
            if (element == null) { throw new InvalidOperationException("Missing native page: " + automationId); }
            object value;
            element.GetCurrentPropertyValue(30102, out value);
            return value as string ?? "";
        } finally {
            if (element != null) { Marshal.ReleaseComObject(element); }
            if (condition != null) { Marshal.ReleaseComObject(condition); }
            if (root != null) { Marshal.ReleaseComObject(root); }
            Marshal.ReleaseComObject(automation);
        }
    }
    private delegate bool Callback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr handle, StringBuilder title, int capacity);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr handle, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr handle);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr handle);
    public static IntPtr Find(int pid) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((handle, parameter) => {
            uint owner; GetWindowThreadProcessId(handle, out owner);
            if (owner == pid) {
                var title = new StringBuilder(256); GetWindowText(handle, title, title.Capacity);
                if (title.ToString() == "Pagination accessibility fixture") { result = handle; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
'@
}
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Wait-Condition([scriptblock]$Check, [string]$Message, [int]$Timeout = 5000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Timeout)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $Check) { return }
        if ($process.HasExited) { throw "Pagination fixture exited early: $($process.ExitCode)" }
        Start-Sleep -Milliseconds 30
    }
    throw $Message
}
function Find-Node([string]$Id, $Root = $script:window) {
    if ($null -eq $Root) { return $null }
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Node([string]$Id, $Root = $script:window) {
    $node = Find-Node $Id $Root
    Require ($null -ne $node) "Missing native node: $Id"
    return $node
}
function Total-Node($Root) {
    # A custom Element's local ID gives it stable identity, not a public UIA AutomationId.
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Text)
    $node = $Root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    Require ($null -ne $node) 'Missing native Pagination total Text'
    return $node
}
function Invoke-Node([string]$Id, $Root = $script:window) {
    (Node $Id $Root).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
function Read-State {
    if (-not (Test-Path -LiteralPath $script:statePath)) { return $null }
    $stream = [IO.FileStream]::new($script:statePath, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { $state = $reader.ReadToEnd() | ConvertFrom-Json }
    finally { $reader.Dispose() }
    if ($state.process_id -eq $process.Id) { return $state }
    return $null
}
function Aria-Properties($Element) {
    # UIA_AriaPropertiesPropertyId, exposed by AccessKit's Windows provider.
    return [PaginationUia]::AriaProperties($script:handle, $Element.Current.AutomationId)
}
function Require-Disabled($Element, [string]$Description) {
    Require (-not $Element.Current.IsEnabled) "$Description is enabled"
    Require (-not $Element.Current.IsKeyboardFocusable) "$Description is keyboard focusable"
    $pattern = $null
    Require (-not $Element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) "$Description advertises Invoke"
}
function Set-Value($Element, [string]$Value) {
    $pattern = $Element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    $pattern.SetValue($Value)
    Wait-Condition { $pattern.Current.Value -eq $Value } 'Native ValuePattern failed to update editor'
}
function Focus-Node($Element) {
    Require $Interactive 'Native focus checks require -Interactive'
    [PaginationUia]::SetForegroundWindow($script:handle) | Out-Null
    Wait-Condition { [PaginationUia]::GetForegroundWindow() -eq $script:handle } 'Could not activate fixture'
    $Element.SetFocus()
    Wait-Condition { $Element.Current.HasKeyboardFocus } 'Native focus did not reach pagination'
}
function Send-Keys([string]$Keys) {
    Require $Interactive 'Keyboard checks require -Interactive'
    Require ([PaginationUia]::GetForegroundWindow() -eq $script:handle) 'Fixture lost foreground; refusing to send keys'
    Require ([System.Windows.Automation.AutomationElement]::FocusedElement.Current.ProcessId -eq $process.Id) 'Fixture lost keyboard focus; refusing to send keys'
    [System.Windows.Forms.SendKeys]::SendWait($Keys)
}
function Save-Snapshot([string]$Name) {
    Read-State | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $output "$Name.json") -Encoding utf8
}

$script:statePath = Join-Path $output 'state.json'
$exe = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/pagination_accessibility.exe'
Require (Test-Path -LiteralPath $exe) "Missing fixture: $exe"
$arguments = @(('"' + $script:statePath + '"'))
if ($Interactive) { $arguments += '--interactive' }
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru `
    -RedirectStandardError (Join-Path $output 'stderr.txt')
# Retain the process handle before it exits so Windows PowerShell can read ExitCode after UIA Close.
$null = $process.Handle
$checks = [Collections.Generic.List[string]]::new()
$script:window = $null
$unavailable = $false
try {
    $script:handle = [IntPtr]::Zero
    Wait-Condition { $script:handle = [PaginationUia]::Find($process.Id); $script:handle -ne [IntPtr]::Zero } 'Fixture window was not created' 30000
    if ($Interactive) {
        [PaginationUia]::ShowWindow($script:handle, 9) | Out-Null
        [PaginationUia]::SetForegroundWindow($script:handle) | Out-Null
    } else {
        Require (-not [PaginationUia]::IsWindowVisible($script:handle)) 'Default fixture launch showed a window'
    }
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    try {
        Wait-Condition { $null -ne (Find-Node 'pagination-main') } 'Pagination native tree did not initialize' 15000
    } catch {
        if (-not $Interactive -and -not $process.HasExited -and $null -ne (Read-State)) {
            $unavailable = $true
            [ordered]@{ status = 'unavailable'; mode = 'hidden'; utc = [DateTime]::UtcNow.ToString('O')
                reason = 'The GPUI Windows backend did not publish a native UIA tree for its hidden window. Offscreen semantic tests remain available; use -Interactive for native focus and keyboard validation.'
                process_id = $process.Id; checks = $checks.ToArray(); keyboard = 'not-run-hidden-mode'
            } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
        }
        throw
    }
    Wait-Condition { $null -ne (Read-State) } 'Fixture did not publish state'
    Wait-Condition { (Read-State).tree.frame.viewport_size.width -gt 992 } 'Hidden fixture did not initialize its viewport'
    $main = Node 'pagination-main'
    Require ($main.Current.Name -eq 'Pagination') 'Pagination root label differs'
    $current = Node 'pagination-page-6' $main
    Require ($current.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'Page does not expose Button'
    Require ($current.Current.Name -eq 'Page 6') 'Page accessible name differs'
    Require ((Aria-Properties $current) -match '(^|;)current=page(;|$)') 'Current page is missing current=page'
    $total = Total-Node $main
    Require ($total.Current.ControlType -eq [System.Windows.Automation.ControlType]::Text) 'Total label is not native Text'
    Require ($total.Current.Name -eq '51-60 of 500 items') 'Total label does not expose its raw readable range'
    Require ($total.Current.Name.IndexOf([char]0x202b) -eq -1 -and $total.Current.Name.IndexOf([char]0x202c) -eq -1) 'Total label exposes directional formatting controls'
    Invoke-Node 'pagination-toggle-direction'
    Wait-Condition { (Read-State).direction -eq 'Rtl' } 'Native direction toggle did not reach RTL'
    $total = Total-Node $main
    Require ($total.Current.Name -eq '51-60 of 500 items') 'RTL total label changed logical text or exposed formatting controls'
    Save-Snapshot 'total-rtl'
    Invoke-Node 'pagination-toggle-direction'
    Wait-Condition { (Read-State).direction -eq 'Ltr' } 'Native direction toggle did not return to LTR'
    Require ((Read-State).events.Count -eq 0) 'Direction changes emitted pagination events'
    Save-Snapshot 'initial'
    $checks.Add('navigation-root-page-name-current-page')

    Invoke-Node 'pagination-page-7' $main
    Wait-Condition { (Read-State).main.current -eq 7 } 'Native page Invoke failed'
    Wait-Condition { (Total-Node $main).Current.Name -eq '61-70 of 500 items' } 'Native total range did not update after page Invoke'
    Invoke-Node 'pagination-previous' $main
    Wait-Condition { (Read-State).main.current -eq 6 } 'Previous Invoke failed'
    Invoke-Node 'pagination-next' $main
    Wait-Condition { (Read-State).main.current -eq 7 } 'Next Invoke failed'
    Invoke-Node 'pagination-jump-next' $main
    Wait-Condition { (Read-State).main.current -eq 12 } 'Forward ellipsis Invoke failed'
    Invoke-Node 'pagination-jump-previous' $main
    Wait-Condition { (Read-State).main.current -eq 7 } 'Backward ellipsis Invoke failed'
    $checks.Add('page-previous-next-jump-invoke')

    $disabled = Node 'pagination-disabled'
    Require-Disabled (Node 'pagination-page-6' $disabled) 'Disabled page'
    Require-Disabled (Node 'pagination-previous' $disabled) 'Disabled previous'
    Require-Disabled (Node 'pagination-next' $disabled) 'Disabled next'
    Require-Disabled (Node 'pagination-jump-next' $disabled) 'Disabled ellipsis'
    Require (-not (Node 'pagination-quick-input' $disabled).Current.IsEnabled) 'Disabled jumper is enabled'
    Require (-not (Node 'pagination-size-changer' $disabled).Current.IsEnabled) 'Disabled size changer is enabled'
    Invoke-Node 'pagination-toggle-disabled'
    Wait-Condition { -not (Node 'pagination-page-7' $main).Current.IsEnabled } 'Live disabled state did not reach page'
    Require-Disabled (Node 'pagination-page-7' $main) 'Runtime disabled page'
    Invoke-Node 'pagination-toggle-disabled'
    Wait-Condition { (Node 'pagination-page-7' $main).Current.IsEnabled } 'Live enabled state did not recover'
    $checks.Add('disabled-actions-focus-and-live-availability')

    $count = (Read-State).events.Count
    # Keep Unicode literals portable to Windows PowerShell's BOM-less script decoding.
    $localeNames = @((-join [char[]]@(0x4e0a, 0x4e00, 0x9875)),
        (-join [char[]]@(0x4e0a, 0x4e00, 0x9801)), 'Previous Page')
    foreach ($expected in $localeNames) {
        Invoke-Node 'pagination-cycle-locale'
        Wait-Condition { (Node 'pagination-previous' $main).Current.Name -eq $expected } 'Native locale did not update'
    }
    Require ((Read-State).events.Count -eq $count) 'Locale changes emitted pagination events'
    $checks.Add('three-live-locales')

    $select = Node 'pagination-size-changer' $main
    Require ($select.Current.ControlType -eq [System.Windows.Automation.ControlType]::ComboBox) 'Size changer is not a ComboBox'
    $expand = $select.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $expand.Expand()
    Wait-Condition { $expand.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded } 'Size changer did not expand'
    Wait-Condition {
        $condition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, '20 / page')
        $script:sizeOption = $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
        $null -ne $script:sizeOption
    } 'Page-size option was not exposed'
    $script:sizeOption.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Wait-Condition { (Read-State).main.page_size -eq 20 } 'Native size selection failed'
    Wait-Condition { (Total-Node $main).Current.Name -eq '121-140 of 500 items' } 'Native total range did not update after size selection'
    $events = (Read-State).events
    Require ($events[-2].type -eq 'size-change' -and $events[-2].current -eq 7 -and $events[-2].page_size -eq 20) 'Page-size callback payload/order differs'
    Require ($events[-1].type -eq 'change' -and $events[-1].current -eq 7 -and $events[-1].page_size -eq 20) 'Page-change callback after size differs'
    $checks.Add('combobox-expand-selection-and-size-event-order')

    $quick = Node 'pagination-quick-input' $main
    Require ($quick.Current.Name -eq 'Page') 'Quick editor name differs'
    Require ($quick.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit) 'Quick jumper is not Edit'
    Set-Value $quick '17'
    $simple = Node 'pagination-simple'
    $simpleInput = Node 'pagination-simple-input' $simple
    Require ($simpleInput.Current.Name -eq 'Go to') 'Simple editor name differs'
    Require ($null -eq (Find-Node 'pagination-simple-input' (Node 'pagination-readonly'))) 'Read-only pager exposes editable input'
    $checks.Add('quick-simple-editors-and-readonly')

    if ($Interactive) {
        Focus-Node $quick
        Send-Keys '{ENTER}'
        Wait-Condition { (Read-State).main.current -eq 17 } 'Quick-jumper Enter failed'
        Set-Value $quick '12'
        Focus-Node (Node 'pagination-outside')
        Wait-Condition { (Read-State).main.current -eq 12 } 'Quick-jumper blur failed'
        Focus-Node $simpleInput
        Set-Value $simpleInput '4'
        Send-Keys '{ENTER}'
        Wait-Condition { (Read-State).simple.current -eq 4 } 'Simple Enter failed'
        Send-Keys '{UP}'
        Wait-Condition { (Read-State).simple.current -eq 3 } 'Simple ArrowUp failed'
        $checks.Add('quick-enter-blur-and-simple-keyboard')

        Invoke-Node 'pagination-reset'
        Wait-Condition { (Read-State).main.current -eq 6 -and (Read-State).main.page_size -eq 10 } 'Reset did not complete'
        Focus-Node (Node 'pagination-page-6' $main)
        Send-Keys '{TAB}'
        Wait-Condition { [System.Windows.Automation.AutomationElement]::FocusedElement.Current.AutomationId -eq 'pagination-page-7' } 'Tab skipped the next page'
        Send-Keys '+{TAB}'
        Wait-Condition { [System.Windows.Automation.AutomationElement]::FocusedElement.Current.AutomationId -eq 'pagination-page-6' } 'Shift+Tab did not return to current page'
        Focus-Node (Node 'pagination-page-7' $main)
        Send-Keys '{ENTER}'
        Wait-Condition { (Read-State).main.current -eq 7 } 'Enter page activation failed'
        Focus-Node (Node 'pagination-page-8' $main)
        Send-Keys ' '
        Wait-Condition { (Read-State).main.current -eq 8 } 'Space page activation failed'
        $checks.Add('tab-shift-tab-enter-space')

        Invoke-Node 'pagination-focus-last'
        Wait-Condition { (Read-State).main.current -eq 50 } 'Last page was not focused'
        Invoke-Node 'pagination-shrink-total'
        Wait-Condition { (Read-State).main.current -eq 5 } 'Total shrink did not clamp page'
        Wait-Condition { [System.Windows.Automation.AutomationElement]::FocusedElement.Current.AutomationId -eq 'pagination-page-5' } 'Disappearing page did not transfer focus to surviving current page'
        Require-Disabled (Node 'pagination-next' $main) 'Last-page next'
        $checks.Add('disappearing-page-focus-and-boundary-disabled')
    }
    Require (@((Read-State).events | Where-Object control -eq 'disabled').Count -eq 0) 'Disabled control emitted a change'
    if (-not $Interactive) {
        Require (-not [PaginationUia]::IsWindowVisible($script:handle)) 'Native actions showed the hidden fixture'
    }
    Save-Snapshot 'final'
    $script:window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
    Require ($process.WaitForExit(5000)) 'Fixture did not exit after normal window close'
    Require ($process.ExitCode -eq 0) "Fixture failed during teardown: exit code $($process.ExitCode)"
    $checks.Add('normal-teardown')
    [ordered]@{ status = 'passed'; mode = $(if ($Interactive) { 'interactive' } else { 'hidden' })
        utc = [DateTime]::UtcNow.ToString('O'); checks = $checks.ToArray()
        keyboard = $(if ($Interactive) { 'passed' } else { 'not-run-hidden-mode' })
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
    Write-Output "Pagination native UIA checks passed. Evidence: $output"
} catch {
    if (-not $unavailable) {
        [ordered]@{ status = 'failed'; mode = $(if ($Interactive) { 'interactive' } else { 'hidden' })
            utc = [DateTime]::UtcNow.ToString('O'); checks = $checks.ToArray(); reason = $_.Exception.Message
        } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
    }
    throw
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id }
    $process.Dispose()
}

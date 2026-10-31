# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/date-time-picker-uia'
New-Item -ItemType Directory -Path $output -Force | Out-Null
. (Join-Path $PSScriptRoot 'date-time-picker-validation-support.ps1')
$taskSources = Get-DateTimePickerSourceFingerprint $crateRoot
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --features test-support --example date_time_picker_accessibility
        if ($LASTEXITCODE -ne 0) { throw 'Date/time picker UIA fixture build failed.' }
    } finally { Pop-Location }
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('DateTimePickerUia' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
[ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface PickerNativeAutomation {
    void CompareElements(); void CompareRuntimeIds(); void GetRootElement();
    [return: MarshalAs(UnmanagedType.Interface)] PickerNativeElement ElementFromHandle(IntPtr handle);
    void ElementFromPoint(); void GetFocusedElement(); void GetRootElementBuildCache();
    void ElementFromHandleBuildCache(); void ElementFromPointBuildCache(); void GetFocusedElementBuildCache();
    void CreateTreeWalker(); void ControlViewWalker(); void ContentViewWalker(); void RawViewWalker();
    void RawViewCondition(); void ControlViewCondition(); void ContentViewCondition(); void CreateCacheRequest();
    void CreateTrueCondition(); void CreateFalseCondition();
    IntPtr CreatePropertyCondition(int property, [MarshalAs(UnmanagedType.Struct)] object value);
}
[ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface PickerNativeElement {
    void SetFocus(); void GetRuntimeId();
    [return: MarshalAs(UnmanagedType.Interface)] PickerNativeElement FindFirst(int scope, IntPtr condition);
    void FindAll(); void FindFirstBuildCache(); void FindAllBuildCache(); void BuildUpdatedCache();
    [return: MarshalAs(UnmanagedType.Struct)] object GetCurrentPropertyValue(int property);
}
public static class DateTimePickerUia {
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    private delegate bool Callback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr handle, StringBuilder title, int capacity);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr handle, out Rect bounds);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr handle, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern IntPtr GetWindowLongPtr(IntPtr handle, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")] private static extern IntPtr SetWindowLongPtr(IntPtr handle, int index, IntPtr value);
    public static uint Owner(IntPtr handle) { uint pid; GetWindowThreadProcessId(handle, out pid); return pid; }
    public static bool InitializeSize(IntPtr handle) {
        long style = GetWindowLongPtr(handle, -20).ToInt64();
        SetWindowLongPtr(handle, -20, new IntPtr(style | 0x08000000L));
        Rect bounds; if (!GetWindowRect(handle, out bounds)) return false;
        int width = bounds.Right - bounds.Left, height = bounds.Bottom - bounds.Top;
        const uint flags = 0x0002 | 0x0004 | 0x0010;
        return SetWindowPos(handle, IntPtr.Zero, 0, 0, width + 1, height, flags)
            && SetWindowPos(handle, IntPtr.Zero, 0, 0, width, height, flags);
    }
    public static IntPtr Find(int pid) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((handle, parameter) => { uint owner; GetWindowThreadProcessId(handle, out owner);
            if (owner == pid) { var title = new StringBuilder(256); GetWindowText(handle, title, title.Capacity);
                if (title.ToString() == "Date/time picker accessibility fixture") { result = handle; return false; }
            } return true;
        }, IntPtr.Zero);
        return result;
    }
    public static string AriaPropertiesByName(IntPtr handle, string name) {
        // The legacy managed UIA assembly omits this property. Read the actual
        // modern UIA COM property using SDK-declared interface/vtable order.
        var automation = (PickerNativeAutomation)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        PickerNativeElement root = null, element = null; IntPtr condition = IntPtr.Zero;
        try {
            root = automation.ElementFromHandle(handle);
            condition = automation.CreatePropertyCondition(30005, name);
            element = root.FindFirst(4, condition);
            return element == null ? null : Convert.ToString(element.GetCurrentPropertyValue(30102));
        } finally {
            if (element != null) Marshal.ReleaseComObject(element);
            if (condition != IntPtr.Zero) Marshal.Release(condition);
            if (root != null) Marshal.ReleaseComObject(root);
            Marshal.ReleaseComObject(automation);
        }
    }
}
'@
}
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Wait-Condition([scriptblock]$Check, [string]$Message, [int]$Timeout = 10000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Timeout)
    while ([DateTime]::UtcNow -lt $deadline) { if (& $Check) { return }; Start-Sleep -Milliseconds 30 }
    throw $Message
}
function Find-Node([string]$Id) {
    return $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id))
}
function Node([string]$Id) { $node = Find-Node $Id; Require ($null -ne $node) "Missing native node: $Id"; return $node }
function Find-Name([string]$Name) {
    return $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Name))
}
function Named([string]$Name) { $node = Find-Name $Name; Require ($null -ne $node) "Missing native label: $Name"; return $node }
function Read-State {
    if (-not (Test-Path -LiteralPath $script:statePath)) { return $null }
    $stream = [IO.FileStream]::new($script:statePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { $state = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    if ($state.process_id -eq $script:process.Id) { return $state }
    return $null
}
function Check-Background([string]$Stage) {
    # The user may switch their own foreground window while the fixture runs.
    # Only this owned hidden process is forbidden from becoming foreground.
    Require ([DateTimePickerUia]::Owner([DateTimePickerUia]::GetForegroundWindow()) -ne $script:process.Id) "Fixture became foreground during $Stage"
    Require (-not (Read-State).ever_native_active) "Hidden fixture received native activation during $Stage"
}
function Value-Of([System.Windows.Automation.AutomationElement]$Node) {
    return $Node.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
}
function Require-RegisteredClick([string]$Role, [string]$Label) {
    $candidates = @((Read-State).tree.nodes.PSObject.Properties.Value | Where-Object {
        $_.aria.role -eq $Role -and $_.aria.label -eq $Label
    })
    Require ($candidates.Count -eq 1 -and @($candidates[0].aria.on_action) -contains 'Click') "Native $Role $Label has no registered Click accessibility handler"
}
function Save-NativeTree([string]$Path) {
    $nodes = $script:window.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    $tree = @($nodes | ForEach-Object {
        $node = $_
        $patterns = @($node.GetSupportedPatterns() | ForEach-Object { $_.ProgrammaticName })
        $value = $null; $selected = $null; $rangeValue = $null
        if ($patterns -contains 'ValuePatternIdentifiers.Pattern') { $value = (Value-Of $node).Current.Value }
        if ($patterns -contains 'SelectionItemPatternIdentifiers.Pattern') { $selected = $node.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected }
        if ($patterns -contains 'RangeValuePatternIdentifiers.Pattern') {
            $range = $node.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern).Current
            $rangeValue = @{value=$range.Value;minimum=$range.Minimum;maximum=$range.Maximum;read_only=$range.IsReadOnly}
        }
        [ordered]@{ name=$node.Current.Name; id=$node.Current.AutomationId; role=$node.Current.ControlType.ProgrammaticName;
            enabled=$node.Current.IsEnabled; focusable=$node.Current.IsKeyboardFocusable; value=$value; selected=$selected; range_value=$rangeValue; patterns=$patterns }
    })
    $tree | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Path -Encoding utf8NoBOM
}
$exe = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/date_time_picker_accessibility.exe'
$taskBinaryHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
$results = @()
[ordered]@{status='running';started_utc=[DateTime]::UtcNow.ToString('o');build_skipped=[bool]$SkipBuild} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8NoBOM
try {
foreach ($locale in @('en_US', 'zh_CN', 'zh_TW')) {
    foreach ($component in @('date', 'time')) {
        $name = "$component-$locale"
        $script:statePath = Join-Path $output "$name-state.json"
        $script:foreground = [DateTimePickerUia]::GetForegroundWindow()
        $script:foregroundOwner = [DateTimePickerUia]::Owner($script:foreground)
        $script:process = Start-Process -FilePath $exe -ArgumentList @($script:statePath,$component,$locale) -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output "$name-stderr.txt")
        try {
            $script:handle = [IntPtr]::Zero
            Wait-Condition { $script:handle = [DateTimePickerUia]::Find($script:process.Id); $script:handle -ne [IntPtr]::Zero } 'Hidden native fixture did not create its HWND' 30000
            Require ([DateTimePickerUia]::Owner($script:handle) -eq $script:process.Id) 'HWND does not belong to fixture process'
            Require ([DateTimePickerUia]::InitializeSize($script:handle)) 'Could not initialize owned HWND without activation'
            $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
            Wait-Condition { $null -ne (Find-Node 'picker.date') -and $null -ne (Read-State) } 'Actual native UIA tree or fresh state did not appear' 30000
            Check-Background "$name ready"
            Save-NativeTree (Join-Path $output "$name-native-tree.json")
            $checks = @()
            $todayAria = $null
            $scrollbarReadOnly = @()
            foreach ($kind in @('date','time')) {
                $node = Node "picker.$kind"
                Require ($node.Current.ControlType -eq [System.Windows.Automation.ControlType]::ComboBox) "$kind trigger is not a native ComboBox"
                $checks += "$kind-combobox"
                $value = Value-Of $node
                $expectedValue = if ($kind -eq 'date') { '2024-02-12' } else { '13:24:35' }
                Require ($value.Current.Value -eq $expectedValue -and -not $value.Current.IsReadOnly) "$kind editable native value differs"
                $checks += "$kind-editable-value"
                $expanded = $node.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Current.ExpandCollapseState
                $expectedExpanded = if ($component -eq $kind) { [System.Windows.Automation.ExpandCollapseState]::Expanded } else { [System.Windows.Automation.ExpandCollapseState]::Collapsed }
                Require ($expanded -eq $expectedExpanded) "$kind expanded metadata differs from its visible popup"
                $checks += "$kind-expanded"
            }
            Require (-not (Node 'picker.disabled').Current.IsEnabled) 'Disabled date is enabled in native UIA'
            Require ((Value-Of (Node 'picker.read-only')).Current.IsReadOnly) 'Read-only date exposes an editable native value'
            Require ((Value-Of (Node 'picker.range.start')).Current.Value -eq '2024-02-08') 'Native range start value differs'
            Require ((Value-Of (Node 'picker.range.end')).Current.Value -eq '2024-02-21') 'Native range end value differs'
            $checks += @('disabled','read-only','independent-range-start','independent-range-end')
            $multiple = Node 'picker.multiple'
            Require ($multiple.Current.ControlType -eq [System.Windows.Automation.ControlType]::ComboBox) 'Multiple date selection is not a native ComboBox'
            $multipleValue = Value-Of $multiple
            $joinedDates = (($multipleValue.Current.Value -split ',') | ForEach-Object { $_.Trim() }) -join ','
            Require ($multipleValue.Current.IsReadOnly -and $joinedDates -eq '2024-02-08,2024-02-12,2024-02-21') 'Multiple date selection lacks its native read-only joined value'
            $checks += @('multiple-combobox','multiple-joined-read-only-value')
            if ($component -eq 'date') {
                $previous = switch ($locale) { 'zh_CN' { '上个月' } 'zh_TW' { '上個月' } default { 'Previous month' } }
                $nav = Named $previous
                Require ($nav.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'Localized calendar navigation is not a native button'
                $null = $nav.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
                $selected = Named '2024-02-12'
                Require ($selected.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected) 'Selected calendar cell lacks native selected state'
                Require-RegisteredClick 'GridCell' '2024-02-12'
                Require (-not (Named '2024-02-17').Current.IsEnabled) 'Disabled Saturday calendar cell is enabled'
                $today = Named '2024-02-15'
                $todayAria = [DateTimePickerUia]::AriaPropertiesByName($script:handle, '2024-02-15')
                Require ($todayAria -match 'current=date') "Today cell lacks native aria-current=date metadata: $todayAria"
                $checks += @('localized-navigation','navigation-invoke-available','calendar-selected','calendar-registered-click','calendar-disabled','calendar-current-date')
            } else {
                $labels = switch ($locale) { 'zh_CN' { @('小时','分钟','秒') } 'zh_TW' { @('小時','分鐘','秒') } default { @('Hour','Minute','Second') } }
                $values = @('13','24','35')
                for ($index=0; $index -lt 3; $index++) {
                    $column = Named $labels[$index]
                    Require ($column.Current.ControlType -eq [System.Windows.Automation.ControlType]::List) 'Time column is not a native List'
                    $selection = @($column.GetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern).Current.GetSelection())
                    Require ($selection.Count -eq 1 -and $selection[0].Current.Name -eq $values[$index]) 'Native time-column selection differs'
                    Require ($selection[0].Current.ControlType -eq [System.Windows.Automation.ControlType]::ListItem) 'Time value is not a native ListItem'
                    $null = $selection[0].GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
                    Require-RegisteredClick 'ListBoxOption' $values[$index]
                    $checks += "localized-time-column-$index-selection"
                    $checks += "time-column-$index-registered-click"
                }
                $bars = @($script:window.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                    [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                        [System.Windows.Automation.ControlType]::ScrollBar)) | Sort-Object { $_.Current.BoundingRectangle.Left })
                Require ($bars.Count -eq 3) 'Time columns do not expose three native ScrollBars'
                $maximums = @(644,1652,1652)
                $offsets = @(364,672,980)
                for ($index=0; $index -lt 3; $index++) {
                    $range = $bars[$index].GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern).Current
                    Require ($range.Minimum -eq 0 -and [Math]::Abs($range.Maximum-$maximums[$index]) -le 1 -and [Math]::Abs($range.Value-$offsets[$index]) -le 1) "Native time scrollbar $index differs: value=$($range.Value), min=$($range.Minimum), max=$($range.Maximum), readonly=$($range.IsReadOnly)"
                    $registered = @((Read-State).tree.nodes.PSObject.Properties.Value | Where-Object {
                        $_.aria.role -eq 'ScrollBar' -and [Math]::Abs($_.aria.numeric_value-$offsets[$index]) -le 1
                    })
                    Require ($registered.Count -eq 1 -and @($registered[0].aria.on_action) -contains 'SetValue' -and @($registered[0].aria.on_action) -contains 'SetScrollOffset') 'Time scrollbar setter handlers are not registered'
                    $scrollbarReadOnly += [bool]$range.IsReadOnly
                    $checks += "time-scrollbar-$index-range-value"
                    $checks += "time-scrollbar-$index-registered-setters"
                }
            }
            Check-Background "$name inspection completed"
            Read-State | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath (Join-Path $output "$name-inspection.json") -Encoding utf8NoBOM
            $foregroundAfter = [DateTimePickerUia]::GetForegroundWindow()
            $results += [ordered]@{name=$name;status='passed';checks=$checks;
                foreground_preserved=($foregroundAfter -eq $script:foreground);fixture_never_foreground=$true;
                foreground_before=@{handle=$script:foreground.ToInt64();owner=$script:foregroundOwner};
                foreground_after=@{handle=$foregroundAfter.ToInt64();owner=[DateTimePickerUia]::Owner($foregroundAfter)};
                native_activation_seen=(Read-State).ever_native_active;today_aria_properties=$todayAria;
                native_scrollbar_range_value_read_only=$scrollbarReadOnly}
        } finally { if (-not $script:process.HasExited) { Stop-Process -Id $script:process.Id }; $script:process.Dispose() }
    }
}
Assert-DateTimePickerSourceFingerprint $taskSources $crateRoot
Require ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() -eq $taskBinaryHash) 'UIA fixture binary changed during inspection'
[ordered]@{status='passed';profile='read-only-background-inspection';utc=[DateTime]::UtcNow.ToString('o');
    actuation='not run: no Invoke/Select/SetFocus or global input; registered Click handlers and native SelectionItem patterns inspected read-only';build_skipped=[bool]$SkipBuild;
    adapter_limitation='AccessKit consumer0.38.0 excludes ScrollBar from is_read_only_supported; native RangeValue IsReadOnly reports true despite registered setters. Numeric metadata and setters are inspected; UIA SetValue actuation is not claimed.';
    source_sha256=$taskSources;executable=@{path=$exe;sha256=$taskBinaryHash};toolchain=(Get-DateTimePickerToolchain);cases=$results} | ConvertTo-Json -Depth 20 |
    Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8NoBOM
Write-Output "Date/time picker native UIA background inspection passed all $($results.Count) profiles. Evidence: $output"
} catch {
    [ordered]@{status='failed';profile='read-only-background-inspection';utc=[DateTime]::UtcNow.ToString('o');
        error=$_.Exception.Message;cases=$results;source_sha256=$taskSources;executable=@{path=$exe;sha256=$taskBinaryHash}} |
        ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8NoBOM
    throw
}

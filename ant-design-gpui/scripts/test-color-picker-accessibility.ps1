# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([switch]$SkipBuild, [switch]$Interactive)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/color-picker-uia'
New-Item -ItemType Directory -Path $output -Force | Out-Null
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --features test-support --example color_picker_accessibility
        if ($LASTEXITCODE -ne 0) { throw 'ColorPicker UIA fixture build failed' }
    } finally { Pop-Location }
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('ColorPickerUia' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class ColorPickerUia {
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    private delegate bool Callback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr handle, StringBuilder title, int capacity);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr handle, out Rect bounds);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool SetWindowPos(IntPtr handle, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern IntPtr GetWindowLongPtr(IntPtr handle, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW", SetLastError = true)] private static extern IntPtr SetWindowLongPtr(IntPtr handle, int index, IntPtr value);
    public static uint Owner(IntPtr handle) { uint pid; GetWindowThreadProcessId(handle, out pid); return pid; }
    public static bool InitializeSize(IntPtr handle, bool backgroundOnly) {
        // Windows UIA may implicitly activate a window when invoking a button.
        // This fixture is intentionally background-only; apply no-activate to
        // its own HWND before exposing any native automation actions.
        if (backgroundOnly) {
            long style = GetWindowLongPtr(handle, -20).ToInt64();
            SetWindowLongPtr(handle, -20, new IntPtr(style | 0x08000000L));
        }
        Rect bounds; if (!GetWindowRect(handle, out bounds)) return false;
        int width = bounds.Right - bounds.Left, height = bounds.Bottom - bounds.Top;
        // The pinned backend's Window::resize omits SWP_NOACTIVATE. Generate
        // WM_SIZE on this fixture only, preserving z-order and foreground.
        const uint flags = 0x0002 | 0x0004 | 0x0010;
        return SetWindowPos(handle, IntPtr.Zero, 0, 0, width + 1, height, flags)
            && SetWindowPos(handle, IntPtr.Zero, 0, 0, width, height, flags);
    }
    public static IntPtr Find(int pid) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((handle, parameter) => { uint owner; GetWindowThreadProcessId(handle, out owner);
            if (owner == pid) { var title = new StringBuilder(256); GetWindowText(handle, title, title.Capacity);
                if (title.ToString() == "ColorPicker accessibility fixture") { result = handle; return false; }
            } return true;
        }, IntPtr.Zero);
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
function Node([string]$Id) { $node = Find-Node $Id; Require ($null -ne $node) "Missing native node: $Id"; return $node }
function Find-Name([string]$Name) {
    $condition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Name)
    return $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Named([string]$Name) { $node = Find-Name $Name; Require ($null -ne $node) "Missing native label: $Name"; return $node }
function Invoke-Node([string]$Id) { Run-Action "invoke $Id" { (Node $Id).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() } }
function Read-State {
    if (-not (Test-Path -LiteralPath $script:statePath)) { return $null }
    $stream = [IO.FileStream]::new($script:statePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { $state = $reader.ReadToEnd() | ConvertFrom-Json }
    finally { $reader.Dispose() }
    if ($state.process_id -eq $process.Id) { return $state }
    return $null
}
function Range([string]$Name) {
    $node = Named $Name
    Require ($node.Current.ControlType -eq [System.Windows.Automation.ControlType]::Slider) "$Name is not a native slider"
    return $node.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
}
function Save-Evidence([string]$Name) { Read-State | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (Join-Path $output "$Name.json") -Encoding utf8 }
function Check-Background([string]$Stage = 'checkpoint') {
    if ($Interactive) { return }
    if ([ColorPickerUia]::Owner([ColorPickerUia]::GetForegroundWindow()) -eq $process.Id -or (Read-State).ever_native_active) {
        [ordered]@{stage=$Stage;foreground_handle=[ColorPickerUia]::GetForegroundWindow().ToInt64();foreground_owner=[ColorPickerUia]::Owner([ColorPickerUia]::GetForegroundWindow());state=(Read-State)} | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (Join-Path $output 'foreground-failure.json') -Encoding utf8
    }
    Require ([ColorPickerUia]::Owner([ColorPickerUia]::GetForegroundWindow()) -ne $process.Id) "Hidden UIA fixture became the foreground application at $Stage"
    Require (-not (Read-State).ever_native_active) "Hidden fixture received native window activation during the run at $Stage"
}
function Run-Action([string]$Name, [scriptblock]$Action) {
    Require $Interactive 'Native UIA actuation can activate a Windows window; use the explicit -Interactive profile to run it'
    Check-Background "before $Name"
    $previousFrame = (Read-State).tree.frame.frame_number
    & $Action
    Wait-Condition { (Read-State).tree.frame.frame_number -gt $previousFrame } "Fixture did not render after $Name"
    Check-Background "after $Name"
}
$script:statePath = Join-Path $output 'state.json'
$exe = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/color_picker_accessibility.exe'
$foreground = [ColorPickerUia]::GetForegroundWindow()
$foregroundOwner = [ColorPickerUia]::Owner($foreground)
$fixtureArguments = @($script:statePath)
if ($Interactive) { $fixtureArguments += '--interactive' } else { $fixtureArguments += '--inspect' }
$process = Start-Process -FilePath $exe -ArgumentList $fixtureArguments -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output 'stderr.txt')
try {
    $script:handle = [IntPtr]::Zero
    Wait-Condition { $script:handle = [ColorPickerUia]::Find($process.Id); $script:handle -ne [IntPtr]::Zero } 'Fixture did not create its hidden native window' 30000
    Require ([ColorPickerUia]::Owner($script:handle) -eq $process.Id) 'Native handle does not belong to the fixture'
    Require ([ColorPickerUia]::InitializeSize($script:handle, (-not $Interactive))) 'Could not initialize fixture geometry without activation'
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    Wait-Condition { $null -ne (Find-Node 'picker.single') } 'Native ColorPicker tree did not appear' 30000
    Wait-Condition { $null -ne (Read-State) } 'Fixture did not publish a fresh snapshot'
    if (-not $Interactive) { Wait-Condition { (Read-State).background_guard[0] } 'Fixture did not install its local no-activate mouse guard' }
    Check-Background
    if (-not $Interactive) {
        Wait-Condition { $null -ne (Find-Name 'Hue') } 'The inspector did not expose a native popup'
        $single = Node 'picker.single'
        Require ($single.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'ColorPicker trigger is not a native button'
        Require (-not (Node 'picker.disabled').Current.IsEnabled) 'Disabled picker is enabled in native UIA'
        $expanded = $single.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
        Require ($expanded.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded) 'Trigger expanded metadata differs from its visible panel'
        $hue = Range 'Hue'
        Require ($hue.Current.Minimum -eq 0 -and $hue.Current.Maximum -eq 360 -and $hue.Current.SmallChange -eq 1) 'Native hue range metadata differs'
        $alpha = Range 'Alpha'
        Require ($alpha.Current.Minimum -eq 0 -and $alpha.Current.Maximum -eq 100) 'Native alpha range metadata differs'
        foreach ($label in @('Saturation', 'Brightness')) {
            $range = Range $label
            Require ($range.Current.Minimum -eq 0 -and $range.Current.Maximum -eq 100) "Native $label range metadata differs"
        }
        $hex = (Named 'Hex color').GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        Require ($hex.Current.Value -eq '1677FF' -and -not $hex.Current.IsReadOnly) 'Native HEX editor value or editability differs'
        $format = Named 'Color format'
        Require ($format.Current.ControlType -eq [System.Windows.Automation.ControlType]::ComboBox) 'Format selector is not a native combobox'
        $null = (Named 'Preset rgb(22,119,255)').GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $null = (Named 'Clear color').GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        Save-Evidence 'inspection'
        Check-Background 'after native inspection'
        $foregroundAfter = [ColorPickerUia]::GetForegroundWindow()
        [ordered]@{status='passed';profile='background-inspection';utc=[DateTime]::UtcNow.ToString('O');foreground_preserved=$true;
            native_activation_seen=(Read-State).ever_native_active;mouse_activation_intercepts=(Read-State).background_guard[1];
            foreground_before=@{handle=$foreground.ToInt64();owner=$foregroundOwner};
            foreground_after=@{handle=$foregroundAfter.ToInt64();owner=[ColorPickerUia]::Owner($foregroundAfter)};
            interactions='not run: Windows UIA Invoke activates the target window; use -Interactive explicitly';
            checks=@('native-button','expanded-metadata','disabled','hue-range','alpha-range','saturation-range','brightness-range','editable-hex-value','format-combobox','preset-invoke-available','clear-invoke-available')
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
        Write-Output "ColorPicker native UIA background inspection passed. Interactive actuation requires -Interactive. Evidence: $output"
        return
    }
    $single = Node 'picker.single'
    Require ($single.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'ColorPicker trigger is not a native button'
    Require (-not (Node 'picker.disabled').Current.IsEnabled) 'Disabled picker is enabled in UIA'
    Invoke-Node 'fixture.focus-trigger'
    Wait-Condition { (Read-State).trigger_focused } 'Native fixture could not focus its trigger'
    Check-Background 'focus trigger'
    $expand = $single.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    Run-Action 'expand color picker' { $expand.Expand() }
    Wait-Condition { $expand.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded } 'ColorPicker did not expand through native UIA'
    Wait-Condition { $null -ne (Find-Name 'Hue') } 'Native color rails are missing'
    Check-Background 'expand'
    $hue = Range 'Hue'
    Require ($hue.Current.Minimum -eq 0 -and $hue.Current.Maximum -eq 360 -and $hue.Current.SmallChange -eq 1) 'Hue range metadata differs'
    Run-Action 'set hue' { $hue.SetValue(180) }
    Wait-Condition { [Math]::Abs((Read-State).single_hsba[0] - 180) -lt 0.01 } 'Hue SetValue did not change the selected color'
    Check-Background 'hue range'
    $alpha = Range 'Alpha'
    Require ($alpha.Current.Maximum -eq 100) 'Alpha range metadata differs'
    Run-Action 'set alpha' { $alpha.SetValue(25) }
    Wait-Condition { [Math]::Abs((Read-State).single_hsba[3] - 0.25) -lt 0.001 } 'Alpha SetValue failed'
    Save-Evidence 'ranges'
    Check-Background
    Invoke-Node 'fixture.focus-input'
    Run-Action 'edit hex' { (Named 'Hex color').GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('ff0000') }
    Wait-Condition { (Read-State).single_hex -eq '#ff0000' } 'Native editable HEX value failed'
    Run-Action 'invoke preset' { (Named 'Preset rgb(22,119,255)').GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
    Wait-Condition { (Read-State).single_hex -eq '#1677ff' } 'Preset native Invoke failed'
    Run-Action 'collapse color picker' { $expand.Collapse() }
    Wait-Condition { -not (Read-State).open -and (Read-State).trigger_focused } 'Collapse did not restore native trigger focus'
    Save-Evidence 'collapsed'
    Check-Background
    Invoke-Node 'fixture.locale'
    Run-Action 'expand localized picker' { $expand.Expand() }
    Wait-Condition { $null -ne (Find-Name '色相') -and $null -ne (Find-Name '十六進位顏色') } 'Locale update did not reach native control names'
    Save-Evidence 'localized'
    Check-Background
    Run-Action 'collapse localized picker' { $expand.Collapse() }
    $gradient = Node 'picker.gradient'
    $gradientExpand = $gradient.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    Run-Action 'expand gradient' { $gradientExpand.Expand() }
    Wait-Condition { $null -ne (Find-Name '漸層色節點 3') } 'Gradient stops are missing from the native tree'
    $stop = Range '漸層色節點 2'
    Require ($stop.Current.Minimum -eq 0 -and $stop.Current.Maximum -eq 100) 'Gradient stop range metadata differs'
    Run-Action 'set gradient stop' { $stop.SetValue(60) }
    Invoke-Node 'fixture.focus-stop'
    Invoke-Node 'fixture.delete-stop'
    Wait-Condition { (Read-State).gradient_stops -eq 2 -and $null -eq (Find-Name '漸層色節點 3') } 'Deleted gradient stop remains in the native accessibility tree'
    Save-Evidence 'gradient'
    Check-Background
    $foregroundAfter = [ColorPickerUia]::GetForegroundWindow()
    [ordered]@{status='passed';profile='interactive';runtime=$PSVersionTable.PSVersion.ToString();utc=[DateTime]::UtcNow.ToString('O');foreground_preserved=$false;
        foreground_before=@{handle=$foreground.ToInt64();owner=$foregroundOwner};
        foreground_after=@{handle=$foregroundAfter.ToInt64();owner=[ColorPickerUia]::Owner($foregroundAfter)};
        native_activation_seen=(Read-State).ever_native_active;mouse_activation_intercepts=(Read-State).background_guard[1];
        checks=@('native-button','expand-collapse','disabled','hue-range','alpha-range','set-value','hex-edit','preset-invoke','focus-restoration','live-localization','gradient-ranges','gradient-removal')
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
    Write-Output "ColorPicker native UIA checks passed. Evidence: $output"
} finally { if (-not $process.HasExited) { Stop-Process -Id $process.Id }; $process.Dispose() }

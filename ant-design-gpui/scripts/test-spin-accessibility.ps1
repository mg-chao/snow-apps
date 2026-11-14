# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/spin-uia'
$summary = Join-Path $crateRoot 'fixtures/spin/accessibility-validation-windows.json'
New-Item -ItemType Directory -Path $output -Force | Out-Null
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example spin_accessibility
        if ($LASTEXITCODE -ne 0) { throw 'Spin accessibility fixture build failed' }
    } finally { Pop-Location }
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('SpinNativeA11y' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SpinNativeA11y {
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
    public static object Property(IntPtr handle, string automationId, int property) {
        var automation = (Automation)Activator.CreateInstance(Type.GetTypeFromCLSID(
            new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        Element root = null, element = null; object condition = null;
        try {
            automation.ElementFromHandle(handle, out root);
            automation.CreatePropertyCondition(30011, automationId, out condition);
            root.FindFirst(4, condition, out element);
            if (element == null) { throw new InvalidOperationException("Missing native Spin: " + automationId); }
            object value; element.GetCurrentPropertyValue(property, out value); return value;
        } finally {
            if (element != null) Marshal.ReleaseComObject(element);
            if (condition != null) Marshal.ReleaseComObject(condition);
            if (root != null) Marshal.ReleaseComObject(root);
            Marshal.ReleaseComObject(automation);
        }
    }
    private delegate bool Callback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr handle, StringBuilder title, int capacity);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr handle);
    public static IntPtr Find(int pid) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((handle, parameter) => {
            uint owner; GetWindowThreadProcessId(handle, out owner);
            if (owner == pid) {
                var title = new StringBuilder(256); GetWindowText(handle, title, title.Capacity);
                if (title.ToString() == "Spin hidden accessibility fixture " + pid) { result = handle; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
'@
}
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Wait-Condition([scriptblock]$Check, [string]$Message, [int]$Timeout = 10000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Timeout)
    while ([DateTime]::UtcNow -lt $deadline) { if (& $Check) { return }; Start-Sleep -Milliseconds 25 }
    throw $Message
}
function Read-State {
    if (-not (Test-Path -LiteralPath $script:statePath)) { return $null }
    try {
        $stream = [IO.FileStream]::new($script:statePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader = [IO.StreamReader]::new($stream)
        try { $state = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if ($state.process_id -eq $script:process.Id) { return $state }
    } catch {}
    return $null
}
function Find-Node([string]$Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Debug-Nodes($State, [string]$Role) {
    return @($State.tree.nodes.PSObject.Properties | ForEach-Object { $_.Value } | Where-Object { $_.aria.role -eq $Role })
}
$script:statePath = Join-Path $output 'state.json'
$commandPath = Join-Path $output 'command.json'
'{"stage":0}' | Set-Content -LiteralPath $commandPath -Encoding ascii
$exe = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/spin_accessibility.exe'
$script:process = Start-Process -FilePath $exe -ArgumentList @($script:statePath, $commandPath) -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output 'stderr.txt')
$null = $script:process.Handle
$checks = [Collections.Generic.List[string]]::new()
$snapshots = [Collections.Generic.List[object]]::new()
$status = 'failed'
try {
    $script:handle = [IntPtr]::Zero
    Wait-Condition { $script:handle = [SpinNativeA11y]::Find($script:process.Id); $script:handle -ne [IntPtr]::Zero } 'Hidden Spin HWND was not created' 30000
    Require (-not [SpinNativeA11y]::IsWindowVisible($script:handle)) 'Fixture showed a native window'
    Wait-Condition { $state = Read-State; $null -ne $state -and $state.a11y_active -and $null -ne $state.tree } 'Hidden Spin accessibility tree did not initialize' 15000
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    Wait-Condition { $null -ne (Find-Node 'spin.main') } 'Native Spin UIA root was not published' 15000
    foreach ($stage in 0..6) {
        @{stage=$stage} | ConvertTo-Json -Compress | Set-Content -LiteralPath $commandPath -Encoding ascii
        $isLoading = $stage -notin @(3,5)
        Wait-Condition {
            $state = Read-State
            if ($null -eq $state -or $state.stage -ne $stage) { return $false }
            $progress = Debug-Nodes $state 'ProgressIndicator'
            return ($isLoading -and $progress.Count -eq 1) -or (-not $isLoading -and $progress.Count -eq 0)
        } "Spin stage $stage did not commit its semantic tree"
        $state = Read-State
        $root = Find-Node 'spin.main'
        Require ($null -ne $root) "Missing Spin root at stage $stage"
        Require ($root.Current.ControlType -eq [System.Windows.Automation.ControlType]::Group) 'Spin root role is not Group'
        Require ($root.Current.Name -eq 'Working results') 'Caller accessible label was lost'
        Require ($root.Current.AutomationId -eq 'spin.main') 'Caller automation ID was lost'
        $aria = [string][SpinNativeA11y]::Property($script:handle, 'spin.main', 30102)
        $live = [SpinNativeA11y]::Property($script:handle, 'spin.main', 30135)
        $description = [string][SpinNativeA11y]::Property($script:handle, 'spin.main', 30159)
        Require ([int]$live -eq 1) 'Spin live setting is not Polite'
        Require ($description -eq 'Retrieving application data') 'Caller accessible description was lost'
        Require (($aria -match '(^|;)busy=true(;|$)') -eq $isLoading) "Busy metadata differs at stage $stage"
        Require ($null -ne (Find-Node 'spin.child')) "Retained child disappeared at stage $stage"
        Require ($null -ne (Find-Node 'spin.outside')) "Outside control disappeared at stage $stage"
        $progress = Debug-Nodes $state 'ProgressIndicator'
        $numeric = if ($stage -eq 1) { 75 } elseif ($stage -eq 4) { 65 } elseif ($stage -eq 6) { 42 } else { $null }
        $nativeRange = $null
        if ($isLoading) {
            $progressCondition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::ProgressBar)
            $nativeProgress = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $progressCondition)
            Require ($null -ne $nativeProgress) 'Native progress role is not ProgressBar'
            $range = $null
            $hasRange = $nativeProgress.TryGetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern, [ref]$range)
            Require ($progress[0].aria.label -eq 'Working results') 'Progress label was lost'
            Require ($progress[0].aria.description -eq 'Retrieving application data') 'Progress description was lost'
            if ($null -ne $numeric) {
                Require ($progress[0].aria.numeric_value -eq $numeric) 'Measured progress value differs'
                Require ($progress[0].aria.min_numeric_value -eq 0 -and $progress[0].aria.max_numeric_value -eq 100) 'Measured progress range differs'
                Require ($hasRange -and $range.Current.Value -eq $numeric -and $range.Current.Minimum -eq 0 -and $range.Current.Maximum -eq 100) 'Native UIA measured range differs'
                $nativeRange = [ordered]@{value=$range.Current.Value; minimum=$range.Current.Minimum; maximum=$range.Current.Maximum}
            } else {
                Require ($null -eq $progress[0].aria.numeric_value) 'Indeterminate progress exposes a measured value'
                Require (-not $hasRange) 'Native UIA exposes a range for indeterminate progress'
            }
        }
        $scenarioName = @('indeterminate','determinate','automatic','inactive','fullscreen','fullscreen-inactive','custom-indicator')[$stage]
        $snapshots.Add([ordered]@{stage=$stage; name=$scenarioName; fullscreen=($stage -in @(4,5)); native_role=$root.Current.ControlType.ProgrammaticName; busy=$isLoading; live_setting=[int]$live; native_aria_properties=$aria; label=$root.Current.Name; description=$description; automation_id=$root.Current.AutomationId; progress=@($progress | ForEach-Object {$_.aria}); native_numeric_range=$nativeRange; child_preserved=$true; outside_preserved=$true})
        $checks.Add("stage-$stage-native-group-label-description-id-busy-live-progress-and-children")
        $state | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $output "stage-$stage.json") -Encoding utf8
    }
    Require (-not [SpinNativeA11y]::IsWindowVisible($script:handle)) 'Fixture became visible during verification'
    $status = 'passed'
} catch {
    $reason = $_.Exception.Message
    if ((Read-State).a11y_active -ne $true -or $null -eq $script:window) { $status = 'unavailable' }
    Write-Warning $reason
} finally {
    '{"quit":true}' | Set-Content -LiteralPath $commandPath -Encoding ascii
    if (-not $script:process.WaitForExit(3000)) { $script:process.Kill() }
    $hashes = [ordered]@{}
    foreach ($file in @('examples/spin_accessibility.rs','scripts/test-spin-accessibility.ps1') + @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src/spin') -Filter '*.rs' | ForEach-Object { 'src/spin/' + $_.Name })) {
        $hashes[$file] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $file) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $evidence = [ordered]@{schema_version=1; status=$status; platform='windows'; mode='hidden-native-window'; keyboard='not-sent'; activation='WM_GETOBJECT UIA root through normal AccessKit registration'; utc=[DateTime]::UtcNow.ToString('O'); reason=$reason; checks=$checks.ToArray(); scenarios=$snapshots.ToArray(); binary_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant(); source_sha256=$hashes; limitations=@('Pinned GPUI debug JSON omits busy/live/author ID; those values are read through native Windows UIA COM properties.','Screen reader speech and interactive keyboard focus are not exercised by this hidden fixture.')}
    $json = ($evidence | ConvertTo-Json -Depth 25).Replace("`r`n", "`n")
    [IO.File]::WriteAllText((Join-Path $output 'result.json'), $json + "`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($summary, $json + "`n", [Text.UTF8Encoding]::new($false))
}
if ($status -ne 'passed') { throw "Spin native accessibility verification: $status" }
Write-Output 'Verified native Spin accessibility in seven hidden-window states.'

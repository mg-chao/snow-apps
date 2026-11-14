# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/message-uia'
$summary = Join-Path $crateRoot 'fixtures/message/accessibility-validation-windows.json'
New-Item -ItemType Directory -Path $output -Force | Out-Null
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example message_accessibility
        if ($LASTEXITCODE -ne 0) { throw 'Message accessibility fixture build failed' }
    } finally { Pop-Location }
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('MessageNativeA11y' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class MessageNativeA11y {
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
            if (element == null) { throw new InvalidOperationException("Missing native Message: " + automationId); }
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
                if (title.ToString() == "Message hidden accessibility fixture " + pid) { result = handle; return false; }
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
$exe = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/message_accessibility.exe'
$script:process = Start-Process -FilePath $exe -ArgumentList @($script:statePath, $commandPath) -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output 'stderr.txt')
$null = $script:process.Handle
$checks = [Collections.Generic.List[string]]::new()
$snapshots = [Collections.Generic.List[object]]::new()
$status = 'failed'
try {
    $script:handle = [IntPtr]::Zero
    Wait-Condition { $script:handle = [MessageNativeA11y]::Find($script:process.Id); $script:handle -ne [IntPtr]::Zero } 'Hidden Message HWND was not created' 30000
    Require (-not [MessageNativeA11y]::IsWindowVisible($script:handle)) 'Fixture showed a native window'
    Wait-Condition { $state = Read-State; $null -ne $state -and $state.a11y_active -and $null -ne $state.tree } 'Hidden Message accessibility tree did not initialize' 15000
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    foreach ($stage in 0..4) {
        @{stage=$stage} | ConvertTo-Json -Compress | Set-Content -LiteralPath $commandPath -Encoding ascii
        Wait-Condition {
            $state = Read-State
            if ($null -eq $state -or $state.stage -ne $stage) { return $false }
            $notice = Find-Node 'message.main'
            if ($stage -eq 3) { return $null -eq $notice }
            $expectedName = @('Saving document','Document saved','Save failed','','Document ready')[$stage]
            return $null -ne $notice -and $notice.Current.Name -eq $expectedName
        } "Message stage $stage did not commit its native tree"
        $state = Read-State
        $openingFocusPreserved = $state.focus_preserved
        Require $openingFocusPreserved 'A message took focus from the existing control'
        Require ($null -ne (Find-Node 'message.outside')) 'Underlying native control disappeared'
        $root = Find-Node 'message.main'
        $native = $null
        if ($stage -eq 3) {
            Require ($null -eq $root -and $state.active_messages -eq 0) 'Destroyed message remains accessible'
        } else {
            $expectedName = @('Saving document','Document saved','Save failed','','Document ready')[$stage]
            Require ($root.Current.Name -eq $expectedName) "Updated accessible name differs at stage $stage"
            Require ($root.Current.AutomationId -eq 'message.main') 'Message automation ID was lost'
            $live = [int][MessageNativeA11y]::Property($script:handle, 'message.main', 30135)
            $description = [string][MessageNativeA11y]::Property($script:handle, 'message.main', 30159)
            $expectedLive = if ($stage -eq 2) { 1 } else { 2 }
            $expectedRole = if ($stage -eq 2) { 'status' } else { 'alert' }
            $role = [string][MessageNativeA11y]::Property($script:handle, 'message.main', 30101)
            Require ($live -eq $expectedLive) 'Message live setting differs from its native role'
            Require ($role -eq $expectedRole) 'Message accessible role differs'
            Require ($description -eq 'Document operation feedback') 'Message description was lost'
            Require (-not $root.Current.IsKeyboardFocusable) 'Passive message unexpectedly became a keyboard stop'
            if ($stage -eq 4) {
                $action = Find-Node 'message.action'
                Require ($null -ne $action) 'Native content action was lost'
                $invoke = $null
                Require ($action.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$invoke)) 'Native content action does not expose InvokePattern'
                $invoke.Invoke()
                Wait-Condition { $current = Read-State; $null -ne $current -and $current.action_count -eq 1 } 'Native UIA invocation did not reach the content callback exactly once'
                $state = Read-State
                $checks.Add('rich-content-native-invoke-routes-once')
            }
            $native = [ordered]@{control_type=$root.Current.ControlType.ProgrammaticName; role=$role; label=$root.Current.Name; description=$description; live_setting=$live; focusable=$root.Current.IsKeyboardFocusable; automation_id=$root.Current.AutomationId}
        }
        $checks.Add("stage-$stage-live-content-update-focus-and-removal")
        $snapshots.Add([ordered]@{stage=$stage; active_messages=$state.active_messages; focus_preserved=$openingFocusPreserved; focus_preserved_after_action=$state.focus_preserved; action_count=$state.action_count; native=$native})
        $state | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $output "stage-$stage.json") -Encoding utf8
    }
    Require (-not [MessageNativeA11y]::IsWindowVisible($script:handle)) 'Fixture became visible'
    $status = 'passed'
} catch {
    $reason = $_.Exception.Message
    if ((Read-State).a11y_active -ne $true -or $null -eq $script:window) { $status = 'unavailable' }
    Write-Warning $reason
} finally {
    '{"quit":true}' | Set-Content -LiteralPath $commandPath -Encoding ascii
    if (-not $script:process.WaitForExit(3000)) { $script:process.Kill() }
    $hashes = [ordered]@{}
    foreach ($file in @('examples/message_accessibility.rs','scripts/test-message-accessibility.ps1','src/root.rs','src/lib.rs','src/config.rs','src/behavior/text.rs','Cargo.toml','Cargo.lock') + @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src/message') -Filter '*.rs' | ForEach-Object { 'src/message/' + $_.Name })) {
        $hashes[$file] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $file) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $evidence = [ordered]@{schema_version=1; status=$status; platform='windows'; mode='hidden-native-window'; keyboard='not-sent'; activation='WM_GETOBJECT UIA root through normal AccessKit registration'; utc=[DateTime]::UtcNow.ToString('O'); reason=$reason; checks=$checks.ToArray(); scenarios=$snapshots.ToArray(); binary_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant(); source_sha256=$hashes; limitations=@('Native UIA properties are queried independently of GPUI debug JSON.','Screen reader speech and global input are not exercised.')}
    $json = ($evidence | ConvertTo-Json -Depth 25).Replace("`r`n", "`n")
    [IO.File]::WriteAllText((Join-Path $output 'result.json'), $json + "`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($summary, $json + "`n", [Text.UTF8Encoding]::new($false))
}
if ($status -ne 'passed') { throw "Message native accessibility verification: $status" }
Write-Output 'Verified native Message accessibility in five hidden-window states.'

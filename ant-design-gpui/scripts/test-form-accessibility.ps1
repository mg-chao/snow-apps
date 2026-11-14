# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [ValidateSet('en_US', 'zh_CN', 'zh_TW')][string]$Locale,
    [ValidateSet('default', 'dark-compact-rtl')][string]$Profile
)
$ErrorActionPreference = 'Stop'
$taskCrate = Split-Path -Parent $PSScriptRoot
$taskOutput = Join-Path (Split-Path -Parent $taskCrate) 'build/ant-design-gpui/form-uia'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
function Sources {
    $hashes = [ordered]@{}
    $paths = @(& rg --files (Join-Path $taskCrate 'src') | Where-Object { $_.EndsWith('.rs') }) +
        @('Cargo.toml', 'Cargo.lock', 'examples/form_accessibility.rs', 'scripts/test-form-accessibility.ps1' | ForEach-Object { Join-Path $taskCrate $_ })
    foreach ($path in $paths) { $hashes[[IO.Path]::GetRelativePath($taskCrate, $path).Replace('\', '/')] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    return $hashes
}
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function NativeTreeField($Tree, [string]$Label) {
    return @($Tree.nodes.PSObject.Properties | ForEach-Object { $_.Value } | Where-Object {
        $_.aria.label -eq $Label -and $_.aria.role -in @('TextInput', 'PasswordInput')
    })
}
function Assert-SettledNativeTrees($State, [int]$ProcessId) {
    Require ($State.process_id -eq $ProcessId) 'Accessibility state belongs to a different fixture process'
    Require $State.a11y_active 'Native accessibility is not active'
    Require (@($State.settled_native_frames).Count -eq 4) 'Need four fresh native accessibility trees with unchanged field metadata'
    $previousFrame = -1
    $identities = @{}
    foreach ($tree in $State.settled_native_frames) {
        Require ($tree.frame.frame_number -gt $previousFrame) 'Settled accessibility tree snapshots are stale or repeated'
        $previousFrame = $tree.frame.frame_number
        Require ([Math]::Abs($tree.frame.viewport_size.width - 900.) -le 1.01 -and [Math]::Abs($tree.frame.viewport_size.height - 850.) -le 1.01) 'Hidden native viewport is not the requested logical client size'
        foreach ($label in @('Username', 'Account note', 'Password', 'Disabled field', 'Enabled override', 'Pending verification')) {
            $field = @(NativeTreeField $tree $label)
            Require ($field.Count -eq 1) "Settled native tree lost or duplicated field '$label' at frame $previousFrame"
            if ($identities.ContainsKey($label)) { Require ($identities[$label] -eq $field[0].accesskit_id) "Native field '$label' changed identity between settled frames" }
            else { $identities[$label] = $field[0].accesskit_id }
        }
        $note = @(NativeTreeField $tree 'Account note')[0]
        Require ($note.aria.description.Contains('Optional account note') -and $note.aria.description.Contains('Use a work address')) "Settled native frame $previousFrame lost help or extra description"
        Require (@(NativeTreeField $tree 'Hidden field').Count -eq 0) 'Hidden field leaked into a settled native tree'
    }
    Require ($State.tree.frame.frame_number -eq $previousFrame) 'Current native tree does not match the newest settled frame'
}
$taskSources = Sources
if (-not $SkipBuild) {
    Push-Location -LiteralPath $taskCrate
    try {
        & cargo build --locked --offline --features test-support --example form_accessibility
        if ($LASTEXITCODE -ne 0) { throw 'Form accessibility fixture build failed.' }
    } finally { Pop-Location }
}
$taskExecutable = Join-Path (Split-Path -Parent $taskCrate) 'build/ant-design-gpui/debug/examples/form_accessibility.exe'
$taskExecutableHash = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('FormUia' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
[ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface FormNativeAutomation {
    void CompareElements(); void CompareRuntimeIds(); void GetRootElement();
    [return: MarshalAs(UnmanagedType.Interface)] FormNativeElement ElementFromHandle(IntPtr handle);
    void ElementFromPoint(); void GetFocusedElement(); void GetRootElementBuildCache();
    void ElementFromHandleBuildCache(); void ElementFromPointBuildCache(); void GetFocusedElementBuildCache();
    void CreateTreeWalker(); void ControlViewWalker(); void ContentViewWalker(); void RawViewWalker();
    void RawViewCondition(); void ControlViewCondition(); void ContentViewCondition(); void CreateCacheRequest();
    void CreateTrueCondition(); void CreateFalseCondition();
    IntPtr CreatePropertyCondition(int property, [MarshalAs(UnmanagedType.Struct)] object value);
}
[ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface FormNativeElement {
    void SetFocus(); void GetRuntimeId();
    [return: MarshalAs(UnmanagedType.Interface)] FormNativeElement FindFirst(int scope, IntPtr condition);
    void FindAll(); void FindFirstBuildCache(); void FindAllBuildCache(); void BuildUpdatedCache();
    [return: MarshalAs(UnmanagedType.Struct)] object GetCurrentPropertyValue(int property);
    [return: MarshalAs(UnmanagedType.Struct)] object GetCurrentPropertyValueEx(int property, [MarshalAs(UnmanagedType.Bool)] bool ignoreDefaultValue);
}
public static class FormUia {
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    private delegate bool Callback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr handle, StringBuilder text, int capacity);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr handle, out Rect bounds);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr handle, out Rect bounds);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr handle);
    [DllImport("user32.dll")] private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr handle, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] private static extern IntPtr GetWindowLongPtr(IntPtr handle, int index);
    [DllImport("user32.dll", EntryPoint="SetWindowLongPtrW")] private static extern IntPtr SetWindowLongPtr(IntPtr handle, int index, IntPtr value);
    public static uint Owner(IntPtr handle) { uint pid; GetWindowThreadProcessId(handle, out pid); return pid; }
    public static IntPtr Find(int pid) {
        IntPtr result=IntPtr.Zero;
        EnumWindows((handle, parameter) => {
            if (Owner(handle)==pid) { var title=new StringBuilder(256); GetWindowText(handle,title,title.Capacity);
                if(title.ToString()=="Form accessibility fixture") { result=handle; return false; }
            } return true;
        },IntPtr.Zero); return result;
    }
    public static bool InitializeSize(IntPtr handle) {
        SetWindowLongPtr(handle,-20,new IntPtr(GetWindowLongPtr(handle,-20).ToInt64()|0x08000000L));
        IntPtr previous=SetThreadDpiAwarenessContext(new IntPtr(-4));
        if(previous==IntPtr.Zero) return false;
        try {
            Rect bounds,client; uint dpi=GetDpiForWindow(handle);
            if(dpi==0||!GetWindowRect(handle,out bounds)||!GetClientRect(handle,out client)) return false;
            int clientWidth=(int)Math.Round(900.0*dpi/96.0),clientHeight=(int)Math.Round(850.0*dpi/96.0);
            int width=clientWidth+bounds.Right-bounds.Left-client.Right+client.Left;
            int height=clientHeight+bounds.Bottom-bounds.Top-client.Bottom+client.Top;
            const uint flags=0x0002|0x0004|0x0010;
            return SetWindowPos(handle,IntPtr.Zero,0,0,width+1,height,flags)
                &&SetWindowPos(handle,IntPtr.Zero,0,0,width,height,flags)
                &&GetClientRect(handle,out client)&&client.Right-client.Left==clientWidth&&client.Bottom-client.Top==clientHeight;
        } finally { SetThreadDpiAwarenessContext(previous); }
    }
    public static object Property(IntPtr handle,string id,int property) {
        var automation=(FormNativeAutomation)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        FormNativeElement root=null,element=null; IntPtr condition=IntPtr.Zero;
        try {
            root=automation.ElementFromHandle(handle); condition=automation.CreatePropertyCondition(30011,id);
            element=root.FindFirst(4,condition); return element==null?null:element.GetCurrentPropertyValueEx(property,true);
        } finally {
            if(element!=null) Marshal.ReleaseComObject(element); if(condition!=IntPtr.Zero) Marshal.Release(condition);
            if(root!=null) Marshal.ReleaseComObject(root); Marshal.ReleaseComObject(automation);
        }
    }
}
'@
}
$taskCases = @()
try {
    foreach ($taskLocale in $(if ($Locale) { @($Locale) } else { @('en_US', 'zh_CN', 'zh_TW') })) {
        foreach ($taskProfile in $(if ($Profile) { @($Profile) } else { @('default', 'dark-compact-rtl') })) {
            $taskName = "$taskLocale-$taskProfile"
            $taskStatePath = Join-Path $taskOutput "$taskName-state.json"
            if (Test-Path -LiteralPath $taskStatePath) { Remove-Item -LiteralPath $taskStatePath }
            $taskProcess = Start-Process -FilePath $taskExecutable -ArgumentList @(('"' + $taskStatePath + '"'), $taskLocale, $taskProfile) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $taskOutput "$taskName.stdout.txt") -RedirectStandardError (Join-Path $taskOutput "$taskName.stderr.txt")
            $taskForeground = [FormUia]::GetForegroundWindow()
            try {
                $taskHandle = [IntPtr]::Zero
                $taskPublished = $false
                for ($taskAttempt=0; $taskAttempt -lt 100; $taskAttempt++) {
                    if ($taskProcess.HasExited) { throw "Form fixture exited before inspection: $taskName" }
                    $taskHandle = [FormUia]::Find($taskProcess.Id)
                    if ($taskHandle -ne [IntPtr]::Zero -and (Test-Path -LiteralPath $taskStatePath)) {
                        $taskInitialState = Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
                        if ($taskInitialState.process_id -eq $taskProcess.Id) { $taskPublished = $true; break }
                    }
                    Start-Sleep -Milliseconds 100
                }
                Require ($taskHandle -ne [IntPtr]::Zero) 'Missing owned Form fixture window'
                Require $taskPublished 'Owned fixture process did not publish fresh state'
                Require ([FormUia]::InitializeSize($taskHandle)) 'Cannot initialize hidden native viewport'
                $taskRoot = [System.Windows.Automation.AutomationElement]::FromHandle($taskHandle)
                $taskFind = { param($id) $taskRoot.FindFirst([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)) }
                $taskState = $null
                for ($taskAttempt=0; $taskAttempt -lt 100; $taskAttempt++) {
                    $null = & $taskFind 'form.username'
                    $taskCandidate = Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
                    if ($taskCandidate.process_id -eq $taskProcess.Id -and $taskCandidate.a11y_active -and @($taskCandidate.settled_native_frames).Count -eq 4) { $taskState = $taskCandidate; break }
                    Start-Sleep -Milliseconds 100
                }
                Require ($null -ne $taskState) 'Four current native accessibility trees were not published by the owned process'
                Assert-SettledNativeTrees $taskState $taskProcess.Id
                $taskUsername = $null
                $taskUsernameDescription = ''
                for ($taskAttempt=0; $taskAttempt -lt 100; $taskAttempt++) {
                    $taskUsername = & $taskFind 'form.username'
                    if ($taskUsername) { $taskUsernameDescription = [string][FormUia]::Property($taskHandle, 'form.username', 30159) }
                    if ($taskUsername -and $taskUsernameDescription) { break }
                    Start-Sleep -Milliseconds 100
                }
                Require ($null -ne $taskUsername) 'Form field is absent from native UIA'
                Require ($taskUsername.Current.Name -eq 'Username') 'Field label is detached from the real editor'
                Require ($taskUsername.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit) 'Field is not a native Edit'
                Require ($taskUsername.Current.IsRequiredForForm) 'Required field lacks native required metadata'
                $taskExpected = switch ($taskLocale) { 'zh_CN' { '请输入Username' } 'zh_TW' { '請輸入Username' } default { 'Please enter Username' } }
                Require ($taskUsernameDescription.Contains($taskExpected)) 'Localized validation message is absent from native FullDescription'
                $taskInvalid = [FormUia]::Property($taskHandle, 'form.username', 30103)
                # The pinned AccessKit Windows adapter exposes required/busy and
                # descriptions, but has no IsDataValidForForm implementation.
                # ignoreDefaultValue prevents a UIA default from passing as evidence.
                $taskInvalidSupported = $taskInvalid -is [bool]
                if ($taskInvalidSupported) { Require (-not $taskInvalid) 'Native invalid field reports valid data' }
                $taskNote = & $taskFind 'form.note'
                Require ($null -ne $taskNote) 'Optional field disappeared from native UIA after settling'
                Require (-not $taskNote.Current.IsRequiredForForm) 'Optional field is incorrectly required'
                $taskNoteDescription = [string][FormUia]::Property($taskHandle, 'form.note', 30159)
                Require ($taskNoteDescription.Contains('Optional account note') -and $taskNoteDescription.Contains('Use a work address')) 'Help/extra descriptions are detached from the native editor'
                $taskPassword = & $taskFind 'form.password'
                Require ($taskPassword.Current.IsPassword) 'Password field is not protected in native UIA'
                Require (-not (& $taskFind 'form.disabled').Current.IsEnabled) 'Disabled FormItem field is enabled'
                Require ((& $taskFind 'form.enabled').Current.IsEnabled) 'Explicit enabled child fails to override inherited disabled'
                Require ($null -eq (& $taskFind 'form.hidden')) 'Hidden field is exposed in native UIA'
                $taskBusy = [string][FormUia]::Property($taskHandle, 'form.pending', 30102)
                Require ($taskBusy -match 'busy=true') "Validating field lacks native busy metadata: $taskBusy"
                $taskState = Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
                Assert-SettledNativeTrees $taskState $taskProcess.Id
                Require ($taskState.native_nodes.username.invalid -eq 'True') 'Invalid flag is absent from the actual native AccessKit node'
                Require ($taskState.native_nodes.username.required) 'Actual native node lost required metadata'
                Require ($taskState.native_nodes.pending.busy) 'Actual native node lost busy metadata'
                Require (-not $taskState.ever_native_active) 'Hidden fixture received native activation'
                Require ([FormUia]::Owner([FormUia]::GetForegroundWindow()) -ne $taskProcess.Id) 'Fixture took the foreground'
                Require (-not (($taskState | ConvertTo-Json -Depth 60 -Compress).Contains('synthetic-form-password-do-not-export'))) 'Protected password leaked to accessibility text'
                $taskNodes = $taskRoot.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
                $taskTree = @($taskNodes | ForEach-Object { [ordered]@{id=$_.Current.AutomationId;name=$_.Current.Name;type=$_.Current.ControlType.ProgrammaticName;help=$_.Current.HelpText;enabled=$_.Current.IsEnabled;required=$_.Current.IsRequiredForForm;password=$_.Current.IsPassword;focusable=$_.Current.IsKeyboardFocusable} })
                $taskTree | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskOutput "$taskName-native-tree.json") -Encoding utf8NoBOM
                $taskCases += [ordered]@{name=$taskName;status='passed';checks=@('owned-window','fresh-process-state','hidden-viewport','correct-dpi-client-viewport','four-settled-native-tree-frames','stable-native-field-identities','field-present','field-label','native-edit','required','localized-full-description','actual-accesskit-invalid','actual-accesskit-required','actual-accesskit-busy','optional','help-and-extra','protected-password','disabled','enabled-override','hidden-field','busy','no-native-activation','never-foreground','no-password-export');settled_frame_numbers=@($taskState.settled_native_frames | ForEach-Object { $_.frame.frame_number });native_invalid_property_supported=$taskInvalidSupported;native_invalid_property_value=$(if($taskInvalidSupported){$taskInvalid}else{'unsupported'});busy_aria=$taskBusy;native_nodes=$taskState.native_nodes;fixture_never_foreground=$true;native_activation_seen=$taskState.ever_native_active;foreground_preserved=([FormUia]::GetForegroundWindow() -eq $taskForeground)}
            } finally { if (-not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }; $taskProcess.Dispose() }
        }
    }
    $taskFinalSources = Sources
    Require (@(Compare-Object @($taskSources.Keys) @($taskFinalSources.Keys)).Count -eq 0) 'Accessibility source manifest changed'
    foreach ($taskPath in $taskSources.Keys) { Require ($taskSources[$taskPath] -eq $taskFinalSources[$taskPath]) "Accessibility source changed during validation: $taskPath" }
    Require ((Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash.ToLowerInvariant() -eq $taskExecutableHash) 'Accessibility executable changed during validation'
    $taskResult = [ordered]@{status='passed';profile='read-only-background-inspection';completed_utc=[DateTime]::UtcNow.ToString('o');actuation='None: no Invoke/SetFocus/keyboard/global input';adapter_limitation='Pinned AccessKit Windows 0.34 does not implement UIA IsDataValidForForm. Invalid metadata is checked in its native AccessKit tree; localized errors/help use native UIA FullDescription. Unsupported property values are recorded, not counted as proof of OS invalid-state support.';build_skipped=[bool]$SkipBuild;source_sha256=$taskSources;executable=@{path=$taskExecutable;sha256=$taskExecutableHash};cases=$taskCases}
    $taskResult | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $taskOutput 'result.json') -Encoding utf8NoBOM
    Write-Output "Form native UIA inspection passed $($taskCases.Count) profiles. Evidence: $taskOutput"
} catch {
    [ordered]@{status='failed';completed_utc=[DateTime]::UtcNow.ToString('o');error=$_.Exception.Message;cases=$taskCases;source_sha256=$taskSources} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $taskOutput 'result.json') -Encoding utf8NoBOM
    throw
}

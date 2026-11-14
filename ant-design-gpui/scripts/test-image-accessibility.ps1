# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Verify native Image accessibility through process-owned UIA and targeted window messages.
#>
[CmdletBinding()]
param([switch]$SkipBuild, [string]$Executable)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/image-uia'
New-Item -ItemType Directory -Path $output -Force | Out-Null
# Never leave a previous failure's tree beside a newer native result.
$failureTree = Join-Path $output 'tree-on-failure.json'
if (Test-Path -LiteralPath $failureTree) { Remove-Item -LiteralPath $failureTree }
[ordered]@{status='running';utc=[DateTime]::UtcNow.ToString('O')} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
function Get-InputHashes {
    $paths = @('Cargo.toml','Cargo.lock','rust-toolchain.toml','.cargo/config.toml',
        'examples/image_accessibility.rs','scripts/test-image-accessibility.ps1')
    $paths += Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src') -Recurse -File -Filter '*.rs' |
        Where-Object { $_.Name -notmatch '(?:^|_)tests\.rs$' } |
        ForEach-Object { [IO.Path]::GetRelativePath($crateRoot,$_.FullName).Replace('\','/') }
    $paths += Get-ChildItem -LiteralPath (Join-Path $crateRoot 'resources/image') -File -Filter '*.png' |
        ForEach-Object { [IO.Path]::GetRelativePath($crateRoot,$_.FullName).Replace('\','/') }
    $hashes = [ordered]@{}
    foreach ($path in ($paths | Sort-Object -Unique)) {
        $hashes[$path] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $path) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $hashes
}
$inputHashes = Get-InputHashes
$receiptPath = Join-Path $output 'build-inputs.json'
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example image_accessibility --message-format=json-render-diagnostics |
            ForEach-Object {
                $message = $_ | ConvertFrom-Json
                if ($message.reason -eq 'compiler-artifact' -and $message.target.name -eq 'ant_design_gpui' -and
                    $message.target.kind -contains 'lib') {
                    $script:libraryPath = @($message.filenames | Where-Object { $_.EndsWith('.rlib') })[0]
                }
            }
        if ($LASTEXITCODE -ne 0) { throw 'Native Image accessibility fixture build failed.' }
    } finally { Pop-Location }
}
if (-not $Executable) {
    $Executable = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/image_accessibility.exe'
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Missing native Image fixture: $Executable" }
$binaryHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
if (-not $SkipBuild) {
    if (($inputHashes | ConvertTo-Json -Compress) -ne ((Get-InputHashes) | ConvertTo-Json -Compress)) {
        throw 'Image accessibility sources changed while compiling; repeat after implementation settles.'
    }
    if (-not $script:libraryPath) { throw 'Cargo did not report the native Image fixture library dependency.' }
    @{source_sha256=$inputHashes;binary_sha256=$binaryHash;library_path=$script:libraryPath;
        library_sha256=(Get-FileHash -LiteralPath $script:libraryPath -Algorithm SHA256).Hash.ToLowerInvariant()} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receiptPath -Encoding utf8
} else {
    if (-not (Test-Path -LiteralPath $receiptPath)) { throw 'Run Image accessibility validation without -SkipBuild once to record its executable receipt.' }
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json -AsHashtable
    if ($receipt.binary_sha256 -ne $binaryHash -or
        ($receipt.source_sha256 | ConvertTo-Json -Compress) -ne ($inputHashes | ConvertTo-Json -Compress)) {
        throw 'Image accessibility executable receipt does not match current sources. Run without -SkipBuild.'
    }
    $script:libraryPath = $receipt.library_path
    if (-not $script:libraryPath -or
        $receipt.library_sha256 -ne (Get-FileHash -LiteralPath $script:libraryPath -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw 'Image accessibility library no longer matches its executable receipt. Run without -SkipBuild.'
    }
}
$dependencyName = [IO.Path]::GetFileNameWithoutExtension($script:libraryPath) -replace '^lib',''
$dependencyInfo = Join-Path (Split-Path -Parent $script:libraryPath) ($dependencyName + '.d')
$binding = Select-String -LiteralPath $dependencyInfo -Pattern '^# env-dep:CARGO_MANIFEST_DIR=' |
    Select-Object -Last 1
if (-not $binding -or
    [IO.Path]::GetFullPath($binding.Line.Substring('# env-dep:CARGO_MANIFEST_DIR='.Length).Replace('\\','\')) -ne $crateRoot) {
    throw 'The linked library was compiled for another checkout. Rebuild the real native Image library and fixture.'
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('AntImageUia' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AntImageUia {
    private delegate bool Callback(IntPtr handle, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr handle, StringBuilder title, int capacity);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr handle, int command);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr handle, uint message, IntPtr key, IntPtr data);
    [DllImport("user32.dll")] private static extern uint MapVirtualKey(uint key, uint type);
    public static IntPtr Find(int processId) {
        IntPtr result=IntPtr.Zero;
        EnumWindows((handle, parameter)=>{
            uint owner; GetWindowThreadProcessId(handle,out owner);
            if (owner==(uint)processId) {
                var title=new StringBuilder(256); GetWindowText(handle,title,title.Capacity);
                if (title.ToString().StartsWith("Ant Design GPUI Image accessibility ")) {result=handle;return false;}
            }
            return true;
        },IntPtr.Zero);
        return result;
    }
    public static void Key(IntPtr handle, int key) {
        uint scan=MapVirtualKey((uint)key,0);
        long flags=1L|((long)scan<<16);
        if (key>=0x21 && key<=0x2E) flags|=1L<<24;
        if (!PostMessage(handle,0x0100,new IntPtr(key),new IntPtr(flags)) ||
            !PostMessage(handle,0x0101,new IntPtr(key),new IntPtr(flags|0xC0000000L)))
            throw new InvalidOperationException("Could not post key to the owned Image window.");
    }
    public static object Property(IntPtr handle,string id,int propertyId) {
        var automation=(IImageAutomation)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        IImageElement root=null, element=null; object condition=null;
        try {
            root=automation.ElementFromHandle(handle);
            condition=automation.CreatePropertyCondition(30011,id);
            element=root.FindFirst(4,condition);
            if (element==null) throw new InvalidOperationException("Missing native Image node: "+id);
            return element.GetCurrentPropertyValue(propertyId);
        } finally {
            if(element!=null) Marshal.ReleaseComObject(element);
            if(condition!=null) Marshal.ReleaseComObject(condition);
            if(root!=null) Marshal.ReleaseComObject(root);
            Marshal.ReleaseComObject(automation);
        }
    }
}
[ComImport,Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IImageAutomation {
    void CompareElements();void CompareRuntimeIds();void GetRootElement();
    [return:MarshalAs(UnmanagedType.Interface)] IImageElement ElementFromHandle(IntPtr handle);
    void ElementFromPoint();void GetFocusedElement();void GetRootElementBuildCache();
    void ElementFromHandleBuildCache();void ElementFromPointBuildCache();void GetFocusedElementBuildCache();
    void CreateTreeWalker();void GetControlViewWalker();void GetContentViewWalker();void GetRawViewWalker();
    void GetRawViewCondition();void GetControlViewCondition();void GetContentViewCondition();
    void CreateCacheRequest();void CreateTrueCondition();void CreateFalseCondition();
    [return:MarshalAs(UnmanagedType.Interface)] object CreatePropertyCondition(int id,[MarshalAs(UnmanagedType.Struct)] object value);
}
[ComImport,Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IImageElement {
    void SetFocus();void GetRuntimeId();
    [return:MarshalAs(UnmanagedType.Interface)] IImageElement FindFirst(int scope,[MarshalAs(UnmanagedType.Interface)] object condition);
    void FindAll();void FindFirstBuildCache();void FindAllBuildCache();void BuildUpdatedCache();
    [return:MarshalAs(UnmanagedType.Struct)] object GetCurrentPropertyValue(int id);
}
'@
}
function Require([bool]$Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Wait-Condition([scriptblock]$Check,[string]$Message,[int]$Timeout=5000) {
    $deadline=[DateTime]::UtcNow.AddMilliseconds($Timeout)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($script:process) {
            $script:process.Refresh()
            if ($script:process.HasExited) { throw "Image fixture exited: $($script:process.ExitCode)" }
        }
        if (& $Check) { return }
        Start-Sleep -Milliseconds 25
    }
    throw $Message
}
function Find-Node([string]$Id) {
    $condition=[System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty,$Id)
    return $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$condition)
}
function Node([string]$Id) { $node=Find-Node $Id; Require ($null -ne $node) "Missing Image node: $Id"; return $node }
function Has-Invoke($Node) {
    $pattern=$null
    return $Node.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,[ref]$pattern)
}
function Invoke-Node([string]$Id) { (Node $Id).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
function Test-Focused([string]$Id) { $node=Find-Node $Id; return $null -ne $node -and $node.Current.HasKeyboardFocus }
function Focus-Node([string]$Id) {
    Wait-Condition {
        $node=Find-Node $Id
        if ($null -eq $node) { return $false }
        if ($node.Current.HasKeyboardFocus) { return $true }
        $node.SetFocus()
        return $false
    } "Native focus did not reach $Id."
}
function Preview-Open { Wait-Condition { $null -ne (Find-Node 'image.preview') -and $null -ne (Find-Node 'image.preview.close') } 'Image preview did not open.' }
function Preview-Closed { Wait-Condition { $null -eq (Find-Node 'image.preview') } 'Image preview did not close.' }
function Count-Is([string]$Expected) {
    Wait-Condition { $node=Find-Node 'image.preview.count'; $null -ne $node -and $node.Current.Name -eq $Expected } "Preview count is not $Expected."
}
$checks=[Collections.Generic.List[string]]::new()
$process=Start-Process -FilePath $Executable -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output 'stderr.txt')
try {
    $script:handle=[IntPtr]::Zero
    Wait-Condition {
        $process.Refresh()
        if ($process.HasExited) { throw "Image fixture exited: $($process.ExitCode)" }
        $script:handle=[AntImageUia]::Find($process.Id)
        $script:handle -ne [IntPtr]::Zero
    } 'Native Image window was not created.' 30000
    # SW_SHOWNOACTIVATE exposes this process's accessibility provider without
    # sending input to, or requesting activation of, another desktop window.
    [AntImageUia]::ShowWindow($script:handle,4) | Out-Null
    $script:window=[System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    Wait-Condition { $node=Find-Node 'image.uia.thumbnail'; $null -ne $node -and (Has-Invoke $node) } 'Image thumbnail did not expose Invoke.' 30000
    Require ($script:window.Current.ProcessId -eq $process.Id) 'UIA root is not owned by this fixture.'
    $thumbnail=Node 'image.uia.thumbnail'
    Require ($thumbnail.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'Previewable thumbnail lost Button semantics.'
    Require ($thumbnail.Current.Name -eq 'Landscape preview') 'Thumbnail lost its alt label.'
    Require $thumbnail.Current.IsKeyboardFocusable 'Previewable thumbnail is not keyboard focusable.'
    $plain=Node 'image.uia.plain'
    Require ($plain.Current.ControlType -eq [System.Windows.Automation.ControlType]::Image) 'Display-only image lost Image semantics.'
    Require ($plain.Current.Name -eq 'Portrait display image') 'Display-only image lost its alt label.'
    Require (-not $plain.Current.IsKeyboardFocusable) 'Preview-disabled image is keyboard focusable.'
    Require (-not (Has-Invoke $plain)) 'Preview-disabled image advertises Invoke.'
    $checks.Add('thumbnail-and-display-only-semantics')
    Focus-Node 'image.uia.thumbnail'
    Invoke-Node 'image.uia.thumbnail'
    Preview-Open
    Require ((Node 'image.preview').Current.Name -eq 'Preview') 'Preview dialog lost its localized accessible name.'
    Require ([string][AntImageUia]::Property($script:handle,'image.preview',30101) -eq 'dialog') 'Preview lost native dialog role.'
    Require ((Node 'image.preview.image').Current.ControlType -eq [System.Windows.Automation.ControlType]::Image) 'Preview content lost Image role.'
    Require ((Node 'image.preview.image').Current.Name -eq 'Landscape preview') 'Preview image lost its alt label.'
    Require ($null -eq (Find-Node 'image.uia.plain')) 'Background image remains exposed during preview.'
    $expected=[ordered]@{'flip-y'='Flip vertically';'flip-x'='Flip horizontally';'rotate-left'='Rotate left';'rotate-right'='Rotate right';'zoom-out'='Zoom out';'zoom-in'='Zoom in';'close'='Close'}
    foreach ($name in $expected.Keys) {
        $node=Node "image.preview.$name"
        Require ($node.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) "$name lost its Button role."
        Require ($node.Current.Name -eq $expected[$name]) "$name has the wrong accessible name."
    }
    Require (-not (Node 'image.preview.zoom-out').Current.IsEnabled) 'Zoom out is enabled at minimum scale.'
    Require (-not (Has-Invoke (Node 'image.preview.zoom-out'))) 'Disabled zoom out advertises Invoke.'
    Invoke-Node 'image.preview.zoom-in'
    Wait-Condition { $node=Find-Node 'image.preview.zoom-out'; $null -ne $node -and $node.Current.IsEnabled } 'Zoom in failed to enable zoom out.'
    $checks.Add('invoke-named-dialog-image-and-actions')
    Focus-Node 'image.preview.close'
    for ($index=0;$index -lt 10;$index++) {
        [AntImageUia]::Key($script:handle,9)
        Start-Sleep -Milliseconds 40
        $focusedCondition=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::HasKeyboardFocusProperty,$true)
        $focused=$script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$focusedCondition)
        Require ($null -ne $focused -and $focused.Current.AutomationId.StartsWith('image.preview')) 'Tab escaped the preview branch.'
    }
    $checks.Add('native-tab-containment')
    Invoke-Node 'image.preview.close'
    Preview-Closed
    Wait-Condition { Test-Focused 'image.uia.thumbnail' } 'Preview did not restore its trigger focus.'
    $checks.Add('invoke-close-and-focus-return')
    foreach ($key in @(13,32)) {
        Focus-Node 'image.uia.thumbnail'
        [AntImageUia]::Key($script:handle,$key)
        Preview-Open
        [AntImageUia]::Key($script:handle,27)
        Preview-Closed
        Wait-Condition { Test-Focused 'image.uia.thumbnail' } 'Keyboard close did not restore its trigger.'
    }
    $checks.Add('targeted-enter-space-and-escape')
    Focus-Node 'image.uia.group'
    Invoke-Node 'image.uia.group'
    Preview-Open
    Count-Is '1 / 3'
    Require (-not (Node 'image.preview.previous').Current.IsEnabled) 'Previous is enabled at the first image.'
    Require (-not (Has-Invoke (Node 'image.preview.previous'))) 'Disabled previous advertises Invoke.'
    Invoke-Node 'image.preview.next'
    Count-Is '2 / 3'
    [AntImageUia]::Key($script:handle,39)
    Count-Is '3 / 3'
    Require (-not (Node 'image.preview.next').Current.IsEnabled) 'Next is enabled at the last image.'
    [AntImageUia]::Key($script:handle,39)
    Start-Sleep -Milliseconds 60
    Count-Is '3 / 3'
    Invoke-Node 'image.preview.previous'
    Count-Is '2 / 3'
    [AntImageUia]::Key($script:handle,27)
    Preview-Closed
    Wait-Condition { Test-Focused 'image.uia.group' } 'Collection preview did not restore its trigger.'
    $checks.Add('bounded-group-invoke-and-arrow-navigation')
    Focus-Node 'image.uia.nested.open'
    Invoke-Node 'image.uia.nested.open'
    Wait-Condition { $null -ne (Find-Node 'image.uia.parent.dialog') } 'Parent dialog did not open.'
    Wait-Condition { $node=Find-Node 'image.uia.nested.thumbnail'; $null -ne $node -and (Has-Invoke $node) } 'Nested thumbnail is not invokable.'
    Focus-Node 'image.uia.nested.thumbnail'
    Invoke-Node 'image.uia.nested.thumbnail'
    Preview-Open
    Require ((Node 'image.preview.image').Current.Name -eq 'Nested portrait preview') 'Nested preview opened the wrong image.'
    [AntImageUia]::Key($script:handle,27)
    Preview-Closed
    Require ($null -ne (Find-Node 'image.uia.parent.dialog')) 'Closing nested preview also closed its parent.'
    Wait-Condition { Test-Focused 'image.uia.nested.thumbnail' } 'Nested preview did not restore its thumbnail focus.'
    [AntImageUia]::Key($script:handle,27)
    Wait-Condition { $null -eq (Find-Node 'image.uia.parent.dialog') } 'Escape did not close the parent dialog.'
    Wait-Condition { Test-Focused 'image.uia.nested.open' } 'Parent dialog did not restore its trigger.'
    $checks.Add('nested-dialog-layering-and-focus-return')
    $script:window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
    Require ($process.WaitForExit(5000)) 'Image fixture did not exit normally.'
    Require ($process.ExitCode -eq 0) 'Image fixture teardown failed.'
    $checks.Add('window-teardown')
    if (($inputHashes | ConvertTo-Json -Compress) -ne ((Get-InputHashes) | ConvertTo-Json -Compress)) {
        throw 'Image accessibility sources changed during native validation.'
    }
    [ordered]@{status='passed';utc=[DateTime]::UtcNow.ToString('O');checks=@($checks);executable_sha256=$binaryHash;library_path=$script:libraryPath;library_sha256=(Get-FileHash -LiteralPath $script:libraryPath -Algorithm SHA256).Hash.ToLowerInvariant();manifest_dir_dependency=$crateRoot;source_sha256=$inputHashes;sources_unchanged_during_run=$true;input='process-owned UIA and targeted PostMessage; no global input';limitations=@('Screen reader speech and physical OS keyboard routing are not exercised.')} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
    Write-Output "Image native UIA passed. Evidence: $output"
} catch {
    if ($script:window) {
        try {
            $nativeNodes=$script:window.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition)
            $nativeTree=foreach ($nativeNode in $nativeNodes) {
                [ordered]@{id=$nativeNode.Current.AutomationId;name=$nativeNode.Current.Name;type=$nativeNode.Current.ControlType.ProgrammaticName;enabled=$nativeNode.Current.IsEnabled;focusable=$nativeNode.Current.IsKeyboardFocusable;focused=$nativeNode.Current.HasKeyboardFocus}
            }
            $nativeTree | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'tree-on-failure.json') -Encoding utf8
        } catch { }
    }
    [ordered]@{status='failed';utc=[DateTime]::UtcNow.ToString('O');checks=@($checks);error=$_.Exception.Message;
        executable_sha256=$binaryHash;source_sha256=$inputHashes;
        native_stderr=$(if (Test-Path -LiteralPath (Join-Path $output 'stderr.txt')) { Get-Content -LiteralPath (Join-Path $output 'stderr.txt') -Raw })} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding utf8
    throw
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    $process.Dispose()
}

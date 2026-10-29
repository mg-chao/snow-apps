# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$TargetDirectory,
    [string]$OutputDirectory,
    [string]$SummaryPath
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($TargetDirectory)) {
    $TargetDirectory = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui'
}
$TargetDirectory = [IO.Path]::GetFullPath($TargetDirectory)
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/carousel-uia'
}
if ([string]::IsNullOrWhiteSpace($SummaryPath)) {
    $SummaryPath = Join-Path $crateRoot 'fixtures/carousel/accessibility-validation-windows.json'
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
$summary = [IO.Path]::GetFullPath($SummaryPath)
New-Item -ItemType Directory -Path $output -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $summary) -Force | Out-Null
function Get-InputSources {
    @('Cargo.toml','Cargo.lock','README.md','rust-toolchain.toml','.cargo/config.toml','examples/carousel_accessibility.rs','scripts/test-carousel-accessibility.ps1') +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src') -Recurse -File -Filter '*.rs' | Sort-Object FullName | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot,$_.FullName).Replace('\','/') }) +
        @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'resources') -Recurse -File | Sort-Object FullName | ForEach-Object { [IO.Path]::GetRelativePath($crateRoot,$_.FullName).Replace('\','/') })
}
$sources = @(Get-InputSources)
$inputHashes = [ordered]@{}
foreach ($source in $sources) {
    $inputHashes[$source] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $source) -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-InputHashes {
    if (@(Compare-Object $sources @(Get-InputSources)).Count -ne 0) { throw 'Carousel accessibility input manifest changed during validation.' }
    foreach ($source in $sources) {
        if ($inputHashes[$source] -ne (Get-FileHash -LiteralPath (Join-Path $crateRoot $source) -Algorithm SHA256).Hash.ToLowerInvariant()) {
            throw "A Carousel accessibility input changed during validation: $source"
        }
    }
}
$exe = Join-Path $TargetDirectory 'debug/examples/carousel_accessibility.exe'
$receiptPath = Join-Path $output 'build-inputs.json'
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example carousel_accessibility --target-dir $TargetDirectory
        if ($LASTEXITCODE -ne 0) { throw 'Carousel accessibility fixture build failed' }
    } finally { Pop-Location }
    Assert-InputHashes
    $binaryHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    @{source_sha256=$inputHashes; binary_sha256=$binaryHash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receiptPath -Encoding utf8
} else {
    if (-not (Test-Path -LiteralPath $receiptPath)) { throw 'Run Carousel accessibility validation without -SkipBuild once to record an executable receipt.' }
    $receipt = Get-Content -Raw -LiteralPath $receiptPath | ConvertFrom-Json
    if (@($receipt.source_sha256.PSObject.Properties).Count -ne $sources.Count) { throw 'Carousel executable receipt has a different input manifest.' }
    foreach ($source in $sources) {
        if ($receipt.source_sha256.$source -ne $inputHashes[$source]) { throw "Carousel executable receipt does not match current input: $source" }
    }
    $binaryHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($receipt.binary_sha256 -ne $binaryHash) { throw 'Carousel executable no longer matches its build receipt.' }
}
Assert-InputHashes
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('CarouselNativeA11y' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CarouselNativeA11y {
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
        return Query(handle, automationId, property, 30011);
    }
    public static object NamedProperty(IntPtr handle, string name, int property) {
        return Query(handle, name, property, 30005);
    }
    private static object Query(IntPtr handle, string query, int property, int selector) {
        var automation = (Automation)Activator.CreateInstance(Type.GetTypeFromCLSID(
            new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        Element root = null, element = null; object condition = null;
        try {
            automation.ElementFromHandle(handle, out root);
            automation.CreatePropertyCondition(selector, query, out condition);
            root.FindFirst(4, condition, out element);
            if (element == null) { throw new InvalidOperationException("Missing native Carousel: " + query); }
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
                if (title.ToString() == "Carousel hidden accessibility fixture " + pid) { result = handle; return false; }
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
function Find-Control([string]$Label) {
    $condition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Label)
    return (Find-Node 'carousel.main').FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Invoke-Control($Control) {
    Require ($null -ne $Control) 'Native Carousel control was not exposed'
    $invoke = $null
    Require ($Control.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$invoke)) 'Native Carousel control does not expose InvokePattern'
    $invoke.Invoke()
}
function Assert-ActiveContent([int]$Index) {
    foreach ($candidate in 0..2) {
        $content = Find-Node "carousel.content.$candidate"
        if ($candidate -eq $Index) {
            Require ($null -ne $content) "Active slide $candidate native content was not exposed"
        } else {
            Require ($null -eq $content) "Inactive slide $candidate remains in the native accessibility tree"
        }
    }
}
function Save-Snapshot([string]$Name) {
    $state = Read-State
    $state | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $output "$Name.json") -Encoding utf8
    $root=Find-Node 'carousel.main'
    Require ($root.Current.ControlType -eq [System.Windows.Automation.ControlType]::Group) 'Carousel region has the wrong native control type'
    $nativeControls=[Collections.Generic.List[object]]::new()
    $labels=if ($state.stage -eq 0) {@('Previous slide','Next slide','Go to slide {index}')} elseif ($state.stage -eq 5) {@('上一張','下一張','跳至第 {index} 張')} else {@('上一张','下一张','转到第 {index} 张')}
    foreach ($label in $labels[0..1]) {
        $control=Find-Control $label
        if ($null -ne $control) {
            Require ($control.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'Carousel arrow has the wrong native control type'
            $nativeControls.Add([ordered]@{label=$label; control_type=$control.Current.ControlType.ProgrammaticName})
        }
    }
    if ($state.count -gt 1) {
        foreach ($index in 0..($state.count-1)) {
            $label=$labels[2].Replace('{index}',($index+1).ToString())
            $dot=Find-Control $label
            Require ($null -ne $dot) "Carousel dot $index was not exposed"
            Require ($dot.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'Carousel dot has the wrong native control type'
            # The managed UIA assembly does not register newer property IDs.
            # Query the real provider through IUIAutomationElement directly.
            $aria=[string][CarouselNativeA11y]::NamedProperty($script:handle,$label,30102)
            $current=$aria -match '(^|;)current=true(;|$)'
            Require ($current -eq ($index -eq $state.current)) 'Carousel native current indicator differs from the active slide'
            $nativeControls.Add([ordered]@{label=$label; control_type=$dot.Current.ControlType.ProgrammaticName; aria_properties=$aria; current=$current})
        }
    }
    $nativeContent=$null
    if ($state.count -gt 0) {
        $content=Find-Node "carousel.content.$($state.current)"
        Require ($null -ne $content -and $content.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button) 'Active slide content has the wrong native control type'
        $nativeContent=[ordered]@{automation_id=$content.Current.AutomationId; label=$content.Current.Name; control_type=$content.Current.ControlType.ProgrammaticName; focusable=$content.Current.IsKeyboardFocusable; has_keyboard_focus=$content.Current.HasKeyboardFocus}
    }
    $native=[ordered]@{label=$root.Current.Name; control_type=$root.Current.ControlType.ProgrammaticName; controls=$nativeControls.ToArray(); content=$nativeContent}
    $script:snapshots.Add([ordered]@{name=$Name; current=$state.current; count=$state.count; outside_focus=$state.outside_focus; content_focus=$state.content_focus; action_count=$state.action_count; events=$state.events; native=$native})
}
function Write-Command($Value) {
    $temporary=$script:commandPath+'.tmp'
    [IO.File]::WriteAllText($temporary,($Value | ConvertTo-Json -Compress),[Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary,$script:commandPath,$true)
}
function Set-Stage([int]$Stage, [string]$Label) {
    Write-Command @{stage=$Stage}
    Wait-Condition { $state=Read-State; $null -ne $state -and $state.stage -eq $Stage -and (Find-Node 'carousel.main').Current.Name -eq $Label } "Carousel stage $Stage did not commit its native tree"
}
$script:statePath = Join-Path $output 'state.json'
$script:commandPath = Join-Path $output 'command.json'
Write-Command @{stage=0}
$script:process = Start-Process -FilePath $exe -ArgumentList @($script:statePath, $script:commandPath) -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output 'stderr.txt')
$null = $script:process.Handle
$checks = [Collections.Generic.List[string]]::new()
$script:snapshots = [Collections.Generic.List[object]]::new()
$status = 'failed'
try {
    $script:handle = [IntPtr]::Zero
    Wait-Condition { $script:handle = [CarouselNativeA11y]::Find($script:process.Id); $script:handle -ne [IntPtr]::Zero } 'Hidden Carousel HWND was not created' 30000
    Require (-not [CarouselNativeA11y]::IsWindowVisible($script:handle)) 'Fixture showed a native window'
    Wait-Condition { $state=Read-State; $null -ne $state -and $state.stage -eq 0 -and $state.a11y_active -and $null -ne $state.tree } 'Hidden Carousel accessibility tree did not initialize' 15000
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    $root=Find-Node 'carousel.main'
    Require ($root.Current.Name -eq 'Carousel') 'Carousel accessible name differs'
    Require (([string][CarouselNativeA11y]::Property($script:handle,'carousel.main',30159)) -eq 'Featured documents') 'Carousel description was lost'
    Require ((Read-State).outside_focus) 'Mounting a carousel took focus'
    Assert-ActiveContent 0
    foreach ($iteration in 1..4) {
        $previousFrame=(Read-State).frame
        Wait-Condition { $state=Read-State; $null -ne $state -and $state.frame -gt $previousFrame } 'Unchanged native accessibility redraw did not complete'
        Assert-ActiveContent 0
        $contentInvoke=$null
        Require ((Find-Node 'carousel.content.0').TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,[ref]$contentInvoke)) 'Warm retained content lost its native activation action'
    }
    Require ($null -eq (Find-Control 'Previous slide')) 'Finite start exposes an unavailable previous arrow'
    Require ($null -ne (Find-Control 'Next slide')) 'Finite start hides its next arrow'
    Save-Snapshot 'initial'
    $checks.Add('late-accessibility-activation-and-four-unchanged-frames-retain-native-content-and-actions')
    $checks.Add('native-root-label-description-and-inactive-content-exclusion')

    Invoke-Control (Find-Control 'Next slide')
    Wait-Condition { $state=Read-State; $null -ne $state -and $state.current -eq 1 -and @($state.events | Where-Object {$_.kind -eq 'after'}).Count -eq 1 } 'UIA arrow invocation did not navigate once'
    Assert-ActiveContent 1
    $state=Read-State
    Require (@($state.events).Count -eq 2) 'One native invocation produced duplicate callbacks'
    Require ($state.events[0].kind -eq 'before' -and $state.events[1].kind -eq 'after' -and $state.events[1].source -eq 'Accessibility') 'Native invocation callback order or source differs'
    Require (-not $state.content_focus[0]) 'UIA navigation left focus in inactive slide content'
    Save-Snapshot 'native-next'
    $checks.Add('native-arrow-invoke-and-exactly-once-before-after-callbacks')

    Invoke-Control (Find-Control 'Go to slide 3')
    Wait-Condition { $state=Read-State; $null -ne $state -and $state.current -eq 2 -and @($state.events | Where-Object {$_.kind -eq 'after'}).Count -eq 2 } 'UIA dot invocation did not select slide 3'
    Assert-ActiveContent 2
    Require ($null -eq (Find-Control 'Next slide')) 'Finite end exposes an unavailable next arrow'
    Require ($null -ne (Find-Control 'Previous slide')) 'Finite end hides its previous arrow'
    Invoke-Control (Find-Node 'carousel.content.2')
    Wait-Condition { $state=Read-State; $null -ne $state -and $state.action_count -eq 1 } 'Active rich content did not receive native invocation exactly once'
    Save-Snapshot 'native-last-dot-and-content'
    $checks.Add('native-dot-content-invocation-and-finite-edge-exclusion')

    Set-Stage 1 '轮播图'
    Wait-Condition { $state=Read-State; $state.content_focus[2] -and (Find-Node 'carousel.content.2').Current.IsKeyboardFocusable } 'Retained active content lost logical focus or native focusability during locale and direction update'
    Assert-ActiveContent 2
    Require ((Read-State).current -eq 2) 'Direction update changed the logical slide index'
    Require ($null -ne (Find-Control '上一张') -and $null -eq (Find-Control '下一张')) 'Simplified Chinese arrows have incorrect finite semantics'
    Require ($null -ne (Find-Control '转到第 2 张')) 'Localized dot label was lost'
    Save-Snapshot 'locale-rtl-retained-focus'
    $checks.Add('retained-content-focus-and-logical-index-survive-rtl-and-locale-change')

    Set-Stage 2 '轮播图'
    Wait-Condition { $state=Read-State; $state.current -eq 1 -and $state.count -eq 2 -and -not $state.content_focus[2] } 'Removing the focused last slide did not clamp selection and release its focus'
    Assert-ActiveContent 1
    Require ($null -eq (Find-Control '转到第 3 张')) 'Removed dot remains accessible'
    Save-Snapshot 'removed-focused-slide'
    $checks.Add('removing-focused-slide-clamps-selection-and-excludes-removed-content')

    foreach ($stage in 3..4) {
        Set-Stage $stage '轮播图'
        Wait-Condition { $state=Read-State; $state.count -eq (4-$stage) } 'Empty/single item update did not settle'
        Assert-ActiveContent $(if ($stage -eq 3) {0} else {-1})
        Require ($null -eq (Find-Control '上一张') -and $null -eq (Find-Control '下一张') -and $null -eq (Find-Control '转到第 1 张')) 'Empty/single carousel exposes navigation controls'
        Save-Snapshot "stage-$stage"
    }
    $checks.Add('single-and-empty-carousels-expose-no-navigation-controls')

    Set-Stage 5 '輪播圖'
    Wait-Condition { $state=Read-State; $state.count -eq 3 -and $state.current -eq 0 } 'Repopulated carousel did not select its first slide'
    Assert-ActiveContent 0
    Require ($null -ne (Find-Control '下一張') -and $null -ne (Find-Control '跳至第 2 張')) 'Traditional Chinese native labels were lost'
    Invoke-Control (Find-Control '下一張')
    Wait-Condition { $state=Read-State; $state.current -eq 1 } 'RTL native next invocation changed the wrong logical index'
    Assert-ActiveContent 1
    Save-Snapshot 'traditional-rtl-native-next'
    $checks.Add('traditional-chinese-rtl-next-invocation-preserves-child-order-indices')
    $checks.Add('native-region-button-control-types-and-unique-current-dot-state')
    Require (-not [CarouselNativeA11y]::IsWindowVisible($script:handle)) 'Fixture became visible'
    Assert-InputHashes
    Require ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() -eq $binaryHash) 'Carousel executable changed during validation'
    $status='passed'
} catch {
    $reason=$_.Exception.Message
    if ((Read-State).a11y_active -ne $true -or $null -eq $script:window) {$status='unavailable'}
    Write-Warning $reason
} finally {
    Write-Command @{quit=$true}
    if (-not $script:process.WaitForExit(3000)) {$script:process.Kill()}
    try { Assert-InputHashes } catch { $status='failed'; $reason=$_.Exception.Message }
    if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() -ne $binaryHash) { $status='failed'; $reason='Carousel executable changed during validation' }
    $evidence=[ordered]@{schema_version=1; status=$status; platform='windows'; source_root=$crateRoot; mode='hidden-native-window'; keyboard='not-sent'; activation='WM_GETOBJECT UIA root through normal AccessKit registration'; utc=[DateTime]::UtcNow.ToString('O'); reason=$reason; checks=$checks.ToArray(); scenarios=$script:snapshots.ToArray(); binary_sha256=$binaryHash; source_sha256=$inputHashes; limitations=@('Native UIA labels, control types, current state and InvokePattern are queried independently of GPUI debug JSON.','The hidden HWND stays inactive: retained logical focus and native focusability are checked without activating the host window; raw UIA focus properties are recorded.','Screen reader speech and global input are not exercised.')}
    $json=($evidence | ConvertTo-Json -Depth 25).Replace("`r`n","`n")
    [IO.File]::WriteAllText((Join-Path $output 'result.json'),$json+"`n",[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($summary,$json+"`n",[Text.UTF8Encoding]::new($false))
}
if ($status -ne 'passed') {throw "Carousel native accessibility verification: $status"}
Write-Output 'Verified native Carousel invocation, labels, focus, finite edges and active content in six hidden-window states.'

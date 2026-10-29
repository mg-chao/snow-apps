# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([switch]$SkipBuild, [string]$Executable)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/descriptions-uia'
$summary = Join-Path $crateRoot 'fixtures/descriptions/accessibility-validation-windows.json'
New-Item -ItemType Directory -Path $output -Force | Out-Null
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example descriptions_accessibility
        if ($LASTEXITCODE -ne 0) { throw 'Descriptions accessibility fixture build failed' }
    } finally { Pop-Location }
}
if (-not $Executable) { $Executable = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/descriptions_accessibility.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "Missing fixture: $Executable" }
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (-not ('DescriptionsNativeA11y' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class DescriptionsNativeA11y {
    private delegate bool Callback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder title, int capacity);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    public static IntPtr Find(int processId) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((window, parameter) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner == processId) {
                var title = new StringBuilder(256); GetWindowText(window, title, title.Capacity);
                if (title.ToString() == "Descriptions hidden accessibility fixture " + processId) {
                    result = window; return false;
                }
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
    while ([DateTime]::UtcNow -lt $deadline) {
        $script:fixtureProcess.Refresh()
        if ($script:fixtureProcess.HasExited) { throw "Descriptions fixture exited: $($script:fixtureProcess.ExitCode)" }
        if (& $Check) { return }
        Start-Sleep -Milliseconds 25
    }
    throw $Message
}
function Read-State {
    try {
        $stream = [IO.FileStream]::new($script:statePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader = [IO.StreamReader]::new($stream)
        try { $state = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if ($state.process_id -eq $script:fixtureProcess.Id) { return $state }
    } catch {}
    return $null
}
function Send-Command([hashtable]$Command) {
    $script:revision++
    $Command['revision'] = $script:revision
    $Command | ConvertTo-Json -Compress | Set-Content -LiteralPath $script:commandPath -Encoding ascii
    Wait-Condition { $state = Read-State; $null -ne $state -and $state.revision -eq $script:revision } 'Fixture command did not commit'
}
function Find-Node([string]$Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $script:window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Tree-Descendants($State, [string]$Id) {
    $node = $State.tree.nodes.PSObject.Properties[$Id].Value
    if ($null -eq $node) { return }
    $node
    foreach ($child in $node.children) { Tree-Descendants $State $child }
}
function Tree-Text($State, [string]$Id) {
    return (@(Tree-Descendants $State $Id | Where-Object { $_.aria.role -eq 'Label' } | ForEach-Object { $_.aria.value }) -join '')
}
function Check-Tree([int]$Stage) {
    $state = Read-State
    Require ($state.a11y_active -and $null -ne $state.tree) 'Native AccessKit tree is inactive'
    $roots = @($state.tree.nodes.PSObject.Properties | Where-Object { $_.Value.aria.role -eq 'DescriptionList' })
    if ($Stage -eq 6) { Require ($roots.Count -eq 0) 'Unmounted description list remains in semantic tree'; return }
    Require ($roots.Count -eq 1) 'Expected exactly one DescriptionList'
    $root = $roots[0].Value
    Require ($root.aria.label -eq 'Account details') 'List name must stay separate from its values'
    Require ($root.aria.description -eq 'Review account details and edit the alias') 'Caller description was lost'
    Require ($null -eq $root.aria.value) 'List redundantly flattened its descendants into a value'
    $descendants = @(Tree-Descendants $state $roots[0].Name)
    $terms = @($descendants | Where-Object { $_.aria.role -eq 'Term' })
    $definitions = @($descendants | Where-Object { $_.aria.role -eq 'Definition' })
    Require ($terms.Count -eq 4 -and $definitions.Count -eq 4) 'Term/Definition pair structure differs'
    $expected = if ($Stage -in 4,5) { @('Alias','Name','Actions','Department') } else { @('Name','Department','Alias','Actions') }
    for ($index = 0; $index -lt 4; $index++) {
        $term = $terms[$index]
        $termText = @($term.children | ForEach-Object { Tree-Text $state $_ }) -join ''
        Require ($termText -eq $expected[$index]) "Term order or logical text differs at $index : $termText"
        Require ($term.aria.label -eq $expected[$index]) 'Term accessible name differs from its item label'
        Require ($definitions[$index].aria.label -eq $expected[$index]) 'Definition must carry its matching item label to disambiguate vertical rows'
        Require ($null -eq $definitions[$index].aria.value) 'Definition redundantly flattened its rich descendants'
        $definitionNodes = @($definitions[$index].children | ForEach-Object { Tree-Descendants $state $_ })
        if ($expected[$index] -eq 'Alias') {
            Require (@($definitionNodes | Where-Object { $_.aria.role -eq 'TextInput' -and $_.aria.label -eq 'Account alias' }).Count -eq 1) 'Alias definition lost its native TextInput'
        } elseif ($expected[$index] -eq 'Actions') {
            Require (@($definitionNodes | Where-Object { $_.aria.role -eq 'Button' -and $_.aria.label -eq 'Save account' }).Count -eq 1) 'Actions definition lost its native Button'
        } else {
            $expectedText = if ($expected[$index] -eq 'Name') { 'Lin' } else { 'Research' }
            $actualText = @($definitionNodes | Where-Object { $_.aria.role -eq 'Label' } | ForEach-Object { $_.aria.value }) -join ''
            Require ($actualText -eq $expectedText) 'Plain definition logical text or pair order differs'
        }
    }
    $native = Find-Node 'descriptions.main'
    Require ($native.Current.ControlType -eq [System.Windows.Automation.ControlType]::List) 'Native UIA role is not List'
    Require ($native.Current.Name -eq 'Account details') 'Native list name differs'
    Require (-not $native.Current.IsKeyboardFocusable) 'DescriptionList must not introduce a tab stop'
    $termCondition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
    $nativeTerms = $native.FindAll([System.Windows.Automation.TreeScope]::Descendants, $termCondition)
    Require ($nativeTerms.Count -eq 4) 'Native UIA did not retain four terms'
    for ($index = 0; $index -lt 4; $index++) { Require ($nativeTerms[$index].Current.Name -eq $expected[$index]) "Native term name or order differs at $index : expected '$($expected[$index])', actual '$($nativeTerms[$index].Current.Name)'" }
    $definitionCondition = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Group)
    $nativeDefinitions = $native.FindAll([System.Windows.Automation.TreeScope]::Descendants, $definitionCondition)
    Require ($nativeDefinitions.Count -eq 4) 'Native UIA did not retain four definition groups'
    for ($index = 0; $index -lt 4; $index++) { Require ($nativeDefinitions[$index].Current.Name -eq $expected[$index]) 'Native definition group lost its matching item name' }
    $state | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (Join-Path $output "stage-$Stage.json") -Encoding utf8
    $script:checks.Add("stage-$Stage-native-list-terms-definitions-order-rich-descendants")
}
$script:statePath = Join-Path $output 'state.json'
$script:commandPath = Join-Path $output 'command.json'
$script:revision = 0
'{"stage":0,"revision":0}' | Set-Content -LiteralPath $script:commandPath -Encoding ascii
$script:fixtureProcess = Start-Process -FilePath $Executable -ArgumentList @($script:statePath, $script:commandPath) -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $output 'stderr.txt')
$null = $script:fixtureProcess.Handle
$script:checks = [Collections.Generic.List[string]]::new()
$status = 'failed'
$reason = $null
try {
    $script:handle = [IntPtr]::Zero
    Wait-Condition { $script:handle = [DescriptionsNativeA11y]::Find($script:fixtureProcess.Id); $script:handle -ne [IntPtr]::Zero } 'Hidden Descriptions HWND was not created' 30000
    Require (-not [DescriptionsNativeA11y]::IsWindowVisible($script:handle)) 'Fixture showed a native window'
    Wait-Condition { $state = Read-State; $null -ne $state -and $state.a11y_active -and $null -ne $state.tree } 'Native Descriptions semantic tree did not initialize' 15000
    $script:window = [System.Windows.Automation.AutomationElement]::FromHandle($script:handle)
    Wait-Condition { $null -ne (Find-Node 'descriptions.main') } 'Native UIA description list is missing' 15000
    foreach ($stage in 0..3) { Send-Command @{stage=$stage}; Check-Tree $stage }
    Send-Command @{stage=0}
    $editor = Find-Node 'descriptions.alias'
    $save = Find-Node 'descriptions.save'
    Require ($editor.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit) 'Rich Input role is not Edit'
    Require ($editor.Current.IsKeyboardFocusable -and $save.Current.IsKeyboardFocusable) 'Rich controls lost native focusability'
    $value = $editor.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    $value.SetValue('Modified alias')
    Wait-Condition { (Read-State).editor_value -eq 'Modified alias' } 'Native ValuePattern did not edit the rich Input'
    $save.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Wait-Condition { (Read-State).action_count -eq 1 } 'Native Invoke did not activate the rich Button once'
    $script:checks.Add('native-valuepattern-and-invokepattern-rich-children')
    Send-Command @{focus='outside'}
    Send-Command @{key='tab'}
    Require ((Read-State).focused -eq 'alias') 'Tab did not enter the rich Input'
    Send-Command @{key='tab'}
    Require ((Read-State).focused -eq 'save') 'Tab did not reach the rich Button'
    Send-Command @{key='space'}
    Require ((Read-State).action_count -eq 2) 'Space did not activate the rich Button exactly once'
    Send-Command @{focus='alias'}
    $runtimeId = ($editor.GetRuntimeId() -join ',')
    Send-Command @{stage=4}
    Wait-Condition { (Read-State).viewport_width -eq 520 } 'Narrow viewport was not applied'
    Check-Tree 4
    Require ((Read-State).viewport_width -eq 520) 'Narrow viewport was not applied'
    Require ((Read-State).focused -eq 'alias') 'Input focus was lost during responsive reflow and reorder'
    $editor = Find-Node 'descriptions.alias'
    Require (($editor.GetRuntimeId() -join ',') -eq $runtimeId) 'Responsive reorder replaced the native rich Input identity'
    Require ($editor.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value -eq 'Modified alias') 'Rich value was lost during responsive reflow'
    Send-Command @{key='tab'}
    Require ((Read-State).focused -eq 'save') 'Reordered Tab traversal differs'
    Send-Command @{key='enter'}
    Require ((Read-State).action_count -eq 3) 'Reordered Enter did not activate exactly once'
    Send-Command @{stage=5}
    Check-Tree 5
    Require ((Read-State).focused -eq 'save') 'Button focus was lost on widening'
    $script:checks.Add('targeted-window-tab-space-enter-responsive-reorder-focus-value-identity')
    Send-Command @{stage=6}
    Check-Tree 6
    Require ($null -eq (Find-Node 'descriptions.main')) 'Unmounted list remains in native UIA'
    $script:checks.Add('unmounted-native-list-removed')
    Require (-not [DescriptionsNativeA11y]::IsWindowVisible($script:handle)) 'Fixture became visible'
    $status = 'passed'
} catch {
    $reason = $_.Exception.Message
    if ((Read-State).a11y_active -ne $true -or $null -eq $script:window) { $status = 'unavailable' }
    Write-Warning $reason
} finally {
    '{"quit":true}' | Set-Content -LiteralPath $script:commandPath -Encoding ascii
    if (-not $script:fixtureProcess.WaitForExit(3000)) { $script:fixtureProcess.Kill() }
    $hashes = [ordered]@{}
    foreach ($file in @('examples/descriptions_accessibility.rs','scripts/test-descriptions-accessibility.ps1','src/behavior/text.rs','src/behavior/accessibility.rs','src/config.rs','src/lib.rs','Cargo.lock') + @(Get-ChildItem -LiteralPath (Join-Path $crateRoot 'src/descriptions') -Filter '*.rs' | ForEach-Object { 'src/descriptions/' + $_.Name })) {
        $hashes[$file] = (Get-FileHash -LiteralPath (Join-Path $crateRoot $file) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $evidence = [ordered]@{schema_version=1; status=$status; platform='windows'; mode='hidden-native-window'; keyboard='GPUI events targeted only to fixture Window; no global keyboard input'; activation='WM_GETOBJECT requests ordinary AccessKit UIA registration'; utc=[DateTime]::UtcNow.ToString('O'); reason=$reason; checks=$script:checks.ToArray(); binary_sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant(); source_sha256=$hashes; limitations=@('Native UIA verifies List/ListItem and named Definition groups, rich Edit/ValuePattern and Button/InvokePattern. Activated native AccessKit tree verifies matching term/definition sequences and item label names, retaining visual-row order in vertical layout.','Tab/Space/Enter use targeted fixture-window GPUI dispatch; physical OS keyboard routing and screen reader speech are not exercised.')}
    $json = ($evidence | ConvertTo-Json -Depth 30).Replace("`r`n", "`n")
    [IO.File]::WriteAllText((Join-Path $output 'result.json'), $json + "`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($summary, $json + "`n", [Text.UTF8Encoding]::new($false))
}
if ($status -ne 'passed') { throw "Descriptions native accessibility verification: $status" }
Write-Output 'Verified native Descriptions tree, rich controls, scoped keyboard traversal, responsive identity, and unmount.'

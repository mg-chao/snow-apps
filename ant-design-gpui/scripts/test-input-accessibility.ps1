# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
# Opt-in native input UI Automation validation; requires an interactive Windows desktop.
[CmdletBinding()]
param([switch]$SkipBuild, [switch]$ChineseIme, [string]$Executable)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) {
    Push-Location $crateRoot
    try {
        & cargo build --locked --offline --example input
        if ($LASTEXITCODE -ne 0) { throw 'Input verification build failed.' }
    } finally { Pop-Location }
}
if (-not $Executable) { $Executable = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/debug/examples/input.exe' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "Missing input verification executable: $Executable" }
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
if (-not ('AntInputUiaFixture' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AntInputUiaFixture {
    private delegate bool EnumWindowsCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("imm32.dll")] private static extern IntPtr ImmGetDefaultIMEWnd(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    // Send scalar control messages to the fixture thread's default IME window;
    // HIMC handles obtained in another process cannot be used directly.
    // https://learn.microsoft.com/windows/win32/api/imm/nf-imm-immgetdefaultimewnd
    private static uint ImeControl(IntPtr window, uint command, uint value) {
        IntPtr ime = ImmGetDefaultIMEWnd(window);
        if (ime == IntPtr.Zero) throw new Exception("Fixture has no native IME window");
        UIntPtr result;
        if (SendMessageTimeout(ime, 0x283, new UIntPtr(command), new IntPtr(value), 3, 2000, out result) == IntPtr.Zero)
            throw new Exception("Fixture IME control message timed out");
        return result.ToUInt32();
    }
    public static uint[] EnableChinese(IntPtr window) {
        uint[] saved = { ImeControl(window, 5, 0), ImeControl(window, 1, 0), ImeControl(window, 3, 0) };
        ImeControl(window, 6, 1);
        ImeControl(window, 2, (saved[1] | 1u) & ~0x408u);
        return saved;
    }
    public static void RestoreIme(IntPtr window, uint[] saved) {
        ImeControl(window, 2, saved[1]);
        ImeControl(window, 4, saved[2]);
        ImeControl(window, 6, saved[0]);
    }
    public static IntPtr FindWindow(int processId) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((window, parameter) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner != processId) return true;
            var title = new StringBuilder(256);
            GetWindowText(window, title, title.Capacity);
            if (!title.ToString().StartsWith("Ant Design GPUI")) return true;
            result = window; return false;
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
        Start-Sleep -Milliseconds 25
    }
    throw $Message
}
function Find-Input([string]$Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $script:inputWindow.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Value-Of($Element) {
    return $Element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
}
function Find-Action([string]$Name) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
    return $script:inputWindow.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}
function Set-Value($Element, [string]$Value) {
    $Element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Value)
    Wait-Condition { (Value-Of $Element) -eq $Value } "ValuePattern did not update $($Element.Current.AutomationId)."
}
function Caret-Offset($Pattern) {
    $prefix = $Pattern.DocumentRange.Clone()
    $prefix.MoveEndpointByRange([System.Windows.Automation.Text.TextPatternRangeEndpoint]::End,
        $Pattern.GetSelection()[0], [System.Windows.Automation.Text.TextPatternRangeEndpoint]::Start)
    return $prefix.GetText(-1).Length
}
function Focus-Input($Element) {
    [AntInputUiaFixture]::SetForegroundWindow($script:nativeWindow) | Out-Null
    Wait-Condition { [AntInputUiaFixture]::GetForegroundWindow() -eq $script:nativeWindow } 'Could not activate input fixture.'
    $Element.SetFocus()
    Wait-Condition { $Element.Current.HasKeyboardFocus } 'UIA focus did not reach the input.'
}
function Send-Keys([string]$Keys) {
    Require ([AntInputUiaFixture]::GetForegroundWindow() -eq $script:nativeWindow) 'Fixture lost foreground; refusing to send keys.'
    Require ([System.Windows.Automation.AutomationElement]::FocusedElement.Current.ProcessId -eq $process.Id) 'Fixture lost focus; refusing to send keys.'
    [System.Windows.Forms.SendKeys]::SendWait($Keys)
}
$process = Start-Process -FilePath $Executable -PassThru -WindowStyle Hidden
$savedIme = $null
try {
    $script:nativeWindow = [IntPtr]::Zero
    Wait-Condition {
        $process.Refresh()
        if ($process.HasExited) { throw "Input fixture exited early: $($process.ExitCode)" }
        $script:nativeWindow = [AntInputUiaFixture]::FindWindow($process.Id)
        $script:nativeWindow -ne [IntPtr]::Zero
    } 'Native input window was not created.' 30000
    [AntInputUiaFixture]::ShowWindow($script:nativeWindow, 1) | Out-Null
    $script:inputWindow = $null
    Wait-Condition {
        $condition = [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
        $script:inputWindow = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        $null -ne $script:inputWindow -and $null -ne (Find-Input 'input.probe.0')
    } 'Native input accessibility tree was not exposed.' 30000

    $basic = Find-Input 'input.probe.0'
    Require ($basic.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit) 'Text input must expose Edit.'
    Require $basic.Current.IsKeyboardFocusable 'Text input must be focusable.'
    Set-Value $basic '中文 é 👩🏽‍💻'
    $text = $basic.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
    Require ($text.DocumentRange.GetText(-1) -eq '中文 é 👩🏽‍💻') 'TextPattern lost Unicode source.'
    Focus-Input $basic
    Send-Keys '^a'
    Wait-Condition { ($text.GetSelection()[0].GetText(-1)) -eq '中文 é 👩🏽‍💻' } 'TextPattern did not track native selection.'
    # Numeric key events bypass language conversion; this is not an IME test.
    Send-Keys '123'
    try { Wait-Condition { (Value-Of $basic) -eq '123' } 'Native editing did not replace selection.' }
    catch { throw "Native editing did not replace selection; current fixture value: $(Value-Of $basic)" }
    Send-Keys '^z'
    try { Wait-Condition { (Value-Of $basic) -eq '中文 é 👩🏽‍💻' } 'Native undo did not restore Unicode.' }
    catch { throw "Native undo did not restore Unicode; current fixture value: $(Value-Of $basic)" }
    Send-Keys '^y'
    Wait-Condition { (Value-Of $basic) -eq '123' } 'Native redo failed.'

    Set-Value $basic 'אבג'
    Focus-Input $basic
    Send-Keys '^{END}'
    foreach ($expected in @(2, 1, 0, 0)) {
        Send-Keys '{LEFT}'
        Wait-Condition { (Caret-Offset $text) -eq $expected } "RTL Left did not reach logical offset $expected."
    }
    Send-Keys '+{RIGHT}'
    Wait-Condition { $text.GetSelection()[0].GetText(-1) -eq 'א' } 'RTL Shift+Right did not select the next logical grapheme.'
    Send-Keys '{LEFT}'
    Wait-Condition { (Caret-Offset $text) -eq 0 } 'RTL Left did not collapse selection at its logical start.'
    Send-Keys '^{RIGHT}'
    Wait-Condition { (Caret-Offset $text) -eq 3 } 'RTL word-right did not reach the word end.'
    Send-Keys '^{LEFT}'
    Wait-Condition { (Caret-Offset $text) -eq 0 } 'RTL word-left did not reach the word start.'
    Set-Value $basic 'abc אבג xyz'
    Send-Keys '^{HOME}'
    foreach ($expected in 1..11) {
        Send-Keys '{RIGHT}'
        Wait-Condition { (Caret-Offset $text) -eq $expected } "Mixed-direction Right did not reach logical offset $expected."
    }
    Send-Keys '^{HOME}'
    Send-Keys '{RIGHT 3}'
    Send-Keys '+{RIGHT 2}'
    Wait-Condition { $text.GetSelection()[0].GetText(-1) -eq ' א' } 'Mixed-direction selection lost its logical text.'
    # Bidi fragment geometry is limited by GPUI's public line layout. Keep the
    # logical TextPattern contract without requiring the removed native extension.

    $readonly = Find-Input 'input.probe.14'
    Require ($readonly.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.IsReadOnly) 'Read-only input reports writable.'
    $disabled = Find-Input 'input.probe.15'
    Require (-not $disabled.Current.IsEnabled) 'Disabled input reports enabled.'
    Require (-not $disabled.Current.IsKeyboardFocusable) 'Disabled input reports focusable.'
    $password = Find-Input 'input.probe.password'
    Require $password.Current.IsPassword 'Password input is not protected.'
    $pattern = $null
    if ($password.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$pattern)) {
        Require ($pattern.Current.Value -eq '') 'Password exports plaintext through ValuePattern.'
    }
    if ($password.TryGetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern, [ref]$pattern)) {
        $protectedText = $pattern.DocumentRange.GetText(-1)
        Require (-not $protectedText.Contains('secret') -and -not $protectedText.Contains('密码')) 'Password exports plaintext through TextPattern.'
    }
    Focus-Input $password
    Send-Keys '{TAB}'
    $reveal = Find-Action 'Show password'
    Require ($null -ne $reveal -and $reveal.Current.HasKeyboardFocus) 'Password visibility action was not reachable by Tab.'
    Send-Keys ' '
    Wait-Condition { $null -ne (Find-Action 'Hide password') } 'Space did not reveal the password.'
    Require $password.Current.IsPassword 'Revealed password lost protected semantics.'
    if ($password.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$pattern)) {
        Require ($pattern.Current.Value -eq '') 'Revealed password exports plaintext through ValuePattern.'
    }
    if ($password.TryGetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern, [ref]$pattern)) {
        $protectedText = $pattern.DocumentRange.GetText(-1)
        Require (-not $protectedText.Contains('secret') -and -not $protectedText.Contains('密码')) 'Revealed password exports plaintext through TextPattern.'
    }
    Send-Keys '{ENTER}'
    Wait-Condition { $null -ne (Find-Action 'Show password') } 'Enter did not hide the password.'

    $search = Find-Input 'input.probe.search'
    $clearCondition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::NameProperty, 'Clear input')
    $clear = $search.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $clearCondition)
    Require ($null -ne $clear) 'Search clear action is missing from the accessibility tree.'
    $previousSearch = Value-Of $search
    $clear.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Wait-Condition { (Value-Of $search) -eq '' -and $search.Current.HasKeyboardFocus } 'Clear invocation did not clear and focus search.'
    Send-Keys '^z'
    Wait-Condition { (Value-Of $search) -eq $previousSearch } 'Clear invocation was not undoable.'

    $number = Find-Input 'input.probe.number.0'
    Require ($number.Current.ControlType -eq [System.Windows.Automation.ControlType]::Spinner) 'Numeric input must expose Spinner.'
    $range = $number.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
    $range.SetValue(12.5)
    Wait-Condition { $range.Current.Value -eq 12.5 } 'RangeValuePattern did not change the numeric value.'
    Focus-Input $number
    Send-Keys '{UP}'
    Wait-Condition { $range.Current.Value -eq 13.5 } 'Numeric native Up did not step.'
    $exact = Find-Input 'input.probe.number.1'
    Require ((Value-Of $exact) -eq '9007199254740993.001') 'Exact numeric text was rounded in accessibility.'

    $code = Find-Input 'input.probe.code.0.cell.0'
    Focus-Input $code
    Send-Keys '{END}'
    $last = Find-Input 'input.probe.code.0.cell.5'
    Wait-Condition { $last.Current.HasKeyboardFocus } 'OTP End did not move native focus.'
    Send-Keys '{TAB}'
    Wait-Condition { -not $last.Current.HasKeyboardFocus } 'OTP Tab did not leave the group.'
    Send-Keys '+{TAB}'
    Wait-Condition { $last.Current.HasKeyboardFocus } 'OTP Shift+Tab did not restore its active cell.'
    Send-Keys '{HOME}'
    Wait-Condition { $code.Current.HasKeyboardFocus } 'OTP Home did not move native focus.'

    if ($ChineseIme) {
        Set-Value $basic ''
        Focus-Input $basic
        $savedIme = [AntInputUiaFixture]::EnableChinese($script:nativeWindow)
        Send-Keys 'nihao'
        Wait-Condition { $script:inputWindow.Current.Name.EndsWith('composing') } 'Installed IME did not start a native composition.'
        Send-Keys ' '
        Wait-Condition { -not $script:inputWindow.Current.Name.EndsWith('composing') } 'Native IME confirmation did not finish composition.'
        $committedChinese = Value-Of $basic
        Require ($committedChinese -match '[\p{IsCJKUnifiedIdeographs}]') "IME did not commit Chinese text: $committedChinese"
        Send-Keys '^z'
        Wait-Condition { (Value-Of $basic) -eq '' } 'IME commit was not a single undo transaction.'
        Send-Keys '^y'
        Wait-Condition { (Value-Of $basic) -eq $committedChinese } 'IME redo did not restore committed text.'
        Send-Keys 'ni'
        Wait-Condition { $script:inputWindow.Current.Name.EndsWith('composing') } 'Second IME composition did not start.'
        Send-Keys '{ESC}'
        Wait-Condition { -not $script:inputWindow.Current.Name.EndsWith('composing') } 'Escape did not end native composition.'
        Require ((Value-Of $basic) -eq $committedChinese) 'Cancelled native composition altered the committed value.'

        Focus-Input $number
        Send-Keys '^a'
        Send-Keys 'yi'
        Wait-Condition { (Value-Of $number) -ne '13.5' } 'Numeric IME preedit was not displayed.'
        Require ($range.Current.Value -eq 13.5) 'Numeric IME preedit changed the accepted value.'
        Send-Keys ' '
        Wait-Condition { (Value-Of $number) -match '[\p{IsCJKUnifiedIdeographs}]' } 'Numeric native IME did not finalize its draft.'
        Require ($range.Current.Value -eq 13.5) 'Invalid Chinese numeric draft changed the accepted value.'
        Send-Keys '{ENTER}'
        Wait-Condition { (Value-Of $number) -eq '13.5' } 'Numeric commit did not restore its accepted value.'

        Focus-Input $code
        Send-Keys '^a'
        Send-Keys 'ni'
        Wait-Condition { (Value-Of $code) -ne '1' } 'OTP native preedit was not displayed.'
        Require $code.Current.HasKeyboardFocus 'OTP advanced before native composition committed.'
        Send-Keys ' '
        $secondCode = Find-Input 'input.probe.code.0.cell.1'
        Wait-Condition { $secondCode.Current.HasKeyboardFocus } 'OTP did not advance after native composition committed.'
        Require ((Value-Of $code) -match '[\p{IsCJKUnifiedIdeographs}]') 'OTP did not keep the committed Chinese grapheme.'
        Send-Keys '^z'
        Wait-Condition {
            $actualCode = (0..5 | ForEach-Object { Value-Of (Find-Input "input.probe.code.0.cell.$_") }) -join ''
            $actualCode -eq '123456'
        } 'OTP native composition did not undo as one aggregate edit.'
        [AntInputUiaFixture]::RestoreIme($script:nativeWindow, $savedIme)
        $savedIme = $null
        Write-Output 'Chinese IME passed: native preedit, candidate confirmation, aggregate undo/redo, Escape cancellation, numeric draft isolation and OTP commit/focus/undo.'
    }

    $script:inputWindow.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
    Require ($process.WaitForExit(5000)) 'Input fixture did not exit on window close.'
    Require ($process.ExitCode -eq 0) 'Input fixture failed during teardown.'
    Write-Output 'Input native UIA passed: Edit/Spinner roles, Unicode Value/Text patterns, logical mixed-text navigation and selection, native editing and undo/redo, read-only/disabled/password protection, keyboard password visibility, accessible clear/undo, exact-decimal export, RangeValue stepping, OTP focus traversal and teardown.'
} finally {
    try {
        if ($null -ne $savedIme) { [AntInputUiaFixture]::RestoreIme($script:nativeWindow, $savedIme) }
    } finally {
        $process.Refresh()
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
}

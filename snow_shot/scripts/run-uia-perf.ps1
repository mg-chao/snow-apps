#requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BeforeExecutable,
    [Parameter(Mandatory)][string]$AfterExecutable,
    [Parameter(Mandatory)][long]$WindowHandle,
    [Parameter(Mandatory)][int[]]$Points,
    [string]$OutputDirectory = "",
    [int]$Rounds = 10,
    [int]$Samples = 15,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$performanceTarget = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
if ($Points.Count -eq 0 -or $Points.Count % 2 -ne 0) {
    throw "Points must contain physical x/y pairs."
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot "../../build/$($performanceTarget.Preset)/uia"
}
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory
$BeforeExecutable = (Resolve-Path -LiteralPath $BeforeExecutable).Path
$AfterExecutable = (Resolve-Path -LiteralPath $AfterExecutable).Path
Assert-SnowPerformanceExecutable -Path $BeforeExecutable -Architecture $Architecture
Assert-SnowPerformanceExecutable -Path $AfterExecutable -Architecture $Architecture

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class UiaPerfWindow {
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrW(IntPtr hwnd, int index);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool SetWindowPos(
        IntPtr hwnd, IntPtr after, int x, int y, int width, int height, uint flags);
}
'@

$fixtureHwnd = [IntPtr]$WindowHandle
if (![UiaPerfWindow]::IsWindow($fixtureHwnd)) { throw "The fixture window no longer exists." }
$wasTopmost = ([UiaPerfWindow]::GetWindowLongPtrW($fixtureHwnd, -20).ToInt64() -band 8) -ne 0
$wasMinimized = [UiaPerfWindow]::IsIconic($fixtureHwnd)
$arguments = @("--backend", "uia", "--samples", $Samples, "--rounds", $Rounds,
               "--interval-ms", "0", "--csv")
for ($index = 0; $index -lt $Points.Count; $index += 2) {
    $arguments += @("--point", $Points[$index], $Points[$index + 1])
}
try {
    if ($wasMinimized) { $null = [UiaPerfWindow]::ShowWindow($fixtureHwnd, 4) }
    # Keep this explicit fixture above overlapping windows while both versions sample it.
    if (![UiaPerfWindow]::SetWindowPos($fixtureHwnd, [IntPtr](-1), 0, 0, 0, 0, 0x13)) {
        throw "Could not raise the fixture window."
    }
    & $BeforeExecutable @arguments | Set-Content -LiteralPath (Join-Path $OutputDirectory "before.csv")
    if ($LASTEXITCODE -ne 0) { throw "The baseline benchmark failed." }
    & $AfterExecutable @arguments | Set-Content -LiteralPath (Join-Path $OutputDirectory "after.csv")
    if ($LASTEXITCODE -ne 0) { throw "The replacement benchmark failed." }
}
finally {
    if (!$wasTopmost -and [UiaPerfWindow]::IsWindow($fixtureHwnd)) {
        $null = [UiaPerfWindow]::SetWindowPos($fixtureHwnd, [IntPtr](-2), 0, 0, 0, 0, 0x13)
    }
    if ($wasMinimized -and [UiaPerfWindow]::IsWindow($fixtureHwnd)) {
        $null = [UiaPerfWindow]::ShowWindow($fixtureHwnd, 7)
    }
}

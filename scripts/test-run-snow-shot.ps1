# Launcher contracts with a deterministic process fixture; no application launches.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetTempPath()) "snow-shot-launch-$([guid]::NewGuid().ToString('N'))"
function Require($Value, [string]$Message) { if (-not $Value) { throw $Message } }
Add-Type @'
public class SnowLaunchProcessFixture {
    public bool Waited;
    public int Result;
    public void WaitForExit() { Waited = true; }
    public int ExitCode {
        get {
            if (!Waited) throw new System.InvalidOperationException("Process was not awaited.");
            return Result;
        }
    }
}
'@
try {
    $scripts = Join-Path $fixture 'scripts'
    $null = New-Item -ItemType Directory -Path $scripts
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'run-snow-shot.ps1') -Destination $scripts
    foreach ($target in @('snow_shot', 'snow_shot_mini')) {
        $directory = Join-Path $fixture "build/windows-msvc-debug/$target/Debug"
        $null = New-Item -ItemType Directory -Path $directory -Force
        [IO.File]::WriteAllText((Join-Path $directory "$target.exe"), 'process fixture')
    }
    $global:SnowLaunchFixtureExitCode = 0
    $global:SnowLaunchFixtureProcess = $null
    $global:SnowLaunchPath = ''
    function Start-Process {
        param($FilePath, $WorkingDirectory, $WindowStyle, [switch]$PassThru)
        Require ($WindowStyle -eq 'Hidden') 'Launch helpers must not open a console.'
        Require ($WorkingDirectory -eq (Split-Path $FilePath)) 'Use the executable directory.'
        $global:SnowLaunchPath = $FilePath
        if ($PassThru) {
            $global:SnowLaunchFixtureProcess = [SnowLaunchProcessFixture]::new()
            $global:SnowLaunchFixtureProcess.Result = $global:SnowLaunchFixtureExitCode
            return $global:SnowLaunchFixtureProcess
        }
    }
    $launcher = Join-Path $scripts 'run-snow-shot.ps1'
    foreach ($edition in @('Full', 'Mini')) {
        $global:SnowLaunchFixtureProcess = $null
        & $launcher -Edition $edition -NoBuild
        Require $global:SnowLaunchFixtureProcess.Waited 'Foreground launches must wait for the GUI process.'
        $target = if ($edition -eq 'Mini') { 'snow_shot_mini' } else { 'snow_shot' }
        Require ($global:SnowLaunchPath.EndsWith("$target.exe")) 'Select the requested edition.'
    }
    $global:SnowLaunchFixtureExitCode = -1073741515
    $global:SnowLaunchFixtureProcess = $null
    $failure = ''
    try { & $launcher -Edition Mini -NoBuild } catch { $failure = $_.Exception.Message }
    Require ($failure -eq 'Snow Shot exited with code -1073741515.') 'Report the actual loader failure.'
    $global:SnowLaunchFixtureProcess = $null
    & $launcher -Edition Mini -NoBuild -Detached
    Require ($null -eq $global:SnowLaunchFixtureProcess) 'Detached launches must return immediately.'
    Write-Output 'PASS: edition selection, foreground waiting, exit reporting, and detached launching.'
}
finally {
    Remove-Variable -Scope Global -Name SnowLaunchFixtureExitCode, SnowLaunchFixtureProcess,
        SnowLaunchPath -ErrorAction SilentlyContinue
    $absolute = [IO.Path]::GetFullPath($fixture)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $absolute.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe fixture cleanup path.'
    }
    if (Test-Path -LiteralPath $absolute) { Remove-Item -LiteralPath $absolute -Recurse -Force }
}

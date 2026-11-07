# Launcher contracts with deterministic process and native-tool fixtures; no application launches.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetTempPath()) "snow-shot launch-$([guid]::NewGuid().ToString('N'))"
$savedEnvironment = @{}
foreach ($name in @('PATH', 'VCPKG_ROOT', 'VCPKG_DEFAULT_HOST_TRIPLET',
        'VCPKG_FORCE_SYSTEM_BINARIES', 'VCPKG_FORCE_DOWNLOADED_BINARIES', 'LIBCLANG_PATH',
        'SNOW_QT_STATIC_DIR', 'Qt6_DIR', 'CFLAGS')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
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
    $policy = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'static-qt-features.json') |
        ConvertFrom-Json
    foreach ($file in @('run-snow-shot.ps1', 'run-snow-shot.bat', 'build.ps1', 'bootstrap.ps1',
            'snow-build-environment.ps1', 'snow-build-cache.ps1',
            'qt-toolchain.json', 'build-toolchain.json', 'static-qt-features.json') + @($policy.windowsSourcePatches)) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $scripts
    }
    # Exercise the real architecture check with a minimal x64 PE header.
    $binary = New-Object byte[] 88
    [BitConverter]::GetBytes([uint16]0x5A4D).CopyTo($binary, 0)
    [BitConverter]::GetBytes([uint32]64).CopyTo($binary, 0x3C)
    [BitConverter]::GetBytes([uint32]0x00004550).CopyTo($binary, 64)
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($binary, 68)
    foreach ($target in @('snow_shot', 'snow_shot_mini')) {
        $directory = Join-Path $fixture "build/windows-msvc-debug/$target/Debug"
        $null = New-Item -ItemType Directory -Path $directory -Force
        [IO.File]::WriteAllBytes((Join-Path $directory "$target.exe"), $binary)
    }
    $global:SnowLaunchFixtureExitCode = 0
    $global:SnowLaunchFixtureProcess = $null
    $global:SnowLaunchPath = ''
    function Start-Process {
        param($FilePath, $WorkingDirectory, $ArgumentList, $WindowStyle, [switch]$PassThru)
        Require ($WindowStyle -eq 'Normal') 'GUI launches must not suppress the first editor window.'
        Require (@($ArgumentList).Count -eq 1 -and $ArgumentList -eq '--show-main-window') `
            'Development launches must explicitly show the main window, including on first startup.'
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
        $output = (& $launcher -Edition $edition -NoBuild 6>&1 | Out-String)
        Require $global:SnowLaunchFixtureProcess.Waited 'Foreground launches must wait for the GUI process.'
        Require ($output.Contains('Starting Snow Shot:') -and
            $output.Contains('waiting for it to exit') -and $output.Contains('-Detached')) `
            'Explain that launch succeeded and the terminal is waiting for application exit.'
        Require (-not $output.Contains('Building Snow Shot')) 'NoBuild must skip the build stage.'
        $target = if ($edition -eq 'Mini') { 'snow_shot_mini' } else { 'snow_shot' }
        Require ($global:SnowLaunchPath.EndsWith("$target.exe")) 'Select the requested edition.'
    }
    $global:SnowLaunchFixtureExitCode = -1073741515
    $global:SnowLaunchFixtureProcess = $null
    $failure = ''
    try { & $launcher -Edition Mini -NoBuild } catch { $failure = $_.Exception.Message }
    Require ($failure -eq 'Snow Shot exited with code -1073741515.') 'Report the actual loader failure.'
    $global:SnowLaunchFixtureProcess = $null
    $output = (& $launcher -Edition Mini -NoBuild -Detached 6>&1 | Out-String)
    Require ($null -eq $global:SnowLaunchFixtureProcess) 'Detached launches must return immediately.'
    Require ($output.Contains('returning to the terminal') -and
        -not $output.Contains('waiting for it to exit')) 'Describe detached launches without a wait message.'

    # A missing performance build exercises the real batch entry point and argument
    # forwarding without starting an application. Restrict PATH to force its 5.1 fallback.
    $originalPath = $env:Path
    $originalModulePath = $env:PSModulePath
    $originalErrorPreference = $ErrorActionPreference
    try {
        # Let each child host initialize its own built-in module search path.
        Remove-Item Env:PSModulePath -ErrorAction SilentlyContinue
        foreach ($path in @($originalPath, "$env:SystemRoot\System32;$env:SystemRoot\System32\WindowsPowerShell\v1.0")) {
            $env:Path = $path
            $ErrorActionPreference = 'Continue'
            $batch = Join-Path $scripts 'run-snow-shot.bat'
            $output = (& $env:ComSpec /d /c "`"$batch`" -NoBuild -Preset windows-msvc-performance -Edition Mini" 2>&1 |
                Out-String)
            $exitCode = $LASTEXITCODE
            $ErrorActionPreference = $originalErrorPreference
            Require ($exitCode -eq 1) 'The batch launcher must propagate a launch failure.'
            $expectedPath = Join-Path $fixture 'build\windows-msvc-performance\snow_shot_mini\Release\snow_shot_mini.exe'
            # PowerShell 7's concise error view wraps long paths with gutter bars.
            $normalizedOutput = $output -replace '[\s|]', ''
            $expectedOutput = "Snow Shot executable was not found at '$expectedPath'." -replace '\s', ''
            Require ($normalizedOutput.Contains($expectedOutput)) `
                "Both batch PowerShell hosts must load the helper and forward arguments. Output: $output"
        }
    }
    finally {
        $env:Path = $originalPath
        $env:PSModulePath = $originalModulePath
        $ErrorActionPreference = $originalErrorPreference
    }

    # Run the real launcher -> build -> environment path. Only native tools are
    # stubbed; every CMake invocation must inherit the same vcpkg tool policy.
    @'
function Get-SnowWindowsHostArchitecture { return 'x64' }
function Add-SnowMsvcToolsToPath {
    param([string]$Architecture)
    if ($global:SnowLaunchStopBootstrap) { throw 'Bootstrap environment captured.' }
    return $PSScriptRoot
}
function Set-SnowQtEnvironment {
    param([string]$Preset)
    $env:SNOW_QT_STATIC_DIR = $PSScriptRoot
    $env:Qt6_DIR = $PSScriptRoot
    return $PSScriptRoot
}
function rustc {
    $global:LASTEXITCODE = 0
    return "host: x86_64-pc-windows-msvc`nrelease: 1.98.0"
}
function dumpbin.exe {}
'@ | Add-Content -LiteralPath (Join-Path $scripts 'snow-build-environment.ps1')
    function global:cmake {
        Require (-not (Test-Path Env:VCPKG_FORCE_DOWNLOADED_BINARIES)) `
            'Builds must allow installed tools that meet vcpkg minimum versions.'
        Require (-not (Test-Path Env:VCPKG_FORCE_SYSTEM_BINARIES)) `
            'Builds must allow vcpkg to download tools when installed tools are too old.'
        Require ($env:VCPKG_ROOT -eq (Join-Path $fixture '.tools\vcpkg')) `
            'Builds must use repository-local vcpkg.'
        $global:SnowLaunchBuildCalls += ,@($args)
        if ($args -contains '-B') {
            if ($global:SnowLaunchConfigureFailure) { $global:LASTEXITCODE = 1; return }
            $directory = $args[[Array]::IndexOf($args, '-B') + 1]
            $root = Split-Path -Parent (Split-Path -Parent $directory)
            $toolset = $args[[Array]::IndexOf($args, '-T') + 1]
            $null = New-Item -ItemType Directory -Path (Join-Path $directory 'CMakeFiles') -Force
            @(
                "Qt6_DIR:PATH=$env:SNOW_QT_STATIC_DIR",
                "Z_VCPKG_POWERSHELL_PATH:INTERNAL=$((Get-Command pwsh.exe).Source)",
                'CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026',
                'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64',
                "CMAKE_GENERATOR_TOOLSET:INTERNAL=$toolset",
                'VCPKG_TARGET_TRIPLET:STRING=x64-windows',
                "VCPKG_INSTALLED_DIR:PATH=$root/.tools/vcpkg/installed/dynamic",
                "CMAKE_HOME_DIRECTORY:INTERNAL=$root"
            ) | Set-Content -LiteralPath (Join-Path $directory 'CMakeCache.txt')
            [IO.File]::WriteAllText((Join-Path $directory 'CMakeFiles/generate.stamp'), 'fixture')
        }
        $global:LASTEXITCODE = 0
    }
    $vcpkgDirectory = Join-Path $fixture '.tools/vcpkg'
    $null = New-Item -ItemType Directory -Path $vcpkgDirectory -Force
    [IO.File]::WriteAllText((Join-Path $vcpkgDirectory 'vcpkg.exe'), 'fixture')
    $global:SnowLaunchStopBootstrap = $false
    $global:SnowLaunchConfigureFailure = $false
    $firstBuild = $true
    foreach ($edition in @('Full', 'Mini')) {
        foreach ($shell in @('terminal tools', 'editor tools')) {
            $env:PATH = "$(Join-Path $fixture $shell);$originalPath"
            $env:VCPKG_FORCE_SYSTEM_BINARIES = '1'
            Remove-Item Env:VCPKG_FORCE_DOWNLOADED_BINARIES -ErrorAction SilentlyContinue
            $global:SnowLaunchBuildCalls = @()
            $output = (& $launcher -Edition $edition -Detached 6>&1 | Out-String)
            Require ($output.Contains("Building Snow Shot ($edition, windows-msvc-debug) before launch...") -and
                $output.IndexOf('Building Snow Shot') -lt $output.IndexOf('Starting Snow Shot:')) `
                'Announce the build stage before launching the application.'
            $expectedCalls = if ($firstBuild) { 2 } else { 1 }
            Require ($global:SnowLaunchBuildCalls.Count -eq $expectedCalls) `
                'Configure initially, then incrementally build across editions and shell PATH changes.'
            $firstBuild = $false
            $target = if ($edition -eq 'Mini') { 'snow_shot_mini' } else { 'snow_shot' }
            Require (($global:SnowLaunchBuildCalls[-1] -join ' ') -eq
                "--build --preset build-windows-msvc-debug --parallel --target $target") `
                'Build only the requested edition without cleaning it.'
        }
    }

    # Build inputs embedded at configure time must invalidate the saved state;
    # source edits still go through the native incremental build on every launch.
    [IO.File]::WriteAllText((Join-Path $fixture 'source.cpp'), 'source edit')
    $global:SnowLaunchBuildCalls = @()
    & $launcher -Detached
    Require ($global:SnowLaunchBuildCalls.Count -eq 1) 'A source edit must reach the incremental build without explicit configuration.'
    $env:CFLAGS = '/DLAUNCH_FIXTURE'
    $global:SnowLaunchBuildCalls = @()
    & $launcher -Detached
    Require ($global:SnowLaunchBuildCalls.Count -eq 2) 'Changed native Cargo flags require configuration.'
    $global:SnowLaunchBuildCalls = @()
    & $launcher -Detached
    Require ($global:SnowLaunchBuildCalls.Count -eq 1) 'Updated configuration must be reusable on the next launch.'
    $global:SnowLaunchBuildCalls = @()
    & $launcher -Detached -Reconfigure
    Require ($global:SnowLaunchBuildCalls.Count -eq 2 -and
        $global:SnowLaunchBuildCalls[0] -notcontains '--fresh') 'Explicit reconfiguration must preserve a compatible cache.'
    $global:SnowLaunchConfigureFailure = $true
    $global:SnowLaunchBuildCalls = @()
    $failure = ''
    try { & $launcher -Detached -Reconfigure } catch { $failure = $_.Exception.Message }
    Require ($failure -like 'CMake configure failed*' -and $global:SnowLaunchBuildCalls.Count -eq 1) `
        'A failed configure must prevent build and launch.'
    $global:SnowLaunchConfigureFailure = $false
    $global:SnowLaunchBuildCalls = @()
    & $launcher -Detached
    Require ($global:SnowLaunchBuildCalls.Count -eq 2) 'A failed configure must invalidate the previous success marker.'

    # Stop bootstrap before compiler discovery, after its real dependency
    # environment setup, so this also covers direct bootstrap invocations.
    $env:VCPKG_FORCE_SYSTEM_BINARIES = '1'
    Remove-Item Env:VCPKG_FORCE_DOWNLOADED_BINARIES -ErrorAction SilentlyContinue
    $global:SnowLaunchStopBootstrap = $true
    $failure = ''
    try { & (Join-Path $scripts 'bootstrap.ps1') -SkipQtValidation }
    catch { $failure = $_.Exception.Message }
    Require ($failure -eq 'Bootstrap environment captured.') 'Capture bootstrap before native work.'
    Require (-not (Test-Path Env:VCPKG_FORCE_DOWNLOADED_BINARIES) -and
        -not (Test-Path Env:VCPKG_FORCE_SYSTEM_BINARIES)) `
        'Bootstrap and subsequent builds must share the minimum-version tool policy.'
    Write-Output "PASS: launcher contracts and batch PowerShell fallback (PowerShell $($PSVersionTable.PSVersion))."
}
finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    Remove-Item Function:\cmake -ErrorAction SilentlyContinue
    Remove-Variable -Scope Global -Name SnowLaunchFixtureExitCode, SnowLaunchFixtureProcess,
        SnowLaunchPath, SnowLaunchBuildCalls, SnowLaunchStopBootstrap, SnowLaunchConfigureFailure -ErrorAction SilentlyContinue
    $absolute = [IO.Path]::GetFullPath($fixture)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $absolute.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe fixture cleanup path.'
    }
    if (Test-Path -LiteralPath $absolute) { Remove-Item -LiteralPath $absolute -Recurse -Force }
}

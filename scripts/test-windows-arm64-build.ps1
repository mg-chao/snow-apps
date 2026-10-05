#requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Require-Rejected([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Require $rejected $Message
}
function Write-PeFixture([string]$Path, [int]$Machine) {
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path)
    $bytes = [byte[]]::new(128)
    $bytes[0] = 0x4D; $bytes[1] = 0x5A; $bytes[0x3C] = 64
    $bytes[64] = 0x50; $bytes[65] = 0x45
    [BitConverter]::GetBytes([uint16]$Machine).CopyTo($bytes, 68)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

$testRoot = Join-Path $script:SnowRepoRoot ('build/windows-arm64-contract-' + [guid]::NewGuid().ToString('N'))
$names = @('PATH', 'INCLUDE', 'LIB', 'VSINSTALLDIR', 'VCINSTALLDIR', 'VCToolsInstallDir',
    'WindowsSdkDir', 'WindowsSDKVersion', 'SNOW_MSVC_HOST_ARCHITECTURE',
    'SNOW_MSVC_TARGET_ARCHITECTURE', 'VSCMD_ARG_HOST_ARCH', 'VSCMD_ARG_TGT_ARCH',
    'Qt6_DIR', 'SNOW_QT_STATIC_DIR', 'QT_HOST_PATH', 'SNOW_QT_HOST_PREFIX', 'LIBCLANG_PATH')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $null = New-Item -ItemType Directory -Force -Path $testRoot
    $arm = Get-SnowWindowsTarget -Architecture arm64
    $x64 = Get-SnowWindowsTarget -Architecture x64
    Require ($arm.RustTarget -ceq 'aarch64-pc-windows-msvc' -and $arm.Machine -ceq 'AA64' -and
        $arm.Platform -ceq 'windows-arm64' -and $arm.GeneratorPlatform -ceq 'ARM64') 'ARM64 target identity must be consistent.'
    Require ($x64.Preset -ceq 'snow-shot-msvc-release' -and $x64.InstalledRoot -cne $arm.InstalledRoot) 'Legacy x64 defaults and separate dependency roots are required.'
    $bridge = Get-Content -Raw (Join-Path $script:SnowRepoRoot 'snow-crates/crates/snow-ocr-process/native/diagnosticsbridge.cpp')
    Require ($bridge -match '#include "util/win/context_wrappers.h"' -and
        $bridge -match 'exception\.ExceptionAddress\s*=\s*crashpad::ProgramCounterFromCONTEXT\(&context\)' -and
        $bridge -notmatch 'context\.(Rip|Eip|Pc)\b') 'Windows panic diagnostics must use Crashpad architecture-aware program-counter access.'
    foreach ($mode in @('Debug', 'Performance', 'Release', 'Fast')) {
        $preset = Resolve-SnowPreset -Configuration $mode -Architecture arm64
        $target = Get-SnowWindowsTarget -Preset $preset
        Require ($target.Architecture -ceq 'arm64') "Preset $preset must select ARM64."
        Require ($target.IsStatic -eq ($mode -in @('Release', 'Fast'))) 'Only production configurations use static dependencies.'
    }
    Require-Rejected { Get-SnowWindowsTarget -Architecture x64 -Preset snow-shot-msvc-arm64-debug } 'Conflicting target selections must fail.'
    Require-Rejected { Get-SnowWindowsTarget -Preset arbitrary-arm64-debug } 'Unknown presets must fail.'

    $binary = Join-Path $testRoot 'arm64.exe'
    Write-PeFixture $binary 0xAA64
    Require ((Get-SnowPeMachine $binary) -eq 0xAA64) 'ARM64 PE headers must be read without execution.'
    Write-PeFixture $binary 0x8664
    Require ((Get-SnowPeMachine $binary) -eq 0x8664) 'x64 PE headers must be distinguished.'
    $bytes = [IO.File]::ReadAllBytes($binary)
    [BitConverter]::GetBytes([uint32]::MaxValue).CopyTo($bytes, 0x3C)
    [IO.File]::WriteAllBytes($binary, $bytes)
    Require-Rejected { Get-SnowPeMachine $binary } 'Out-of-range PE offsets must fail.'
    [IO.File]::WriteAllBytes($binary, [byte[]]@(0x4D, 0x5A))
    Require-Rejected { Get-SnowPeMachine $binary } 'Truncated PE files must fail.'

    $qtPrefix = Join-Path $testRoot 'Qt target'
    $qtDir = Join-Path $qtPrefix 'lib/cmake/Qt6'
    $null = New-Item -ItemType Directory -Force -Path $qtDir
    [IO.File]::WriteAllText((Join-Path $qtDir 'Qt6ConfigVersion.cmake'), 'set(PACKAGE_VERSION "6.12.0")')
    Write-PeFixture (Join-Path $qtPrefix 'bin/Qt6Core.dll') 0xAA64
    Require (Test-SnowQtArchitecture -Qt6Dir $qtDir -Architecture arm64) 'ARM64 Qt must match target headers.'
    Require (-not (Test-SnowQtArchitecture -Qt6Dir $qtDir -Architecture x64)) 'An ARM64 Qt kit cannot satisfy an x64 target.'
    Write-PeFixture (Join-Path $qtPrefix 'bin/Qt6Core.dll') 0x8664
    Require (-not (Test-SnowQtArchitecture -Qt6Dir $qtDir -Architecture arm64)) 'An x64 Qt kit cannot satisfy an ARM64 target.'

    # Simulate both physical hosts, including emulated PowerShell on ARM64.
    $script:FixtureHost = 'x64'
    function Get-SnowWindowsHostArchitecture { return $script:FixtureHost }
    $vs = Join-Path $testRoot 'VS'
    $msvc = Join-Path $vs 'VC/Tools/MSVC/14.51.99999'
    $sdk = Join-Path $testRoot 'Windows SDK'
    foreach ($hostArch in @('x64', 'arm64')) {
        foreach ($targetArch in @('x64', 'arm64')) {
            $bin = Join-Path $msvc "bin/Host$hostArch/$targetArch"
            $null = New-Item -ItemType Directory -Force -Path $bin
            foreach ($tool in @('cl.exe', 'link.exe', 'lib.exe', 'dumpbin.exe', 'armasm64.exe')) {
                [IO.File]::WriteAllText((Join-Path $bin $tool), 'fixture')
            }
        }
        $sdkBin = Join-Path $sdk "bin/10.0.99999.0/$hostArch"
        $null = New-Item -ItemType Directory -Force -Path $sdkBin
        [IO.File]::WriteAllText((Join-Path $sdkBin 'rc.exe'), 'fixture')
        $null = New-Item -ItemType Directory -Force -Path (Join-Path $msvc "lib/$hostArch"),
            (Join-Path $sdk "Lib/10.0.99999.0/um/$hostArch"),
            (Join-Path $sdk "Lib/10.0.99999.0/ucrt/$hostArch")
    }
    $env:VSINSTALLDIR = $vs
    $env:WindowsSdkDir = $sdk
    $env:WindowsSDKVersion = '10.0.99999.0'
    foreach ($pair in @(@('x64', 'x64'), @('x64', 'arm64'), @('arm64', 'arm64'))) {
        $script:FixtureHost = $pair[0]
        $path = Add-SnowMsvcToolsToPath -Architecture $pair[1]
        Require ($path -like "*Host$($pair[0])\$($pair[1])") 'MSVC compiler must reflect host and target independently.'
        Require (@($env:LIB -split ';' | Where-Object { $_ -notlike "*\$($pair[1])" }).Count -eq 0) 'Every SDK/MSVC library root must match target.'
        Require (($env:PATH -split ';')[0] -ceq $path) 'Target compiler must be first for bare cl/armasm custom commands.'
        Require ((Get-SnowWindowsTarget -Architecture $pair[1]).HostTriplet -ceq "$($pair[0])-windows") 'Host packages must remain runnable.'
    }
    $script:FixtureHost = 'x64'
    $hostQt = Join-Path $testRoot 'Qt host'
    $hostQtDir = Join-Path $hostQt 'lib/cmake/Qt6'
    $null = New-Item -ItemType Directory -Force -Path $hostQtDir
    [IO.File]::WriteAllText((Join-Path $hostQtDir 'Qt6ConfigVersion.cmake'), 'set(PACKAGE_VERSION "6.12.0")')
    foreach ($name in @('moc', 'rcc', 'uic', 'lrelease', 'lupdate')) { Write-PeFixture (Join-Path $hostQt "bin/$name.exe") 0x8664 }
    $script:FixtureToolVersion = '6.12.0'
    function Get-SnowQtHostToolVersion { param([string]$Path, [string]$Name) return $script:FixtureToolVersion }
    Require ((Resolve-SnowQtHostPrefix -HostQtPrefix $hostQt) -ceq $hostQt) 'Cross builds must select a same-version runnable host kit.'
    $script:FixtureToolVersion = '6.11.1'
    Require-Rejected { Resolve-SnowQtHostPrefix -HostQtPrefix $hostQt } 'A host binary version must match the target, even if kit metadata matches.'
    $script:FixtureToolVersion = '6.12.0'
    Write-PeFixture (Join-Path $hostQt 'bin/moc.exe') 0xAA64
    # Explicit bad kits must fail rather than silently falling through to an installed kit.
    Require-Rejected { Resolve-SnowQtHostPrefix -HostQtPrefix $hostQt } 'An explicit host kit containing target binaries must fail.'

    $env:Qt6_DIR = $qtDir
    $env:SNOW_QT_STATIC_DIR = $qtDir
    $env:QT_HOST_PATH = $hostQt
    $target = Get-SnowWindowsTarget -Architecture arm64
    $env:LIBCLANG_PATH = Join-Path $testRoot 'host libclang'
    $arguments = @(Get-SnowConfigureArguments -Preset $target.Preset -BuildDirectory $testRoot)
    Require ($arguments -ccontains "-DSNOW_LIBCLANG_BIN_DIR:PATH=$env:LIBCLANG_PATH" -and
        $arguments -ccontains "-DSNOW_SHOT_LIBCLANG_DIR:PATH=$env:LIBCLANG_PATH") 'CMake and Rust must use the validated host libclang.'
    $cachePath = Join-Path $testRoot 'CMakeCache.txt'
    $cacheLines = @(
        "Qt6_DIR:PATH=$($qtDir.Replace('\', '/'))",
        "SNOW_QT_STATIC_DIR:PATH=$($qtDir.Replace('\', '/'))",
        "QT_HOST_PATH:PATH=$($hostQt.Replace('\', '/'))",
        "Z_VCPKG_POWERSHELL_PATH:INTERNAL=$((Get-Command pwsh.exe).Source)",
        'CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026',
        'CMAKE_GENERATOR_PLATFORM:INTERNAL=ARM64',
        'CMAKE_GENERATOR_TOOLSET:INTERNAL=version=14.51',
        'SNOW_RUST_TARGET:STRING=aarch64-pc-windows-msvc',
        'VCPKG_HOST_TRIPLET:STRING=x64-windows',
        'VCPKG_TARGET_TRIPLET:STRING=arm64-windows-static',
        "VCPKG_INSTALLED_DIR:PATH=$($target.InstalledRoot.Replace('\', '/'))",
        "CMAKE_HOME_DIRECTORY:INTERNAL=$($script:SnowRepoRoot.Replace('\', '/'))"
    )
    $cacheText = $cacheLines -join "`n"
    [IO.File]::WriteAllText($cachePath, $cacheText)
    Require (Test-SnowCacheAlignment -CachePath $cachePath -Preset $target.Preset) 'A matching cross-build cache must be reusable.'
    foreach ($drift in @(@('ARM64', 'x64'), @('aarch64-pc-windows-msvc', 'x86_64-pc-windows-msvc'),
            @('VCPKG_HOST_TRIPLET:STRING=x64-windows', 'VCPKG_HOST_TRIPLET:STRING=arm64-windows'),
            @($hostQt.Replace('\', '/'), $qtPrefix.Replace('\', '/')))) {
        [IO.File]::WriteAllText($cachePath, $cacheText.Replace($drift[0], $drift[1]))
        Require (-not (Test-SnowCacheAlignment -CachePath $cachePath -Preset $target.Preset)) 'Architecture or host-tool cache drift must require a fresh configure.'
    }

    $presets = Get-Content -Raw (Join-Path $script:SnowRepoRoot 'CMakePresets.json') | ConvertFrom-Json
    foreach ($mode in @('debug', 'performance', 'release', 'fast')) {
        $name = "snow-shot-msvc-arm64-$mode"
        Require (@($presets.configurePresets | Where-Object name -CEQ $name).Count -eq 1) 'Each ARM64 build configuration must exist exactly once.'
        Require (@($presets.buildPresets | Where-Object configurePreset -CEQ $name).Count -eq 1) 'Each ARM64 configuration needs a build preset.'
    }
    Write-Output 'Windows ARM64 build architecture tests passed.'
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    $buildRoot = [IO.Path]::GetFullPath((Join-Path $script:SnowRepoRoot 'build')) + [IO.Path]::DirectorySeparatorChar
    $resolved = [IO.Path]::GetFullPath($testRoot)
    if (-not $resolved.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture cleanup path.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}

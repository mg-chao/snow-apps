[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Require-Arguments([string[]]$Actual, [string[]]$Expected, [string]$Message) {
    Require (($Actual -join "`n") -ceq ($Expected -join "`n")) $Message
}

$testRoot = Join-Path $repoRoot ("build/windows-release-workflow-tests-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
try {
    $customBuild = Join-Path $testRoot "custom-release"
    New-Item -ItemType Directory -Path $customBuild | Out-Null
    $expectedConfigure = @("--preset", "snow-shot-msvc-release", "-S", $repoRoot,
        "-B", $customBuild, "-DVCPKG_HOST_TRIPLET=$((Get-SnowWindowsTarget).HostTriplet)",
        '-DSNOW_RUST_TARGET=x86_64-pc-windows-msvc')
    if ($env:LIBCLANG_PATH) {
        $expectedConfigure += "-DSNOW_LIBCLANG_BIN_DIR:PATH=$env:LIBCLANG_PATH"
        $expectedConfigure += "-DSNOW_SHOT_LIBCLANG_DIR:PATH=$env:LIBCLANG_PATH"
    }
    if ((Get-SnowWindowsHostArchitecture) -ceq 'x64') { $expectedConfigure += '-UQT_HOST_PATH' }
    else { $expectedConfigure += "-DQT_HOST_PATH:PATH=$env:QT_HOST_PATH" }

    $missingCacheArguments = @(Get-SnowConfigureArguments -Preset snow-shot-msvc-release `
        -BuildDirectory $customBuild)
    Require-Arguments $missingCacheArguments $expectedConfigure "A new build must use the requested directory."

    Set-Content -LiteralPath (Join-Path $customBuild "CMakeCache.txt") -Value "fixture" -Encoding utf8
    function Test-SnowCacheAlignment {
        param([string]$CachePath, [string]$Preset)
        return $script:CacheAligned
    }
    $script:CacheAligned = $true
    $alignedArguments = @(Get-SnowConfigureArguments -Preset snow-shot-msvc-release `
        -BuildDirectory $customBuild)
    Require-Arguments $alignedArguments $expectedConfigure "An aligned cache must be reused."

    $script:CacheAligned = $false
    $unalignedArguments = @(Get-SnowConfigureArguments -Preset snow-shot-msvc-release `
        -BuildDirectory $customBuild)
    Require-Arguments $unalignedArguments (@("--fresh") + $expectedConfigure) `
        "An unaligned cache must be replaced."

    $presetData = Get-Content -LiteralPath (Join-Path $repoRoot "CMakePresets.json") -Raw |
        ConvertFrom-Json
    foreach ($name in @("build-snow-shot-msvc-release", "build-snow-shot-msvc-fast")) {
        $preset = @($presetData.buildPresets | Where-Object { $_.name -ceq $name })
        Require ($preset.Count -eq 1 -and @($preset[0].targets).Count -eq 2 -and
            $preset[0].targets[0] -ceq "snow_shot" -and $preset[0].targets[1] -ceq "snow_shot_mini") "$name must build both Snow Shot editions by default."
    }

    $fixtureScripts = Join-Path $testRoot "scripts"
    New-Item -ItemType Directory -Path $fixtureScripts | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'stage-snow-shot-release.ps1') -Destination $fixtureScripts
    Set-Content -LiteralPath (Join-Path $fixtureScripts 'package-snow-shot.ps1') -Value @'
param([string]$Architecture, [string]$BuildDirectory, [string]$InstallDirectory,
    [int]$Parallelism, [string]$OcrRuntimeArchive, [switch]$SkipBuild)
$global:SnowStageParameters = $PSBoundParameters
$global:LASTEXITCODE = 0
'@
    & (Join-Path $fixtureScripts 'stage-snow-shot-release.ps1') -Architecture arm64 -SkipBuild
    Require ($global:SnowStageParameters.Architecture -ceq 'arm64' -and
        $global:SnowStageParameters.BuildDirectory -ceq 'build/snow-shot-msvc-arm64-release' -and
        $global:SnowStageParameters.InstallDirectory -ceq 'artifacts/windows-arm64/snow-shot' -and
        $global:SnowStageParameters.SkipBuild) 'ARM release staging must stay inside its architecture artifact root.'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "build.ps1") -Destination $fixtureScripts
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'run-snow-shot.ps1') -Destination $fixtureScripts
    @'
function Set-SnowBuildEnvironment { param([string]$Preset) return $null }
function Get-SnowWindowsTarget {
    param([string]$Preset)
    return [pscustomobject]@{ IsStatic = ($Preset -match '(release|fast)$'); HostArchitecture = 'x64'
        Configuration = $(if ($Preset -match '-debug$') { 'Debug' } else { 'Release' })
        Architecture = $(if ($Preset -match '-arm64-') { 'arm64' } else { 'x64' }) }
}
function Get-SnowConfigureArguments {
    param([string]$Preset, [string]$BuildDirectory)
    return @("--preset", $Preset, "-B", $BuildDirectory)
}
'@ | Set-Content -LiteralPath (Join-Path $fixtureScripts "snow-build-environment.ps1") -Encoding utf8
    @'
param([switch]$SkipDependencyInstall, [string[]]$VcpkgVariants, [string]$Architecture, [string]$Preset)
$global:SnowBootstrapVariants = @($VcpkgVariants)
$global:LASTEXITCODE = 0
'@ | Set-Content -LiteralPath (Join-Path $fixtureScripts "bootstrap.ps1") -Encoding utf8

    function global:cmake {
        $global:SnowBuildCalls += ,@($args)
        if ($args -contains '-B') {
            $configuredBuild = $args[[Array]::IndexOf($args, '-B') + 1]
            $null = New-Item -ItemType Directory -Path $configuredBuild -Force
            "SNOW_APPS_BUILD_SNOW_SHOT_MINI:BOOL=$global:SnowMiniEnabled" |
                Set-Content -LiteralPath (Join-Path $configuredBuild 'CMakeCache.txt')
        }
        $global:LASTEXITCODE = 0
    }
    $global:SnowMiniEnabled = 'ON'
    $launchRejected = $false
    try { & (Join-Path $fixtureScripts 'run-snow-shot.ps1') -Preset snow-shot-msvc-arm64-fast -NoBuild }
    catch { $launchRejected = $_.Exception.Message -match 'requires a Windows ARM64 host' }
    Require $launchRejected 'Cross-built ARM64 launchers must reject x64 hosts before stopping or building applications.'
    $global:SnowBuildCalls = @()
    $global:SnowBootstrapVariants = @()
    & (Join-Path $fixtureScripts "build.ps1") -Preset snow-shot-msvc-release
    Require-Arguments $global:SnowBootstrapVariants @("Static") `
        "Release builds must bootstrap only static host tools."
    Require ($global:SnowBuildCalls.Count -eq 2) "Release build must configure and build once."
    Require-Arguments $global:SnowBuildCalls[1] `
        @("--build", "--preset", "build-snow-shot-msvc-release", "--parallel") `
        "Release build must use the preset's Snow Shot target."

    $global:SnowMiniEnabled = 'OFF'
    $global:SnowBuildCalls = @()
    & (Join-Path $fixtureScripts "build.ps1") -Preset snow-shot-msvc-release -SkipBootstrap
    Require-Arguments $global:SnowBuildCalls[1] `
        @('--build', '--preset', 'build-snow-shot-msvc-release', '--parallel', '--target', 'snow_shot') `
        'Default release wrapper must respect an explicitly disabled Mini target.'

    $global:SnowBuildCalls = @()
    & (Join-Path $fixtureScripts "build.ps1") -Preset windows-msvc-debug -Target snow_shot
    Require-Arguments $global:SnowBootstrapVariants @("Dynamic") `
        "Debug builds must bootstrap only dynamic host tools."
    Require-Arguments $global:SnowBuildCalls[1] `
        @("--build", "--preset", "build-windows-msvc-debug", "--parallel", "--target", "snow_shot") `
        "An explicit target must override the preset default."
}
finally {
    Remove-Item Function:\cmake -ErrorAction SilentlyContinue
    Remove-Variable SnowBuildCalls, SnowBootstrapVariants, SnowMiniEnabled -Scope Global -ErrorAction SilentlyContinue
    Remove-Variable SnowStageParameters -Scope Global -ErrorAction SilentlyContinue
    $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $resolvedBuildRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot "build"))
    if (-not $resolvedTestRoot.StartsWith($resolvedBuildRoot.TrimEnd('\') + '\',
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a test directory outside $resolvedBuildRoot"
    }
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}

Write-Output "Windows release workflow tests passed."

#requires -Version 7.2
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $repoRoot "snow_shot/scripts/performance-environment.ps1")

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$environmentNames = @("PATH", "QTDIR", "Qt6_DIR", "QT_QPA_PLATFORM", "QT_QPA_PLATFORM_PLUGIN_PATH")
$savedEnvironment = @{}
foreach ($name in $environmentNames) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}
$testRoot = Join-Path $repoRoot ("build/performance-qt-contract-" + [guid]::NewGuid().ToString("N"))
try {
    $kit = Join-Path $testRoot "Qt kit"
    $qtDir = Join-Path $kit "lib/cmake/Qt6"
    $coreDir = Join-Path $kit "lib/cmake/Qt6Core"
    $bin = Join-Path $kit "bin"
    New-Item -ItemType Directory -Path $qtDir, $coreDir, $bin -Force | Out-Null
    $coreTargets = Join-Path $coreDir "Qt6CoreTargets.cmake"
    $env:Qt6_DIR = $qtDir
    $env:QTDIR = $kit
    $env:QT_QPA_PLATFORM = "original-platform"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = "original-plugin-path"
    $originalPath = $env:PATH

    Set-Content -LiteralPath $coreTargets -Value 'QT_ENABLED_PUBLIC_FEATURES "static;timezone"'
    $snapshot = Set-SnowPerformanceQtRuntime -Platform offscreen
    Require ($env:QT_QPA_PLATFORM -ceq "offscreen") "Static replay must select offscreen QPA."
    Require ([string]::IsNullOrEmpty($env:QT_QPA_PLATFORM_PLUGIN_PATH)) `
        "Static Qt must not inherit a stale external platform plugin directory."
    Require (-not (Test-Path -LiteralPath (Join-Path $bin "Qt6Core.dll"))) `
        "The static fixture must work without any Qt DLL."
    Restore-SnowPerformanceQtRuntime -Snapshot $snapshot
    Require ($env:PATH -ceq $originalPath -and
        $env:QT_QPA_PLATFORM -ceq "original-platform" -and
        $env:QT_QPA_PLATFORM_PLUGIN_PATH -ceq "original-plugin-path") `
        "Runtime cleanup must restore the caller's environment."

    Set-Content -LiteralPath $coreTargets -Value 'QT_ENABLED_PUBLIC_FEATURES "shared;timezone"'
    $rejected = $false
    try { Set-SnowPerformanceQtRuntime | Out-Null }
    catch { $rejected = $_.Exception.Message -match "selected shared Qt $script:SnowQtVersion runtime" }
    Require $rejected "A shared kit without its runtime DLL must fail before launching benchmarks."
    Require ($env:PATH -ceq $originalPath -and $env:QT_QPA_PLATFORM -ceq "original-platform") `
        "Failed shared-kit validation must preserve the environment."
    Set-Content -LiteralPath (Join-Path $bin "Qt6Core.dll") -Value "fixture"
    $snapshot = Set-SnowPerformanceQtRuntime
    Require ($env:QT_QPA_PLATFORM -ceq "windows" -and
        $env:QT_QPA_PLATFORM_PLUGIN_PATH -ceq (Join-Path $kit "plugins/platforms")) `
        "Shared native benchmarks must use the selected kit's platform plugins."
    Restore-SnowPerformanceQtRuntime -Snapshot $snapshot

    function Set-SnowQtEnvironment {
        param([string]$Qt6Dir, [string]$Preset)
        $global:SnowPerformanceFixtureQtDir = $Qt6Dir
        $global:SnowPerformanceFixtureQtPreset = $Preset
    }
    function Set-SnowBuildEnvironment {
        param([string]$Preset)
        $global:SnowPerformanceFixtureBuildPreset = $Preset
    }
    Initialize-SnowPerformanceEnvironment -QtBin $bin
    Require ($global:SnowPerformanceFixtureQtDir -ceq $qtDir -and
        $global:SnowPerformanceFixtureQtPreset -ceq "windows-msvc-performance" -and
        $global:SnowPerformanceFixtureBuildPreset -ceq "windows-msvc-performance") `
        "An explicit Qt bin must select the same kit for the Release performance build."

    # Exercise the actual configure wrapper with stubbed toolchain/native calls.
    # No compiler, network, Rust run, benchmark, or interactive window is started.
    $fixtureScripts = Join-Path $testRoot "workspace/snow_shot/scripts"
    New-Item -ItemType Directory -Path $fixtureScripts -Force | Out-Null
    $configure = Join-Path $fixtureScripts "configure-msvc-perf.ps1"
    Copy-Item -LiteralPath (Join-Path $repoRoot "snow_shot/scripts/configure-msvc-perf.ps1") `
        -Destination $configure
    Set-Content -LiteralPath (Join-Path $fixtureScripts "performance-environment.ps1") -Value @'
function Initialize-SnowPerformanceEnvironment {
    param([string]$QtBin)
    $global:SnowPerformanceFixtureInitialized = $QtBin
}
function Get-SnowConfigureArguments {
    param([string]$Preset, [string]$BuildDirectory)
    $arguments = @("--preset", $Preset, "-B", $BuildDirectory)
    if ($global:SnowPerformanceFixtureAlignedFresh) { $arguments = @("--fresh") + $arguments }
    return $arguments
}
'@
    function cmake {
        $global:SnowPerformanceFixtureArguments = @($args)
        $global:LASTEXITCODE = 0
    }
    foreach ($alreadyFresh in @($false, $true)) {
        $global:SnowPerformanceFixtureAlignedFresh = $alreadyFresh
        & $configure -Fresh -QtBin $bin
        Require ($global:SnowPerformanceFixtureInitialized -ceq $bin) `
            "Configure must initialize the selected toolchain in the current PowerShell host."
        Require (@($global:SnowPerformanceFixtureArguments | Where-Object { $_ -ceq "--fresh" }).Count -eq 1) `
            "Explicit Fresh and automatic cache alignment must not duplicate --fresh."
        Require ($global:SnowPerformanceFixtureArguments -contains "windows-msvc-performance") `
            "Benchmarks must configure the dedicated Release performance preset."
    }

    foreach ($name in @("configure-msvc-perf", "performance-environment", "run-scrolling-perf",
            "run-toolbar-perf", "run-pin-to-screen-perf", "run-capture-startup-perf", "run-file-pin-batch-perf")) {
        $path = Join-Path $repoRoot "snow_shot/scripts/$name.ps1"
        $errors = $null
        $ast = [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$null, [ref]$errors)
        Require ($errors.Count -eq 0) "$name must parse successfully."
        $legacyHost = $ast.Find({
            param($node)
            $node -is [System.Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -eq "powershell"
        }, $true)
        Require ($null -eq $legacyHost) "$name must not launch Windows PowerShell 5."
    }
}
finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], "Process")
    }
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "build")) + [IO.Path]::DirectorySeparatorChar
        $resolvedTarget = (Resolve-Path -LiteralPath $testRoot).Path
        if (-not $resolvedTarget.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove a performance fixture outside the build directory."
        }
        Remove-Item -LiteralPath $resolvedTarget -Recurse -Force
    }
}
Write-Output "Performance Qt runtime and configure contract tests passed."

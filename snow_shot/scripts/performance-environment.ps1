#requires -Version 7.2

. (Join-Path $PSScriptRoot "../../scripts/snow-build-environment.ps1")

function Get-SnowPerformanceTarget {
    param([ValidateSet("x64", "arm64")][string]$Architecture = "x64", [switch]$RequireNative)

    $target = Get-SnowWindowsTarget -Preset (Resolve-SnowPreset -Configuration Performance -Architecture $Architecture)
    if ($RequireNative -and $target.HostArchitecture -cne $target.Architecture) {
        throw "Performance benchmarks require a native $Architecture Windows host; this host is $($target.HostArchitecture)."
    }
    return $target
}

function Assert-SnowPerformanceExecutable {
    param([Parameter(Mandatory)][string]$Path,
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64")

    $target = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
    if ((Get-SnowPeMachine -Path $Path) -ne $target.PeMachine) {
        throw "Performance executable does not match the native $Architecture target: $Path"
    }
}

function Initialize-SnowPerformanceEnvironment {
    param([string]$QtBin = "", [ValidateSet("x64", "arm64")][string]$Architecture = "x64")

    $target = Get-SnowPerformanceTarget -Architecture $Architecture

    if (-not [string]::IsNullOrWhiteSpace($QtBin)) {
        $qtDir = [IO.Path]::GetFullPath((Join-Path $QtBin "../lib/cmake/Qt6"))
        Set-SnowQtEnvironment -Qt6Dir $qtDir -Preset $target.Preset | Out-Null
    }
    Set-SnowBuildEnvironment -Preset $target.Preset | Out-Null
}

function Set-SnowPerformanceQtRuntime {
    param([ValidateSet("windows", "offscreen")][string]$Platform = "windows",
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64")

    $target = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative

    $qtBin = Join-Path $env:QTDIR "bin"
    $coreTargets = Join-Path $env:Qt6_DIR "../Qt6Core/Qt6CoreTargets.cmake"
    $staticQt = Test-SnowQtTargetFeature (Get-Content -LiteralPath $coreTargets -Raw) `
        "PUBLIC" "static" $true
    if (-not $staticQt -and -not (Test-Path -LiteralPath (Join-Path $qtBin "Qt6Core.dll"))) {
        throw "The selected shared Qt $script:SnowQtVersion runtime is missing from $qtBin"
    }
    if (-not $staticQt -and (Get-SnowPeMachine -Path (Join-Path $qtBin "Qt6Core.dll")) -ne $target.PeMachine) {
        throw "The selected shared Qt runtime does not match the native $Architecture target."
    }
    $snapshot = @{
        PATH = $env:PATH
        QT_QPA_PLATFORM = $env:QT_QPA_PLATFORM
        QT_QPA_PLATFORM_PLUGIN_PATH = $env:QT_QPA_PLATFORM_PLUGIN_PATH
    }
    $dependencyBin = Join-Path $target.InstalledRoot "$($target.Triplet)/bin"
    $env:PATH = "$qtBin;$dependencyBin;$env:PATH"
    $env:QT_QPA_PLATFORM = $Platform
    # Static Qt imports platform plugins into each benchmark at link time.
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = if ($staticQt) { $null } else {
        Join-Path $env:QTDIR "plugins/platforms"
    }
    return $snapshot
}

function Restore-SnowPerformanceQtRuntime {
    param([Parameter(Mandatory = $true)][hashtable]$Snapshot)

    foreach ($name in $Snapshot.Keys) {
        [Environment]::SetEnvironmentVariable($name, $Snapshot[$name], "Process")
    }
}

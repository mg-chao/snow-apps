#requires -Version 7.2

. (Join-Path $PSScriptRoot "../../scripts/snow-build-environment.ps1")

function Initialize-SnowPerformanceEnvironment {
    param([string]$QtBin = "")

    if (-not [string]::IsNullOrWhiteSpace($QtBin)) {
        $qtDir = [IO.Path]::GetFullPath((Join-Path $QtBin "../lib/cmake/Qt6"))
        Set-SnowQtEnvironment -Qt6Dir $qtDir -Preset "windows-msvc-performance" | Out-Null
    }
    Set-SnowBuildEnvironment -Preset "windows-msvc-performance" | Out-Null
}

function Set-SnowPerformanceQtRuntime {
    param([ValidateSet("windows", "offscreen")][string]$Platform = "windows")

    $qtBin = Join-Path $env:QTDIR "bin"
    $coreTargets = Join-Path $env:Qt6_DIR "../Qt6Core/Qt6CoreTargets.cmake"
    $staticQt = Test-SnowQtTargetFeature (Get-Content -LiteralPath $coreTargets -Raw) `
        "PUBLIC" "static" $true
    if (-not $staticQt -and -not (Test-Path -LiteralPath (Join-Path $qtBin "Qt6Core.dll"))) {
        throw "The selected shared Qt $script:SnowQtVersion runtime is missing from $qtBin"
    }
    $snapshot = @{
        PATH = $env:PATH
        QT_QPA_PLATFORM = $env:QT_QPA_PLATFORM
        QT_QPA_PLATFORM_PLUGIN_PATH = $env:QT_QPA_PLATFORM_PLUGIN_PATH
    }
    $env:PATH = "$qtBin;$env:PATH"
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

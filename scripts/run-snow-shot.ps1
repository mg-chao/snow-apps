[CmdletBinding()]
param(
    [ValidateSet(
        "windows-msvc-debug",
        "windows-msvc-performance",
        "snow-shot-msvc-release",
        "snow-shot-msvc-fast"
    )]
    [string]$Preset = "windows-msvc-debug",
    [switch]$Clean,
    [switch]$NoBuild,
    [switch]$Detached
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildDirectory = Join-Path $repoRoot "build\$Preset"
$configuration = switch ($Preset) {
    "windows-msvc-debug" { "Debug" }
    default { "Release" }
}
$executablePath = Join-Path $buildDirectory "snow_shot\$configuration\snow_shot.exe"

function Test-PathIsUnderDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Directory
    )

    $directoryPath = [System.IO.Path]::GetFullPath($Directory).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    ) + [System.IO.Path]::DirectorySeparatorChar
    $candidatePath = [System.IO.Path]::GetFullPath($Path)

    return $candidatePath.StartsWith(
        $directoryPath,
        [System.StringComparison]::OrdinalIgnoreCase
    )
}

function Stop-RunningBuildInstance {
    param([Parameter(Mandatory = $true)][string]$BuildDirectory)

    $runningProcesses = @(Get-Process -Name "snow_shot" -ErrorAction SilentlyContinue)
    foreach ($process in $runningProcesses) {
        try {
            $processPath = $process.Path
        }
        catch {
            Write-Verbose "Unable to inspect snow_shot process $($process.Id); leaving it running."
            continue
        }

        if ([string]::IsNullOrWhiteSpace($processPath) -or
            -not (Test-PathIsUnderDirectory -Path $processPath -Directory $BuildDirectory)) {
            continue
        }

        Write-Host "Stopping the running development instance (PID $($process.Id))..."
        try {
            if ($process.CloseMainWindow() -and $process.WaitForExit(1500)) {
                continue
            }
        }
        catch {
            # The process may have exited between discovery and shutdown.
        }

        $process.Refresh()
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force
            if (-not $process.WaitForExit(5000)) {
                throw "snow_shot process $($process.Id) did not stop; the executable is still locked."
            }
        }
    }
}

if (-not $NoBuild) {
    Stop-RunningBuildInstance -BuildDirectory $buildDirectory

    $buildScript = Join-Path $PSScriptRoot "build.ps1"
    $vcpkgExecutable = Join-Path $repoRoot ".tools\vcpkg\vcpkg.exe"
    $skipBootstrap = Test-Path -LiteralPath $vcpkgExecutable -PathType Leaf
    & $buildScript -Preset $Preset -Target "snow_shot" -Clean:$Clean -SkipBootstrap:$skipBootstrap
    if ($LASTEXITCODE -ne 0) {
        throw "Snow Shot build failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    $buildHint = if ($NoBuild) {
        "Run this script without -NoBuild to create it."
    }
    else {
        "The build completed without producing the expected target."
    }
    throw "Snow Shot executable was not found at '$executablePath'. $buildHint"
}

$workingDirectory = Split-Path -Parent $executablePath
if ($Detached) {
    Start-Process -FilePath $executablePath -WorkingDirectory $workingDirectory | Out-Null
    return
}

Push-Location $workingDirectory
try {
    & $executablePath
    if ($LASTEXITCODE -ne 0) {
        throw "Snow Shot exited with code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

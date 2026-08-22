[CmdletBinding()]
param(
    [ValidateSet(
        "windows-msvc-debug",
        "windows-msvc-performance",
        "snow-shot-msvc-release",
        "snow-shot-msvc-fast"
    )]
    [string]$Preset = "windows-msvc-debug",
    [string]$Target = "snow-all",
    [switch]$Clean,
    [switch]$SkipBootstrap
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
Set-SnowBuildEnvironment -Preset $Preset | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDirectory = Join-Path $repoRoot "build/$Preset"

if ($Clean -and (Test-Path -LiteralPath $buildDirectory)) {
    $resolvedBuild = (Resolve-Path -LiteralPath $buildDirectory).Path
    $resolvedRoot = (Resolve-Path -LiteralPath (Join-Path $repoRoot "build")).Path
    if (-not $resolvedBuild.StartsWith($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a build directory outside $resolvedRoot"
    }
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}

if (-not $SkipBootstrap) {
    & (Join-Path $PSScriptRoot "bootstrap.ps1") -SkipDependencyInstall
    if ($LASTEXITCODE -ne 0) {
        throw "Build environment bootstrap failed."
    }
}

Push-Location $repoRoot
try {
    $cachePath = Join-Path $buildDirectory "CMakeCache.txt"
    $configureArguments = @("--preset", $Preset)
    if ((Test-Path -LiteralPath $cachePath -PathType Leaf) -and
        -not (Test-SnowCacheAlignment -CachePath $cachePath -Preset $Preset)) {
        Write-Host "The existing CMake cache does not match preset $Preset; configuring from a fresh cache."
        $configureArguments = @("--fresh") + $configureArguments
    }
    elseif (Test-Path -LiteralPath $cachePath -PathType Leaf) {
        Write-Host "Reusing the existing CMake cache for preset $Preset."
    }

    & cmake @configureArguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed for preset $Preset."
    }

    $buildArguments = @("--build", "--preset", "build-$Preset", "--parallel")
    if (-not [string]::IsNullOrWhiteSpace($Target)) {
        $buildArguments += @("--target", $Target)
    }
    & cmake @buildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed for preset $Preset."
    }
}
finally {
    Pop-Location
}

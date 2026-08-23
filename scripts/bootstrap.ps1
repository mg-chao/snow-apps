[CmdletBinding()]
param(
    [string]$Qt6Dir = "",
    [ValidateSet("Dynamic", "Static")]
    [string[]]$VcpkgVariants = @("Dynamic", "Static"),
    [switch]$Reset,
    [switch]$SkipVcpkgInstall,
    [switch]$SkipDependencyInstall,
    [switch]$SkipQtValidation
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
    )

    Push-Location $WorkingDirectory
    try {
        & $Command @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed ($LASTEXITCODE): $Command $($Arguments -join ' ')"
        }
    }
    finally {
        Pop-Location
    }
}

function Require-Command {
    param([Parameter(Mandatory = $true)][string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Required command was not found: $Name"
    }
    return $command
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$toolsRoot = Join-Path $repoRoot ".tools"
$vcpkgRoot = Join-Path $toolsRoot "vcpkg"
$env:VCPKG_ROOT = $vcpkgRoot
$vcpkgExe = Join-Path $vcpkgRoot "vcpkg.exe"
$vcpkgInstalledRoot = Join-Path $vcpkgRoot "installed"
# Keep this paired with the available vcpkg tool release and CMake 4.2.3:
# newer snapshots require an unsupported manifest-tool schema or bundled CMake 4.4+.
$vcpkgBaseline = "4497409a47f19db373a410a0efb84eca4747adbf"
$rustToolchain = "1.97.1"
$rustTarget = "x86_64-pc-windows-msvc"

if (-not $SkipQtValidation) {
    # Environment variables often outlive a Qt upgrade, so each candidate is
    # version-checked before it is exported to the bootstrap subprocesses.
    $Qt6Dir = Set-SnowQtEnvironment -Qt6Dir $Qt6Dir
}

$git = Require-Command "git"
$cmake = Require-Command "cmake"
$cargo = Require-Command "cargo"
$rustup = Require-Command "rustup"

# vcpkg's app-local packaging invokes dumpbin to discover DLL dependencies of
# host tools. Without the MSVC bin directory, it can install unusable tools and
# still record their packages as successfully installed.
Add-SnowMsvcToolsToPath | Out-Null
Require-Command "dumpbin" | Out-Null

$cmakeVersionLine = (& $cmake.Source --version | Select-Object -First 1)
if ($cmakeVersionLine -notmatch "cmake version (\d+)\.(\d+)") {
    throw "Unable to determine the installed CMake version: $cmakeVersionLine"
}
if ([int]$Matches[1] -lt 4 -or ([int]$Matches[1] -eq 4 -and [int]$Matches[2] -lt 2)) {
    throw "CMake 4.2 or newer is required for the Visual Studio 2026 generator; found $cmakeVersionLine"
}

if ($Reset) {
    foreach ($path in @($vcpkgRoot, (Join-Path $repoRoot "build"))) {
        if (Test-Path -LiteralPath $path) {
            $resolved = (Resolve-Path -LiteralPath $path).Path
            if (-not $resolved.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to remove a path outside the repository: $resolved"
            }
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}

New-Item -ItemType Directory -Path $toolsRoot -Force | Out-Null

$vcpkgGitDirectory = Join-Path $vcpkgRoot ".git"
if (-not (Test-Path -LiteralPath $vcpkgExe) -and
    -not (Test-Path -LiteralPath $vcpkgGitDirectory -PathType Container)) {
    Invoke-Checked -Command $git.Source -Arguments @(
        "clone", "https://github.com/microsoft/vcpkg.git", $vcpkgRoot
    ) -WorkingDirectory $repoRoot
}

if (-not (Test-Path -LiteralPath $vcpkgGitDirectory)) {
    throw "Repository-local vcpkg must be a Git checkout so the pinned baseline can be enforced: $vcpkgRoot. Rerun with -Reset."
}
if (Test-Path -LiteralPath $vcpkgGitDirectory) {
    Invoke-Checked -Command $git.Source -Arguments @(
        "fetch", "origin", $vcpkgBaseline, "--depth=1"
    ) -WorkingDirectory $vcpkgRoot
    Invoke-Checked -Command $git.Source -Arguments @(
        "checkout", "--detach", $vcpkgBaseline
    ) -WorkingDirectory $vcpkgRoot
}

function Test-VcpkgToolAlignment {
    if (-not (Test-Path -LiteralPath $vcpkgExe -PathType Leaf)) {
        return $false
    }

    $metadataPath = Join-Path $vcpkgRoot "scripts\vcpkg-tool-metadata.txt"
    if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
        return $false
    }
    $metadata = ConvertFrom-StringData (Get-Content -LiteralPath $metadataPath -Raw)
    $expectedRelease = $metadata.VCPKG_TOOL_RELEASE_TAG
    if ([string]::IsNullOrWhiteSpace($expectedRelease)) {
        return $false
    }

    $versionOutput = & $vcpkgExe version --disable-metrics 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch "version\s+($([regex]::Escape($expectedRelease)))") {
        return $false
    }
    return $true
}

$vcpkgNeedsBootstrap = -not (Test-VcpkgToolAlignment)
if ($vcpkgNeedsBootstrap -and $SkipVcpkgInstall) {
    Write-Host "Repository-local vcpkg.exe is missing or stale; refreshing it from the pinned checkout."
}
if ($vcpkgNeedsBootstrap) {
    $bootstrap = Join-Path $vcpkgRoot "bootstrap-vcpkg.bat"
    if (-not (Test-Path -LiteralPath $bootstrap)) {
        throw "vcpkg bootstrap script was not found at $bootstrap"
    }
    Invoke-Checked -Command $env:ComSpec -Arguments @(
        "/d", "/c", $bootstrap, "-disableMetrics"
    ) -WorkingDirectory $vcpkgRoot
}

function Repair-VcpkgHostTools {
    param([Parameter(Mandatory = $true)][string]$InstallRoot)

    $hostRoot = Join-Path $InstallRoot "x64-windows"
    $hostBin = Join-Path $hostRoot "bin"
    $hostTools = Join-Path $hostRoot "tools"
    if (-not (Test-Path -LiteralPath $hostTools -PathType Container) -or
        -not (Test-Path -LiteralPath $hostBin -PathType Container)) {
        return
    }

    $appLocal = Join-Path $vcpkgRoot "scripts\buildsystems\msbuild\applocal.ps1"
    if (-not (Test-Path -LiteralPath $appLocal -PathType Leaf)) {
        throw "vcpkg app-local deployment script was not found at $appLocal"
    }
    Get-ChildItem -LiteralPath $hostTools -Filter "*.exe" -File -Recurse | ForEach-Object {
        & $appLocal -TargetBinary $_.FullName -InstalledDir $hostBin
    }
}

foreach ($variant in $VcpkgVariants) {
    $installVariant = $variant.ToLowerInvariant()
    Repair-VcpkgHostTools -InstallRoot (Join-Path $vcpkgInstalledRoot $installVariant)
}

if (-not $SkipDependencyInstall) {
    if (-not (Test-Path -LiteralPath $vcpkgExe)) {
        throw "Repository-local vcpkg is not installed. Rerun without -SkipVcpkgInstall."
    }
    foreach ($variant in $VcpkgVariants) {
        $triplet = if ($variant -eq "Static") { "x64-windows-static" } else { "x64-windows" }
        $installVariant = $variant.ToLowerInvariant()
        $tripletInstallRoot = Join-Path $vcpkgInstalledRoot $installVariant
        $overlayPortArguments = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot "cmake/vcpkg-overlay-ports") -Directory |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "portfile.cmake") -PathType Leaf } |
            Sort-Object Name |
            ForEach-Object { "--overlay-ports=$($_.FullName)" })
        $vcpkgArguments = @(
            "install",
            "--x-manifest-root=$repoRoot",
            "--x-install-root=$tripletInstallRoot",
            "--triplet=$triplet",
              "--x-feature=snow-shot"
          ) + $overlayPortArguments + @(
              "--overlay-triplets=$(Join-Path $repoRoot 'cmake/vcpkg-overlay-triplets')",
              "--clean-after-build"
          )
        if ($variant -eq "Dynamic") {
            $vcpkgArguments += "--x-feature=full-codecs"
        }
        Invoke-Checked -Command $vcpkgExe -Arguments $vcpkgArguments -WorkingDirectory $repoRoot
    }
}

Invoke-Checked -Command $rustup.Source -Arguments @(
    "toolchain", "install", $rustToolchain,
    "--profile", "minimal",
    "--component", "rustfmt",
    "--component", "clippy",
    "--target", $rustTarget
) -WorkingDirectory $repoRoot

$null = Invoke-Checked -Command $cargo.Source -Arguments @(
    "+$rustToolchain", "metadata", "--format-version", "1", "--no-deps"
) -WorkingDirectory (Join-Path $repoRoot "snow-crates")
$null = Invoke-Checked -Command $cargo.Source -Arguments @(
    "+$rustToolchain", "metadata", "--format-version", "1", "--no-deps"
) -WorkingDirectory (Join-Path $repoRoot "snow_draw_engine_qt")

$libclangDirectory = Join-Path $toolsRoot "llvm\bin"
if (-not (Test-Path -LiteralPath (Join-Path $libclangDirectory "libclang.dll"))) {
    Write-Warning "libclang.dll was not found under $libclangDirectory; Rust bindgen builds may fail."
}

Write-Host "Snow Apps build environment is ready."
Write-Host "Repository: $repoRoot"
Write-Host "CMake: $cmakeVersionLine"
Write-Host "vcpkg: $vcpkgRoot"
foreach ($variant in $VcpkgVariants) {
    $installVariant = $variant.ToLowerInvariant()
    Write-Host "vcpkg $installVariant installed: $(Join-Path $vcpkgInstalledRoot $installVariant)"
}
if (-not $SkipQtValidation) {
    Write-Host "Qt6_DIR: $Qt6Dir"
    if (-not [string]::IsNullOrWhiteSpace($env:SNOW_QT_STATIC_DIR)) {
        Write-Host "SNOW_QT_STATIC_DIR: $env:SNOW_QT_STATIC_DIR"
    }
}
Write-Host "Rust: $rustToolchain ($rustTarget)"

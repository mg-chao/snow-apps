[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$presetData = Get-Content -LiteralPath (Join-Path $repoRoot "CMakePresets.json") -Raw |
    ConvertFrom-Json

function Get-PresetVariable([string]$Name, [string]$Variable) {
    $preset = @($presetData.configurePresets | Where-Object { $_.name -ceq $Name })[0]
    $property = $preset.cacheVariables.PSObject.Properties[$Variable]
    if ($property) { return $property.Value }
    if ($preset.inherits) { return Get-PresetVariable $preset.inherits $Variable }
    return $null
}

# Exercise the real bootstrap install block without downloading or building packages.
$parseErrors = $null
$bootstrapAst = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot "bootstrap.ps1"), [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw "bootstrap.ps1 must parse successfully." }
$installBlocks = @($bootstrapAst.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.IfStatementAst] -and
        $node.Clauses[0].Item1.Extent.Text -ceq '-not $SkipDependencyInstall'
}, $false))
if ($installBlocks.Count -ne 1) { throw "Expected one bootstrap dependency install block." }

function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments, [string]$WorkingDirectory)
    $script:InstallCalls += [pscustomobject]@{
        Command = $Command
        Arguments = $Arguments
        WorkingDirectory = $WorkingDirectory
    }
}

$script:InstallCalls = @()
$vcpkgRoot = Join-Path $repoRoot ".tools/vcpkg"
$vcpkgInstalledRoot = Join-Path $vcpkgRoot "installed"
$vcpkgExe = Join-Path $vcpkgRoot "vcpkg.exe"
$VcpkgVariants = @("Dynamic", "Static")
$SkipDependencyInstall = $false

# Test-Path is stubbed only for the executable check; overlay discovery uses real files.
function Test-Path {
    param([string]$LiteralPath, [string]$PathType)
    if ($LiteralPath -eq $vcpkgExe) { return $true }
    return Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
& ([scriptblock]::Create($installBlocks[0].Extent.Text))
if ($script:InstallCalls.Count -ne 2) { throw "Bootstrap must install each requested variant." }

$failures = @()
foreach ($index in 0..1) {
    $preset = @("windows-msvc-debug", "snow-shot-msvc-release")[$index]
    $expectedFeatures = @(Get-PresetVariable $preset "VCPKG_MANIFEST_FEATURES") -split ';' |
        Sort-Object
    $actualFeatures = @($script:InstallCalls[$index].Arguments |
        Where-Object { $_ -like '--x-feature=*' } |
        ForEach-Object { $_.Substring('--x-feature='.Length) } | Sort-Object)
    if (($actualFeatures -join ';') -cne ($expectedFeatures -join ';')) {
        $failures += "Bootstrap $($VcpkgVariants[$index]) features must match ${preset}: " +
            "expected $($expectedFeatures -join ';'), got $($actualFeatures -join ';')."
    }
    $scanOverlay = "--overlay-ports=$(Join-Path $repoRoot 'cmake/vcpkg-overlay-ports/libde265')"
    if ($scanOverlay -notin $script:InstallCalls[$index].Arguments) {
        $failures += "Bootstrap must install the libde265 scan initialization correction."
    }
}

foreach ($preset in @("windows-msvc-debug", "windows-msvc-performance",
        "windows-clang-portability", "snow-shot-msvc-release", "snow-shot-msvc-fast")) {
    if ((Get-PresetVariable $preset "VCPKG_MANIFEST_INSTALL") -cne "ON") {
        $failures += "$preset must enable dependency installation when reusing a cache."
    }
    $overlays = @(Get-PresetVariable $preset "VCPKG_OVERLAY_PORTS") -split ';'
    if (-not ($overlays | Where-Object { $_ -match '/vcpkg-overlay-ports(?:/libde265)?$' })) {
        $failures += "$preset must use the libde265 scan initialization correction."
    }
}

if ($failures.Count -ne 0) { throw ($failures -join "`n") }
Write-Output "Windows build dependency tests passed."

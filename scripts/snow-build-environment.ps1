Set-StrictMode -Version Latest

$script:SnowRepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$script:SnowQtToolchain = Get-Content -Raw -LiteralPath (
    Join-Path $PSScriptRoot "qt-toolchain.json") | ConvertFrom-Json
if ($script:SnowQtToolchain.schemaVersion -cne 1 -or
    $script:SnowQtToolchain.qtVersion -cnotmatch '^6\.[0-9]+\.[0-9]+$' -or
    $script:SnowQtToolchain.sourceArchiveSha256 -cnotmatch '^[a-f0-9]{64}$') {
    throw "The repository Qt toolchain manifest is invalid."
}
$script:SnowQtVersion = $script:SnowQtToolchain.qtVersion
$script:SnowMsvcToolset = "14.51"
$script:SnowRustToolchain = "1.97.1"
$script:SnowRustTarget = "x86_64-pc-windows-msvc"
$script:SnowStaticQtSchemaVersion = 5
$script:SnowStaticQtFeaturePolicy = Get-Content -Raw -LiteralPath (
    Join-Path $PSScriptRoot "static-qt-features.json") | ConvertFrom-Json
$script:SnowStaticQtSourcePatches = @($script:SnowStaticQtFeaturePolicy.windowsSourcePatches |
    ForEach-Object {
        [pscustomobject]@{
            File = $_
            SHA256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (
                Join-Path $PSScriptRoot $_)).Hash.ToLowerInvariant()
        }
    })
$featureHashes = @((Get-FileHash -Algorithm SHA256 -LiteralPath (
    Join-Path $PSScriptRoot "static-qt-features.json")).Hash.ToLowerInvariant()) +
    @($script:SnowStaticQtSourcePatches | ForEach-Object { $_.SHA256 })
$script:SnowStaticQtFeatureFingerprint = [Convert]::ToHexString(
    [System.Security.Cryptography.SHA256]::HashData(
        [System.Text.Encoding]::UTF8.GetBytes($featureHashes -join '|'))).ToLowerInvariant()

function Test-SnowQtTargetFeature {
    param(
        [Parameter(Mandatory = $true)][string]$TargetsText,
        [Parameter(Mandatory = $true)][ValidateSet("PUBLIC", "PRIVATE")][string]$Visibility,
        [Parameter(Mandatory = $true)][string]$Feature,
        [Parameter(Mandatory = $true)][bool]$Enabled
    )

    $state = if ($Enabled) { "ENABLED" } else { "DISABLED" }
    $oppositeState = if ($Enabled) { "DISABLED" } else { "ENABLED" }
    $match = [regex]::Match($TargetsText, "QT_${state}_${Visibility}_FEATURES `"([^`"]*)`"")
    $oppositeMatch = [regex]::Match(
        $TargetsText, "QT_${oppositeState}_${Visibility}_FEATURES `"([^`"]*)`"")
    return $match.Success -and ($match.Groups[1].Value -csplit ';') -ccontains $Feature -and
        (-not $oppositeMatch.Success -or
            ($oppositeMatch.Groups[1].Value -csplit ';') -cnotcontains $Feature)
}

function Get-SnowQtKitVersion {
    param([Parameter(Mandatory = $true)][string]$Qt6Dir)

    foreach ($name in @("Qt6ConfigVersionImpl.cmake", "Qt6ConfigVersion.cmake")) {
        $path = Join-Path $Qt6Dir $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $match = [regex]::Match((Get-Content -LiteralPath $path -Raw),
            '(?m)^\s*set\s*\(\s*PACKAGE_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"\s*\)')
        if ($match.Success) { return $match.Groups[1].Value }
    }
    return ""
}

function Test-SnowQtDevelopmentModules {
    param([Parameter(Mandatory = $true)][string]$Qt6Dir)

    foreach ($component in @("Qt6Concurrent", "Qt6Test")) {
        if (-not (Test-Path -LiteralPath (Join-Path $Qt6Dir "../$component/$($component)Config.cmake"))) {
            return $false
        }
    }
    $corePath = Join-Path $Qt6Dir "../Qt6Core/Qt6CoreTargets.cmake"
    if (-not (Test-Path -LiteralPath $corePath)) { return $false }
    $coreText = Get-Content -LiteralPath $corePath -Raw
    return (Test-SnowQtTargetFeature $coreText "PUBLIC" "concurrent" $true) -and
        (Test-SnowQtTargetFeature $coreText "PRIVATE" "testlib" $true)
}

function Test-SnowQtTranslationKit {
    param([Parameter(Mandatory = $true)][string]$Qt6Dir)

    $prefix = [IO.Path]::GetFullPath((Join-Path $Qt6Dir "../../.."))
    foreach ($language in @("zh_CN", "zh_TW")) {
        $catalog = Join-Path $prefix "translations/qtbase_$language.qm"
        if (-not (Test-Path -LiteralPath $catalog -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

function Test-SnowStaticQtStamp {
    param(
        [Parameter(Mandatory = $true)][object]$Stamp,
        [string]$Version = $script:SnowQtVersion,
        [string]$Configuration = "Release"
    )

    $expected = [ordered]@{
        SchemaVersion = $script:SnowStaticQtSchemaVersion
        QtVersion = $Version
        SourceArchiveSha256 = $script:SnowQtToolchain.sourceArchiveSha256
        Configuration = $Configuration
        FeatureFingerprint = $script:SnowStaticQtFeatureFingerprint
        Ltcg = ($Configuration -eq "Release")
        SystemPng = ($Configuration -eq "Release")
        SystemZlib = ($Configuration -eq "Release")
        Timezone = $true
        TimezoneLocale = $false
    }
    foreach ($name in $expected.Keys) {
        $property = $Stamp.PSObject.Properties[$name]
        if ($null -eq $property -or $property.Value -cne $expected[$name]) { return $false }
        if ($expected[$name] -is [bool] -and $property.Value -isnot [bool]) { return $false }
    }
    $patchProperty = $Stamp.PSObject.Properties["SourcePatches"]
    if ($null -eq $patchProperty) { return $false }
    $patches = @($patchProperty.Value)
    if ($patches.Count -ne $script:SnowStaticQtSourcePatches.Count) { return $false }
    for ($index = 0; $index -lt $patches.Count; $index++) {
        foreach ($name in @("File", "SHA256")) {
            $property = $patches[$index].PSObject.Properties[$name]
            if ($null -eq $property -or
                $property.Value -cne $script:SnowStaticQtSourcePatches[$index].$name) {
                return $false
            }
        }
    }
    return $true
}

function Test-SnowQtSystemCodecKit {
    param(
        [Parameter(Mandatory = $true)][string]$Qt6Dir,
        [ValidateSet("Debug", "Release")][string]$Configuration = "Release"
    )

    $coreTargets = Join-Path $Qt6Dir "..\Qt6Core\Qt6CoreTargets.cmake"
    $guiTargets = Join-Path $Qt6Dir "..\Qt6Gui\Qt6GuiTargets.cmake"
    if (-not (Test-Path -LiteralPath $coreTargets -PathType Leaf) -or
        -not (Test-Path -LiteralPath $guiTargets -PathType Leaf)) {
        return $false
    }
    $coreText = Get-Content -LiteralPath $coreTargets -Raw
    $guiText = Get-Content -LiteralPath $guiTargets -Raw
    foreach ($feature in @("static", "static_runtime")) {
        if (-not (Test-SnowQtTargetFeature $coreText "PUBLIC" $feature $true)) { return $false }
    }
    foreach ($feature in $script:SnowStaticQtFeaturePolicy.features.PSObject.Properties) {
        if ($feature.Name -ceq "ltcg" -and $Configuration -eq "Debug") {
            # Qt omits this configuration-dependent feature from Debug exports.
            $enabledFeatures = [regex]::Match($coreText, 'QT_ENABLED_PRIVATE_FEATURES "([^"]*)"')
            if (-not $enabledFeatures.Success -or
                ($enabledFeatures.Groups[1].Value -csplit ';') -ccontains "ltcg") { return $false }
            continue
        }
        $text = if ($feature.Name -ceq "system_png") { $guiText } else { $coreText }
        $visibility = if ($feature.Name -ceq "timezone") { "PUBLIC" } else { "PRIVATE" }
        $enabled = if ($feature.Name -cin @("ltcg", "system_png", "system_zlib")) {
            $Configuration -eq "Release"
        } else { $feature.Value }
        if (-not (Test-SnowQtTargetFeature $text $visibility $feature.Name $enabled)) {
            return $false
        }
    }
    return $true
}

function Test-SnowValidatedStaticQtKit {
    param([Parameter(Mandatory = $true)][string]$Qt6Dir)

    if ((Get-SnowQtKitVersion -Qt6Dir $Qt6Dir) -cne $script:SnowQtVersion -or
        -not (Test-SnowQtSystemCodecKit -Qt6Dir $Qt6Dir) -or
        -not (Test-SnowQtTranslationKit -Qt6Dir $Qt6Dir)) { return $false }
    $prefix = [System.IO.Path]::GetFullPath((Join-Path $Qt6Dir "..\..\.."))
    $stampPath = Join-Path $prefix "share\snow-apps\static-qt-build.json"
    if (-not (Test-Path -LiteralPath $stampPath -PathType Leaf)) { return $false }
    try {
        return Test-SnowStaticQtStamp -Stamp (Get-Content -LiteralPath $stampPath -Raw |
            ConvertFrom-Json)
    }
    catch { return $false }
}

function Resolve-SnowQtDir {
    param(
        [string]$Qt6Dir = "",
        [string]$Preset = ""
    )

    if (-not [string]::IsNullOrWhiteSpace($Qt6Dir)) {
        $candidates = @($Qt6Dir)
    }
    else {
        $explicitCandidates = @(
            $env:SNOW_QT_STATIC_DIR,
            $env:Qt6_DIR,
            $(if (-not [string]::IsNullOrWhiteSpace($env:QTDIR)) {
                Join-Path $env:QTDIR "lib\cmake\Qt6"
            })
        ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
        $searchRoots = @(
            $env:SNOW_QT_ROOT,
            (Join-Path $script:SnowRepoRoot ".tools/qt"),
            $(if (-not [string]::IsNullOrWhiteSpace($env:ProgramFiles)) {
                Join-Path $env:ProgramFiles "Qt"
            }),
            $(if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
                Join-Path ${env:ProgramFiles(x86)} "Qt"
            }),
            "C:\Qt"
        ) | Where-Object {
            -not [string]::IsNullOrWhiteSpace($_) -and
            (Test-Path -LiteralPath $_ -PathType Container)
        }
        $discoveredCandidates = foreach ($root in $searchRoots) {
            Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -eq $script:SnowQtVersion } |
                ForEach-Object {
                    Get-ChildItem -LiteralPath $_.FullName -Directory -ErrorAction SilentlyContinue |
                        ForEach-Object { Join-Path $_.FullName "lib\cmake\Qt6" }
                }
        }
        $candidates = @($explicitCandidates + $discoveredCandidates) | Select-Object -Unique
    }

    $requiredConfiguration = switch ($Preset) {
        "windows-msvc-debug" { "Debug" }
        "windows-clang-portability" { "Debug" }
        "windows-msvc-performance" { "Release" }
        "snow-shot-msvc-release" { "Release" }
        "snow-shot-msvc-fast" { "Release" }
        default { "" }
    }
    foreach ($candidate in $candidates) {
        try {
            $resolved = [System.IO.Path]::GetFullPath($candidate)
        }
        catch {
            continue
        }
        $config = Join-Path $resolved "Qt6Config.cmake"
        if (-not (Test-Path -LiteralPath $config -PathType Leaf)) { continue }
        if ((Get-SnowQtKitVersion -Qt6Dir $resolved) -cne $script:SnowQtVersion) { continue }

        if (-not [string]::IsNullOrWhiteSpace($requiredConfiguration)) {
            $configurationTargets = Join-Path $resolved (
                "..\Qt6Core\Qt6CoreTargets-{0}.cmake" -f $requiredConfiguration.ToLowerInvariant()
            )
            if (-not (Test-Path -LiteralPath $configurationTargets -PathType Leaf)) { continue }
        }
        if ($Preset -eq "windows-msvc-performance" -and
            -not (Test-SnowQtDevelopmentModules -Qt6Dir $resolved)) {
            continue
        }
        if ($Preset -in @("snow-shot-msvc-release", "snow-shot-msvc-fast") -and
            -not (Test-SnowValidatedStaticQtKit -Qt6Dir $resolved)) {
            continue
        }
        return $resolved
    }
    if (-not [string]::IsNullOrWhiteSpace($Qt6Dir)) {
        $configurationHint = if ([string]::IsNullOrWhiteSpace($requiredConfiguration)) {
            ""
        }
        else {
            " with $requiredConfiguration libraries"
        }
        throw "Qt $script:SnowQtVersion$configurationHint was not found at the explicit Qt6Dir: $Qt6Dir"
    }
    $configurationHint = if ([string]::IsNullOrWhiteSpace($requiredConfiguration)) {
        ""
    }
    else {
        " with $requiredConfiguration libraries"
    }
    throw "Qt $script:SnowQtVersion$configurationHint was not found. Set SNOW_QT_STATIC_DIR, Qt6_DIR, QTDIR, or SNOW_QT_ROOT."
}

function Set-SnowQtEnvironment {
    param(
        [string]$Qt6Dir = "",
        [string]$Preset = ""
    )

    $qtDir = Resolve-SnowQtDir -Qt6Dir $Qt6Dir -Preset $Preset
    $env:SNOW_QT_STATIC_DIR = $qtDir
    $env:Qt6_DIR = $qtDir
    $env:QTDIR = [System.IO.Path]::GetFullPath((Join-Path $qtDir "..\..\.."))
    # Qt installations commonly add their MinGW toolchain to PATH. That
    # compiler is incompatible with this project's MSVC-only triplets and
    # would make CMake pick gcc for native dependency builds.
    $env:Path = @($env:Path -split ';' | Where-Object {
        $_ -and $_ -notmatch '(?i)[\\/]Qt[\\/]Tools[\\/]mingw[^\\/]*([\\/]bin)?$'
    }) -join ';'
    $env:Path = "$(Join-Path $env:QTDIR 'bin');$env:Path"
    return $qtDir
}

function Add-SnowMsvcToolsToPath {
    $vswhereCommand = Get-Command "vswhere.exe" -ErrorAction SilentlyContinue
    $vswhere = @(
        @(
            $(if ($vswhereCommand) { $vswhereCommand.Source }),
            $(if (-not [string]::IsNullOrWhiteSpace(${env:ProgramFiles(x86)})) {
                Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
            }),
            $(if (-not [string]::IsNullOrWhiteSpace($env:ProgramFiles)) {
                Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe"
            })
        ) | Where-Object {
            -not [string]::IsNullOrWhiteSpace($_) -and
            (Test-Path -LiteralPath $_ -PathType Leaf)
        }
    )
    $visualStudioRoot = @($env:VSINSTALLDIR) | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and
        (Test-Path -LiteralPath (Join-Path $_ "VC\Tools\MSVC") -PathType Container)
    } | Select-Object -First 1
    if (-not $visualStudioRoot -and $vswhere) {
        $visualStudioRoot = & $vswhere[0] -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -First 1
    }
    if (-not $visualStudioRoot) {
        throw "A Visual Studio installation with the MSVC x64 component was not found. Install the required Build Tools or set VSINSTALLDIR."
    }

    $env:VSINSTALLDIR = [System.IO.Path]::GetFullPath($visualStudioRoot)
    $env:VCINSTALLDIR = Join-Path $env:VSINSTALLDIR "VC"
    $msvcTools = Get-ChildItem -LiteralPath (Join-Path $env:VCINSTALLDIR "Tools\MSVC") -Directory |
        Where-Object { $_.Name -match '^14\.51' } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if (-not $msvcTools) {
        throw "MSVC toolset $script:SnowMsvcToolset was not found under $env:VCINSTALLDIR."
    }

    $env:VCToolsInstallDir = "$($msvcTools.FullName)\"
    $msvcBin = Join-Path $msvcTools.FullName "bin\Hostx64\x64"
    $env:Path = "$msvcBin;$env:Path"

    $sdkRoot = $env:WindowsSdkDir
    $sdkVersion = $env:WindowsSDKVersion
    if ([string]::IsNullOrWhiteSpace($sdkRoot) -or [string]::IsNullOrWhiteSpace($sdkVersion)) {
        $sdkIncludeRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include"
        $sdkInclude = Get-ChildItem -LiteralPath $sdkIncludeRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            Select-Object -First 1
        if ($sdkInclude) {
            $sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
            $sdkVersion = $sdkInclude.Name
        }
    }
    if ([string]::IsNullOrWhiteSpace($sdkRoot) -or [string]::IsNullOrWhiteSpace($sdkVersion)) {
        throw "A Windows 10 SDK was not found. Install one or set WindowsSdkDir and WindowsSDKVersion."
    }
    $env:WindowsSdkDir = "$([System.IO.Path]::GetFullPath($sdkRoot))\"
    $env:WindowsSDKVersion = "$sdkVersion\"
    $sdkBin = Join-Path $env:WindowsSdkDir "bin\$sdkVersion\x64"
    if (-not (Test-Path -LiteralPath (Join-Path $sdkBin "rc.exe") -PathType Leaf)) {
        throw "Windows SDK resource compiler was not found under $sdkBin."
    }
    $env:Path = "$sdkBin;$env:Path"
    $sdkIncludeRoot = Join-Path $env:WindowsSdkDir "Include\$sdkVersion"
    $env:INCLUDE = @(
        (Join-Path $msvcTools.FullName "include"),
        (Join-Path $sdkIncludeRoot "ucrt"),
        (Join-Path $sdkIncludeRoot "shared"),
        (Join-Path $sdkIncludeRoot "um"),
        (Join-Path $sdkIncludeRoot "winrt"),
        (Join-Path $sdkIncludeRoot "cppwinrt")
    ) -join ";"
    $env:LIB = @(
        (Join-Path $msvcTools.FullName "lib\x64"),
        (Join-Path $env:WindowsSdkDir "Lib\$sdkVersion\um\x64"),
        (Join-Path $env:WindowsSdkDir "Lib\$sdkVersion\ucrt\x64")
    ) -join ";"
    return $msvcBin
}

function Set-SnowBuildEnvironment {
    param([string]$Preset = "")

    $qtDir = Set-SnowQtEnvironment -Preset $Preset
    $env:VCPKG_ROOT = Join-Path $script:SnowRepoRoot ".tools\vcpkg"
    Add-SnowMsvcToolsToPath | Out-Null
    $libclang = Join-Path $script:SnowRepoRoot ".tools\llvm\bin"
    if (Test-Path -LiteralPath (Join-Path $libclang "libclang.dll") -PathType Leaf) {
        $env:LIBCLANG_PATH = $libclang
    }
    $env:Path = "$(Join-Path $libclang '..');$env:Path"
    return [pscustomobject]@{
        RepoRoot = $script:SnowRepoRoot
        Qt6Dir = $qtDir
        QtVersion = $script:SnowQtVersion
        MsvcToolset = $script:SnowMsvcToolset
        RustToolchain = $script:SnowRustToolchain
        RustTarget = $script:SnowRustTarget
        VcpkgRoot = $env:VCPKG_ROOT
    }
}

function Resolve-SnowPreset {
    param([ValidateSet("Debug", "Release", "Performance", "Fast")][string]$Configuration)
    switch ($Configuration) {
        "Debug" { return "windows-msvc-debug" }
        "Release" { return "snow-shot-msvc-release" }
        "Performance" { return "windows-msvc-performance" }
        "Fast" { return "snow-shot-msvc-fast" }
    }
}

function Invoke-SnowCMake {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    Push-Location $script:SnowRepoRoot
    try {
        & cmake @Arguments
        if ($LASTEXITCODE -ne 0) { throw "CMake failed ($LASTEXITCODE): cmake $($Arguments -join ' ')" }
    }
    finally { Pop-Location }
}

function Test-SnowCacheAlignment {
    param(
        [Parameter(Mandatory = $true)][string]$CachePath,
        [Parameter(Mandatory = $true)][string]$Preset
    )
    if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) { return $false }
    $cache = Get-Content -LiteralPath $CachePath -Raw
    $qtNeedle = [regex]::Escape(($env:SNOW_QT_STATIC_DIR -replace '\\', '/'))
    $repoNeedle = [regex]::Escape(($script:SnowRepoRoot -replace '\\', '/'))
    $expectedTriplet = if ($Preset -in @("snow-shot-msvc-release", "snow-shot-msvc-fast")) {
        "x64-windows-static"
    }
    else {
        "x64-windows"
    }
    $installedVariant = if ($expectedTriplet -eq "x64-windows-static") { "static" } else { "dynamic" }
    $installedDir = Join-Path $script:SnowRepoRoot ".tools\vcpkg\installed\$installedVariant"
    $installedDirNeedle = [regex]::Escape(($installedDir -replace '\\', '/'))
    $lineEnd = '\r?$'

    $qtAligned = $cache -match "(?m)^Qt6_DIR:PATH=$qtNeedle$lineEnd"
    if ($Preset -in @("snow-shot-msvc-release", "snow-shot-msvc-fast")) {
        $qtAligned = $qtAligned -and $cache -match "(?m)^SNOW_QT_STATIC_DIR:PATH=.+$lineEnd"
    }
    $powerShellMatch = [regex]::Match(
        $cache,
        "(?m)^Z_VCPKG_POWERSHELL_PATH:INTERNAL=(.+)$lineEnd"
    )
    $powerShellAligned = $powerShellMatch.Success
    if ($powerShellAligned) {
        $cachedPowerShell = $powerShellMatch.Groups[1].Value.Trim()
        if ([System.IO.Path]::IsPathRooted($cachedPowerShell)) {
            $powerShellAligned = Test-Path -LiteralPath $cachedPowerShell -PathType Leaf
        }
        else {
            $powerShellAligned = $null -ne (Get-Command $cachedPowerShell -ErrorAction SilentlyContinue)
        }
    }

    $generatorAligned = if ($Preset -eq "windows-clang-portability") {
        $cache -match "(?m)^CMAKE_GENERATOR:INTERNAL=Ninja$lineEnd" -and
            $cache -match "(?im)^CMAKE_CXX_COMPILER:(?:FILEPATH|STRING)=.*clang-cl(?:\.exe)?$lineEnd"
    }
    else {
        $cache -match "(?m)^CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026$lineEnd" -and
            $cache -match "(?m)^CMAKE_GENERATOR_PLATFORM:INTERNAL=x64$lineEnd" -and
            $cache -match "(?m)^CMAKE_GENERATOR_TOOLSET:INTERNAL=host=x64,version=14\.51$lineEnd"
    }

    return $qtAligned -and $powerShellAligned -and $generatorAligned -and
        $cache -match "(?m)^VCPKG_TARGET_TRIPLET:.*=$([regex]::Escape($expectedTriplet))$lineEnd" -and
        $cache -match "(?m)^VCPKG_INSTALLED_DIR:PATH=$installedDirNeedle$lineEnd" -and
        $cache -match "(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=$repoNeedle$lineEnd"
}

function Get-SnowConfigureArguments {
    param(
        [Parameter(Mandatory = $true)][string]$Preset,
        [Parameter(Mandatory = $true)][string]$BuildDirectory
    )

    $cachePath = Join-Path $BuildDirectory "CMakeCache.txt"
    $arguments = @("--preset", $Preset, "-S", $script:SnowRepoRoot, "-B", $BuildDirectory)
    if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
        if (-not (Test-SnowCacheAlignment -CachePath $cachePath -Preset $Preset)) {
            Write-Host "The existing CMake cache does not match preset $Preset; configuring from a fresh cache."
            return @("--fresh") + $arguments
        }
        Write-Host "Reusing the existing CMake cache for preset $Preset."
    }
    return $arguments
}

function Resolve-SnowExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$Preset,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $configuration = if ($Preset -eq "windows-msvc-debug") { "Debug" } else { "Release" }
    $buildRoot = Join-Path $script:SnowRepoRoot "build\$Preset"
    $match = Get-ChildItem -LiteralPath $buildRoot -Filter $Name -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\$configuration\\" } |
        Sort-Object FullName |
        Select-Object -First 1
    if (-not $match) { throw "$Name was not found under $buildRoot ($configuration)." }
    return $match
}

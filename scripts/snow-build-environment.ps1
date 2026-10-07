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
$script:SnowStaticQtSchemaVersion = 6
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

function Get-SnowWindowsHostArchitecture {
    # OSArchitecture remains correct when PowerShell runs under emulation on ARM64.
    switch ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()) {
        "X64" { return "x64" }
        "Arm64" { return "arm64" }
        default { throw "Snow Apps requires an x64 or ARM64 Windows build host." }
    }
}

function Get-SnowWindowsTarget {
    param(
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
        [string]$Preset = ""
    )
    if ($Preset) {
        $presetArchitecture = if ($Preset -match '^snow-shot-msvc-arm64-(debug|performance|release|fast)$') {
            "arm64"
        } elseif ($Preset -in @("windows-msvc-debug", "windows-msvc-performance",
                "windows-clang-portability", "snow-shot-msvc-release", "snow-shot-msvc-fast")) {
            "x64"
        } else { throw "Unsupported Windows preset: $Preset" }
        if ($PSBoundParameters.ContainsKey("Architecture") -and $Architecture -cne $presetArchitecture) {
            throw "Architecture $Architecture does not match preset $Preset."
        }
        $Architecture = $presetArchitecture
    } else {
        $Preset = if ($Architecture -eq "arm64") { "snow-shot-msvc-arm64-release" }
            else { "snow-shot-msvc-release" }
    }
    $configuration = if ($Preset -match '(debug|portability)$') { "Debug" } else { "Release" }
    $static = $Preset -match '(release|fast)$'
    $variant = if ($static) { "static" } else { "dynamic" }
    $hostArchitecture = Get-SnowWindowsHostArchitecture
    $installedRoot = Join-Path $script:SnowRepoRoot ".tools/vcpkg/installed"
    if ($Architecture -eq "arm64") { $installedRoot = Join-Path $installedRoot "arm64" }
    $installedRoot = Join-Path $installedRoot $variant
    return [pscustomobject]@{
        Architecture = $Architecture
        HostArchitecture = $hostArchitecture
        Platform = "windows-$Architecture"
        RustTarget = if ($Architecture -eq "arm64") { "aarch64-pc-windows-msvc" }
            else { "x86_64-pc-windows-msvc" }
        Machine = if ($Architecture -eq "arm64") { "AA64" } else { "8664" }
        PeMachine = if ($Architecture -eq "arm64") { 0xAA64 } else { 0x8664 }
        Triplet = "$Architecture-windows$(if ($static) { '-static' })"
        HostTriplet = "$hostArchitecture-windows"
        InstalledRoot = [IO.Path]::GetFullPath($installedRoot)
        Preset = $Preset
        ReleasePreset = if ($Architecture -eq "arm64") { "snow-shot-msvc-arm64-release" }
            else { "snow-shot-msvc-release" }
        Configuration = $configuration
        IsStatic = [bool]$static
        GeneratorPlatform = if ($Architecture -eq "arm64") { "ARM64" } else { "x64" }
        Toolset = if ($Architecture -eq "arm64") { "version=14.51" }
            else { "host=x64,version=14.51" }
    }
}

function Get-SnowPeMachine {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) {
            throw "Not a Windows PE binary: $Path"
        }
        $stream.Position = 0x3C
        $offset = $reader.ReadUInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length - 24) {
            throw "Invalid Windows PE header: $Path"
        }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "Invalid PE signature: $Path" }
        return $reader.ReadUInt16()
    } finally { $stream.Dispose() }
}

function Test-SnowQtArchitecture {
    param(
        [Parameter(Mandatory = $true)][string]$Qt6Dir,
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
        [ValidateSet("Debug", "Release")][string]$Configuration = "Release"
    )
    $prefix = [IO.Path]::GetFullPath((Join-Path $Qt6Dir "../../.."))
    $machine = if ($Architecture -eq "arm64") { 0xAA64 } else { 0x8664 }
    $suffix = if ($Configuration -eq "Debug") { "d" } else { "" }
    $dll = Join-Path $prefix "bin/Qt6Core$suffix.dll"
    if (Test-Path -LiteralPath $dll -PathType Leaf) {
        try { return (Get-SnowPeMachine -Path $dll) -eq $machine } catch { return $false }
    }
    $library = Join-Path $prefix "lib/Qt6Core$suffix.lib"
    if (-not (Test-Path -LiteralPath $library -PathType Leaf)) { return $false }
    return Test-SnowWindowsLibraryArchitecture -Path $library -Architecture $Architecture
}

function Test-SnowWindowsLibraryArchitecture {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $machine = if ($Architecture -eq "arm64") { 0xAA64 } else { 0x8664 }
    # MSVC archives may contain /GL objects. dumpbin understands those and the
    # archive linker members; use it instead of guessing from a directory name.
    $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $dumpbin) { return $false }
    $headers = (& $dumpbin.Source /nologo /headers $Path 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { return $false }
    $machines = @([regex]::Matches($headers, '(?im)^\s*([0-9a-f]{4}) machine\s*\(') |
        ForEach-Object { [Convert]::ToInt32($_.Groups[1].Value, 16) } | Select-Object -Unique)
    return $machines.Count -eq 1 -and $machines[0] -eq $machine
}

function Get-SnowQtHostToolVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $option = if ($Name -in @("lrelease", "lupdate")) { "-version" } else { "-v" }
    $output = (& $Path $option 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "Qt host tool cannot run: $Path" }
    $match = [regex]::Match($output, '\b([0-9]+\.[0-9]+\.[0-9]+)\b')
    if (-not $match.Success) { throw "Qt host tool did not report its version: $Path" }
    return $match.Groups[1].Value
}

function Resolve-SnowQtHostPrefix {
    param([string]$HostQtPrefix = "")
    $hostArchitecture = Get-SnowWindowsHostArchitecture
    $candidates = if ($HostQtPrefix) { @($HostQtPrefix) }
        else { @($env:SNOW_QT_HOST_PREFIX, $env:QT_HOST_PATH) }
    $qtRoot = Join-Path $script:SnowRepoRoot ".tools/qt/$script:SnowQtVersion"
    if (-not $HostQtPrefix -and (Test-Path -LiteralPath $qtRoot -PathType Container)) {
        $candidates += @(Get-ChildItem -LiteralPath $qtRoot -Directory |
            ForEach-Object { $_.FullName })
    }
    foreach ($candidate in @($candidates | Where-Object { $_ } | Select-Object -Unique)) {
        $prefix = [IO.Path]::GetFullPath($candidate)
        if ((Get-SnowQtKitVersion -Qt6Dir (Join-Path $prefix "lib/cmake/Qt6")) -cne $script:SnowQtVersion) {
            continue
        }
        $valid = $true
        foreach ($tool in @("moc", "rcc", "uic", "lrelease", "lupdate")) {
            $path = Join-Path $prefix "bin/$tool.exe"
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $valid = $false; break }
            try {
                $machine = Get-SnowPeMachine -Path $path
                if ($machine -ne $(if ($hostArchitecture -eq "arm64") { 0xAA64 } else { 0x8664 })) {
                    $valid = $false; break
                }
                if ((Get-SnowQtHostToolVersion -Path $path -Name $tool) -cne $script:SnowQtVersion) {
                    $valid = $false; break
                }
            } catch { $valid = $false; break }
        }
        if ($valid) { return $prefix }
    }
    throw "Qt $script:SnowQtVersion $hostArchitecture host tools were not found. Build a host kit and set SNOW_QT_HOST_PREFIX or pass -HostQtPrefix."
}

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

function Get-SnowStaticQtLtcgEnabled {
    param(
        [ValidateSet("Debug", "Release")][string]$Configuration = "Release",
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
    )
    # MSVC 14.51 ARM64 LTCG emits invalid stack-cookie return sequences.
    return $Configuration -ceq "Release" -and $Architecture -ceq "x64"
}

function Test-SnowStaticQtStamp {
    param(
        [Parameter(Mandatory = $true)][object]$Stamp,
        [string]$Version = $script:SnowQtVersion,
        [string]$Configuration = "Release",
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
        [string]$HostArchitecture = ""
    )

    $expected = [ordered]@{
        SchemaVersion = $script:SnowStaticQtSchemaVersion
        QtVersion = $Version
        SourceArchiveSha256 = $script:SnowQtToolchain.sourceArchiveSha256
        Configuration = $Configuration
        FeatureFingerprint = $script:SnowStaticQtFeatureFingerprint
        Ltcg = (Get-SnowStaticQtLtcgEnabled -Configuration $Configuration -Architecture $Architecture)
        SystemPng = ($Configuration -eq "Release")
        SystemZlib = ($Configuration -eq "Release")
        Timezone = $true
        TimezoneLocale = $false
    }
    foreach ($name in $expected.Keys) {
        $property = $Stamp.PSObject.Properties[$name]
        # Schema 5 predates cross builds; retain audited x64 kits while checking
        # their actual machine type separately before selecting them.
        if ($name -ceq "SchemaVersion" -and $Architecture -ceq "x64" -and
            $HostArchitecture -in @('', 'x64') -and
            $null -ne $property -and $property.Value -ceq 5) { continue }
        if ($null -eq $property -or $property.Value -cne $expected[$name]) { return $false }
        if ($expected[$name] -is [bool] -and $property.Value -isnot [bool]) { return $false }
    }
    if ($Stamp.SchemaVersion -ne 5) {
        $property = $Stamp.PSObject.Properties["Architecture"]
        if ($null -eq $property -or $property.Value -cne $Architecture) { return $false }
        $property = $Stamp.PSObject.Properties["HostArchitecture"]
        if ($null -eq $property -or $property.Value -cnotin @("x64", "arm64")) { return $false }
        if ($HostArchitecture -and $property.Value -cne $HostArchitecture) { return $false }
        $prefixProperty = $Stamp.PSObject.Properties['HostQtPrefix']
        $toolsProperty = $Stamp.PSObject.Properties['HostTools']
        if ($null -eq $prefixProperty -or -not $prefixProperty.Value -or $null -eq $toolsProperty) {
            return $false
        }
        $tools = @($toolsProperty.Value)
        $names = @('moc.exe', 'rcc.exe', 'uic.exe', 'lrelease.exe', 'lupdate.exe')
        if ($tools.Count -ne $names.Count) { return $false }
        foreach ($name in $names) {
            $matching = @($tools | Where-Object {
                $_.PSObject.Properties['Name'] -and $_.Name -ceq $name
            })
            if ($matching.Count -ne 1 -or -not $matching[0].PSObject.Properties['SHA256'] -or
                $matching[0].SHA256 -cnotmatch '^[a-f0-9]{64}$') { return $false }
        }
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

function Test-SnowQtHostToolHashes {
    param([Parameter(Mandatory = $true)][object]$Stamp,
        [Parameter(Mandatory = $true)][string]$Prefix)
    if ($Stamp.SchemaVersion -eq 5) { return $true }
    try {
        foreach ($tool in $Stamp.HostTools) {
            $path = Join-Path $Prefix "bin/$($tool.Name)"
            if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() -cne $tool.SHA256) {
                return $false
            }
        }
        return $true
    } catch { return $false }
}

function Test-SnowQtSystemCodecKit {
    param(
        [Parameter(Mandatory = $true)][string]$Qt6Dir,
        [ValidateSet("Debug", "Release")][string]$Configuration = "Release",
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
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
    $ltcgEnabled = Get-SnowStaticQtLtcgEnabled -Configuration $Configuration -Architecture $Architecture
    foreach ($feature in $script:SnowStaticQtFeaturePolicy.features.PSObject.Properties) {
        if ($feature.Name -ceq "ltcg" -and -not $ltcgEnabled) {
            # Qt can omit this configuration-dependent feature from disabled exports.
            $enabledFeatures = [regex]::Match($coreText, 'QT_ENABLED_PRIVATE_FEATURES "([^"]*)"')
            if (-not $enabledFeatures.Success -or
                ($enabledFeatures.Groups[1].Value -csplit ';') -ccontains "ltcg") { return $false }
            continue
        }
        $text = if ($feature.Name -ceq "system_png") { $guiText } else { $coreText }
        $visibility = if ($feature.Name -ceq "timezone") { "PUBLIC" } else { "PRIVATE" }
        $enabled = if ($feature.Name -ceq "ltcg") {
            $ltcgEnabled
        } elseif ($feature.Name -cin @("system_png", "system_zlib")) {
            $Configuration -eq "Release"
        } else { $feature.Value }
        if (-not (Test-SnowQtTargetFeature $text $visibility $feature.Name $enabled)) {
            return $false
        }
    }
    return $true
}

function Test-SnowValidatedStaticQtKit {
    param(
        [Parameter(Mandatory = $true)][string]$Qt6Dir,
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
    )

    if ((Get-SnowQtKitVersion -Qt6Dir $Qt6Dir) -cne $script:SnowQtVersion -or
        -not (Test-SnowQtSystemCodecKit -Qt6Dir $Qt6Dir -Architecture $Architecture) -or
        -not (Test-SnowQtTranslationKit -Qt6Dir $Qt6Dir)) { return $false }
    $prefix = [System.IO.Path]::GetFullPath((Join-Path $Qt6Dir "..\..\.."))
    $stampPath = Join-Path $prefix "share\snow-apps\static-qt-build.json"
    if (-not (Test-Path -LiteralPath $stampPath -PathType Leaf)) { return $false }
    try {
        $stamp = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
        if (-not (Test-SnowStaticQtStamp -Stamp $stamp -Architecture $Architecture)) { return $false }
        if ($stamp.SchemaVersion -ne 5 -and $stamp.HostArchitecture -ceq $Architecture) {
            return Test-SnowQtHostToolHashes -Stamp $stamp -Prefix $prefix
        }
        return $true
    }
    catch { return $false }
}

function Resolve-SnowQtDir {
    param(
        [string]$Qt6Dir = "",
        [string]$Preset = "",
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
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

    $target = if ($Preset) { Get-SnowWindowsTarget -Preset $Preset } else { $null }
    $requiredConfiguration = if ($target) { $target.Configuration } else { "" }
    $architecture = if ($target) { $target.Architecture } else { $Architecture }
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
        if ($Preset -match '-performance$' -and
            -not (Test-SnowQtDevelopmentModules -Qt6Dir $resolved)) {
            continue
        }
        if ($target -and $target.IsStatic -and
            -not (Test-SnowValidatedStaticQtKit -Qt6Dir $resolved -Architecture $architecture)) {
            continue
        }
        if (-not (Test-SnowQtArchitecture -Qt6Dir $resolved -Architecture $architecture `
                -Configuration $(if ($requiredConfiguration) { $requiredConfiguration } else { "Release" }))) {
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
        [string]$Preset = "",
        [string]$HostQtPrefix = "",
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
    )

    $qtDir = Resolve-SnowQtDir -Qt6Dir $Qt6Dir -Preset $Preset -Architecture $Architecture
    $env:SNOW_QT_STATIC_DIR = $qtDir
    $env:Qt6_DIR = $qtDir
    $env:QTDIR = [System.IO.Path]::GetFullPath((Join-Path $qtDir "..\..\.."))
    # Qt installations commonly add their MinGW toolchain to PATH. That
    # compiler is incompatible with this project's MSVC-only triplets and
    # would make CMake pick gcc for native dependency builds.
    $env:Path = @($env:Path -split ';' | Where-Object {
        $_ -and $_ -notmatch '(?i)[\\/]Qt[\\/]Tools[\\/]mingw[^\\/]*([\\/]bin)?$'
    }) -join ';'
    $target = if ($Preset) { Get-SnowWindowsTarget -Preset $Preset }
        else { Get-SnowWindowsTarget -Architecture $Architecture }
    if ($target.Architecture -cne $target.HostArchitecture) {
        $hostPrefix = Resolve-SnowQtHostPrefix -HostQtPrefix $HostQtPrefix
        $env:QT_HOST_PATH = $hostPrefix
        $env:SNOW_QT_HOST_PREFIX = $hostPrefix
        $env:Path = "$(Join-Path $hostPrefix 'bin');$env:Path"
    } else {
        $null = Resolve-SnowQtHostPrefix -HostQtPrefix $env:QTDIR
        Remove-Item Env:QT_HOST_PATH -ErrorAction SilentlyContinue
        $env:Path = "$(Join-Path $env:QTDIR 'bin');$env:Path"
    }
    return $qtDir
}

function Add-SnowMsvcToolsToPath {
    param([ValidateSet("x64", "arm64")][string]$Architecture = "x64")
    $hostArchitecture = Get-SnowWindowsHostArchitecture
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
        $component = if ($Architecture -eq "arm64") { "Microsoft.VisualStudio.Component.VC.Tools.ARM64" }
            else { "Microsoft.VisualStudio.Component.VC.Tools.x86.x64" }
        $visualStudioRoot = & $vswhere[0] -latest -products * `
            -requires $component `
            -property installationPath |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -First 1
    }
    if (-not $visualStudioRoot) {
        throw "A Visual Studio installation with the MSVC $Architecture component was not found. Install the required Build Tools or set VSINSTALLDIR."
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
    $msvcBin = Join-Path $msvcTools.FullName "bin\Host$hostArchitecture\$Architecture"
    foreach ($tool in @("cl.exe", "link.exe", "lib.exe", "dumpbin.exe") +
            $(if ($Architecture -eq "arm64") { @("armasm64.exe") } else { @() })) {
        if (-not (Test-Path -LiteralPath (Join-Path $msvcBin $tool) -PathType Leaf)) {
            throw "Required MSVC $hostArchitecture to $Architecture tool was not found: $msvcBin/$tool"
        }
    }
    $env:SNOW_MSVC_HOST_ARCHITECTURE = $hostArchitecture
    $env:SNOW_MSVC_TARGET_ARCHITECTURE = $Architecture
    $env:VSCMD_ARG_HOST_ARCH = $hostArchitecture
    $env:VSCMD_ARG_TGT_ARCH = $Architecture
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
    $sdkBin = Join-Path $env:WindowsSdkDir "bin\$sdkVersion\$hostArchitecture"
    if ($hostArchitecture -eq "arm64" -and
        -not (Test-Path -LiteralPath (Join-Path $sdkBin "rc.exe") -PathType Leaf)) {
        $sdkBin = Join-Path $env:WindowsSdkDir "bin\$sdkVersion\x64"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $sdkBin "rc.exe") -PathType Leaf)) {
        throw "Windows SDK resource compiler was not found under $sdkBin."
    }
    $env:Path = "$msvcBin;$sdkBin;$env:Path"
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
        (Join-Path $msvcTools.FullName "lib\$Architecture"),
        (Join-Path $env:WindowsSdkDir "Lib\$sdkVersion\um\$Architecture"),
        (Join-Path $env:WindowsSdkDir "Lib\$sdkVersion\ucrt\$Architecture")
    ) -join ";"
    foreach ($directory in ($env:LIB -split ';')) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            throw "The selected MSVC/Windows SDK has no $Architecture libraries: $directory"
        }
    }
    return $msvcBin
}

function Set-SnowBuildEnvironment {
    param([string]$Preset = "")

    $target = if ($Preset) { Get-SnowWindowsTarget -Preset $Preset } else { Get-SnowWindowsTarget }
    $msvcBin = Add-SnowMsvcToolsToPath -Architecture $target.Architecture
    $qtDir = Set-SnowQtEnvironment -Preset $Preset
    $env:PATH = "$msvcBin;$env:PATH"
    $env:VCPKG_ROOT = Join-Path $script:SnowRepoRoot ".tools\vcpkg"
    $env:VCPKG_DEFAULT_HOST_TRIPLET = $target.HostTriplet
    $rustVersion = (& rustc -vV | Out-String)
    if ($LASTEXITCODE -ne 0 -or $rustVersion -notmatch '(?m)^host: (x86_64|aarch64)-pc-windows-msvc\s*$') {
        throw 'Rust 1.97.1 for an x64 or ARM64 Windows MSVC host is required.'
    }
    $clangArchitecture = if ($Matches[1] -ceq 'aarch64') { "arm64" } else { "x64" }
    $hostLibclang = Join-Path $script:SnowRepoRoot ".tools/llvm-$clangArchitecture/bin"
    $clangCommand = Get-Command clang.exe -ErrorAction SilentlyContinue
    $clangCandidates = @($env:LIBCLANG_PATH, $hostLibclang,
        (Join-Path $script:SnowRepoRoot ".tools/llvm/bin"),
        (Join-Path $env:VCINSTALLDIR "Tools/Llvm/ARM64/bin"),
        (Join-Path $env:VCINSTALLDIR "Tools/Llvm/bin"),
        $(if ($clangCommand) { Split-Path -Parent $clangCommand.Source })) | Where-Object { $_ }
    $clangMachine = if ($clangArchitecture -eq "arm64") { 0xAA64 } else { 0x8664 }
    $selectedClang = $null
    foreach ($candidate in $clangCandidates) {
        $dll = Join-Path $candidate "libclang.dll"
        if (Test-Path -LiteralPath $dll -PathType Leaf) {
            if ((Get-SnowPeMachine -Path $dll) -eq $clangMachine) { $selectedClang = $candidate; break }
        }
    }
    if ($selectedClang) {
        $env:LIBCLANG_PATH = $selectedClang
    } elseif ($clangArchitecture -eq "arm64") {
        throw "Native ARM64 Rust requires libclang.dll in .tools/llvm-arm64/bin or LIBCLANG_PATH."
    }
    if ($target.Architecture -eq "arm64") {
        $env:CARGO_TARGET_AARCH64_PC_WINDOWS_MSVC_LINKER = (Get-Command link.exe).Source
    }
    return [pscustomobject]@{
        RepoRoot = $script:SnowRepoRoot
        Qt6Dir = $qtDir
        QtVersion = $script:SnowQtVersion
        MsvcToolset = $script:SnowMsvcToolset
        RustToolchain = $script:SnowRustToolchain
        RustTarget = $target.RustTarget
        Architecture = $target.Architecture
        HostArchitecture = $target.HostArchitecture
        Target = $target
        DumpbinPath = (Get-Command dumpbin.exe).Source
        VcpkgRoot = $env:VCPKG_ROOT
    }
}

function Resolve-SnowPreset {
    param(
        [ValidateSet("Debug", "Release", "Performance", "Fast")][string]$Configuration,
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
    )
    if ($Architecture -eq "arm64") { return "snow-shot-msvc-arm64-$($Configuration.ToLowerInvariant())" }
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
    $target = Get-SnowWindowsTarget -Preset $Preset
    $expectedTriplet = $target.Triplet
    $installedDir = $target.InstalledRoot
    $installedDirNeedle = [regex]::Escape(($installedDir -replace '\\', '/'))
    $lineEnd = '\r?$'

    $qtAligned = $cache -match "(?m)^Qt6_DIR:PATH=$qtNeedle$lineEnd"
    if ($target.IsStatic) {
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
            $cache -match "(?m)^CMAKE_GENERATOR_PLATFORM:INTERNAL=$($target.GeneratorPlatform)$lineEnd" -and
            $cache -match "(?m)^CMAKE_GENERATOR_TOOLSET:INTERNAL=$([regex]::Escape($target.Toolset))$lineEnd"
    }

    $crossAligned = $true
    if ($target.Architecture -cne $target.HostArchitecture) {
        $hostNeedle = [regex]::Escape(($env:QT_HOST_PATH -replace '\\', '/'))
        $crossAligned = $cache -match "(?m)^QT_HOST_PATH:PATH=$hostNeedle$lineEnd"
    }
    $rustAligned = ($target.Architecture -eq 'x64' -and $cache -notmatch '(?m)^SNOW_RUST_TARGET:') -or
        $cache -match "(?m)^SNOW_RUST_TARGET:.*=$($target.RustTarget)$lineEnd"
    $hostAligned = ($target.Architecture -eq 'x64' -and $target.HostArchitecture -eq 'x64' -and
        $cache -notmatch '(?m)^VCPKG_HOST_TRIPLET:') -or
        $cache -match "(?m)^VCPKG_HOST_TRIPLET:.*=$($target.HostTriplet)$lineEnd"
    return $qtAligned -and $powerShellAligned -and $generatorAligned -and
        $crossAligned -and $rustAligned -and $hostAligned -and
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
    $target = Get-SnowWindowsTarget -Preset $Preset
    $arguments = @("--preset", $Preset, "-S", $script:SnowRepoRoot, "-B", $BuildDirectory,
        "-DVCPKG_HOST_TRIPLET=$($target.HostTriplet)", "-DSNOW_RUST_TARGET=$($target.RustTarget)")
    if ($env:LIBCLANG_PATH) {
        $arguments += "-DSNOW_LIBCLANG_BIN_DIR:PATH=$env:LIBCLANG_PATH"
        $arguments += "-DSNOW_SHOT_LIBCLANG_DIR:PATH=$env:LIBCLANG_PATH"
    }
    if ($target.Architecture -cne $target.HostArchitecture) {
        $arguments += "-DQT_HOST_PATH:PATH=$env:QT_HOST_PATH"
    } else { $arguments += "-UQT_HOST_PATH" }
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
    $configuration = (Get-SnowWindowsTarget -Preset $Preset).Configuration
    $buildRoot = Join-Path $script:SnowRepoRoot "build\$Preset"
    $match = Get-ChildItem -LiteralPath $buildRoot -Filter $Name -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\$configuration\\" } |
        Sort-Object FullName |
        Select-Object -First 1
    if (-not $match) { throw "$Name was not found under $buildRoot ($configuration)." }
    return $match
}

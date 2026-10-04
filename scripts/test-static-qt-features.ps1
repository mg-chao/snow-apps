[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Import-ScriptFunction([string]$Path, [string]$Name) {
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        $Path, [ref]$null, [ref]$errors)
    Require ($errors.Count -eq 0) "$Path must parse successfully."
    $function = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $Name
    }, $false)
    Require ($null -ne $function) "$Path must define $Name."
    $bodyText = $function.Body.Extent.Text.Replace('$PSScriptRoot',
        ("'" + $PSScriptRoot.Replace("'", "''") + "'"))
    Set-Item -Path "Function:script:$Name" -Value ([scriptblock]::Create(
        $bodyText.Substring(1, $bodyText.Length - 2)))
}

Import-ScriptFunction (Join-Path $PSScriptRoot "build-static-qt.ps1") "Write-StaticQtBuildStamp"
Import-ScriptFunction (Join-Path $PSScriptRoot "build-static-qt.ps1") "Install-QtSourcePatches"
Import-ScriptFunction (Join-Path $PSScriptRoot "build-static-qt.ps1") "Invoke-Checked"
Import-ScriptFunction (Join-Path $PSScriptRoot "build-static-qt.ps1") "Assert-QtSourceArchive"
Import-ScriptFunction (Join-Path $PSScriptRoot "build-static-qt.ps1") "Test-InstalledQtLicenseBundle"
Import-ScriptFunction (Join-Path $PSScriptRoot "build-static-qt.ps1") "Install-QtLicenseBundle"
Import-ScriptFunction (Join-Path $PSScriptRoot "build-static-qt.ps1") "Test-PathIsSameOrDescendant"
Import-ScriptFunction (Join-Path $PSScriptRoot "build-static-qt.ps1") "Get-NormalizedDirectoryPath"
Import-ScriptFunction (Join-Path $PSScriptRoot "build-static-qt.ps1") "Assert-QtDirectoryIsolation"
Import-ScriptFunction (Join-Path $PSScriptRoot "build-static-qt.ps1") "Assert-QtDependencyArchitecture"
Import-ScriptFunction (Join-Path $PSScriptRoot "package-snow-shot.ps1") "Get-ValidatedStaticQtStamp"

function Require-Rejected([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Require $rejected $Message
}

$testRoot = Join-Path $script:SnowRepoRoot ("build/static-qt-feature-tests-" + [guid]::NewGuid().ToString("N"))
try {
    $sourceFixture = Join-Path $testRoot 'source-isolated'
    $buildFixture = Join-Path $testRoot 'build-isolated'
    $installFixture = Join-Path $testRoot 'target-isolated'
    Assert-QtDirectoryIsolation -Source $sourceFixture -Build $buildFixture -Install $installFixture `
        -HostPrefix (Join-Path $testRoot 'host-isolated')
    foreach ($hostPrefix in @($installFixture, (Join-Path $installFixture 'nested-host'), $testRoot,
            (Join-Path $buildFixture 'nested-host'), (Join-Path $sourceFixture 'nested-host'))) {
        Require-Rejected { Assert-QtDirectoryIsolation -Source $sourceFixture -Build $buildFixture `
            -Install $installFixture -HostPrefix $hostPrefix } 'Qt source/build/install paths cannot overlap required host tools.'
    }
    $corruptArchive = Join-Path $testRoot "corrupt-source.tar.xz"
    $null = New-Item -ItemType Directory -Path $testRoot -Force
    if (-not (Get-Command dumpbin.exe -ErrorAction SilentlyContinue)) {
        Add-SnowMsvcToolsToPath -Architecture x64 | Out-Null
    }
    # Valid minimal COFF headers exercise the real host inspector without
    # requiring either target compiler or executing a target binary.
    $dependencyPrefix = Join-Path $testRoot 'dependencies'
    $dependencyLib = Join-Path $dependencyPrefix 'lib'
    $null = New-Item -ItemType Directory -Path $dependencyLib -Force
    foreach ($architecture in @('x64', 'arm64')) {
        $coff = [byte[]]::new(20)
        $machine = if ($architecture -eq 'arm64') { 0xaa64 } else { 0x8664 }
        [BitConverter]::GetBytes([uint16]$machine).CopyTo($coff, 0)
        foreach ($name in @('zlibstatic.lib', 'libpng16.lib')) {
            [IO.File]::WriteAllBytes((Join-Path $dependencyLib $name), $coff)
        }
        Assert-QtDependencyArchitecture -Prefix $dependencyPrefix -Architecture $architecture
        $opposite = if ($architecture -eq 'arm64') { 'x64' } else { 'arm64' }
        Require-Rejected {
            Assert-QtDependencyArchitecture -Prefix $dependencyPrefix -Architecture $opposite
        } 'Static Qt must reject libraries for the opposite target.'
        Move-Item -LiteralPath (Join-Path $dependencyLib 'zlibstatic.lib') `
            -Destination (Join-Path $dependencyLib 'zlib.lib')
        Require-Rejected {
            Assert-QtDependencyArchitecture -Prefix $dependencyPrefix -Architecture $architecture
        } 'Static Qt requires the pinned static zlib library, not an import library with a similar name.'
        Remove-Item -LiteralPath (Join-Path $dependencyLib 'zlib.lib')
    }
    [IO.File]::WriteAllText($corruptArchive, "incomplete or altered download")
    Require-Rejected { Assert-QtSourceArchive -Path $corruptArchive } `
        "A downloaded or cached source archive must match the release SHA256 before extraction."
    $qtDir = Join-Path $testRoot "lib/cmake/Qt6"
    $coreDir = Join-Path $testRoot "lib/cmake/Qt6Core"
    $guiDir = Join-Path $testRoot "lib/cmake/Qt6Gui"
    $null = New-Item -ItemType Directory -Path $qtDir, $coreDir, $guiDir -Force
    # Kit selection checks the actual machine independently of feature metadata.
    # These PE headers are inert fixtures; no Qt binary is loaded or executed.
    $qtBin = Join-Path $testRoot 'bin'
    $null = New-Item -ItemType Directory -Path $qtBin -Force
    $pe = [byte[]]::new(128)
    $pe[0] = 0x4D; $pe[1] = 0x5A; $pe[0x3C] = 64
    $pe[64] = 0x50; $pe[65] = 0x45; $pe[68] = 0x64; $pe[69] = 0x86
    foreach ($name in @('Qt6Core.dll', 'Qt6Cored.dll')) {
        [IO.File]::WriteAllBytes((Join-Path $qtBin $name), $pe)
    }
    $patchSource = Join-Path $testRoot "source"
    $patchTarget = Join-Path $patchSource "qtbase/src/corelib/time/qtimezoneprivate_win.cpp"
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $patchTarget) -Force
    $versionPath = Join-Path $patchSource "qtbase/.cmake.conf"
    [IO.File]::WriteAllText($versionPath, 'set(QT_REPO_MODULE_VERSION "6.12.0")' + "`n")
    $upstreamFragment = @'

#include <private/qwinregistry_p.h>

QT_REQUIRE_CONFIG(timezone_locale);

QT_BEGIN_NAMESPACE

QString QWinTimeZonePrivate::displayName(QTimeZone::TimeType timeType,
                                         QTimeZone::NameType nameType,
                                         const QLocale &locale) const
{
    // Registry gave us long names for the system locale:
    if (nameType == QTimeZone::LongName && locale == QLocale::system()) {
        switch (timeType) {
        case  QTimeZone::DaylightTime :
            return m_daylightName;
        case  QTimeZone::GenericTime :
            return m_displayName;
        case  QTimeZone::StandardTime :
            return m_standardName;
        }
    }
    // Fall back to base class for everything else.
    return QTimeZonePrivate::displayName(timeType, nameType, locale);;
}

        tran.daylightTimeOffset = 0;
    }
    tran.offsetFromUtc = tran.standardTimeOffset + tran.daylightTimeOffset;
    tran.abbreviation = localeName(atMSecsSinceEpoch, tran.offsetFromUtc,
                                   type, QTimeZone::ShortName, QLocale::system());
    return tran;
}

'@
    [IO.File]::WriteAllText($patchTarget, ($upstreamFragment -replace "`r`n", "`n") + "`n")
    Install-QtSourcePatches -Source $patchSource
    $patchedHash = (Get-FileHash -LiteralPath $patchTarget -Algorithm SHA256).Hash
    Require ((Get-Content -LiteralPath $patchTarget -Raw) -match 'QT_REQUIRE_CONFIG\(timezone\);') `
        "The patch must be applied to the requested source tree, including one nested in this repository."
    Install-QtSourcePatches -Source $patchSource
    Require ((Get-FileHash -LiteralPath $patchTarget -Algorithm SHA256).Hash -ceq $patchedHash) `
        "Reapplying the audited patch must preserve its exact result."
    [IO.File]::WriteAllText($patchTarget, "unrecognized upstream source`n")
    Require-Rejected { Install-QtSourcePatches -Source $patchSource 2>$null } `
        "A patch that does not match upstream source must fail before configuration."
    [IO.File]::WriteAllText($versionPath, 'set(QT_REPO_MODULE_VERSION "6.11.1")' + "`n")
    Require-Rejected { Install-QtSourcePatches -Source $patchSource 2>$null } `
        "An unsupported Qt source version must be rejected before applying patches."
    Set-Content -LiteralPath (Join-Path $qtDir "Qt6Config.cmake") -Value "# fixture"
    Set-Content -LiteralPath (Join-Path $qtDir "Qt6ConfigVersion.cmake") -Value 'set(PACKAGE_VERSION "6.12.0")'
    Set-Content -LiteralPath (Join-Path $coreDir "Qt6CoreTargets-release.cmake") -Value "# fixture"
    foreach ($component in @("Qt6Concurrent", "Qt6Test")) {
        $componentDir = Join-Path $testRoot "lib/cmake/$component"
        $null = New-Item -ItemType Directory -Path $componentDir -Force
        Set-Content -LiteralPath (Join-Path $componentDir "$($component)Config.cmake") -Value "# fixture"
    }
    $coreTargets = @'
QT_ENABLED_PUBLIC_FEATURES "static;static_runtime;timezone;concurrent"
QT_ENABLED_PRIVATE_FEATURES "system_zlib;ltcg;testlib"
QT_DISABLED_PRIVATE_FEATURES "timezone_locale"
'@
    $corePath = Join-Path $coreDir "Qt6CoreTargets.cmake"
    Set-Content -LiteralPath $corePath -Value $coreTargets
    Require (Test-SnowQtDevelopmentModules $qtDir) "A complete development kit must be recognized."
    Set-Content -LiteralPath $corePath -Value ($coreTargets -replace ';concurrent', '')
    Require (-not (Test-SnowQtDevelopmentModules $qtDir)) `
        "Stale component files must not hide a disabled QtConcurrent feature."
    Require-Rejected { Resolve-SnowQtDir -Qt6Dir $qtDir -Preset windows-msvc-performance } `
        "The default performance workspace requires the actual QtConcurrent and Test features."
    Set-Content -LiteralPath $corePath -Value $coreTargets
    Set-Content -LiteralPath (Join-Path $guiDir "Qt6GuiTargets.cmake") -Value 'QT_ENABLED_PRIVATE_FEATURES "system_png"'
    $translationDirectory = Join-Path $testRoot "translations"
    New-Item -ItemType Directory -Path $translationDirectory -Force | Out-Null
    foreach ($language in @("zh_CN", "zh_TW")) {
        Set-Content -LiteralPath (Join-Path $translationDirectory "qtbase_$language.qm") -Value "fixture"
    }
    Require (Test-SnowQtTranslationKit $qtDir) "Chinese catalogs must not require an English Qt catalog."
    $traditionalCatalog = Join-Path $translationDirectory "qtbase_zh_TW.qm"
    Remove-Item -LiteralPath $traditionalCatalog
    Require (-not (Test-SnowQtTranslationKit $qtDir)) "Both Chinese Qt stock-dialog catalogs are required."
    Set-Content -LiteralPath $traditionalCatalog -Value "fixture"
    $stampPath = Join-Path $testRoot "share/snow-apps/static-qt-build.json"
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $testRoot 'bin')
    foreach ($name in @('moc', 'rcc', 'uic', 'lrelease', 'lupdate')) {
        [IO.File]::WriteAllText((Join-Path $testRoot "bin/$name.exe"), 'host tool fixture')
    }
    Write-StaticQtBuildStamp -Path $stampPath -Version "6.12.0" -BuildConfiguration Release `
        -Fingerprint "dependencies" -SourceArchive "https://example.invalid/qt.tar.xz" -BuildParallelism 4 -HostArchitecture x64
    $originalStamp = Get-Content -LiteralPath $stampPath -Raw
    $nativeStamp = $originalStamp | ConvertFrom-Json
    Require (Test-SnowQtHostToolHashes -Stamp $nativeStamp -Prefix $testRoot) 'Native host-tool hashes must match their stamp.'
    $nativeTool = Join-Path $testRoot 'bin/moc.exe'
    [IO.File]::WriteAllText($nativeTool, 'replaced host tool')
    Require (-not (Test-SnowQtHostToolHashes -Stamp $nativeStamp -Prefix $testRoot)) 'Replaced native host tools must invalidate Qt reuse.'
    Remove-Item -LiteralPath $nativeTool
    Require (-not (Test-SnowQtHostToolHashes -Stamp $nativeStamp -Prefix $testRoot)) 'Missing native host tools must invalidate Qt reuse.'
    [IO.File]::WriteAllText($nativeTool, 'host tool fixture')
    $crossStamp = $originalStamp | ConvertFrom-Json
    $crossStamp.Architecture = 'arm64'
    $crossStamp.HostArchitecture = 'x64'
    Require (Test-SnowStaticQtStamp -Stamp $crossStamp -Architecture arm64 -HostArchitecture x64) `
        'Cross-built Qt must retain all five host-tool hashes.'
    Require (-not (Test-SnowStaticQtStamp -Stamp $crossStamp -Architecture x64)) `
        'Qt stamps must reject the opposite target architecture.'
    Require (-not (Test-SnowStaticQtStamp -Stamp $crossStamp -Architecture arm64 -HostArchitecture arm64)) `
        'Qt build reuse must bind the host architecture.'
    $crossStamp.HostTools[1].Name = 'moc.exe'
    Require (-not (Test-SnowStaticQtStamp -Stamp $crossStamp -Architecture arm64)) `
        'Duplicate host-tool records cannot replace missing provenance.'
    $crossStamp = $originalStamp | ConvertFrom-Json
    $crossStamp.Architecture = 'arm64'
    $crossStamp.HostArchitecture = 'x64'
    $crossStamp.HostTools[0].SHA256 = 'invalid'
    Require (-not (Test-SnowStaticQtStamp -Stamp $crossStamp -Architecture arm64)) `
        'Cross-built Qt must reject malformed host-tool hashes.'
    Write-StaticQtBuildStamp -Path $stampPath -Version $script:SnowQtVersion -BuildConfiguration Debug `
        -Fingerprint "dependencies" -SourceArchive "https://example.invalid/qt.tar.xz" -BuildParallelism 4 -HostArchitecture x64
    $debugStamp = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
    Require (Test-SnowStaticQtStamp -Stamp $debugStamp -Configuration Debug) `
        "Debug provenance must reflect Qt's supported non-LTCG configuration."
    Require (-not $debugStamp.Ltcg -and -not $debugStamp.SystemPng -and -not $debugStamp.SystemZlib -and
        $debugStamp.DisabledFeatures -notcontains "testlib" -and
        $debugStamp.DisabledFeatures -notcontains "concurrent") `
        "A development kit must retain Qt Test and Concurrent without Release-only LTCG."
    Set-Content -LiteralPath $stampPath -Value $originalStamp
    $debugTargets = $coreTargets -replace 'system_zlib;ltcg', '' `
        -replace 'QT_DISABLED_PRIVATE_FEATURES "timezone_locale"', 'QT_DISABLED_PRIVATE_FEATURES "timezone_locale;ltcg;system_zlib"'
    Set-Content -LiteralPath $corePath -Value $debugTargets
    $guiPath = Join-Path $guiDir "Qt6GuiTargets.cmake"
    Set-Content -LiteralPath $guiPath -Value 'QT_ENABLED_PRIVATE_FEATURES "" QT_DISABLED_PRIVATE_FEATURES "system_png"'
    Require (Test-SnowQtSystemCodecKit -Qt6Dir $qtDir -Configuration Debug) `
        "Development feature validation must accept Qt's disabled Debug LTCG."
    Set-Content -LiteralPath $corePath -Value ($debugTargets -replace ';ltcg;system_zlib', ';system_zlib')
    Require (Test-SnowQtSystemCodecKit -Qt6Dir $qtDir -Configuration Debug) `
        "Qt omits inapplicable LTCG from both Debug feature export lists."
    Require (-not (Test-SnowQtSystemCodecKit -Qt6Dir $qtDir)) `
        "Production feature validation must still require Release LTCG."
    Set-Content -LiteralPath $corePath -Value $coreTargets
    Set-Content -LiteralPath $guiPath -Value 'QT_ENABLED_PRIVATE_FEATURES "system_png"'
    $patchDirectory = Join-Path $testRoot "share/snow-apps/qt-licenses/patches"
    $null = New-Item -ItemType Directory -Path $patchDirectory -Force
    foreach ($patch in $script:SnowStaticQtSourcePatches) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $patch.File) -Destination $patchDirectory
    }
    $licenseRoot = Split-Path -Parent $patchDirectory
    $licenseSource = Join-Path $testRoot "license-source"
    foreach ($component in @("root", "qtbase", "qtsvg", "qttools", "qttranslations")) {
        $componentSource = if ($component -eq "root") { $licenseSource } else { Join-Path $licenseSource $component }
        $licensesDirectory = Join-Path $componentSource "LICENSES"
        New-Item -ItemType Directory -Path $licensesDirectory -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $licensesDirectory "GPL-3.0-only.txt") -Value "license text"
        $metadataName = if ($component -eq "qttranslations") { "licenseRule.json" } else { "REUSE.toml" }
        Set-Content -LiteralPath (Join-Path $componentSource $metadataName) -Value "upstream $component metadata"
    }
    Install-QtLicenseBundle -Source $licenseSource -Prefix $testRoot -Version $script:SnowQtVersion `
        -SourceArchive "https://example.invalid/qt.tar.xz"
    Require (Test-InstalledQtLicenseBundle -Prefix $testRoot) "The source-license bundle must include Qt translations."
    $translationLicense = Join-Path $licenseRoot "qttranslations/licenseRule.json"
    Require ((Get-Content -LiteralPath $translationLicense -Raw) -ceq
        (Get-Content -LiteralPath (Join-Path $licenseSource "qttranslations/licenseRule.json") -Raw)) `
        "Translation license rules must retain their upstream contents."
    Require (-not (Test-Path -LiteralPath (Join-Path $licenseRoot "qttranslations/REUSE.toml"))) `
        "The bundle must not invent REUSE metadata absent from Qt Translations."
    $sourceTranslationLicense = Join-Path $licenseSource "qttranslations/licenseRule.json"
    Remove-Item -LiteralPath $sourceTranslationLicense
    Require-Rejected {
        Install-QtLicenseBundle -Source $licenseSource -Prefix $testRoot -Version $script:SnowQtVersion `
            -SourceArchive "https://example.invalid/qt.tar.xz"
    } "Incomplete source metadata must be rejected before replacing an existing bundle."
    Require (Test-InstalledQtLicenseBundle -Prefix $testRoot) "A failed refresh must preserve the existing license bundle."
    Remove-Item -LiteralPath $translationLicense
    Require (-not (Test-InstalledQtLicenseBundle -Prefix $testRoot)) `
        "A source license bundle without qttranslations provenance must be rejected."
    Set-Content -LiteralPath $translationLicense -Value "upstream qttranslations metadata"
    $licenseManifestPath = Join-Path $licenseRoot "manifest.json"
    $licenseManifest = Get-Content -LiteralPath $licenseManifestPath -Raw
    $incompleteManifest = $licenseManifest | ConvertFrom-Json
    $incompleteManifest.Components = @("root", "qtbase", "qtsvg", "qttools")
    $incompleteManifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $licenseManifestPath
    Require (-not (Test-InstalledQtLicenseBundle -Prefix $testRoot)) `
        "The manifest must record Qt Translations as a bundled source component."
    Set-Content -LiteralPath $licenseManifestPath -Value $licenseManifest

    Require (Test-SnowValidatedStaticQtKit $qtDir) "The current trimmed kit must be accepted."
    Require ((Resolve-SnowQtDir -Qt6Dir $qtDir -Preset snow-shot-msvc-release) -eq $qtDir) `
        "Release bootstrap must resolve a validated kit."
    $stamp = Get-ValidatedStaticQtStamp -Prefix $testRoot -ExpectedVersion "6.12.0" -ExpectedConfiguration Release
    Require ($stamp.Timezone -and -not $stamp.TimezoneLocale) "Packaging must retain time-zone handling."

    $versionConfig = Join-Path $qtDir "Qt6ConfigVersion.cmake"
    Set-Content -LiteralPath $versionConfig -Value 'set(PACKAGE_VERSION "6.11.1")'
    Require (-not (Test-SnowValidatedStaticQtKit $qtDir)) `
        "A current stamp must not hide a stale installed Qt version."
    Require-Rejected {
        Get-ValidatedStaticQtStamp -Prefix $testRoot -ExpectedVersion $script:SnowQtVersion -ExpectedConfiguration Release
    } "Packaging must verify the installed version independently of its stamp."
    Set-Content -LiteralPath $versionConfig -Value ('set(PACKAGE_VERSION "' + $script:SnowQtVersion + '")')

    foreach ($change in @(
        @{ Name = "SchemaVersion"; Value = 3 },
        @{ Name = "FeatureFingerprint"; Value = "stale-policy" },
        @{ Name = "SourceArchiveSha256"; Value = "altered-source" },
        @{ Name = "Timezone"; Value = $false },
        @{ Name = "TimezoneLocale"; Value = $true },
        @{ Name = "TimezoneLocale"; Value = "false" }
    )) {
        $value = $originalStamp | ConvertFrom-Json
        $value.($change.Name) = $change.Value
        $value | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $stampPath
        Require (-not (Test-SnowValidatedStaticQtKit $qtDir)) "Bootstrap accepted an invalid $($change.Name)."
        Require-Rejected {
            Get-ValidatedStaticQtStamp -Prefix $testRoot -ExpectedVersion "6.12.0" -ExpectedConfiguration Release
        } "Packaging accepted an invalid $($change.Name)."
    }
    Set-Content -LiteralPath $stampPath -Value $originalStamp
    $value = $originalStamp | ConvertFrom-Json
    $value.SourcePatches[0].SHA256 = "altered-patch"
    $value | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $stampPath
    Require (-not (Test-SnowValidatedStaticQtKit $qtDir)) "A kit with altered source-patch metadata must be rejected."
    Set-Content -LiteralPath $stampPath -Value $originalStamp
    $installedPatch = Join-Path $patchDirectory $script:SnowStaticQtSourcePatches[0].File
    Set-Content -LiteralPath $installedPatch -Value "altered-patch"
    Require-Rejected {
        Get-ValidatedStaticQtStamp -Prefix $testRoot -ExpectedVersion "6.12.0" -ExpectedConfiguration Release
    } "Packaging must reject altered installed source-patch provenance."
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script:SnowStaticQtSourcePatches[0].File) `
        -Destination $installedPatch -Force

    foreach ($invalidTargets in @(
        ($coreTargets -replace 'static;static_runtime;timezone', 'static_runtime;timezone'),
        ($coreTargets -replace 'static;static_runtime;timezone', 'static;timezone'),
        ($coreTargets -replace 'static;static_runtime;timezone', 'static;static_runtime'),
        ($coreTargets -replace 'system_zlib;ltcg', 'system_zlib_extra;ltcg'),
        ($coreTargets -replace 'system_zlib;ltcg', 'system_zlib;ltcg;timezone_locale'),
        ($coreTargets -replace 'QT_DISABLED_PRIVATE_FEATURES "timezone_locale"', 'QT_DISABLED_PRIVATE_FEATURES ""')
    )) {
        Set-Content -LiteralPath $corePath -Value $invalidTargets
        Require (-not (Test-SnowValidatedStaticQtKit $qtDir)) "Bootstrap trusted a stamp with conflicting installed features."
        Require-Rejected {
            Get-ValidatedStaticQtStamp -Prefix $testRoot -ExpectedVersion "6.12.0" -ExpectedConfiguration Release
        } "Packaging trusted a stamp with conflicting installed features."
    }
    Set-Content -LiteralPath $corePath -Value $coreTargets
    Remove-Item -LiteralPath $stampPath
    Require (-not (Test-SnowValidatedStaticQtKit $qtDir)) "An unstamped release kit must be rejected."
    Require ((Resolve-SnowQtDir -Qt6Dir $qtDir -Preset windows-msvc-performance) -eq $qtDir) `
        "Development and performance kits do not require the production stamp."
    Set-Content -LiteralPath (Join-Path $coreDir "Qt6CoreTargets-debug.cmake") -Value "# fixture"
    Set-Content -LiteralPath $corePath -Value ($coreTargets -replace 'system_zlib;ltcg',
        'system_zlib;ltcg;timezone_locale' -replace 'QT_DISABLED_PRIVATE_FEATURES "timezone_locale"',
        'QT_DISABLED_PRIVATE_FEATURES ""')
    Require ((Resolve-SnowQtDir -Qt6Dir $qtDir -Preset windows-msvc-debug) -eq $qtDir) `
        "Existing Debug kits with CLDR name data must remain usable."
    Require-Rejected { Resolve-SnowQtDir -Qt6Dir $qtDir -Preset snow-shot-msvc-release } `
        "Production discovery must reject an old untrimmed kit."
}
finally {
    $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $resolvedBuildRoot = [System.IO.Path]::GetFullPath((Join-Path $script:SnowRepoRoot "build"))
    $separator = [System.IO.Path]::DirectorySeparatorChar
    $comparison = if ($separator -eq '\') {
        [System.StringComparison]::OrdinalIgnoreCase
    } else {
        [System.StringComparison]::Ordinal
    }
    if (-not $resolvedTestRoot.StartsWith($resolvedBuildRoot.TrimEnd($separator) + $separator,
            $comparison)) {
        throw "Refusing to remove a test directory outside $resolvedBuildRoot"
    }
    if (Test-Path -LiteralPath $resolvedTestRoot) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}

Write-Output "Static Qt feature policy tests passed."

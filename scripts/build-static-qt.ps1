[CmdletBinding()]
param(
    [string]$QtVersion = "",
    [Parameter(Mandatory = $true)][string]$InstallPrefix,
    [string]$SourceDirectory = "",
    [string]$BuildDirectory = "",
    [string]$DependencyPrefix = "",
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
    [string]$HostQtPrefix = "",
    [ValidateSet("Debug", "Release")][string]$Configuration = "Release",
    [string]$QtMirrorBaseUrl = "https://qt.mirror.constant.com/official_releases",
    [ValidateRange(1, 256)][int]$Parallelism = 4,
    [switch]$DevelopmentModules,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
if ([string]::IsNullOrWhiteSpace($QtVersion)) { $QtVersion = $script:SnowQtVersion }
if ($QtVersion -cne $script:SnowQtVersion) {
    throw "The audited Qt toolchain requires Qt $script:SnowQtVersion, requested $QtVersion."
}
$hostArchitecture = Get-SnowWindowsHostArchitecture
$crossCompilation = $Architecture -cne $hostArchitecture
if ($crossCompilation) { $HostQtPrefix = Resolve-SnowQtHostPrefix -HostQtPrefix $HostQtPrefix }
else { $HostQtPrefix = "" }
Add-SnowMsvcToolsToPath -Architecture $Architecture | Out-Null
$enableDevelopmentModules = $DevelopmentModules -or $Configuration -eq "Debug"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$WorkingDirectory = (Get-Location).Path
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

function Get-NormalizedDirectoryPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $rootPath = [System.IO.Path]::GetPathRoot($fullPath)
    if ($fullPath -eq $rootPath) {
        return $rootPath
    }
    return $fullPath.TrimEnd([char[]]@('\', '/'))
}

function Test-PathIsSameOrDescendant {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent
    )

    $normalizedPath = Get-NormalizedDirectoryPath -Path $Path
    $normalizedParent = Get-NormalizedDirectoryPath -Path $Parent
    if ($normalizedPath.Equals(
            $normalizedParent,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    $parentPrefix = $normalizedParent
    if (-not $parentPrefix.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $parentPrefix += [System.IO.Path]::DirectorySeparatorChar
    }
    return $normalizedPath.StartsWith(
        $parentPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-SafeRecursiveRemovalTarget {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $normalizedPath = Get-NormalizedDirectoryPath -Path $Path
    $rootPath = Get-NormalizedDirectoryPath -Path (
        [System.IO.Path]::GetPathRoot($normalizedPath))
    if ($normalizedPath.Equals($rootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description cannot be a filesystem root: $normalizedPath"
    }

    $protectedPaths = @(
        $repoRoot,
        [Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile),
        [Environment]::GetFolderPath([Environment+SpecialFolder]::Windows),
        [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles),
        [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86),
        [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonApplicationData),
        [System.IO.Path]::GetTempPath()
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

    foreach ($protectedPath in $protectedPaths) {
        if (Test-PathIsSameOrDescendant -Path $protectedPath -Parent $normalizedPath) {
            throw "$Description cannot be a protected path or one of its ancestors: $normalizedPath"
        }
    }
}

function Assert-QtDirectoryIsolation {
    param([Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Build,
        [Parameter(Mandatory = $true)][string]$Install,
        [string]$HostPrefix = '')
    $directories = [ordered]@{ Source = $Source; Build = $Build; Install = $Install }
    if ($HostPrefix) { $directories.Host = $HostPrefix }
    $names = @($directories.Keys)
    for ($left = 0; $left -lt $names.Count; $left++) {
        for ($right = $left + 1; $right -lt $names.Count; $right++) {
            $leftPath = $directories[$names[$left]]
            $rightPath = $directories[$names[$right]]
            if ((Test-PathIsSameOrDescendant -Path $leftPath -Parent $rightPath) -or
                (Test-PathIsSameOrDescendant -Path $rightPath -Parent $leftPath)) {
                throw "Qt $($names[$left]) and $($names[$right]) directories must be distinct and non-overlapping: '$leftPath', '$rightPath'."
            }
        }
    }
}

function Save-RemoteFile {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $client = [System.Net.Http.HttpClient]::new()
    $client.Timeout = [TimeSpan]::FromHours(2)
    $response = $null
    $inputStream = $null
    $outputStream = $null
    try {
        Write-Output "Downloading $Uri"
        $response = $client.GetAsync(
            $Uri,
            [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead
        ).GetAwaiter().GetResult()
        $response.EnsureSuccessStatusCode()
        $contentLength = $response.Content.Headers.ContentLength
        $inputStream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $outputStream = [System.IO.File]::Open(
            $Destination,
            [System.IO.FileMode]::Create,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None
        )
        $buffer = [byte[]]::new(1024 * 1024)
        $downloaded = [int64]0
        $lastLog = [DateTime]::UtcNow
        while (($read = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $outputStream.Write($buffer, 0, $read)
            $downloaded += $read
            $now = [DateTime]::UtcNow
            if (($now - $lastLog).TotalSeconds -ge 2) {
                $downloadedMiB = [math]::Round($downloaded / 1MB, 1)
                if ($contentLength) {
                    $percent = [math]::Round(($downloaded * 100.0) / $contentLength, 1)
                    $totalMiB = [math]::Round($contentLength / 1MB, 1)
                    Write-Progress -Activity "Downloading Qt $QtVersion sources" `
                        -Status "$downloadedMiB / $totalMiB MiB ($percent%)" `
                        -PercentComplete $percent
                    Write-Output "Qt source download: $downloadedMiB / $totalMiB MiB ($percent%)"
                }
                else {
                    Write-Progress -Activity "Downloading Qt $QtVersion sources" `
                        -Status "$downloadedMiB MiB received"
                    Write-Output "Qt source download: $downloadedMiB MiB received"
                }
                $lastLog = $now
            }
        }
        Write-Progress -Activity "Downloading Qt $QtVersion sources" -Completed
        Write-Output "Qt source download complete: $([math]::Round($downloaded / 1MB, 1)) MiB"
    }
    finally {
        if ($outputStream) { $outputStream.Dispose() }
        if ($inputStream) { $inputStream.Dispose() }
        if ($response) { $response.Dispose() }
        $client.Dispose()
    }
}

function Get-DependencyFingerprint {
    param([Parameter(Mandatory = $true)][string]$Prefix)

    $abiFiles = @(
        (Join-Path $Prefix "share\zlib\vcpkg_abi_info.txt"),
        (Join-Path $Prefix "share\libpng\vcpkg_abi_info.txt")
    )
    foreach ($abiFile in $abiFiles) {
        if (-not (Test-Path -LiteralPath $abiFile -PathType Leaf)) {
            throw "The static Qt dependency package is incomplete: $abiFile"
        }
    }
    $fingerprintText = ($abiFiles | ForEach-Object {
        "$([System.IO.Path]::GetFileName((Split-Path -Parent $_))):$((Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash)"
    }) -join "|"
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($fingerprintText)
    return [Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($bytes)
    ).ToLowerInvariant()
}

function Assert-QtDependencyArchitecture {
    param([Parameter(Mandatory = $true)][string]$Prefix,
        [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64')

    # The pinned zlib-ng port names its static MSVC library zlibstatic.lib.
    foreach ($library in @('zlibstatic.lib', 'libpng16.lib')) {
        if (-not (Test-SnowWindowsLibraryArchitecture -Path (Join-Path $Prefix "lib/$library") `
                -Architecture $Architecture)) {
            throw "Static Qt dependency $library does not match target architecture $Architecture."
        }
    }
}

function Assert-QtSourceArchive {
    param([Parameter(Mandatory = $true)][string]$Path)

    $actualHash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -cne $script:SnowQtToolchain.sourceArchiveSha256) {
        throw "Qt $script:SnowQtVersion source archive SHA256 mismatch: $Path"
    }
}

function Test-InstalledQtSystemCodecs {
    param(
        [Parameter(Mandatory = $true)][string]$Prefix,
        [ValidateSet("Debug", "Release")][string]$Configuration = "Release"
    )

    return Test-SnowQtSystemCodecKit -Qt6Dir (Join-Path $Prefix "lib\cmake\Qt6") `
        -Configuration $Configuration
}

function Test-InstalledQtLicenseBundle {
    param([Parameter(Mandatory = $true)][string]$Prefix)

    $licenseRoot = Join-Path $Prefix "share\snow-apps\qt-licenses"
    foreach ($requiredLicenseFile in @(
            "manifest.json",
            "root\REUSE.toml",
            "root\LICENSES\GPL-3.0-only.txt",
            "qtbase\REUSE.toml",
            "qtsvg\REUSE.toml",
            "qttools\REUSE.toml",
            "qttranslations\licenseRule.json",
            "qttranslations\LICENSES\GPL-3.0-only.txt")) {
        if (-not (Test-Path -LiteralPath (Join-Path $licenseRoot $requiredLicenseFile) -PathType Leaf)) {
            return $false
        }
    }
    try {
        $manifest = Get-Content -LiteralPath (Join-Path $licenseRoot "manifest.json") -Raw |
            ConvertFrom-Json
        if ($manifest.SchemaVersion -ne 1 -or $manifest.QtVersion -cne $script:SnowQtVersion -or
            $manifest.SourceArchiveSha256 -cne $script:SnowQtToolchain.sourceArchiveSha256) {
            return $false
        }
        foreach ($component in @("root", "qtbase", "qtsvg", "qttools", "qttranslations")) {
            if ($component -cnotin $manifest.Components) { return $false }
        }
    }
    catch { return $false }
    foreach ($patch in $script:SnowStaticQtSourcePatches) {
        $patchPath = Join-Path $licenseRoot "patches\$($patch.File)"
        if (-not (Test-Path -LiteralPath $patchPath -PathType Leaf) -or
            (Get-FileHash -LiteralPath $patchPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne
                $patch.SHA256) { return $false }
    }
    return $true
}

function Install-QtSourcePatches {
    param([Parameter(Mandatory = $true)][string]$Source)

    $versionPath = Join-Path $Source "qtbase/.cmake.conf"
    $expectedVersion = [regex]::Escape($script:SnowQtVersion)
    if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf) -or
        (Get-Content -LiteralPath $versionPath -Raw) -notmatch
            "(?m)^set\(QT_REPO_MODULE_VERSION `"$expectedVersion`"\)\r?`$") {
        throw "The audited Qt source patches require qtbase $($script:SnowQtVersion): $versionPath"
    }
    foreach ($patch in $script:SnowStaticQtSourcePatches) {
        $patchPath = Join-Path $PSScriptRoot $patch.File
        $previousGitCeiling = $env:GIT_CEILING_DIRECTORIES
        # A source tree inside this repository must not inherit its patch root.
        $env:GIT_CEILING_DIRECTORIES = [System.IO.Path]::GetDirectoryName(
            [System.IO.Path]::GetFullPath($Source)).Replace('\', '/')
        Push-Location $Source
        try {
            & git apply --reverse --check $patchPath 2>$null
            if ($LASTEXITCODE -eq 0) {
                Write-Output "Qt source patch already applied: $($patch.File)"
                continue
            }
            Invoke-Checked -Command "git" -Arguments @("apply", "--check", $patchPath) `
                -WorkingDirectory $Source
            Invoke-Checked -Command "git" -Arguments @("apply", $patchPath) `
                -WorkingDirectory $Source
        }
        finally {
            Pop-Location
            $env:GIT_CEILING_DIRECTORIES = $previousGitCeiling
        }
    }
}

function Install-QtLicenseBundle {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Prefix,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$SourceArchive
    )

    $licenseRoot = Join-Path $Prefix "share\snow-apps\qt-licenses"
    if (-not (Test-PathIsSameOrDescendant -Path $licenseRoot -Parent $Prefix)) {
        throw "The Qt license bundle destination escapes the install prefix: $licenseRoot"
    }
    $licenseSources = [ordered]@{
        root = $Source
        qtbase = Join-Path $Source "qtbase"
        qtsvg = Join-Path $Source "qtsvg"
        qttools = Join-Path $Source "qttools"
        qttranslations = Join-Path $Source "qttranslations"
    }
    foreach ($component in $licenseSources.Keys) {
        $componentSource = $licenseSources[$component]
        # Qt Translations retains its upstream license-rule metadata rather than REUSE.toml.
        $metadataName = if ($component -eq "qttranslations") { "licenseRule.json" } else { "REUSE.toml" }
        $metadataFile = Join-Path $componentSource $metadataName
        $licensesDirectory = Join-Path $componentSource "LICENSES"
        if (-not (Test-Path -LiteralPath $metadataFile -PathType Leaf) -or
            -not (Test-Path -LiteralPath $licensesDirectory -PathType Container)) {
            throw "Qt licensing metadata is incomplete for $component at $componentSource"
        }
    }
    # Validate every component before replacing a previously usable bundle.
    if (Test-Path -LiteralPath $licenseRoot) {
        Remove-Item -LiteralPath $licenseRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
    foreach ($component in $licenseSources.Keys) {
        $componentSource = $licenseSources[$component]
        $metadataName = if ($component -eq "qttranslations") { "licenseRule.json" } else { "REUSE.toml" }
        $componentDestination = Join-Path $licenseRoot $component
        New-Item -ItemType Directory -Path $componentDestination -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $componentSource $metadataName) -Destination $componentDestination
        Copy-Item -LiteralPath (Join-Path $componentSource "LICENSES") -Destination $componentDestination -Recurse
    }
    $patchesDirectory = Join-Path $licenseRoot "patches"
    New-Item -ItemType Directory -Path $patchesDirectory -Force | Out-Null
    foreach ($patch in $script:SnowStaticQtSourcePatches) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $patch.File) -Destination $patchesDirectory
    }

    [ordered]@{
        SchemaVersion = 1
        QtVersion = $Version
        SourceArchiveSha256 = $script:SnowQtToolchain.sourceArchiveSha256
        SourceArchive = $SourceArchive
        Components = @($licenseSources.Keys)
        SourcePatches = $script:SnowStaticQtSourcePatches
    } | ConvertTo-Json -Depth 3 | Set-Content `
        -LiteralPath (Join-Path $licenseRoot "manifest.json") -Encoding utf8
}

function Write-StaticQtBuildStamp {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$BuildConfiguration,
        [Parameter(Mandatory = $true)][string]$Fingerprint,
        [Parameter(Mandatory = $true)][string]$SourceArchive,
        [Parameter(Mandatory = $true)][int]$BuildParallelism,
        [bool]$WithDevelopmentModules = ($BuildConfiguration -eq "Debug"),
        [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
        [string]$HostArchitecture = (Get-SnowWindowsHostArchitecture),
        [string]$HostQtPrefix = ""
    )

    $stampDirectory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $stampDirectory | Out-Null
    if (-not $HostQtPrefix -and $Architecture -ceq $HostArchitecture) {
        $HostQtPrefix = Split-Path -Parent (Split-Path -Parent $stampDirectory)
    }
    [ordered]@{
        SchemaVersion = $script:SnowStaticQtSchemaVersion
        QtVersion = $Version
        SourceArchiveSha256 = $script:SnowQtToolchain.sourceArchiveSha256
        Configuration = $BuildConfiguration
        Architecture = $Architecture
        HostArchitecture = $HostArchitecture
        HostQtPrefix = $HostQtPrefix
        HostTools = if ($HostQtPrefix) {
            @(foreach ($name in @("moc", "rcc", "uic", "lrelease", "lupdate")) {
                [ordered]@{ Name = "$name.exe"; SHA256 = (Get-FileHash -Algorithm SHA256 `
                    -LiteralPath (Join-Path $HostQtPrefix "bin/$name.exe")).Hash.ToLowerInvariant() }
            })
        } else { @() }
        DevelopmentModules = $WithDevelopmentModules
        DependencyFingerprint = $Fingerprint
        FeatureFingerprint = $script:SnowStaticQtFeatureFingerprint
        SourcePatches = $script:SnowStaticQtSourcePatches
        Ltcg = ($BuildConfiguration -eq "Release")
        SystemPng = ($BuildConfiguration -eq "Release")
        SystemZlib = ($BuildConfiguration -eq "Release")
        Timezone = $true
        TimezoneLocale = $false
        LicenseBundle = "share/snow-apps/qt-licenses"
        SourceArchive = $SourceArchive
        Submodules = @("qtbase", "qtsvg", "qttools", "qttranslations")
        SkippedSubmodules = @(
            "qtactiveqt",
            "qtdeclarative",
            "qtimageformats",
            "qtlanguageserver",
            "qtshadertools"
        )
        DisabledFeatures = @(
            "androiddeployqt",
            "concurrent",
            "dbus",
            "dynamicgl",
            "opengl",
            "opengl_dynamic",
            "printsupport",
            "qdoc",
            "qmake",
            "sql",
            "testlib",
            "timezone_locale",
            "wasmdeployqt",
            "windeployqt"
        ) | Where-Object { $_ -notin @("testlib", "concurrent") -or -not $WithDevelopmentModules }
        Parallelism = $BuildParallelism
    } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Assert-CacheEntry {
    param(
        [Parameter(Mandatory = $true)][string]$Cache,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if ($Cache -notmatch $Pattern) {
        throw "Qt configuration does not satisfy $Description."
    }
}

if ([string]::IsNullOrWhiteSpace($DependencyPrefix)) {
    $releaseTarget = Get-SnowWindowsTarget -Architecture $Architecture
    $DependencyPrefix = Join-Path $releaseTarget.InstalledRoot $releaseTarget.Triplet
}
$dependencyPrefix = [System.IO.Path]::GetFullPath($DependencyPrefix)
foreach ($requiredDependencyFile in @(
        "include\zlib.h",
        "include\png.h",
        "share\zlib\zlib-config.cmake",
        "share\libpng\libpng-config.cmake")) {
    $requiredPath = Join-Path $dependencyPrefix $requiredDependencyFile
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Static Qt requires the release static vcpkg PNG/zlib packages: $requiredPath"
    }
}
$dependencyFingerprint = Get-DependencyFingerprint -Prefix $dependencyPrefix
Assert-QtDependencyArchitecture -Prefix $dependencyPrefix -Architecture $Architecture

$installPrefix = [System.IO.Path]::GetFullPath($InstallPrefix)
$qtConfig = Join-Path $installPrefix "lib\cmake\Qt6\Qt6Config.cmake"
$stampPath = Join-Path $installPrefix "share\snow-apps\static-qt-build.json"

$workRoot = if (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    [System.IO.Path]::GetFullPath($env:RUNNER_TEMP)
}
else {
    [System.IO.Path]::GetTempPath()
}
if ([string]::IsNullOrWhiteSpace($SourceDirectory)) {
    $SourceDirectory = Join-Path $workRoot "snow-qt-$QtVersion-$Architecture/qt-everywhere-src-$QtVersion"
}
$sourceDirectory = [System.IO.Path]::GetFullPath($SourceDirectory)
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $workRoot "qt-build-$QtVersion-$Architecture-static-system-codecs-no-timezone-locale-$($Configuration.ToLowerInvariant())"
}
$buildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)

Assert-QtDirectoryIsolation -Source $sourceDirectory -Build $buildDirectory `
    -Install $installPrefix -HostPrefix $HostQtPrefix
Assert-SafeRecursiveRemovalTarget -Path $installPrefix -Description "Qt install prefix"
Assert-SafeRecursiveRemovalTarget -Path $buildDirectory -Description "Qt build directory"

$refreshLicenseBundleOnly = $false
if (Test-Path -LiteralPath $qtConfig -PathType Leaf) {
    $stampMatches = $false
    $binaryStampMatches = $false
    if (Test-Path -LiteralPath $stampPath -PathType Leaf) {
        $stamp = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
        $binaryStampMatches = (Test-SnowStaticQtStamp -Stamp $stamp -Version $QtVersion `
            -Configuration $Configuration -Architecture $Architecture -HostArchitecture $hostArchitecture) -and
            $stamp.DependencyFingerprint -eq $dependencyFingerprint
        if ($binaryStampMatches) {
            $toolPrefix = if ($crossCompilation) { $HostQtPrefix } else { $installPrefix }
            $binaryStampMatches = Test-SnowQtHostToolHashes -Stamp $stamp -Prefix $toolPrefix
            if ($crossCompilation) { $binaryStampMatches = $binaryStampMatches -and $stamp.HostQtPrefix -ceq $HostQtPrefix }
        }
        $stampMatches = $binaryStampMatches
    }
    $systemCodecsMatch = Test-InstalledQtSystemCodecs -Prefix $installPrefix -Configuration $Configuration
    $systemCodecsMatch = $systemCodecsMatch -and
        (Get-SnowQtKitVersion -Qt6Dir (Join-Path $installPrefix "lib/cmake/Qt6")) -ceq $QtVersion -and
        (Test-SnowQtArchitecture -Qt6Dir (Join-Path $installPrefix "lib/cmake/Qt6") `
            -Architecture $Architecture -Configuration $Configuration)
    $developmentModulesMatch = -not $enableDevelopmentModules -or
        (Test-SnowQtDevelopmentModules -Qt6Dir (Join-Path $installPrefix "lib/cmake/Qt6"))
    $translationsMatch = Test-SnowQtTranslationKit -Qt6Dir (Join-Path $installPrefix "lib/cmake/Qt6")
    if ($stampMatches -and $systemCodecsMatch -and
        $developmentModulesMatch -and $translationsMatch -and
        (Test-InstalledQtLicenseBundle -Prefix $installPrefix)) {
        Write-Output "Validated static Qt $QtVersion ($Configuration) at $installPrefix"
        exit 0
    }
    if ($binaryStampMatches -and $systemCodecsMatch -and -not $Force) {
        $refreshLicenseBundleOnly = $developmentModulesMatch -and $translationsMatch
        if ($refreshLicenseBundleOnly) {
            Write-Output "Refreshing the audited Qt source-license bundle without rebuilding validated binaries."
        } else {
            Write-Output "Adding missing development modules or stock-dialog translations to the validated Qt kit."
        }
    }
    elseif (-not $Force) {
        throw "The Qt installation at $installPrefix does not match the audited $Configuration feature policy. Use a distinct prefix or pass -Force to replace it."
    }
    else {
        Remove-Item -LiteralPath $installPrefix -Recurse -Force
    }
}

if ($Force -and (Test-Path -LiteralPath $buildDirectory -PathType Container)) {
    Remove-Item -LiteralPath $buildDirectory -Recurse -Force
}

$configurationArgument = if ($Configuration -eq "Debug") { "-debug" } else { "-release" }
$ltcgArgument = if ($Configuration -eq "Debug") { "-no-ltcg" } else { "-ltcg" }
$zlibArgument = if ($Configuration -eq "Debug") { "-qt-zlib" } else { "-system-zlib" }
$pngArgument = if ($Configuration -eq "Debug") { "-qt-libpng" } else { "-system-libpng" }
$archivePath = Join-Path ([System.IO.Path]::GetDirectoryName($sourceDirectory)) "qt-everywhere-src-$QtVersion.tar.xz"
$sourceRelativePath = "qt/$($QtVersion.Substring(0, $QtVersion.LastIndexOf('.')))/$QtVersion/single/qt-everywhere-src-$QtVersion.tar.xz"
$sourceUrls = @(
    "$($QtMirrorBaseUrl.TrimEnd('/'))/$sourceRelativePath",
    "https://download.qt.io/official_releases/$sourceRelativePath"
) | Select-Object -Unique

if (-not (Test-Path -LiteralPath $sourceDirectory -PathType Container)) {
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        $downloaded = $false
        foreach ($sourceUrl in $sourceUrls) {
            try {
                Save-RemoteFile -Uri $sourceUrl -Destination $archivePath
                Assert-QtSourceArchive -Path $archivePath
                $downloaded = $true
                break
            }
            catch {
                Write-Warning "Qt source mirror failed ($sourceUrl): $($_.Exception.Message)"
                Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
            }
        }
        if (-not $downloaded) {
            throw "All Qt source mirrors failed."
        }
    }
    Assert-QtSourceArchive -Path $archivePath
    $sourceParent = Split-Path -Parent $sourceDirectory
    $extractedSourceDirectory = Join-Path $sourceParent "qt-everywhere-src-$QtVersion"
    New-Item -ItemType Directory -Force -Path $sourceParent | Out-Null
    if (-not (Test-Path -LiteralPath $extractedSourceDirectory -PathType Container)) {
        Invoke-Checked -Command "tar" -Arguments @("-xf", $archivePath, "-C", $sourceParent)
    }
    if ($sourceDirectory -ne $extractedSourceDirectory -and
        (Test-Path -LiteralPath $extractedSourceDirectory -PathType Container)) {
        Move-Item -LiteralPath $extractedSourceDirectory -Destination $sourceDirectory
    }
    if (-not (Test-Path -LiteralPath $sourceDirectory -PathType Container)) {
        throw "Qt source archive did not produce the expected directory: $sourceDirectory"
    }
}

Install-QtSourcePatches -Source $sourceDirectory

if ($refreshLicenseBundleOnly) {
    Install-QtLicenseBundle -Source $sourceDirectory -Prefix $installPrefix `
        -Version $QtVersion -SourceArchive $sourceUrls[-1]
    if (-not (Test-InstalledQtLicenseBundle -Prefix $installPrefix)) {
        throw "The refreshed Qt licensing bundle failed validation."
    }
    Write-StaticQtBuildStamp -Path $stampPath -Version $QtVersion `
        -BuildConfiguration $Configuration -Fingerprint $dependencyFingerprint `
        -SourceArchive $sourceUrls[-1] -BuildParallelism $Parallelism `
        -Architecture $Architecture -HostArchitecture $hostArchitecture -HostQtPrefix $HostQtPrefix `
        -WithDevelopmentModules (Test-SnowQtDevelopmentModules -Qt6Dir (Join-Path $installPrefix "lib/cmake/Qt6"))
    Write-Output "Validated static Qt $QtVersion ($Configuration) and refreshed its license provenance at $installPrefix"
    exit 0
}

New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
$configureArguments = @(
    "-static",
    $configurationArgument,
    "-static-runtime",
    $ltcgArgument,
    $zlibArgument,
    $pngArgument,
    "-no-opengl",
    "-no-feature-androiddeployqt",
    "-no-feature-wasmdeployqt",
    "-opensource",
    "-confirm-license",
    "-prefix", $installPrefix,
    "-submodules", "qtbase,qtsvg,qttools,qttranslations",
    "-skip", "qtactiveqt",
    "-skip", "qtdeclarative",
    "-skip", "qtimageformats",
    "-skip", "qtlanguageserver",
    "-skip", "qtshadertools",
    "-nomake", "tests",
    "-nomake", "examples",
    "--",
    "-UFEATURE_opengl*",
    "-UQT_FEATURE_opengl*",
    "-UFEATURE_dynamicgl",
    "-UQT_FEATURE_dynamicgl",
    "-DCMAKE_PREFIX_PATH=$dependencyPrefix",
    "-DZLIB_ROOT=$dependencyPrefix",
    "-DPNG_ROOT=$dependencyPrefix",
    "-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON",
    "-DFEATURE_timezone=ON",
    "-DFEATURE_timezone_locale=OFF",
    "-DQT_FEATURE_concurrent=$(if ($enableDevelopmentModules) { 'ON' } else { 'OFF' })",
    "-DQT_FEATURE_dbus=OFF",
    "-DQT_FEATURE_linguist=ON",
    "-DQT_FEATURE_printsupport=OFF",
    "-DQT_FEATURE_qdoc=OFF",
    "-DQT_FEATURE_qmake=OFF",
    "-DQT_FEATURE_sql=OFF",
    "-DQT_FEATURE_testlib=$(if ($enableDevelopmentModules) { 'ON' } else { 'OFF' })",
    "-DQT_FEATURE_windeployqt=OFF",
    "-DQT_FEATURE_assistant=OFF",
    "-DQT_FEATURE_designer=OFF",
    "-DQT_FEATURE_distancefieldgenerator=OFF",
    "-DQT_FEATURE_kmap2qmap=OFF",
    "-DQT_FEATURE_pixeltool=OFF",
    "-DQT_FEATURE_qdbus=OFF",
    "-DQT_FEATURE_qev=OFF",
    "-DQT_FEATURE_qtattributionsscanner=OFF",
    "-DQT_FEATURE_qtdiag=OFF",
    "-DQT_FEATURE_qtplugininfo=OFF"
)
if ($crossCompilation) {
    # configure's platform switches belong before the CMake argument delimiter.
    $delimiter = [Array]::IndexOf($configureArguments, "--")
    $targetSpec = if ($Architecture -eq "arm64") { "win32-arm64-msvc" } else { "win32-msvc" }
    $targetProcessor = if ($Architecture -eq "arm64") { "ARM64" } else { "AMD64" }
    $configureArguments = @($configureArguments[0..($delimiter - 1)]) +
        @("-xplatform", $targetSpec, "-qt-host-path", $HostQtPrefix) +
        @($configureArguments[$delimiter..($configureArguments.Count - 1)]) +
        @("-DCMAKE_SYSTEM_NAME=Windows", "-DCMAKE_SYSTEM_PROCESSOR=$targetProcessor",
            "-DQT_HOST_PATH:PATH=$HostQtPrefix")
}
Invoke-Checked -Command (Join-Path $sourceDirectory "configure.bat") `
    -Arguments $configureArguments -WorkingDirectory $buildDirectory

$cachePath = Join-Path $buildDirectory "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "Qt configure did not produce $cachePath."
}
$cache = Get-Content -LiteralPath $cachePath -Raw
$ltcgState = if ($Configuration -eq "Release") { "ON" } else { "OFF" }
Assert-CacheEntry -Cache $cache -Pattern "(?m)^FEATURE_ltcg:BOOL=$ltcgState\r?`$" -Description "$Configuration LTCG policy"
Assert-CacheEntry -Cache $cache -Pattern "(?m)^QT_FEATURE_ltcg:INTERNAL=$ltcgState\r?`$" -Description "the internal $Configuration LTCG policy"
Assert-CacheEntry -Cache $cache -Pattern "(?m)^FEATURE_system_png:BOOL=$ltcgState\r?`$" -Description "$Configuration libpng linkage"
Assert-CacheEntry -Cache $cache -Pattern "(?m)^QT_FEATURE_system_png:INTERNAL=$ltcgState\r?`$" -Description "the internal $Configuration libpng linkage"
Assert-CacheEntry -Cache $cache -Pattern "(?m)^FEATURE_system_zlib:BOOL=$ltcgState\r?`$" -Description "$Configuration zlib linkage"
Assert-CacheEntry -Cache $cache -Pattern "(?m)^QT_FEATURE_system_zlib:INTERNAL=$ltcgState\r?`$" -Description "the internal $Configuration zlib linkage"
Assert-CacheEntry -Cache $cache -Pattern '(?m)^FEATURE_timezone:BOOL=ON\r?$' -Description "time-zone handling"
Assert-CacheEntry -Cache $cache -Pattern '(?m)^QT_FEATURE_timezone:INTERNAL=ON\r?$' -Description "the internal time-zone feature"
Assert-CacheEntry -Cache $cache -Pattern '(?m)^FEATURE_timezone_locale:BOOL=OFF\r?$' -Description "disabled localized time-zone display names"
Assert-CacheEntry -Cache $cache `
    -Pattern '(?m)^QT_FEATURE_linguist:(?:INTERNAL|UNINITIALIZED)=ON\r?$' `
    -Description "the LinguistTools feature"
foreach ($disabledFeature in @(
        "androiddeployqt",
        "concurrent",
        "dbus",
        "dynamicgl",
        "opengl",
        "opengl_dynamic",
        "printsupport",
        "qdoc",
        "qmake",
        "sql",
        "testlib",
        "timezone_locale",
        "wasmdeployqt",
        "windeployqt")) {
    if ($disabledFeature -in @("testlib", "concurrent") -and $enableDevelopmentModules) {
        Assert-CacheEntry -Cache $cache `
            -Pattern "(?m)^QT_FEATURE_${disabledFeature}:(?:INTERNAL|UNINITIALIZED)=ON\r?`$" `
            -Description "the development $disabledFeature feature"
        continue
    }
    Assert-CacheEntry -Cache $cache `
        -Pattern "(?m)^QT_FEATURE_$([regex]::Escape($disabledFeature)):(?:INTERNAL|UNINITIALIZED)=OFF\r?`$" `
        -Description "the disabled $disabledFeature feature"
}
foreach ($skippedSubmodule in @(
        "qtactiveqt",
        "qtdeclarative",
        "qtimageformats",
        "qtlanguageserver",
        "qtshadertools")) {
    Assert-CacheEntry -Cache $cache `
        -Pattern "(?m)^BUILD_$([regex]::Escape($skippedSubmodule)):(?:BOOL|UNINITIALIZED)=OFF\r?`$" `
        -Description "the skipped $skippedSubmodule submodule"
}
$normalizedDependencyPrefix = $dependencyPrefix -replace '\\', '/'
if (($cache -replace '\\', '/') -notmatch [regex]::Escape($normalizedDependencyPrefix)) {
    throw "Qt did not resolve its dependencies from $dependencyPrefix."
}

$buildArguments = @(
    "--build", $buildDirectory,
    "--config", $Configuration,
    "--parallel", $Parallelism.ToString()
)
Invoke-Checked -Command "cmake" -Arguments $buildArguments
Invoke-Checked -Command "cmake" -Arguments @(
    "--install", $buildDirectory, "--config", $Configuration
)

if (-not (Test-Path -LiteralPath $qtConfig -PathType Leaf)) {
    throw "Qt $QtVersion installation did not produce $qtConfig"
}
if (-not (Test-SnowQtTranslationKit -Qt6Dir (Join-Path $installPrefix "lib/cmake/Qt6"))) {
    throw "The installed Qt kit does not provide Simplified and Traditional Chinese stock-dialog translations."
}
if (-not (Test-InstalledQtSystemCodecs -Prefix $installPrefix -Configuration $Configuration)) {
    throw "The installed Qt targets do not export the audited $Configuration codec/LTCG/timezone feature policy."
}
if (-not (Test-SnowQtArchitecture -Qt6Dir (Join-Path $installPrefix "lib/cmake/Qt6") `
        -Architecture $Architecture -Configuration $Configuration)) {
    throw "The installed Qt libraries do not match the requested $Architecture target."
}

Install-QtLicenseBundle -Source $sourceDirectory -Prefix $installPrefix `
    -Version $QtVersion -SourceArchive $sourceUrls[-1]
if (-not (Test-InstalledQtLicenseBundle -Prefix $installPrefix)) {
    throw "The installed Qt licensing bundle failed validation."
}

Write-StaticQtBuildStamp -Path $stampPath -Version $QtVersion `
    -BuildConfiguration $Configuration -Fingerprint $dependencyFingerprint `
    -SourceArchive $sourceUrls[-1] -BuildParallelism $Parallelism -WithDevelopmentModules $enableDevelopmentModules `
    -Architecture $Architecture -HostArchitecture $hostArchitecture -HostQtPrefix $HostQtPrefix

Write-Output "Static Qt $QtVersion ($Configuration) installed at $installPrefix"

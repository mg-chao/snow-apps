[CmdletBinding()]
param(
    [string]$QtVersion = "6.11.1",
    [Parameter(Mandatory = $true)][string]$InstallPrefix,
    [string]$SourceDirectory = "",
    [string]$BuildDirectory = "",
    [string]$DependencyPrefix = "",
    [ValidateSet("Debug", "Release")][string]$Configuration = "Release",
    [string]$QtMirrorBaseUrl = "https://qt.mirror.constant.com/official_releases",
    [ValidateRange(1, 256)][int]$Parallelism = 4,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
Add-SnowMsvcToolsToPath | Out-Null

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

function Test-InstalledQtSystemCodecs {
    param([Parameter(Mandatory = $true)][string]$Prefix)

    $coreTargets = Join-Path $Prefix "lib\cmake\Qt6Core\Qt6CoreTargets.cmake"
    $guiTargets = Join-Path $Prefix "lib\cmake\Qt6Gui\Qt6GuiTargets.cmake"
    if (-not (Test-Path -LiteralPath $coreTargets -PathType Leaf) -or
        -not (Test-Path -LiteralPath $guiTargets -PathType Leaf)) {
        return $false
    }
    $coreText = Get-Content -LiteralPath $coreTargets -Raw
    $guiText = Get-Content -LiteralPath $guiTargets -Raw
    return $coreText -match 'QT_ENABLED_PRIVATE_FEATURES "[^"]*system_zlib' -and
        $guiText -match 'QT_ENABLED_PRIVATE_FEATURES "[^"]*system_png'
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
            "qttools\REUSE.toml")) {
        if (-not (Test-Path -LiteralPath (Join-Path $licenseRoot $requiredLicenseFile) -PathType Leaf)) {
            return $false
        }
    }
    return $true
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
    if (Test-Path -LiteralPath $licenseRoot) {
        Remove-Item -LiteralPath $licenseRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null

    $licenseSources = [ordered]@{
        root = $Source
        qtbase = Join-Path $Source "qtbase"
        qtsvg = Join-Path $Source "qtsvg"
        qttools = Join-Path $Source "qttools"
    }
    foreach ($component in $licenseSources.Keys) {
        $componentSource = $licenseSources[$component]
        $reuseFile = Join-Path $componentSource "REUSE.toml"
        $licensesDirectory = Join-Path $componentSource "LICENSES"
        if (-not (Test-Path -LiteralPath $reuseFile -PathType Leaf) -or
            -not (Test-Path -LiteralPath $licensesDirectory -PathType Container)) {
            throw "Qt licensing metadata is incomplete for $component at $componentSource"
        }
        $componentDestination = Join-Path $licenseRoot $component
        New-Item -ItemType Directory -Path $componentDestination -Force | Out-Null
        Copy-Item -LiteralPath $reuseFile -Destination $componentDestination
        Copy-Item -LiteralPath $licensesDirectory -Destination $componentDestination -Recurse
    }

    [ordered]@{
        SchemaVersion = 1
        QtVersion = $Version
        SourceArchive = $SourceArchive
        Components = @($licenseSources.Keys)
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
        [Parameter(Mandatory = $true)][int]$BuildParallelism
    )

    $stampDirectory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $stampDirectory | Out-Null
    [ordered]@{
        SchemaVersion = 3
        QtVersion = $Version
        Configuration = $BuildConfiguration
        DependencyFingerprint = $Fingerprint
        Ltcg = $true
        SystemPng = $true
        SystemZlib = $true
        LicenseBundle = "share/snow-apps/qt-licenses"
        SourceArchive = $SourceArchive
        Submodules = @("qtbase", "qtsvg", "qttools")
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
            "wasmdeployqt",
            "windeployqt"
        )
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
    $DependencyPrefix = Join-Path $repoRoot ".tools\vcpkg\installed\static\x64-windows-static"
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
    $SourceDirectory = Join-Path $workRoot "qt-everywhere-src-$QtVersion"
}
$sourceDirectory = [System.IO.Path]::GetFullPath($SourceDirectory)
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $workRoot "qt-build-$QtVersion-static-system-codecs-$($Configuration.ToLowerInvariant())"
}
$buildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)

$qtDirectories = [ordered]@{
    Source = $sourceDirectory
    Build = $buildDirectory
    Install = $installPrefix
}
$qtDirectoryNames = @($qtDirectories.Keys)
for ($leftIndex = 0; $leftIndex -lt $qtDirectoryNames.Count; $leftIndex++) {
    for ($rightIndex = $leftIndex + 1; $rightIndex -lt $qtDirectoryNames.Count; $rightIndex++) {
        $leftName = $qtDirectoryNames[$leftIndex]
        $rightName = $qtDirectoryNames[$rightIndex]
        $leftPath = $qtDirectories[$leftName]
        $rightPath = $qtDirectories[$rightName]
        if ((Test-PathIsSameOrDescendant -Path $leftPath -Parent $rightPath) -or
            (Test-PathIsSameOrDescendant -Path $rightPath -Parent $leftPath)) {
            throw "Qt $leftName and $rightName directories must be distinct and non-overlapping: '$leftPath', '$rightPath'."
        }
    }
}
Assert-SafeRecursiveRemovalTarget -Path $installPrefix -Description "Qt install prefix"
Assert-SafeRecursiveRemovalTarget -Path $buildDirectory -Description "Qt build directory"

$refreshLicenseBundleOnly = $false
if (Test-Path -LiteralPath $qtConfig -PathType Leaf) {
    $stampMatches = $false
    $binaryStampMatches = $false
    if (Test-Path -LiteralPath $stampPath -PathType Leaf) {
        $stamp = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
        $binaryStampMatches = $stamp.SchemaVersion -in @(2, 3) -and
            $stamp.QtVersion -eq $QtVersion -and
            $stamp.Configuration -eq $Configuration -and
            $stamp.DependencyFingerprint -eq $dependencyFingerprint -and
            $stamp.Ltcg -eq $true -and
            $stamp.SystemPng -eq $true -and
            $stamp.SystemZlib -eq $true
        $stampMatches = $binaryStampMatches -and $stamp.SchemaVersion -eq 3
    }
    $systemCodecsMatch = Test-InstalledQtSystemCodecs -Prefix $installPrefix
    if ($stampMatches -and $systemCodecsMatch -and
        (Test-InstalledQtLicenseBundle -Prefix $installPrefix)) {
        Write-Output "Validated static Qt $QtVersion ($Configuration) at $installPrefix"
        exit 0
    }
    if ($binaryStampMatches -and $systemCodecsMatch -and -not $Force) {
        $refreshLicenseBundleOnly = $true
        Write-Output "Refreshing the audited Qt source-license bundle without rebuilding validated binaries."
    }
    elseif (-not $Force) {
        throw "The Qt installation at $installPrefix is not the validated system-codec/LTCG build. Use a distinct prefix or pass -Force to replace it."
    }
    else {
        Remove-Item -LiteralPath $installPrefix -Recurse -Force
    }
}

if ($Force -and (Test-Path -LiteralPath $buildDirectory -PathType Container)) {
    Remove-Item -LiteralPath $buildDirectory -Recurse -Force
}

$configurationArgument = if ($Configuration -eq "Debug") { "-debug" } else { "-release" }
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

if ($refreshLicenseBundleOnly) {
    Install-QtLicenseBundle -Source $sourceDirectory -Prefix $installPrefix `
        -Version $QtVersion -SourceArchive $sourceUrls[-1]
    if (-not (Test-InstalledQtLicenseBundle -Prefix $installPrefix)) {
        throw "The refreshed Qt licensing bundle failed validation."
    }
    Write-StaticQtBuildStamp -Path $stampPath -Version $QtVersion `
        -BuildConfiguration $Configuration -Fingerprint $dependencyFingerprint `
        -SourceArchive $sourceUrls[-1] -BuildParallelism $Parallelism
    Write-Output "Validated static Qt $QtVersion ($Configuration) and refreshed its license provenance at $installPrefix"
    exit 0
}

New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
$configureArguments = @(
    "-static",
    $configurationArgument,
    "-static-runtime",
    "-ltcg",
    "-system-zlib",
    "-system-libpng",
    "-no-opengl",
    "-no-feature-androiddeployqt",
    "-no-feature-wasmdeployqt",
    "-opensource",
    "-confirm-license",
    "-prefix", $installPrefix,
    "-submodules", "qtbase,qtsvg,qttools",
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
    "-DQT_FEATURE_concurrent=OFF",
    "-DQT_FEATURE_dbus=OFF",
    "-DQT_FEATURE_linguist=ON",
    "-DQT_FEATURE_printsupport=OFF",
    "-DQT_FEATURE_qdoc=OFF",
    "-DQT_FEATURE_qmake=OFF",
    "-DQT_FEATURE_sql=OFF",
    "-DQT_FEATURE_testlib=OFF",
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
Invoke-Checked -Command (Join-Path $sourceDirectory "configure.bat") `
    -Arguments $configureArguments -WorkingDirectory $buildDirectory

$cachePath = Join-Path $buildDirectory "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "Qt configure did not produce $cachePath."
}
$cache = Get-Content -LiteralPath $cachePath -Raw
Assert-CacheEntry -Cache $cache -Pattern '(?m)^FEATURE_ltcg:BOOL=ON\r?$' -Description "LTCG"
Assert-CacheEntry -Cache $cache -Pattern '(?m)^QT_FEATURE_ltcg:INTERNAL=ON\r?$' -Description "the internal LTCG feature"
Assert-CacheEntry -Cache $cache -Pattern '(?m)^FEATURE_system_png:BOOL=ON\r?$' -Description "system libpng"
Assert-CacheEntry -Cache $cache -Pattern '(?m)^QT_FEATURE_system_png:INTERNAL=ON\r?$' -Description "the internal system libpng feature"
Assert-CacheEntry -Cache $cache -Pattern '(?m)^FEATURE_system_zlib:BOOL=ON\r?$' -Description "system zlib"
Assert-CacheEntry -Cache $cache -Pattern '(?m)^QT_FEATURE_system_zlib:INTERNAL=ON\r?$' -Description "the internal system zlib feature"
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
        "wasmdeployqt",
        "windeployqt")) {
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
if (-not (Test-InstalledQtSystemCodecs -Prefix $installPrefix)) {
    throw "The installed Qt targets do not export system_zlib and system_png."
}

Install-QtLicenseBundle -Source $sourceDirectory -Prefix $installPrefix `
    -Version $QtVersion -SourceArchive $sourceUrls[-1]
if (-not (Test-InstalledQtLicenseBundle -Prefix $installPrefix)) {
    throw "The installed Qt licensing bundle failed validation."
}

Write-StaticQtBuildStamp -Path $stampPath -Version $QtVersion `
    -BuildConfiguration $Configuration -Fingerprint $dependencyFingerprint `
    -SourceArchive $sourceUrls[-1] -BuildParallelism $Parallelism

Write-Output "Static Qt $QtVersion ($Configuration) installed at $installPrefix"

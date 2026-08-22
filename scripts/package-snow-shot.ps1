[CmdletBinding()]
param(
    [string]$BuildDirectory = "build\snow-shot-msvc-release",
    [string]$InstallDirectory = "artifacts\snow-shot",
    [ValidateRange(1, 256)][int]$Parallelism = 4,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
$buildEnvironment = Set-SnowBuildEnvironment -Preset "snow-shot-msvc-release"
$script:DumpbinPath = Join-Path $env:VCToolsInstallDir "bin\Hostx64\x64\dumpbin.exe"
if (-not (Test-Path -LiteralPath $script:DumpbinPath -PathType Leaf)) {
    throw "The x64 PE inspection tool was not found: $script:DumpbinPath"
}
Write-Host "Visual Studio C++ tools: $env:VCToolsInstallDir"
Write-Host "MSVC toolset: $($buildEnvironment.MsvcToolset)"
Write-Host "PE dependency inspector: $script:DumpbinPath"

$nsisCommand = Get-Command makensis -ErrorAction SilentlyContinue
if (-not $nsisCommand) {
    $nsisCandidates = @(
        "${env:ProgramFiles(x86)}\NSIS\makensis.exe",
        "${env:ProgramFiles}\NSIS\makensis.exe"
    )
    $nsisPath = $nsisCandidates |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    if ($nsisPath) {
        $env:Path = "$(Split-Path -Parent $nsisPath);$env:Path"
        $nsisCommand = Get-Command makensis -ErrorAction SilentlyContinue
    }
}
if (-not $nsisCommand) {
    throw "NSIS compiler 'makensis' was not found. Install NSIS before packaging Snow Shot."
}

function Resolve-RepoPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Assert-NoPeExports {
    param([Parameter(Mandatory = $true)][string]$Path)

    $headerOutput = @(& $script:DumpbinPath /nologo /headers $Path 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "PE header inspection failed for $Path"
    }
    $exportDirectoryLines = @($headerOutput |
        Where-Object { $_ -match 'RVA \[size\] of Export Directory\s*$' })
    if ($exportDirectoryLines.Count -ne 1) {
        throw "PE header inspection found $($exportDirectoryLines.Count) export-directory entries for $Path; expected one."
    }
    if ($exportDirectoryLines[0] -notmatch '^\s+0+\s+\[\s*0+\]\s+RVA \[size\] of Export Directory\s*$') {
        throw "The application PE contains an export directory: $Path. $($exportDirectoryLines[0].Trim())"
    }

    $exportsOutput = @(& $script:DumpbinPath /nologo /exports $Path 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "PE export inspection failed for $Path"
    }
    if ($exportsOutput -match '^\s*Section contains the following exports' -or
        $exportsOutput -match '^\s+\d+ number of (?:functions|names)\s*$') {
        throw "The application PE exports symbols even though none are allowed: $Path"
    }
}

function Assert-ExactStringSet {
    param(
        [Parameter(Mandatory = $true)][string]$Description,
        [AllowEmptyCollection()][string[]]$Expected = @(),
        [AllowEmptyCollection()][string[]]$Actual = @()
    )

    $expectedSet = @($Expected | Sort-Object -Unique)
    $actualSet = @($Actual | Sort-Object -Unique)
    if (($expectedSet -join "`n") -cne ($actualSet -join "`n")) {
        $expectedText = if ($expectedSet.Count -gt 0) { $expectedSet -join ", " } else { "<none>" }
        $actualText = if ($actualSet.Count -gt 0) { $actualSet -join ", " } else { "<none>" }
        throw "$Description does not match the release contract. Expected: $expectedText. Actual: $actualText."
    }
}

function Get-ValidatedStaticQtStamp {
    param(
        [Parameter(Mandatory = $true)][string]$Prefix,
        [Parameter(Mandatory = $true)][string]$ExpectedVersion,
        [Parameter(Mandatory = $true)][string]$ExpectedConfiguration
    )

    $stampPath = Join-Path $Prefix "share\snow-apps\static-qt-build.json"
    if (-not (Test-Path -LiteralPath $stampPath -PathType Leaf)) {
        throw "The audited static Qt build stamp was not found: $stampPath"
    }
    try {
        $stamp = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
    }
    catch {
        throw "The static Qt build stamp is invalid JSON: $stampPath. $($_.Exception.Message)"
    }

    $expectedValues = [ordered]@{
        SchemaVersion = 3
        QtVersion = $ExpectedVersion
        Configuration = $ExpectedConfiguration
    }
    foreach ($property in $expectedValues.Keys) {
        if ($stamp.PSObject.Properties.Name -notcontains $property -or
            $stamp.$property -ne $expectedValues[$property]) {
            throw "Static Qt build stamp '$property' is '$($stamp.$property)'; expected '$($expectedValues[$property])'."
        }
    }
    foreach ($property in @("Ltcg", "SystemPng", "SystemZlib")) {
        if ($stamp.PSObject.Properties.Name -notcontains $property -or
            $stamp.$property -isnot [bool] -or
            $stamp.$property -ne $true) {
            throw "Static Qt build stamp '$property' must be the JSON boolean true."
        }
    }

    return $stamp
}

function Assert-SnowShotStaticDependencies {
    param(
        [Parameter(Mandatory = $true)][string]$InstalledRoot,
        [Parameter(Mandatory = $true)][string]$Prefix
    )

    if (-not (Test-Path -LiteralPath $Prefix -PathType Container)) {
        throw "The Snow Shot static vcpkg prefix was not found: $Prefix"
    }

    $ffmpegComponentsPath = Join-Path $Prefix "share\ffmpeg\snow-shot-config-components.h"
    if (-not (Test-Path -LiteralPath $ffmpegComponentsPath -PathType Leaf)) {
        throw "The audited Snow Shot FFmpeg component header was not found: $ffmpegComponentsPath"
    }
    $componentPattern = '^#define CONFIG_(?<Name>[A-Z0-9_]+?)_(?<Kind>DEMUXER|DECODER|ENCODER|HWACCEL|PARSER|MUXER|BSF|PROTOCOL|FILTER|INDEV|OUTDEV) 1$'
    $enabledFfmpegComponents = @(Get-Content -LiteralPath $ffmpegComponentsPath | ForEach-Object {
        if ($_ -match $componentPattern) {
            [pscustomobject]@{
                Kind = $Matches.Kind
                Name = $Matches.Name
            }
        }
    })
    $expectedFfmpegComponents = [ordered]@{
        BSF = @("AAC_ADTSTOASC", "H264_MP4TOANNEXB", "PGS_FRAME_MERGE", "VP9_SUPERFRAME")
        DECODER = @("H264")
        ENCODER = @("AAC", "APNG", "GIF", "H263", "H264_MF", "LIBWEBP_ANIM", "LIBX264", "LIBX265", "MP3_MF", "MPEG4")
        HWACCEL = @("H264_D3D11VA", "H264_D3D11VA2", "H264_DXVA2")
        PARSER = @("AAC", "AC3", "H264", "MPEGAUDIO")
        DEMUXER = @("MATROSKA")
        MUXER = @("APNG", "AVI", "GIF", "MATROSKA", "MOV", "MP4", "WEBP")
        PROTOCOL = @("FILE")
        FILTER = @()
        INDEV = @()
        OUTDEV = @()
    }
    foreach ($entry in $expectedFfmpegComponents.GetEnumerator()) {
        $actual = @($enabledFfmpegComponents |
            Where-Object { $_.Kind -ceq $entry.Key } |
            ForEach-Object { $_.Name })
        Assert-ExactStringSet -Description "Enabled FFmpeg $($entry.Key) components" `
            -Expected $entry.Value -Actual $actual
    }

    $libraryDirectory = Join-Path $Prefix "lib"
    $opencvLibraries = @(Get-ChildItem -LiteralPath $libraryDirectory -File |
        Where-Object { $_.Name -match '(?i)opencv_.+\.(?:a|lib)$' })
    foreach ($component in @("core", "imgproc", "objdetect")) {
        if (-not ($opencvLibraries.Name -match "(?i)opencv_$component\d*\.(?:a|lib)$")) {
            throw "The Snow Shot static prefix is missing the OpenCV $component library."
        }
    }
    $forbiddenOpenCvLibraries = @($opencvLibraries |
        Where-Object { $_.Name -match '(?i)opencv_(?:dnn|wechat_qrcode)\d*\.(?:a|lib)$' })
    if ($forbiddenOpenCvLibraries.Count -gt 0) {
        throw "The Snow Shot static prefix contains forbidden OpenCV modules: $($forbiddenOpenCvLibraries.Name -join ', ')"
    }

    $opencvTargetFiles = @(Get-ChildItem -LiteralPath (Join-Path $Prefix "share\opencv4") `
        -Recurse -File -Filter "OpenCVModules.cmake")
    if ($opencvTargetFiles.Count -ne 1) {
        throw "Expected one installed OpenCV target export, found $($opencvTargetFiles.Count)."
    }
    if ((Get-Content -LiteralPath $opencvTargetFiles[0].FullName -Raw) -match '(?i)pthread') {
        throw "The MSVC OpenCV target export contains a forbidden pthread dependency."
    }

    $libheifConfig = Join-Path $Prefix "share\libheif\libheif-config.cmake"
    if (-not (Test-Path -LiteralPath $libheifConfig -PathType Leaf) -or
        (Get-Content -LiteralPath $libheifConfig -Raw) -notmatch
            '(?m)^find_dependency\(AOM CONFIG\)\r?$') {
        throw "The libheif target export does not declare its AOM dependency."
    }

    $libde265Artifacts = [System.Collections.Generic.List[string]]::new()
    foreach ($path in @(
        (Join-Path $Prefix "include\libde265"),
        (Join-Path $Prefix "share\libde265")
    )) {
        if (Test-Path -LiteralPath $path) {
            $libde265Artifacts.Add($path)
        }
    }
    Get-ChildItem -LiteralPath $libraryDirectory -File -Filter "*de265*" -ErrorAction SilentlyContinue |
        ForEach-Object { $libde265Artifacts.Add($_.FullName) }
    Get-ChildItem -LiteralPath (Join-Path $InstalledRoot "vcpkg\info") -File `
        -Filter "libde265_*" -ErrorAction SilentlyContinue |
        ForEach-Object { $libde265Artifacts.Add($_.FullName) }
    if ($libde265Artifacts.Count -gt 0) {
        throw "The Snow Shot static prefix contains forbidden libde265 artifacts: $($libde265Artifacts -join ', ')"
    }

    $debugDirectory = Join-Path $Prefix "debug"
    $debugDependencyArtifacts = @(if (Test-Path -LiteralPath $debugDirectory -PathType Container) {
        Get-ChildItem -LiteralPath $debugDirectory -Recurse -File
    })
    if ($debugDependencyArtifacts.Count -gt 0) {
        throw "The Release-only static prefix contains Debug artifacts: $($debugDependencyArtifacts.FullName -join ', ')"
    }

    Write-Output "Static dependency audit: $($enabledFfmpegComponents.Count) FFmpeg components and $($opencvLibraries.Count) OpenCV libraries checked"
}

$buildDirectory = Resolve-RepoPath $BuildDirectory
$installDirectory = Resolve-RepoPath $InstallDirectory
$artifactRoot = Resolve-RepoPath "artifacts"
$artifactPrefix = $artifactRoot + [System.IO.Path]::DirectorySeparatorChar
if (-not $installDirectory.StartsWith($artifactPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "InstallDirectory must be a child of $artifactRoot"
}

$staticVcpkgInstalledRoot = Join-Path $buildEnvironment.VcpkgRoot "installed\static"
$staticVcpkgPrefix = Join-Path $staticVcpkgInstalledRoot "x64-windows-static"
Assert-SnowShotStaticDependencies -InstalledRoot $staticVcpkgInstalledRoot -Prefix $staticVcpkgPrefix
$qtPrefix = [System.IO.Path]::GetFullPath((Join-Path $buildEnvironment.Qt6Dir "..\..\.."))
$qtStamp = Get-ValidatedStaticQtStamp -Prefix $qtPrefix `
    -ExpectedVersion "6.11.1" -ExpectedConfiguration "Release"

$cachePath = Join-Path $buildDirectory "CMakeCache.txt"
if (-not $SkipBuild) {
    & cmake --fresh --preset snow-shot-msvc-release
    if ($LASTEXITCODE -ne 0) {
        throw "Snow Shot release configuration failed."
    }
}
elseif (-not (Test-Path -LiteralPath $cachePath)) {
    throw "CMake cache was not found: $cachePath"
}

$requiredCacheEntries = @(
    "SNOW_APPS_BUILD_TESTS:BOOL=OFF",
    "SNOW_APPS_BUILD_BENCHMARKS:BOOL=OFF",
    "SNOW_APPS_RELEASE_STATIC:BOOL=ON",
    "SNOW_APPS_QT_STATIC:BOOL=ON",
    "SNOW_APPS_PACKAGE_SNOW_SHOT:BOOL=ON",
    "SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC:INTERNAL=ON",
    "QT_FEATURE_static:INTERNAL=ON"
)
$cache = Get-Content -LiteralPath $cachePath
foreach ($entry in $requiredCacheEntries) {
    if ($cache -notcontains $entry) {
        throw "Release cache is not production-safe; missing '$entry'."
    }
}

if (-not $SkipBuild) {
    & cmake --build --preset build-snow-shot-msvc-release --parallel $Parallelism
    if ($LASTEXITCODE -ne 0) {
        throw "Snow Shot release build failed."
    }
}

$thirdPartyLicenseCollector = Join-Path $PSScriptRoot "collect-third-party-licenses.ps1"
$thirdPartyLicenseDirectory = Join-Path $buildDirectory "snow_shot\third-party-licenses\third-party"
& $thirdPartyLicenseCollector `
    -Destination $thirdPartyLicenseDirectory `
    -AllowedRoot $buildDirectory `
    -VcpkgPrefix $staticVcpkgPrefix `
    -QtPrefix $qtPrefix `
    -CargoManifest (Join-Path $repoRoot "snow_rust_ffi\Cargo.toml") `
    -AntDesignNotice (Join-Path $repoRoot "ant_design_qt\THIRD_PARTY_NOTICES.md") `
    -FallbackLicenseDirectory (Join-Path $repoRoot "licenses")
if ($LASTEXITCODE -ne 0) {
    throw "Snow Shot third-party license collection failed."
}

if (Test-Path -LiteralPath $installDirectory) {
    Remove-Item -LiteralPath $installDirectory -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $installDirectory | Out-Null

& cmake --install $buildDirectory --config Release --prefix $installDirectory
if ($LASTEXITCODE -ne 0) {
    throw "Snow Shot install step failed."
}

$mainExecutable = Join-Path $installDirectory "bin\snow_shot.exe"
if (-not (Test-Path -LiteralPath $mainExecutable)) {
    throw "The staged application was not found: $mainExecutable"
}

$versionInfo = (Get-Item -LiteralPath $mainExecutable).VersionInfo
$expectedBinaryMetadata = @{
    CompanyName = "Snow Apps"
    FileDescription = "Snow Shot screenshot utility"
    InternalName = "snow_shot"
    LegalCopyright = "Copyright (C) 2025-2026 mg-chao"
    OriginalFilename = "snow_shot.exe"
    ProductName = "Snow Shot"
}
foreach ($property in $expectedBinaryMetadata.Keys) {
    if ($versionInfo.$property -ne $expectedBinaryMetadata[$property]) {
        throw "Snow Shot binary metadata '$property' is '$($versionInfo.$property)'; expected '$($expectedBinaryMetadata[$property])'."
    }
}

$requiredStageFiles = @(
    "bin\snow_shot.exe",
    "bin\DirectML.dll",
    "share\snow-shot\licenses\LICENSE",
    "share\snow-shot\licenses\COPYRIGHT",
    "share\snow-shot\licenses\THIRD_PARTY_NOTICES.md",
    "share\snow-shot\licenses\components\ant-design-qt\COPYRIGHT",
    "share\snow-shot\licenses\components\ant-design-qt\LICENSE",
    "share\snow-shot\licenses\components\snow-crates\COPYRIGHT",
    "share\snow-shot\licenses\components\snow-crates\LICENSE",
    "share\snow-shot\licenses\components\snow-image\COPYRIGHT",
    "share\snow-shot\licenses\components\snow-image\LICENSE",
    "share\snow-shot\licenses\components\snow-draw-engine-qt\COPYRIGHT",
    "share\snow-shot\licenses\components\snow-draw-engine-qt\LICENSE",
    "share\snow-shot\licenses\components\snow-rust-ffi\COPYRIGHT",
    "share\snow-shot\licenses\components\snow-rust-ffi\LICENSE",
    "share\snow-shot\licenses\third-party\INDEX.md",
    "share\snow-shot\licenses\third-party\manifest.json"
)
$missingStageFiles = @($requiredStageFiles | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path $installDirectory $_) -PathType Leaf)
})
if ($missingStageFiles.Count -gt 0) {
    throw "Release staging is missing required runtime files: $($missingStageFiles -join ', ')"
}

$forbiddenRuntimeFiles = @(Get-ChildItem -LiteralPath $installDirectory -Recurse -File |
    Where-Object {
        $_.Name -match '(?i)^(?:dxcompiler|dxil|msvcp\d+(?:_\d+)?|vcruntime\d+(?:_\d+)?|concrt\d+)\.dll$'
    })
if ($forbiddenRuntimeFiles.Count -gt 0) {
    throw "Static release staging contains unused bundled runtimes: $($forbiddenRuntimeFiles.FullName -join ', ')"
}

$stagedQtDlls = @(Get-ChildItem -LiteralPath $installDirectory -Recurse -File -Filter "Qt6*.dll")
if ($stagedQtDlls.Count -gt 0) {
    throw "Static Qt release staging contains Qt DLLs: $($stagedQtDlls.FullName -join ', ')"
}
$stagedQtPluginDirectory = Join-Path $installDirectory "plugins"
if (Test-Path -LiteralPath $stagedQtPluginDirectory -PathType Container) {
    throw "Static Qt release staging contains a Qt plugin directory: $stagedQtPluginDirectory"
}

$stagedExecutables = @(Get-ChildItem -LiteralPath $installDirectory -Recurse -File -Filter "*.exe")
$unexpectedExecutables = @($stagedExecutables | Where-Object { $_.Name -ne "snow_shot.exe" })
if ($unexpectedExecutables.Count -gt 0) {
    throw "Release staging contains unexpected executables: $($unexpectedExecutables.FullName -join ', ')"
}

$testArtifacts = @(Get-ChildItem -LiteralPath $installDirectory -Recurse -File |
    Where-Object { $_.Name -match "(?i)(test|benchmark)" })
if ($testArtifacts.Count -gt 0) {
    throw "Release staging contains test or benchmark artifacts: $($testArtifacts.FullName -join ', ')"
}

$debugArtifacts = @(Get-ChildItem -LiteralPath $installDirectory -Recurse -File |
    Where-Object { $_.Extension.ToLowerInvariant() -in @(".pdb", ".ilk", ".iobj", ".ipdb") })
if ($debugArtifacts.Count -gt 0) {
    throw "Release staging contains debug artifacts: $($debugArtifacts.FullName -join ', ')"
}

$stagedBinaries = @(Get-ChildItem -LiteralPath $installDirectory -Recurse -File |
    Where-Object { $_.Extension.ToLowerInvariant() -in @(".dll", ".exe") })
$expectedBinaryPaths = @(
    "bin\snow_shot.exe",
    "bin\DirectML.dll"
)
$unexpectedBinaries = @($stagedBinaries | Where-Object {
    $relativePath = [System.IO.Path]::GetRelativePath($installDirectory, $_.FullName)
    $relativePath -notin $expectedBinaryPaths
})
if ($unexpectedBinaries.Count -gt 0) {
    throw "Release staging contains unexpected binary files: $($unexpectedBinaries.FullName -join ', ')"
}
$applicationPath = Join-Path $installDirectory "bin\snow_shot.exe"
Assert-NoPeExports -Path $applicationPath
Write-Output "PE export audit: snow_shot.exe has no export directory or exported symbols"
$stagedBinDirectory = Join-Path $installDirectory "bin"
$windowsSystemDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
$debugRuntimeImports = [System.Collections.Generic.List[string]]::new()
$unresolvedImports = [System.Collections.Generic.List[string]]::new()
$unexpectedImports = [System.Collections.Generic.List[string]]::new()
$allowedSystemImports = @(
    "advapi32.dll",
    "authz.dll",
    "bcrypt.dll",
    "bcryptprimitives.dll",
    "cfgmgr32.dll",
    "combase.dll",
    "comctl32.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "cryptbase.dll",
    "d3d9.dll",
    "d3d11.dll",
    "d3d12.dll",
    "dbghelp.dll",
    "dnsapi.dll",
    "dwrite.dll",
    "dwmapi.dll",
    "dxgi.dll",
    "gdi32.dll",
    "icu.dll",
    "imm32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "mswsock.dll",
    "ncrypt.dll",
    "netapi32.dll",
    "netutils.dll",
    "normaliz.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleacc.dll",
    "oleaut32.dll",
    "powrprof.dll",
    "propsys.dll",
    "rpcrt4.dll",
    "runtimeobject.dll",
    "sechost.dll",
    "secur32.dll",
    "setupapi.dll",
    "shell32.dll",
    "shcore.dll",
    "shlwapi.dll",
    "srvcli.dll",
    "sspicli.dll",
    "uiautomationcore.dll",
    "user32.dll",
    "userenv.dll",
    "uxtheme.dll",
    "version.dll",
    "windowscodecs.dll",
    "winhttp.dll",
    "winmm.dll",
    "ws2_32.dll",
    "wtsapi32.dll"
)
$allowedLocalImports = @{
    "snow_shot.exe" = @("directml.dll")
    "directml.dll" = @()
}
foreach ($binary in $stagedBinaries) {
    $dependencyOutput = @(& $script:DumpbinPath /nologo /dependents $binary.FullName 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "PE dependency inspection failed for $($binary.FullName)"
    }

    foreach ($line in $dependencyOutput) {
        if ($line -notmatch '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
            continue
        }
        $dependencyName = $Matches[1]
        if ($dependencyName -match '(?i)^(?:Qt6.+d|(?:msvcp|vcruntime|concrt)\d+(?:(?:_\d+)?d(?:_.*)?|_threadsd)|ucrtbased)\.dll$') {
            $debugRuntimeImports.Add("$($binary.Name) -> $dependencyName")
        }

        if ($dependencyName -match '(?i)^(?:api|ext)-ms-') {
            continue
        }
        $binaryName = $binary.Name.ToLowerInvariant()
        $dependencyKey = $dependencyName.ToLowerInvariant()
        if ($dependencyKey -in $allowedLocalImports[$binaryName]) {
            $localDependency = Join-Path $stagedBinDirectory $dependencyName
            if (-not (Test-Path -LiteralPath $localDependency -PathType Leaf)) {
                $unresolvedImports.Add("$($binary.Name) -> $dependencyName")
            }
            continue
        }
        if ($dependencyKey -notin $allowedSystemImports) {
            $unexpectedImports.Add("$($binary.Name) -> $dependencyName")
            continue
        }
        if (-not (Test-Path -LiteralPath (Join-Path $windowsSystemDirectory $dependencyName) -PathType Leaf)) {
            $unresolvedImports.Add("$($binary.Name) -> $dependencyName")
        }
    }
}
if ($debugRuntimeImports.Count -gt 0) {
    throw "Release staging imports debug runtime libraries: $($debugRuntimeImports -join ', ')"
}
if ($unresolvedImports.Count -gt 0) {
    throw "Release staging has unresolved PE dependencies: $($unresolvedImports -join ', ')"
}
if ($unexpectedImports.Count -gt 0) {
    throw "Release staging imports non-system or disallowed libraries: $($unexpectedImports -join ', ')"
}
Write-Output "PE dependency audit: $($stagedBinaries.Count) binaries checked"

$linkMapPath = Join-Path $buildDirectory "snow_shot\Release\snow_shot.map"
if (-not (Test-Path -LiteralPath $linkMapPath -PathType Leaf)) {
    throw "The Snow Shot Release link map was not found: $linkMapPath"
}
$ffmpegRegistrationPattern =
    '^\s+[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+(?<Name>ff_[A-Za-z0-9_]+_(?:bsf|decoder|encoder|hwaccel|parser|demuxer|muxer|protocol))\s+[0-9A-Fa-f]+\s{2,}\S'
$linkedFfmpegRegistrations = @(Select-String -LiteralPath $linkMapPath `
    -Pattern $ffmpegRegistrationPattern | ForEach-Object {
        $_.Matches[0].Groups["Name"].Value
    })
$expectedFfmpegRegistrations = @(
    "ff_aac_adtstoasc_bsf",
    "ff_h264_mp4toannexb_bsf",
    "ff_pgs_frame_merge_bsf",
    "ff_vp9_superframe_bsf",
    "ff_h264_decoder",
    "ff_aac_encoder",
    "ff_apng_encoder",
    "ff_gif_encoder",
    "ff_h263_encoder",
    "ff_h264_mf_encoder",
    "ff_libwebp_anim_encoder",
    "ff_libx264_encoder",
    "ff_libx265_encoder",
    "ff_mp3_mf_encoder",
    "ff_mpeg4_encoder",
    "ff_h264_d3d11va_hwaccel",
    "ff_h264_d3d11va2_hwaccel",
    "ff_h264_dxva2_hwaccel",
    "ff_aac_parser",
    "ff_ac3_parser",
    "ff_h264_parser",
    "ff_mpegaudio_parser",
    "ff_matroska_demuxer",
    "ff_apng_muxer",
    "ff_avi_muxer",
    "ff_gif_muxer",
    "ff_matroska_muxer",
    "ff_mov_muxer",
    "ff_mp4_muxer",
    "ff_webp_muxer",
    "ff_file_protocol"
)
Assert-ExactStringSet -Description "Linked FFmpeg component registrations" `
    -Expected $expectedFfmpegRegistrations -Actual $linkedFfmpegRegistrations

$forbiddenFfmpegProviderSymbols = @(
    "ff_print_debug_info2",
    "ff_mjpeg_add_icc_profile_size",
    "ff_mjpeg_encode_picture_trailer",
    "ff_mjpeg_encode_stuffing",
    "ff_speedhq_end_slice",
    "ff_speedhq_mb_y_order_to_mb",
    "ff_h261_reorder_mb_index",
    "ff_mpeg1_encode_slice_header",
    "ff_mpeg1_clean_buffers",
    "ff_h261_loop_filter",
    "ff_mpeg4_mcsel_motion"
)
$linkedForbiddenFfmpegSymbols = @($forbiddenFfmpegProviderSymbols | Where-Object {
    Select-String -LiteralPath $linkMapPath -Quiet `
        -Pattern "^\s+[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+$([regex]::Escape($_))\s"
})
if ($linkedForbiddenFfmpegSymbols.Count -gt 0) {
    throw "Release link map contains disabled FFmpeg provider symbols: $($linkedForbiddenFfmpegSymbols -join ', ')"
}
Write-Output "FFmpeg link-map audit: $($linkedFfmpegRegistrations.Count) registrations and no disabled providers"

$cpackConfig = Join-Path $buildDirectory "CPackConfig.cmake"
if (-not (Test-Path -LiteralPath $cpackConfig)) {
    throw "CPack configuration was not generated: $cpackConfig"
}

$cpackConfiguration = Get-Content -LiteralPath $cpackConfig -Raw
$requiredCpackSettings = @{
    CPACK_CREATE_DESKTOP_LINKS = "snow_shot"
    CPACK_PACKAGE_EXECUTABLES = "snow_shot;Snow Shot"
    CPACK_PACKAGE_HOMEPAGE_URL = "https://snowshot.top"
    CPACK_PACKAGE_INSTALL_DIRECTORY = "SnowShot"
    CPACK_PACKAGE_INSTALL_REGISTRY_KEY = "SnowShot"
    CPACK_NSIS_INSTALLED_ICON_NAME = "bin\\snow_shot.exe"
}
foreach ($setting in $requiredCpackSettings.Keys) {
    $escapedSetting = [regex]::Escape($setting)
    $escapedValue = [regex]::Escape($requiredCpackSettings[$setting])
    $settingPresent = if ($setting -eq "CPACK_NSIS_INSTALLED_ICON_NAME") {
        $cpackConfiguration -match 'set\(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\+snow_shot\.exe"\)'
    }
    else {
        $cpackConfiguration -match "set\($escapedSetting `"$escapedValue`"\)"
    }
    if (-not $settingPresent) {
        throw "CPack configuration is missing '$setting=$($requiredCpackSettings[$setting])'."
    }
}
if ($cpackConfiguration -notmatch 'set\(CPACK_PACKAGE_VERSION "([^"]+)"\)') {
    throw "CPack configuration does not declare the Snow Shot package version."
}
$packageVersion = $Matches[1]
if ($versionInfo.FileVersion -ne "$packageVersion.0" -or
    $versionInfo.ProductVersion -ne $packageVersion) {
    throw "Snow Shot binary version '$($versionInfo.FileVersion)'/'$($versionInfo.ProductVersion)' does not match package version '$packageVersion'."
}

Push-Location $buildDirectory
try {
    & cpack --config $cpackConfig -G NSIS -C Release
}
finally {
    Pop-Location
}
if ($LASTEXITCODE -ne 0) {
    throw "NSIS packaging failed."
}

$packages = @(Get-ChildItem -LiteralPath $buildDirectory -File -Filter "snow-shot-*.exe" |
    Sort-Object LastWriteTime)
if ($packages.Count -eq 0) {
    throw "NSIS did not produce a Snow Shot installer under $buildDirectory"
}

$installerVersionInfo = $packages[-1].VersionInfo
$expectedInstallerMetadata = @{
    CompanyName = "Snow Apps"
    FileDescription = "Snow Shot installer"
    FileVersion = "$packageVersion.0"
    InternalName = "snow-shot-installer"
    LegalCopyright = "Copyright (C) 2025-2026 mg-chao"
    OriginalFilename = "snow-shot-$packageVersion-windows-x64.exe"
    ProductName = "Snow Shot"
    ProductVersion = $packageVersion
}
foreach ($property in $expectedInstallerMetadata.Keys) {
    if ($installerVersionInfo.$property -ne $expectedInstallerMetadata[$property]) {
        throw "Snow Shot installer metadata '$property' is '$($installerVersionInfo.$property)'; expected '$($expectedInstallerMetadata[$property])'."
    }
}

$checksumPath = "$($packages[-1].FullName).sha256"
if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf)) {
    throw "CPack did not produce the installer checksum: $checksumPath"
}

$installerHash = (Get-FileHash -LiteralPath $packages[-1].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumText = Get-Content -LiteralPath $checksumPath -Raw
if ($checksumText -notmatch [regex]::Escape($installerHash)) {
    throw "The generated installer checksum does not match $($packages[-1].Name)."
}
$installedFiles = @(Get-ChildItem -LiteralPath $installDirectory -Recurse -File | Sort-Object FullName)
$installedFileManifest = @($installedFiles | ForEach-Object {
    [ordered]@{
        Path = [System.IO.Path]::GetRelativePath($installDirectory, $_.FullName).Replace('\', '/')
        Bytes = $_.Length
        Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$manifestPath = Join-Path $buildDirectory "snow-shot-$packageVersion-windows-x64.manifest.json"
[ordered]@{
    SchemaVersion = 1
    PackageVersion = $packageVersion
    Preset = "snow-shot-msvc-release"
    Architecture = "x64"
    StaticCrt = $true
    StaticQt = $true
    StaticImageCodecBackend = $true
    Qt = $qtStamp
    InstallTreeBytes = [long](($installedFiles | Measure-Object -Property Length -Sum).Sum)
    InstallFiles = $installedFileManifest
    Installer = [ordered]@{
        Path = $packages[-1].Name
        Bytes = $packages[-1].Length
        Sha256 = $installerHash
    }
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8

Write-Output "Snow Shot install tree: $installDirectory"
Write-Output "Snow Shot installer: $($packages[-1].FullName)"
Write-Output "Snow Shot installer checksum: $checksumPath"
Write-Output "Snow Shot release manifest: $manifestPath"

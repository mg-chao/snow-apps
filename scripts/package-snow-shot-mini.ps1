# Invoked by package-snow-shot.ps1 after auditing the shared static toolchain.
param([switch]$FunctionsOnly)

function Assert-SnowShotMiniPayload([string]$Stage,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64', [string]$OcrAssetManifest) {
    $files = @(Get-ChildItem -LiteralPath $Stage -Recurse -File -Force)
    $binaries = @($files | Where-Object { $_.Extension -in @('.exe', '.dll') } |
        ForEach-Object { [IO.Path]::GetRelativePath($Stage, $_.FullName).Replace('\', '/') })
    $expected = @('bin/snow_shot_mini.exe', 'bin/snow-shot-mini-updater.exe',
        'bin/snow-shot-mini-mcp.exe', 'bin/crashpad_handler.exe')
    if ((($binaries | Sort-Object) -join "`n") -cne (($expected | Sort-Object) -join "`n")) {
        throw 'Mini must contain exactly its application, updater, MCP bridge, and crash collector.'
    }
    $allowedBinFiles = $expected + @('bin/assets/ocr/asset-manifest.json',
        'bin/audios/camera_shutter.mp3', 'bin/__mini_data_directory')
    foreach ($file in $files) {
        $relative = [IO.Path]::GetRelativePath($Stage, $file.FullName).Replace('\', '/')
        if ($relative.StartsWith('bin/assets/qrcode/') -or
            ($relative.StartsWith('bin/assets/ocr/') -and $relative -cne 'bin/assets/ocr/asset-manifest.json')) {
            throw "Mini Windows must not bundle OCR or QR payloads: $relative"
        }
        if ($relative.StartsWith('bin/', [StringComparison]::OrdinalIgnoreCase) -and
            $relative -notin $allowedBinFiles) {
            throw "Mini Windows contains an unexpected runtime or resource file: $relative"
        }
        if (-not $relative.StartsWith('bin/', [StringComparison]::OrdinalIgnoreCase) -and
            -not $relative.StartsWith('share/snow-shot-mini/licenses/', [StringComparison]::Ordinal) -and
            $relative -cne 'snow-shot-mini-installation.json') {
            throw "Mini Windows contains a file outside its runtime and license bundle: $relative"
        }
    }
    $manifest = Join-Path $Stage 'bin/assets/ocr/asset-manifest.json'
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { throw 'Mini is missing its trusted OCR descriptor.' }
    if ($OcrAssetManifest) {
        $trusted = Get-Content -LiteralPath $OcrAssetManifest -Raw | ConvertFrom-Json
        $staged = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
        if ($trusted.runtime.platform -cne "windows-$Architecture" -or
            $staged.runtime.platform -cne "windows-$Architecture" -or
            (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash -cne
                (Get-FileHash -LiteralPath $OcrAssetManifest -Algorithm SHA256).Hash) {
            throw 'Mini OCR descriptor must match the selected trusted target manifest exactly.'
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Stage 'share/snow-shot-mini/licenses/LICENSE'))) {
        throw 'Mini is missing its license bundle.'
    }
}

function Invoke-SnowShotMiniPackaging {
    $miniInstall = Join-Path $artifactRoot 'snow-shot-mini'
    Reset-ReleaseDirectory $miniInstall
    & cmake --install $buildDirectory --config Release --component SnowShotMini --prefix $miniInstall
    if ($LASTEXITCODE -ne 0) { throw 'Snow Shot Mini install failed.' }
    Assert-SnowShotMiniPayload -Stage $miniInstall -Architecture $Architecture -OcrAssetManifest $ocrAssetManifestPath
    $main = Join-Path $miniInstall 'bin/snow_shot_mini.exe'
    $metadata = (Get-Item -LiteralPath $main).VersionInfo
    if ($metadata.ProductName -cne 'Snow Shot Mini' -or $metadata.OriginalFilename -cne 'snow_shot_mini.exe' -or
        $metadata.ProductVersion -cne $packageVersion) { throw 'Mini binary identity/version differs from this release.' }
    Assert-NoPeExports $main
    foreach ($binary in @(Get-ChildItem -LiteralPath (Join-Path $miniInstall 'bin') -File -Filter '*.exe' -Force)) {
        Assert-SnowPeArchitecture -Path $binary.FullName -Architecture $Architecture
        $imports = @(& $script:DumpbinPath /nologo /dependents $binary.FullName 2>&1)
        if ($LASTEXITCODE -ne 0) { throw "Mini PE dependency inspection failed: $($binary.Name)" }
        foreach ($line in $imports) {
            if ($line -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
                $dependency = $Matches[1].ToLowerInvariant()
                if ($dependency -notmatch '^(?:api|ext)-ms-' -and
                    ($dependency -notin $allowedSystemImports -or
                     -not (Test-Path -LiteralPath (Join-Path $windowsSystemDirectory $dependency)))) {
                    throw "Mini imports a non-system or missing DLL: $($binary.Name) -> $dependency"
                }
            }
        }
    }
    & (Join-Path $PSScriptRoot 'collect-snow-shot-symbols.ps1') -BuildDirectory $buildDirectory `
        -InstallDirectory $miniInstall -Edition Mini -Architecture $Architecture `
        -OcrAssetManifest $ocrAssetManifestPath `
        -UpdaterProfileDirectory (Join-Path $buildDirectory "cargo-mini\$($windowsTarget.RustTarget)\release-size")
    $config = Join-Path $buildDirectory 'CPackSnowShotMiniConfig.cmake'
    if (-not (Test-Path -LiteralPath $config)) { throw "Mini CPack configuration was not generated: $config" }
    foreach ($variant in @('online', 'portable')) {
        $stage = Join-Path $artifactRoot "snow-shot-mini-$packageVersion-$variant-stage"
        Reset-ReleaseDirectory $stage
        Copy-Item -Path (Join-Path $miniInstall '*') -Destination $stage -Recurse -Force
        if ($variant -eq 'portable') {
            [IO.File]::WriteAllText((Join-Path $stage 'bin/__mini_data_directory'), 'portable', [Text.UTF8Encoding]::new($false))
        }
        Assert-SnowShotMiniPayload -Stage $stage -Architecture $Architecture -OcrAssetManifest $ocrAssetManifestPath
        Assert-SnowArm64StackCookieSafety -Path $stage -BuildDirectory $buildDirectory -Architecture $Architecture
        $owned = @(Get-ReleaseTreeFileManifest $stage | ForEach-Object {
            [ordered]@{ path = $_.Path; size = $_.Bytes; sha256 = $_.Sha256 }
        })
        [ordered]@{ schema = 1; product = 'snow-shot-mini'; platform = $ocrPlatform; version = $packageVersion; variant = $variant; files = $owned } |
            ConvertTo-Json -Depth 8 | Set-Content (Join-Path $stage 'snow-shot-mini-installation.json') -Encoding utf8NoBOM
        Assert-SnowNativeStartup -Stage $stage -Executable snow_shot_mini -Updater snow-shot-mini-updater -Version $packageVersion
        $base = "snow-shot-mini-$packageVersion-$ocrPlatform-$variant"
        $zipBase = if ($variant -eq 'online') { "$base-update" } else { $base }
        $zip = Join-Path $buildDirectory "$zipBase.zip"
        New-DeterministicZip $stage $zip
        $files = @(Get-ReleaseTreeFileManifest $stage)
        Assert-ZipMatchesFileManifest $zip $files
        $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $zipBase.zip" | Set-Content "$zip.sha256" -Encoding ascii
        $audit = [ordered]@{ SchemaVersion = 3; Product = 'snow-shot-mini'; PackageVersion = $packageVersion;
            Variant = $variant; Preset = $releasePreset; Architecture = $Architecture; Platform = $ocrPlatform; StaticCrt = $true;
            NativeValidationRequired = (-not $nativeTarget); NativeValidation = (New-SnowNativeValidation $zip);
            StaticQt = $true; InstallFiles = $files; Archive = [ordered]@{ Path = "$zipBase.zip";
                Bytes = (Get-Item $zip).Length; Sha256 = $hash } }
        $audit | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $buildDirectory "$zipBase.manifest.json") -Encoding utf8NoBOM
        if ($variant -eq 'online') {
            $variantConfig = Join-Path $buildDirectory 'CPackSnowShotMiniConfig-online.cmake'
            $baseConfig = $config.Replace('\', '/')
            $stagePath = $stage.Replace('\', '/')
            $nsisWork = $nsisWorkDirectory.Replace('\', '/')
            @"
include("$baseConfig")
set(CPACK_INSTALL_CMAKE_PROJECTS "")
set(CPACK_INSTALLED_DIRECTORIES "$stagePath;/")
set(CPACK_PACKAGE_DIRECTORY "$nsisWork")
set(CPACK_PACKAGE_FILE_NAME "$base")
string(REPLACE "snow-shot-mini-$packageVersion-$ocrPlatform.exe" "$base.exe" CPACK_NSIS_DEFINES "`${CPACK_NSIS_DEFINES}")
"@ | Set-Content $variantConfig -Encoding utf8NoBOM
            & cpack --config $variantConfig -G NSIS -C Release
            if ($LASTEXITCODE -ne 0) { throw 'Mini NSIS packaging failed.' }
            $exe = Join-Path $buildDirectory "$base.exe"
            foreach ($suffix in @('.exe', '.exe.sha256')) {
                Move-Item -LiteralPath (Join-Path $nsisWorkDirectory "$base$suffix") -Destination (Join-Path $buildDirectory "$base$suffix") -Force
            }
            $info = (Get-Item $exe).VersionInfo
            if ($info.ProductName -cne 'Snow Shot Mini' -or $info.ProductVersion -cne $packageVersion) {
                throw 'Mini installer metadata differs from this release.'
            }
            $exeHash = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant()
            "$exeHash  $base.exe" | Set-Content "$exe.sha256" -Encoding ascii
            $audit.Remove('Archive')
            $audit.Installer = [ordered]@{ Path = "$base.exe"; Bytes = (Get-Item $exe).Length; Sha256 = $exeHash }
            $audit.NativeValidation = New-SnowNativeValidation $exe
            $audit | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $buildDirectory "$base.manifest.json") -Encoding utf8NoBOM
        }
    }
    Write-Output "Packaged Snow Shot Mini $packageVersion (online installer, update ZIP, portable ZIP)."
}

if (-not $FunctionsOnly) { Invoke-SnowShotMiniPackaging }

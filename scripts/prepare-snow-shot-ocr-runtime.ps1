[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/snow-shot-msvc-release',
    [ValidateRange(1, 256)][int]$Parallelism = 4,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
Set-SnowBuildEnvironment -Preset snow-shot-msvc-release | Out-Null
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDirectory))
}
$artifactRoot = Join-Path $repoRoot 'artifacts'
if (-not $SkipBuild) {
    $cachePath = Join-Path $buildRoot 'CMakeCache.txt'
    $configureArguments = @('--preset', 'snow-shot-msvc-release', '-S', $repoRoot, '-B', $buildRoot)
    if ((Test-Path -LiteralPath $cachePath -PathType Leaf) -and
        -not (Test-SnowCacheAlignment -CachePath $cachePath -Preset snow-shot-msvc-release)) {
        $configureArguments = @('--fresh') + $configureArguments
    }
    & cmake @configureArguments
    if ($LASTEXITCODE -ne 0) { throw 'OCR Release configuration failed.' }
    & cmake --build $buildRoot --config Release --target snow_ocr_process_build --parallel $Parallelism
    if ($LASTEXITCODE -ne 0) { throw 'OCR Release build failed.' }
}

# Runtime-only preparation intentionally has no dependency on the application,
# updater, installer, translations, or application symbol bundles.
$executable = Join-Path $buildRoot 'cargo/x86_64-pc-windows-msvc/release/snow-ocr-process.exe'
$version = (& $executable --version 2>$null)
if ($LASTEXITCODE -ne 0 -or $version -cnotmatch '^snow-ocr-process (\d+\.\d+\.\d+) windows-x86_64 protocol 3$') {
    throw "Unexpected OCR runtime identity: $version"
}
$runtimeVersion = $Matches[1]
$platform = 'windows-x64'
$runtimeName = "snow-ocr-process-$runtimeVersion-$platform.exe"
$archiveName = "snow-ocr-runtime-$runtimeVersion-$platform.zip"
$uploadUrl = "https://www.modelscope.cn/models/mgchao/SnowShotOCR/resolve/master/runtime/$runtimeVersion/$platform/$archiveName"
$cache = Get-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt')
$installed = @($cache | Where-Object { $_ -cmatch '^VCPKG_INSTALLED_DIR:PATH=' })
if ($installed.Count -ne 1) { throw 'The configured vcpkg dependency directory is missing.' }
$directMl = Join-Path ($installed[0] -replace '^VCPKG_INSTALLED_DIR:PATH=', '') 'x64-windows-static/bin/DirectML.dll'
$dumpbin = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/dumpbin.exe'
foreach ($binary in @($executable, $directMl)) {
    $headers = @(& $dumpbin /nologo /headers $binary 2>&1)
    if ($LASTEXITCODE -ne 0 -or -not ($headers -match '8664 machine')) { throw "OCR runtime is not x64: $binary" }
    $imports = @(& $dumpbin /nologo /dependents $binary 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect OCR runtime dependencies: $binary" }
    foreach ($line in $imports) {
        if ($line -notmatch '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { continue }
        $dependency = $Matches[1]
        if ($dependency -match '^(?:api|ext)-ms-' -or $dependency -ieq 'DirectML.dll') { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32/$dependency") -PathType Leaf) -or
            $dependency -match '^(?:onnxruntime|Qt\d|vcruntime|msvcp)') {
            throw "Unbundled OCR runtime dependency: $dependency"
        }
    }
}

function Get-RuntimeFile([string]$Path, [string]$Name) {
    $file = Get-Item -LiteralPath $Path
    return [ordered]@{ name = $Name; size = $file.Length; sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
}

$stage = Join-Path $buildRoot "ocr-runtime-prepare-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $stage | Out-Null
Copy-Item -LiteralPath $executable -Destination (Join-Path $stage $runtimeName)
Copy-Item -LiteralPath $directMl -Destination (Join-Path $stage 'DirectML.dll')
$payload = @(
    (Get-RuntimeFile (Join-Path $stage $runtimeName) $runtimeName),
    (Get-RuntimeFile (Join-Path $stage 'DirectML.dll') 'DirectML.dll')
)
$runtimeManifest = Join-Path $stage 'runtime-manifest.json'
[ordered]@{ schema = 1; version = $runtimeVersion; platform = $platform; protocol = 3; files = $payload } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $runtimeManifest -Encoding utf8
$files = @($payload) + @((Get-RuntimeFile $runtimeManifest 'runtime-manifest.json'))
$stagedArchive = Join-Path $buildRoot "$archiveName.pending"
$stream = [IO.File]::Open($stagedArchive, [IO.FileMode]::Create)
try {
    $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        foreach ($name in @($files.name | Sort-Object -CaseSensitive)) {
            $entry = $zip.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
            $entry.ExternalAttributes = 0
            $input = [IO.File]::OpenRead((Join-Path $stage $name))
            $output = $entry.Open()
            try { $input.CopyTo($output) } finally { $input.Dispose(); $output.Dispose() }
        }
    } finally { $zip.Dispose() }
} finally { $stream.Dispose() }
$archive = Get-RuntimeFile $stagedArchive $archiveName
$archive.url = $uploadUrl
$publishedMarker = Join-Path $artifactRoot "$archiveName.published.sha256"
if ((Test-Path -LiteralPath $publishedMarker) -and
    (Get-Content -LiteralPath $publishedMarker -Raw).Trim().ToLowerInvariant() -cne $archive.sha256) {
    throw 'This runtime version was already published with different bytes. Increment its version.'
}

# Verify the entire generated archive using the same importer as application releases.
$verified = Join-Path $stage 'verified'
New-Item -ItemType Directory -Path $verified | Out-Null
. (Join-Path $PSScriptRoot 'snow-shot-ocr-release-runtime.ps1')
Expand-PinnedSnowOcrRuntime -ArchivePath $stagedArchive -Runtime ([pscustomobject]@{
    version = $runtimeVersion; platform = $platform; archive = $archive; files = $files
}) -Destination $verified
$destination = Join-Path $artifactRoot "snow-ocr-runtime-$runtimeVersion"
New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($name in $files.name) { Copy-Item -LiteralPath (Join-Path $verified $name) -Destination (Join-Path $destination $name) -Force }
$archivePath = Join-Path $buildRoot $archiveName
Copy-Item -LiteralPath $stagedArchive -Destination $archivePath -Force
Copy-Item -LiteralPath $stagedArchive -Destination (Join-Path $artifactRoot $archiveName) -Force
"$($archive.sha256)  $archiveName" | Set-Content -LiteralPath "$archivePath.sha256" -Encoding ascii
$manifestPath = Join-Path $buildRoot "snow-ocr-runtime-$runtimeVersion-$platform.manifest.json"
[ordered]@{ SchemaVersion = 1; RuntimeVersion = $runtimeVersion; Platform = $platform; Protocol = 3;
    UploadUrl = $uploadUrl; Archive = $archive; Files = $files } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8
Write-Output "OCR runtime upload artifact: $archivePath"
Write-Output "OCR runtime checksum: $archivePath.sha256"
Write-Output "OCR runtime manifest: $manifestPath"

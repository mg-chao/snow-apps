#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [switch]$FunctionsOnly
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
. (Join-Path $PSScriptRoot 'snow-shot-ocr-release-runtime.ps1')
. (Join-Path $PSScriptRoot 'snow-shot-ocr-native-validation.ps1')

function Expand-SnowNativePackage {
    param([Parameter(Mandatory)][string]$ArchivePath, [Parameter(Mandatory)]$Manifest,
        [Parameter(Mandatory)][string]$Destination)
    $descriptor = $Manifest.Archive
    if (-not $descriptor -or $descriptor.Path -cne [IO.Path]::GetFileName($ArchivePath) -or
        $descriptor.Bytes -ne (Get-Item -LiteralPath $ArchivePath).Length -or
        $descriptor.Sha256 -cne (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw 'The package archive differs from its audited bytes.'
    }
    $inventory = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $Manifest.InstallFiles) {
        $name = $file.Path.Replace('\', '/')
        if (-not $name -or $name.StartsWith('/') -or $name.Contains(':') -or
            @($name.Split('/') | Where-Object { $_ -in @('', '.', '..') }).Count -or
            $file.Bytes -lt 0 -or $file.Bytes -gt 1GB -or $file.Sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            -not $inventory.TryAdd($name, $file)) { throw 'Unsafe or invalid package file inventory.' }
    }
    if ($inventory.Count -eq 0 -or $inventory.Count -gt 10000) { throw 'Invalid package inventory size.' }
    if (-not (Test-Path -LiteralPath $Destination)) { $null = New-Item -ItemType Directory -Path $Destination -Force }
    $directory = Get-Item -LiteralPath $Destination
    if (-not $directory.PSIsContainer -or @(Get-ChildItem -LiteralPath $Destination -Force).Count) {
        throw 'Native package validation requires an empty extraction directory.'
    }
    for ($ancestor = $directory; $null -ne $ancestor; $ancestor = $ancestor.Parent) {
        if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Native validation may not traverse links.' }
    }
    $zip = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        if ($zip.Entries.Count -ne $inventory.Count) { throw 'Package ZIP entry count differs from its inventory.' }
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $zip.Entries) {
            $unixType = ($entry.ExternalAttributes -shr 16) -band 0xf000
            if (-not $inventory.ContainsKey($entry.FullName) -or -not $seen.Add($entry.FullName) -or
                $unixType -notin @(0, 0x8000) -or ($entry.ExternalAttributes -band 0x410)) {
                throw 'Unsafe or duplicate package archive entry.'
            }
            $file = $inventory[$entry.FullName]
            if ($entry.FullName -cne $file.Path.Replace('\', '/') -or $entry.Length -ne $file.Bytes) {
                throw 'Package entry name or size differs from its inventory.'
            }
            $input = $entry.Open()
            try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($input)).ToLowerInvariant() }
            finally { $input.Dispose() }
            if ($hash -cne $file.Sha256) { throw 'Package entry hash differs from its inventory.' }
        }
        foreach ($entry in $zip.Entries) {
            $path = Join-Path $Destination $entry.FullName
            $null = New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force
            $input = $entry.Open()
            $output = [IO.File]::Open($path, [IO.FileMode]::CreateNew)
            try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
        }
    } finally { $zip.Dispose() }
}

function Invoke-SnowNativePackageProbe {
    param([Parameter(Mandatory)][string]$Stage, [Parameter(Mandatory)][string]$Product,
        [Parameter(Mandatory)][string]$TargetArchitecture, [Parameter(Mandatory)][string]$Version)
    $null = Test-SnowOcrNativeValidationHost -Architecture $TargetArchitecture -RequireNative
    foreach ($binary in Get-ChildItem -LiteralPath $Stage -Recurse -File | Where-Object { $_.Extension -in @('.exe', '.dll') }) {
        Assert-SnowPeArchitecture -Path $binary.FullName -Architecture $TargetArchitecture
    }
    $executable = if ($Product -eq 'snow-shot-mini') { 'snow_shot_mini' } else { 'snow_shot' }
    foreach ($probe in @(
        @{ File = (Join-Path $Stage "bin/$executable.exe"); Arguments = @('--update-probe', $Version) },
        @{ File = (Join-Path $Stage "bin/$Product-updater.exe"); Arguments = @('--transaction-state', '--target', "`"$Stage`"") }
    )) {
        $process = Start-Process -FilePath $probe.File -ArgumentList $probe.Arguments -PassThru -WindowStyle Hidden
        try {
            if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Packaged native executable probe timed out.' }
            if ($process.ExitCode -ne 0) { throw "Packaged native executable probe failed: $($probe.File)" }
        } finally { $process.Dispose() }
    }
    $assetRoot = Join-Path $Stage 'bin/assets/ocr'
    $assetManifest = Join-Path $assetRoot 'asset-manifest.json'
    $packagedRuntimes = @(Get-ChildItem -LiteralPath $Stage -Recurse -File -Filter 'snow-ocr-process-*.exe')
    if (($Product -ceq 'snow-shot' -or $packagedRuntimes.Count) -and
        -not (Test-Path -LiteralPath $assetManifest -PathType Leaf)) {
        throw 'Packaged OCR runtime has no trusted asset manifest.'
    }
    if (Test-Path -LiteralPath $assetManifest -PathType Leaf) {
        $assets = Get-Content -LiteralPath $assetManifest -Raw | ConvertFrom-Json
        if ($assets.schema -ne 2 -or $assets.runtime.platform -cne "windows-$TargetArchitecture" -or
            $assets.runtime.version -cnotmatch '^\d+\.\d+\.\d+$' -or $assets.default_model -cne 'small') {
            throw 'Packaged OCR asset manifest identity differs from the native target.'
        }
        $standaloneRuntime = Join-Path $Stage 'bin/snow-ocr-process.exe'
        if (Test-Path -LiteralPath $standaloneRuntime -PathType Leaf) {
            $packagedRuntimes += Get-Item -LiteralPath $standaloneRuntime
        }
    }
    foreach ($runtime in $packagedRuntimes) {
        if ($runtime.Name -ceq 'snow-ocr-process.exe') {
            $version = $assets.runtime.version
        } else {
            if ($runtime.Name -cnotmatch '^snow-ocr-process-(\d+\.\d+\.\d+)-windows-(x64|arm64)\.exe$' -or
                $Matches[2] -cne $TargetArchitecture) { throw 'Unexpected packaged OCR runtime identity.' }
            $version = $Matches[1]
            if ($version -cne $assets.runtime.version) { throw 'Packaged OCR runtime version differs from its trusted manifest.' }
        }
        $workerArchitecture = if ($TargetArchitecture -eq 'arm64') { 'aarch64' } else { 'x86_64' }
        $identity = & $runtime.FullName --version 2>$null
        if ($LASTEXITCODE -ne 0 -or $identity -cne "snow-ocr-process $version windows-$workerArchitecture protocol 5") {
            throw 'Packaged native OCR runtime probe failed.'
        }
    }
    $modelsRoot = Join-Path $assetRoot 'models'
    if (@($packagedRuntimes | Where-Object { $_.Name -cne 'snow-ocr-process.exe' }).Count -and
        -not (Test-Path -LiteralPath $modelsRoot -PathType Container)) {
        throw 'Packaged versioned OCR runtime has no bundled models.'
    }
    if (Test-Path -LiteralPath $modelsRoot -PathType Container) {
        $runtime = @($packagedRuntimes | Where-Object { $_.Name -cne 'snow-ocr-process.exe' })
        if ($runtime.Count -ne 1) { throw 'Bundled OCR models require exactly one packaged versioned runtime.' }
        $models = @(Get-ChildItem -LiteralPath $modelsRoot -Directory)
        if (-not $models.Count) { throw 'Packaged OCR model directory is empty.' }
        $defaultModel = @($assets.models | Where-Object { $_.type -ceq $assets.default_model })
        if ($defaultModel.Count -ne 1 -or $defaultModel[0].id -cnotin $models.Name) {
            throw 'Packaged OCR models omit the trusted default model.'
        }
        foreach ($directory in $models) {
            $model = @($assets.models | Where-Object { $_.id -ceq $directory.Name })
            if ($model.Count -ne 1) { throw 'Packaged OCR model set is absent from the trusted manifest.' }
            if (@($model[0].files).Count -ne 3) { throw 'Packaged OCR model set has an invalid pinned file list.' }
            foreach ($field in @('detector', 'recognizer', 'dictionary')) {
                $name = $model[0].$field
                if (-not $name -or $name.Contains('/') -or $name.Contains('\') -or $name.Contains(':') -or
                    $name -in @('.', '..')) { throw 'Invalid packaged OCR model filename.' }
                $file = @($model[0].files | Where-Object { $_.name -ceq $name })
                $path = Join-Path $directory.FullName $name
                if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "OCR model validation input is missing: $path" }
                if ($file.Count -ne 1 -or $file[0].size -ne (Get-Item -LiteralPath $path).Length -or
                    $file[0].sha256 -cnotmatch '^[0-9a-f]{64}$' -or
                    $file[0].sha256 -cne (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()) {
                    throw 'Packaged OCR model bytes differ from the trusted asset manifest.'
                }
            }
            $null = Invoke-SnowOcrModelSetValidation -Architecture $TargetArchitecture -RequireNative `
                -Executable $runtime[0].FullName -Detector (Join-Path $directory.FullName $model[0].detector) `
                -Recognizer (Join-Path $directory.FullName $model[0].recognizer) `
                -Dictionary (Join-Path $directory.FullName $model[0].dictionary)
        }
    }
}

function Write-SnowNativePackageProof {
    param([Parameter(Mandatory)][string]$ArtifactPath, [Parameter(Mandatory)][string]$Platform,
        [Parameter(Mandatory)][string]$Directory)
    $proof = [ordered]@{ Platform = $Platform; HostPlatform = $Platform; Passed = $true
        Artifacts = @([ordered]@{ Name = [IO.Path]::GetFileName($ArtifactPath)
            Bytes = (Get-Item -LiteralPath $ArtifactPath).Length
            Sha256 = (Get-FileHash -LiteralPath $ArtifactPath -Algorithm SHA256).Hash.ToLowerInvariant() }) }
    $path = Join-Path $Directory "$([IO.Path]::GetFileName($ArtifactPath)).native-validation.json"
    $proof | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $path -Encoding utf8NoBOM
    Write-Output "Native package validation: $path"
}

if ($FunctionsOnly) { return }
$target = Get-SnowWindowsTarget -Architecture $Architecture
if ($target.HostArchitecture -cne $Architecture) { throw "Native package validation requires a $($target.Platform) host." }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $script:SnowRepoRoot "build/$($target.ReleasePreset)" }
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $buildRoot 'native-validation' }
$proofRoot = [IO.Path]::GetFullPath($OutputDirectory)
$null = New-Item -ItemType Directory -Path $proofRoot -Force
$version = [regex]::Match((Get-Content -LiteralPath (Join-Path $script:SnowRepoRoot 'CMakeLists.txt') -Raw),
    'set\(SNOW_SHOT_VERSION "([^"]+)"\)').Groups[1].Value
$work = Join-Path $buildRoot ("native-package-test-" + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $work
$validated = @{}
$installers = @()
try {
    foreach ($product in @('snow-shot', 'snow-shot-mini')) {
        $variants = if ($product -eq 'snow-shot-mini') { @('online', 'portable') } else { @('online', 'offline', 'portable') }
        foreach ($variant in $variants) {
            $base = "$product-$version-$($target.Platform)-$variant"
            $archiveBase = if ($variant -eq 'portable') { $base } else { "$base-update" }
            $manifest = Get-Content -LiteralPath (Join-Path $buildRoot "$archiveBase.manifest.json") -Raw | ConvertFrom-Json
            if ($manifest.Architecture -cne $Architecture -or $manifest.PackageVersion -cne $version -or $manifest.Variant -cne $variant) {
                throw 'Native package manifest identity differs from this release.'
            }
            $archive = Join-Path $buildRoot "$archiveBase.zip"
            $stage = Join-Path $work $archiveBase
            Expand-SnowNativePackage -ArchivePath $archive -Manifest $manifest -Destination $stage
            Invoke-SnowNativePackageProbe -Stage $stage -Product $product -TargetArchitecture $Architecture -Version $version
            $validated[$archiveBase] = $manifest
            if ($variant -ne 'portable') { $installers += @{ Base = $base; Product = $product; Variant = $variant } }
        }
    }
    # Installer and update ZIP are produced from the same audited stage. Bind the
    # installer proof to those exact owned bytes without altering a live install.
    foreach ($installer in $installers) {
        $manifest = Get-Content -LiteralPath (Join-Path $buildRoot "$($installer.Base).manifest.json") -Raw | ConvertFrom-Json
        $update = $validated["$($installer.Base)-update"]
        if ($manifest.Architecture -cne $Architecture -or $manifest.PackageVersion -cne $version -or
            $manifest.Variant -cne $installer.Variant -or
            ($manifest.InstallFiles | ConvertTo-Json -Depth 8 -Compress) -cne ($update.InstallFiles | ConvertTo-Json -Depth 8 -Compress)) {
            throw 'Installer payload inventory differs from the natively tested update ZIP.'
        }
        $path = Join-Path $buildRoot "$($installer.Base).exe"
        if ($manifest.Installer.Path -cne [IO.Path]::GetFileName($path) -or
            $manifest.Installer.Bytes -ne (Get-Item -LiteralPath $path).Length -or
            $manifest.Installer.Sha256 -cne (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()) {
            throw 'Installer differs from its final audited bytes.'
        }
    }
    # Emit proof only after all editions and variants have passed validation.
    foreach ($archiveBase in $validated.Keys) {
        Write-SnowNativePackageProof -ArtifactPath (Join-Path $buildRoot "$archiveBase.zip") -Platform $target.Platform -Directory $proofRoot
    }
    foreach ($installer in $installers) {
        Write-SnowNativePackageProof -ArtifactPath (Join-Path $buildRoot "$($installer.Base).exe") -Platform $target.Platform -Directory $proofRoot
    }
} finally {
    $resolved = [IO.Path]::GetFullPath($work)
    if (-not $resolved.StartsWith($buildRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe native package test cleanup directory.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

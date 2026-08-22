[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Destination,
    [Parameter(Mandatory = $true)][string]$AllowedRoot,
    [Parameter(Mandatory = $true)][string]$VcpkgPrefix,
    [Parameter(Mandatory = $true)][string]$QtPrefix,
    [Parameter(Mandatory = $true)][string]$CargoManifest,
    [Parameter(Mandatory = $true)][string]$AntDesignNotice,
    [Parameter(Mandatory = $true)][string]$FallbackLicenseDirectory,
    [string]$CargoTarget = "x86_64-pc-windows-msvc"
)

$ErrorActionPreference = "Stop"

function Resolve-ExistingPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description,
        [ValidateSet("Leaf", "Container")][string]$PathType = "Container"
    )

    if (-not (Test-Path -LiteralPath $Path -PathType $PathType)) {
        throw "$Description was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function ConvertTo-SafeName {
    param([Parameter(Mandatory = $true)][string]$Value)

    $safe = $Value -replace '[^A-Za-z0-9._+-]', '_'
    if ([string]::IsNullOrWhiteSpace($safe)) {
        throw "A license bundle entry has an empty safe name: $Value"
    }
    return $safe
}

$destinationPath = [System.IO.Path]::GetFullPath($Destination)
$allowedRootPath = Resolve-ExistingPath -Path $AllowedRoot -Description "allowed output root"
if ((Split-Path -Leaf $destinationPath) -cne "third-party") {
    throw "The license bundle destination must end in a directory named 'third-party': $destinationPath"
}
$destinationParent = Split-Path -Parent $destinationPath
if ([string]::IsNullOrWhiteSpace($destinationParent) -or
    $destinationPath -eq [System.IO.Path]::GetPathRoot($destinationPath)) {
    throw "The license bundle destination is unsafe: $destinationPath"
}
$allowedRootPrefix = $allowedRootPath.TrimEnd([char[]]@('\', '/')) +
    [System.IO.Path]::DirectorySeparatorChar
if (-not $destinationPath.StartsWith(
        $allowedRootPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The license bundle destination must be below the allowed output root '$allowedRootPath': $destinationPath"
}

$vcpkgPrefixPath = Resolve-ExistingPath -Path $VcpkgPrefix -Description "vcpkg package prefix"
$qtPrefixPath = Resolve-ExistingPath -Path $QtPrefix -Description "static Qt prefix"
$cargoManifestPath = Resolve-ExistingPath -Path $CargoManifest -Description "Cargo manifest" -PathType Leaf
$antDesignNoticePath = Resolve-ExistingPath -Path $AntDesignNotice `
    -Description "Ant Design third-party notice" -PathType Leaf
$fallbackLicenseDirectoryPath = Resolve-ExistingPath -Path $FallbackLicenseDirectory `
    -Description "canonical fallback license directory"

if (Test-Path -LiteralPath $destinationPath) {
    Remove-Item -LiteralPath $destinationPath -Recurse -Force
}
New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null

$records = [System.Collections.Generic.List[object]]::new()

function Copy-LicenseNotice {
    param(
        [Parameter(Mandatory = $true)][string]$Category,
        [Parameter(Mandatory = $true)][string]$Package,
        [Parameter(Mandatory = $true)][string]$DeclaredLicense,
        [Parameter(Mandatory = $true)][string]$Source,
        [string]$RelativeName = ""
    )

    $safeCategory = ConvertTo-SafeName -Value $Category
    $safePackage = ConvertTo-SafeName -Value $Package
    $packageDirectory = Join-Path $destinationPath "$safeCategory\$safePackage"
    New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null

    $targetName = if ([string]::IsNullOrWhiteSpace($RelativeName)) {
        [System.IO.Path]::GetFileName($Source)
    }
    else {
        $RelativeName
    }
    $targetName = ConvertTo-SafeName -Value $targetName
    $targetPath = Join-Path $packageDirectory $targetName
    if (Test-Path -LiteralPath $targetPath) {
        $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
        $targetHash = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash
        if ($sourceHash -ne $targetHash) {
            throw "Conflicting license notices map to the same destination: $targetPath"
        }
    }
    else {
        Copy-Item -LiteralPath $Source -Destination $targetPath
    }

    $records.Add([pscustomobject][ordered]@{
        Category = $Category
        Package = $Package
        DeclaredLicense = $DeclaredLicense
        Notice = [System.IO.Path]::GetRelativePath($destinationPath, $targetPath).Replace('\', '/')
    })
}

$vcpkgShare = Join-Path $vcpkgPrefixPath "share"
$vcpkgCopyrights = @(Get-ChildItem -LiteralPath $vcpkgShare -Directory |
    ForEach-Object {
        $copyright = Join-Path $_.FullName "copyright"
        if (Test-Path -LiteralPath $copyright -PathType Leaf) {
            Get-Item -LiteralPath $copyright
        }
    } | Sort-Object DirectoryName)
if ($vcpkgCopyrights.Count -eq 0) {
    throw "The vcpkg prefix contains no package copyright files: $vcpkgShare"
}
foreach ($copyright in $vcpkgCopyrights) {
    Copy-LicenseNotice -Category "vcpkg" -Package $copyright.Directory.Name `
        -DeclaredLicense "See collected package copyright" -Source $copyright.FullName `
        -RelativeName "copyright.txt"
}

$qtLicenseRoot = Join-Path $qtPrefixPath "share\snow-apps\qt-licenses"
if (-not (Test-Path -LiteralPath $qtLicenseRoot -PathType Container)) {
    throw "The audited static Qt kit has no installed license bundle: $qtLicenseRoot. Rebuild it with scripts/build-static-qt.ps1."
}
$qtLicenseFiles = @(Get-ChildItem -LiteralPath $qtLicenseRoot -Recurse -File | Sort-Object FullName)
if ($qtLicenseFiles.Count -eq 0) {
    throw "The audited static Qt license bundle is empty: $qtLicenseRoot"
}
foreach ($licenseFile in $qtLicenseFiles) {
    $relative = [System.IO.Path]::GetRelativePath($qtLicenseRoot, $licenseFile.FullName)
    $segments = $relative -split '[\\/]'
    $package = if ($segments.Count -gt 1) { $segments[0] } else { "qt" }
    $relativeName = ($segments -join "__")
    Copy-LicenseNotice -Category "qt" -Package $package `
        -DeclaredLicense "See Qt REUSE metadata and collected license texts" `
        -Source $licenseFile.FullName -RelativeName $relativeName
}

$cargoCommand = Get-Command cargo -ErrorAction SilentlyContinue
if (-not $cargoCommand) {
    throw "cargo is required to collect Rust dependency licenses."
}
$metadataOutput = @(& $cargoCommand.Source metadata --locked --offline `
    --filter-platform $CargoTarget --format-version 1 `
    --manifest-path $cargoManifestPath)
if ($LASTEXITCODE -ne 0) {
    throw "cargo metadata failed while collecting Rust dependency licenses. Build the release first so all locked packages are available offline."
}
try {
    $metadata = ($metadataOutput -join "`n") | ConvertFrom-Json
}
catch {
    throw "cargo metadata returned invalid JSON: $($_.Exception.Message)"
}

$packageById = @{}
foreach ($package in $metadata.packages) {
    $packageById[$package.id] = $package
}
$nodeById = @{}
foreach ($node in $metadata.resolve.nodes) {
    $nodeById[$node.id] = $node
}
$rootId = $metadata.resolve.root
if ([string]::IsNullOrWhiteSpace($rootId)) {
    $rootPackage = @($metadata.packages | Where-Object {
        [System.IO.Path]::GetFullPath($_.manifest_path) -eq $cargoManifestPath
    })
    if ($rootPackage.Count -ne 1) {
        throw "Could not identify the root Rust package for $cargoManifestPath"
    }
    $rootId = $rootPackage[0].id
}

$reachable = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$pending = [System.Collections.Generic.Queue[string]]::new()
$pending.Enqueue($rootId)
while ($pending.Count -gt 0) {
    $id = $pending.Dequeue()
    if (-not $reachable.Add($id)) {
        continue
    }
    $node = $nodeById[$id]
    if (-not $node) {
        throw "The Cargo resolve graph has no node for $id"
    }
    foreach ($dependency in @($node.deps)) {
        $nonDevelopmentKinds = @($dependency.dep_kinds | Where-Object { $_.kind -ne "dev" })
        if ($dependency.dep_kinds.Count -eq 0 -or $nonDevelopmentKinds.Count -gt 0) {
            $pending.Enqueue([string]$dependency.pkg)
        }
    }
}

$missingCargoNotices = [System.Collections.Generic.List[string]]::new()
$externalCargoPackages = @($reachable | ForEach-Object { $packageById[$_] } |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_.source) } |
    Sort-Object name, version)
foreach ($package in $externalCargoPackages) {
    $packageDirectory = Split-Path -Parent $package.manifest_path
    $licenseFiles = @(Get-ChildItem -LiteralPath $packageDirectory -File |
        Where-Object {
            $_.Name -match '^(?i:LICENSE|LICENCE|COPYING|NOTICE|UNLICENSE)(?:[._-].*)?$'
        } | Sort-Object Name)
    if ($licenseFiles.Count -eq 0) {
        $fallbackName = if ([string]$package.license -match '(?:^|\s|\()Apache-2\.0(?:$|\s|\))') {
            "Apache-2.0.txt"
        }
        elseif ([string]$package.license -match '(?:^|\s|\()WTFPL(?:$|\s|\))') {
            "WTFPL.txt"
        }
        else {
            ""
        }
        $fallbackPath = if ([string]::IsNullOrWhiteSpace($fallbackName)) {
            ""
        }
        else {
            Join-Path $fallbackLicenseDirectoryPath $fallbackName
        }
        if ([string]::IsNullOrWhiteSpace($fallbackPath) -or
            -not (Test-Path -LiteralPath $fallbackPath -PathType Leaf)) {
            $missingCargoNotices.Add("$($package.name) $($package.version) [$($package.license)]")
            continue
        }
        Copy-LicenseNotice -Category "cargo" `
            -Package "$($package.name)-$($package.version)" `
            -DeclaredLicense "$($package.license) (canonical fallback: $fallbackName)" `
            -Source $fallbackPath -RelativeName $fallbackName
        continue
    }
    $packageLabel = "$($package.name)-$($package.version)"
    $declaredLicense = if ([string]::IsNullOrWhiteSpace($package.license)) {
        "Not declared in Cargo metadata"
    }
    else {
        [string]$package.license
    }
    foreach ($licenseFile in $licenseFiles) {
        Copy-LicenseNotice -Category "cargo" -Package $packageLabel `
            -DeclaredLicense $declaredLicense -Source $licenseFile.FullName
    }
}
if ($missingCargoNotices.Count -gt 0) {
    throw "Resolved Rust packages are missing distributable license files: $($missingCargoNotices -join '; ')"
}

Copy-LicenseNotice -Category "project-notices" -Package "ant-design-icons" `
    -DeclaredLicense "MIT" -Source $antDesignNoticePath -RelativeName "THIRD_PARTY_NOTICES.md"

$sortedRecords = @($records | Sort-Object Category, Package, Notice)
$indexLines = [System.Collections.Generic.List[string]]::new()
$indexLines.Add("# Snow Shot Third-Party License Index")
$indexLines.Add("")
$indexLines.Add("This bundle was generated from the release build's resolved, non-development Rust dependency graph, installed vcpkg prefix, audited static Qt kit, and repository attribution notices.")
$indexLines.Add("")
$indexLines.Add("| Source | Package | Declared license | Notice file |")
$indexLines.Add("| --- | --- | --- | --- |")
foreach ($record in $sortedRecords) {
    $category = $record.Category.Replace('|', '\|')
    $package = $record.Package.Replace('|', '\|')
    $declaredLicense = $record.DeclaredLicense.Replace('|', '\|')
    $notice = $record.Notice.Replace('|', '\|')
    $indexLines.Add("| $category | $package | $declaredLicense | ``$notice`` |")
}
$indexLines.Add("")
$indexLines.Add("A listed SPDX expression summarizes package metadata; the collected notice text controls if the two differ.")
$indexLines | Set-Content -LiteralPath (Join-Path $destinationPath "INDEX.md") -Encoding utf8

[ordered]@{
    SchemaVersion = 1
    CargoTarget = $CargoTarget
    ReachableRustPackages = $reachable.Count
    ExternalRustPackages = $externalCargoPackages.Count
    VcpkgPackages = $vcpkgCopyrights.Count
    QtLicenseFiles = $qtLicenseFiles.Count
    Notices = $sortedRecords
} | ConvertTo-Json -Depth 5 | Set-Content `
    -LiteralPath (Join-Path $destinationPath "manifest.json") -Encoding utf8

Write-Output "Third-party license bundle: $destinationPath"
Write-Output "Collected $($sortedRecords.Count) notices for $($externalCargoPackages.Count) Rust packages, $($vcpkgCopyrights.Count) vcpkg packages, and Qt."

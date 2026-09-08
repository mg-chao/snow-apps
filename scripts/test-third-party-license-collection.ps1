[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$fixtureRoot = Join-Path $repoRoot ("build/license-collection-test-" + [guid]::NewGuid().ToString('N'))
$utf8 = [Text.UTF8Encoding]::new($false)

function Write-Fixture([string]$RelativePath, [string]$Content) {
    $path = Join-Path $fixtureRoot $RelativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [IO.File]::WriteAllText($path, $Content.Replace("`r`n", "`n") + "`n", $utf8)
}

foreach ($leaf in @('left', 'right', 'unused')) {
    Write-Fixture "deps/$leaf/Cargo.toml" @"
[package]
name = "license-$leaf"
version = "0.1.0"
edition = "2024"
license = "MIT"
"@
    Write-Fixture "deps/$leaf/src/lib.rs" 'pub fn fixture() {}'
    Write-Fixture "deps/$leaf/LICENSE" "$leaf dependency license fixture"
}
Write-Fixture 'deps/shared/Cargo.toml' @'
[package]
name = "license-shared"
version = "0.1.0"
edition = "2024"
license = "MIT"
[features]
left = ["dep:license-left"]
right = ["dep:license-right"]
unused = ["dep:license-unused"]
[dependencies]
license-left = { path = "../left", optional = true }
license-right = { path = "../right", optional = true }
license-unused = { path = "../unused", optional = true }
'@
Write-Fixture 'deps/shared/src/lib.rs' 'pub fn fixture() {}'
Write-Fixture 'deps/shared/LICENSE' 'shared dependency license fixture'
$dependencyRepository = Join-Path $fixtureRoot 'deps'
git init --quiet $dependencyRepository
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize dependency fixture' }
git -C $dependencyRepository add .
if ($LASTEXITCODE -ne 0) { throw 'Could not stage dependency fixture' }
git -C $dependencyRepository -c user.name=Fixture -c user.email=fixture@example.invalid `
    commit --quiet -m 'Create dependency fixture'
if ($LASTEXITCODE -ne 0) { throw 'Could not commit dependency fixture' }
$dependencyUrl = ([Uri]$dependencyRepository).AbsoluteUri

$manifests = @()
$cargoOptions = @{}
foreach ($feature in @('left', 'right')) {
    Write-Fixture "root-$feature/Cargo.toml" @"
[workspace]
members = ["unshipped"]
[package]
name = "private-root-$feature"
version = "0.1.0"
edition = "2024"
license = "Apache-2.0"
[features]
default = ["license-shared/unused"]
selected = ["license-shared/$feature"]
[dependencies]
license-shared = { git = '$dependencyUrl' }
"@
    Write-Fixture "root-$feature/src/lib.rs" 'pub fn fixture() {}'
    Write-Fixture "root-$feature/unshipped/Cargo.toml" @"
[package]
name = "unshipped-$feature"
version = "0.1.0"
edition = "2024"
[dependencies]
license-shared = { git = '$dependencyUrl', features = ["unused"] }
"@
    Write-Fixture "root-$feature/unshipped/src/lib.rs" 'pub fn fixture() {}'
    $manifest = Join-Path $fixtureRoot "root-$feature/Cargo.toml"
    # Only the local Git fixture needs fetching; this test never uses the network.
    cargo generate-lockfile --manifest-path $manifest
    if ($LASTEXITCODE -ne 0) { throw "Failed to resolve fixture: $feature" }
    $manifests += $manifest
    $cargoOptions[$manifest] = @('--no-default-features', '--features', 'selected')
}
Write-Fixture 'vcpkg/share/native/copyright' 'Native license fixture'
Write-Fixture 'qt/share/snow-apps/qt-licenses/qt/LICENSE' 'Qt license fixture'
Write-Fixture 'ant-notice.md' 'Ant notice fixture'
Write-Fixture 'fallback/Apache-2.0.txt' 'Canonical fallback fixture'

$destination = Join-Path $fixtureRoot 'third-party'
& (Join-Path $PSScriptRoot 'collect-third-party-licenses.ps1') `
    -Destination $destination -AllowedRoot $fixtureRoot `
    -VcpkgPrefix (Join-Path $fixtureRoot 'vcpkg') -QtPrefix (Join-Path $fixtureRoot 'qt') `
    -CargoManifest $manifests -CargoOptions $cargoOptions `
    -AntDesignNotice (Join-Path $fixtureRoot 'ant-notice.md') `
    -FallbackLicenseDirectory (Join-Path $fixtureRoot 'fallback')

foreach ($name in @('left', 'right', 'shared')) {
    $notice = Join-Path $destination "cargo/license-$name-0.1.0/LICENSE"
    if (!(Test-Path -LiteralPath $notice) -or
        [IO.File]::ReadAllText($notice).Trim() -cne "$name dependency license fixture") {
        throw "Both shipped binaries' dependency licenses must survive: $name"
    }
}
$cargoDirectories = @(Get-ChildItem -LiteralPath (Join-Path $destination 'cargo') -Directory)
if ($cargoDirectories.Count -ne 3) {
    throw 'Collect only the selected packages: exclude unshipped workspace features and private roots'
}
Write-Output 'License collection regression passed: selected packages, multiple roots, and Git dependencies.'

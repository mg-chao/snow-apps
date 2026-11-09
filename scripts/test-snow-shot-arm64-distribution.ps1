# Deterministic multi-architecture manifest and publication gates; no network or installers.
param([string]$SchemaPath = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-winget.ps1')
. (Join-Path $PSScriptRoot 'snow-shot-scoop.ps1')
. (Join-Path $PSScriptRoot 'snow-shot-native-validation.ps1')
function Require($Value, [string]$Message) { if (-not $Value) { throw $Message } }
function Must-Fail([scriptblock]$Action) {
    try { & $Action | Out-Null } catch { return }
    throw 'Invalid ARM64 distribution input was accepted.'
}
$root = Join-Path ([IO.Path]::GetTempPath()) "snow-arm-distribution-$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory $root
$script:downloads = @{}
$tag = 'v2.0.0_snow-shot'
$script:release = [pscustomobject]@{ tag_name = $tag; draft = $false; prerelease = $false;
    published_at = '2026-10-04T00:00:00Z'; body = 'ARM64 release fixture'; assets = @() }
function Invoke-SnowShotWingetApi([string]$Path) { return $script:release }
function Invoke-SnowShotScoopRelease([string]$Tag) { return $script:release }
function Invoke-WebRequest([string]$Uri, [string]$OutFile, [int]$TimeoutSec) {
    if (-not $script:downloads.ContainsKey($Uri)) { throw "Unexpected download: $Uri" }
    [IO.File]::WriteAllBytes($OutFile, $script:downloads[$Uri])
}
try {
    foreach ($edition in @('Full', 'Mini')) {
        foreach ($architecture in @('x64', 'arm64')) {
            $product = Get-SnowShotEdition $edition $architecture
            $zipPath = Join-Path $root "$edition-$architecture.zip"
            $zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
            try {
                foreach ($pair in @(@("bin/$($product.Executable).exe", 'fixture'), @("bin/$($product.Marker)", 'portable'))) {
                    $writer = [IO.StreamWriter]::new($zip.CreateEntry($pair[0]).Open())
                    try { $writer.Write($pair[1]) } finally { $writer.Dispose() }
                }
            } finally { $zip.Dispose() }
            $names = @($product.Feed)
            foreach ($variant in $product.Variants) {
                $base = "$($product.Product)-2.0.0-windows-$architecture-$variant"
                $names += if ($variant -eq 'portable') { "$base.zip" } else { @("$base.exe", "$base-update.zip") }
            }
            foreach ($name in $names) {
                $bytes = if ($name.EndsWith('.zip')) { [IO.File]::ReadAllBytes($zipPath) } else { [byte[]](1, 2, 3, 4) }
                $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
                $url = "https://github.com/mg-chao/snow-shot/releases/download/$tag/$name"
                $script:downloads[$url] = $bytes
                $script:release.assets += [pscustomobject]@{ name = $name; size = $bytes.Length; digest = "sha256:$hash"; browser_download_url = $url }
            }
        }
    }
    Require ((@(Get-SnowShotReleaseArchitectures $script:release '2.0.0') -join ',') -ceq 'x64,arm64') 'Both architectures must be detected.'
    $directory = New-SnowShotWingetManifest $tag $root
    $yaml = Get-Content (Join-Path $directory 'mg-chao.snow-shot.installer.yaml') -Raw
    Require ($yaml.Contains('Architecture: arm64') -and $yaml.Contains('Architecture: x64')) 'Full WinGet must contain both native architectures.'
    foreach ($edition in @('Full', 'Mini')) {
        $product = Get-SnowShotEdition $edition
        $path = New-SnowShotScoopManifest $tag (Join-Path $root "$edition.json") $edition
        $manifest = Get-Content $path -Raw | ConvertFrom-Json
        if ($SchemaPath) {
            Require (Test-Json -Json (Get-Content $path -Raw) -SchemaFile $SchemaPath) 'The two-architecture Scoop manifest must match the pinned upstream schema.'
        }
        Require ($manifest.architecture.arm64.url.Contains("$($product.Product)-2.0.0-windows-arm64-portable.zip")) 'Scoop ARM64 URL must match its edition.'
        Require ($manifest.autoupdate.architecture.arm64.url.Contains('windows-arm64')) 'Scoop ARM64 autoupdate is required.'
    }
    $saved = $script:release.assets
    # WinGet depends only on Full offline installers, independently of Mini feeds/payloads.
    $script:release.assets = @($saved | Where-Object { $_.name -clike 'snow-shot-2.0.0-windows-*-offline.exe' })
    Require ((@(Get-SnowShotWingetArchitectures $script:release '2.0.0') -join ',') -ceq 'x64,arm64') 'Full-only releases must retain both WinGet architectures.'
    $fullOnly = New-SnowShotWingetManifest $tag $root
    Require ((Get-Content (Join-Path $fullOnly 'mg-chao.snow-shot.installer.yaml') -Raw) -ceq $yaml) 'Mini assets must not affect Full WinGet manifests.'
    $script:release.assets += $script:release.assets[1]
    Must-Fail { Get-SnowShotWingetArchitectures $script:release '2.0.0' }
    $script:release.assets = @($saved | Where-Object { $_.name -ceq 'snow-shot-2.0.0-windows-x64-offline.exe' -or $_.name -clike 'snow-shot-mini-*' })
    Require ((@(Get-SnowShotWingetArchitectures $script:release '2.0.0') -join ',') -ceq 'x64') 'Mini ARM64 assets must not imply Full ARM64 support.'
    $script:release.assets += [pscustomobject]@{ name = 'snow-shot-2.0.0-windows-arm64-portable.zip' }
    Must-Fail { Get-SnowShotWingetArchitectures $script:release '2.0.0' }
    $script:release.assets = @($saved | Where-Object { $_.name -cne 'latest-version-mini-windows-arm64.json' })
    Must-Fail { Get-SnowShotReleaseArchitectures $script:release '2.0.0' }
    $script:release.assets = @($saved | Where-Object { $_.name -notlike 'snow-shot-mini-*' -and $_.name -notlike 'latest-version-mini*.json' })
    Must-Fail { Get-SnowShotReleaseArchitectures $script:release '2.0.0' }
    $script:release.assets = $saved
    $artifact = Join-Path $root 'artifact.zip'
    [IO.File]::WriteAllBytes($artifact, [byte[]](1, 2, 3))
    $proof = [pscustomobject]@{ Platform = 'windows-arm64'; HostPlatform = 'windows-arm64'; Passed = $true;
        Artifacts = @([pscustomobject]@{ Name = 'artifact.zip'; Bytes = 3; Sha256 = (Get-FileHash $artifact).Hash.ToLowerInvariant() }) }
    $audit = [pscustomobject]@{ NativeValidation = $proof }
    $null = Assert-SnowShotNativeValidation $audit $artifact 'windows-arm64'
    $proof.HostPlatform = 'windows-x64'
    Must-Fail { Assert-SnowShotNativeValidation $audit $artifact 'windows-arm64' }
    $proof.HostPlatform = 'windows-arm64'; $proof.Artifacts[0].Sha256 = '0' * 64
    Must-Fail { Assert-SnowShotNativeValidation $audit $artifact 'windows-arm64' }
    $proof.Artifacts[0].Sha256 = (Get-FileHash $artifact).Hash.ToLowerInvariant(); $proof.Passed = $false
    Must-Fail { Assert-SnowShotNativeValidation $audit $artifact 'windows-arm64' }
    $waiver = Assert-SnowShotNativeValidation $audit $artifact 'windows-arm64' -SkipArm64NativeValidation
    Require ($waiver.Waived -and -not $waiver.Passed -and
        $waiver.Artifacts[0].Sha256 -ceq (Get-FileHash $artifact).Hash.ToLowerInvariant()) 'An explicit waiver must retain exact bytes without claiming a native pass.'
    Must-Fail { Assert-SnowShotNativeValidation $audit $artifact 'windows-x64' -SkipArm64NativeValidation }
    Write-Output 'PASS: ARM64 WinGet/Scoop completeness and exact-byte native publication proof.'
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    if (-not $resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolved -Leaf) -notlike 'snow-arm-distribution-*') { throw 'Unsafe fixture cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

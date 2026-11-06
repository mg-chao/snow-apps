[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-codec-profile.ps1')
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$testRoot = Join-Path $repoRoot ('build/codec-profile-tests-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $testRoot

function New-Profile([string]$Name, [hashtable]$Overrides = @{}) {
    $prefix = Join-Path $testRoot $Name
    $noticeDirectory = Join-Path $prefix 'share/x265'
    $null = New-Item -ItemType Directory -Path $noticeDirectory -Force
    $capability = [ordered]@{
        schemaVersion = 2
        bitDepth8 = $true
        bitDepth10 = $false
        bitDepth12 = $false
        dynamicDepthLoading = $false
        singlePublicApi = $true
    }
    foreach ($key in $Overrides.Keys) { $capability[$key] = $Overrides[$key] }
    $capability | ConvertTo-Json | Set-Content -LiteralPath (
        Join-Path $noticeDirectory 'snow-encoder-capabilities.json') -Encoding utf8NoBOM
    return $prefix
}

function Require-Rejected([string]$Prefix, [string]$Expected) {
    try { Assert-SnowShotX265Profile -Prefix $Prefix }
    catch {
        if ($_.Exception.Message -match $Expected) { return }
        throw
    }
    throw "The incompatible profile was accepted: $Prefix"
}

$valid = New-Profile 'main8'
Assert-SnowShotX265Profile -Prefix $valid
$legacy = Join-Path $testRoot 'legacy/share/x265'
$null = New-Item -ItemType Directory -Path $legacy -Force
'{"schemaVersion":1,"bitDepth8":true,"bitDepth10":true,"singlePublicApi":true}' |
    Set-Content -LiteralPath (Join-Path $legacy 'snow-main10-capability.json')
Require-Rejected (Join-Path $testRoot 'legacy') 'capabilities were not found'

foreach ($case in @(
    @{ Name = 'old-schema'; Values = @{ schemaVersion = 1 } },
    @{ Name = 'combined'; Values = @{ bitDepth10 = $true } },
    @{ Name = 'main10-only'; Values = @{ bitDepth8 = $false; bitDepth10 = $true } },
    @{ Name = 'main12'; Values = @{ bitDepth12 = $true } },
    @{ Name = 'dynamic-depth'; Values = @{ dynamicDepthLoading = $true } },
    @{ Name = 'unknown-dynamic-depth'; Values = @{ dynamicDepthLoading = $null } },
    @{ Name = 'no-public-api'; Values = @{ singlePublicApi = $false } },
    @{ Name = 'incomplete'; Values = @{ bitDepth12 = $null } }
)) {
    Require-Rejected (New-Profile $case.Name $case.Values) 'only 8-bit software HEVC'
}
foreach ($directory in @('lib', 'debug/lib')) {
    $prefix = New-Profile ($directory.Replace('/', '-') + '-stale-archive')
    $libraryDirectory = Join-Path $prefix $directory
    $null = New-Item -ItemType Directory -Path $libraryDirectory -Force
    $null = New-Item -ItemType File -Path (Join-Path $libraryDirectory 'x265-main10.lib')
    Require-Rejected $prefix 'depth archive remains'

    $prefix = New-Profile ($directory.Replace('/', '-') + '-stale-metadata')
    $metadataDirectory = Join-Path $prefix "$directory/pkgconfig"
    $null = New-Item -ItemType Directory -Path $metadataDirectory -Force
    'Libs.private: -lx265-main10' |
        Set-Content -LiteralPath (Join-Path $metadataDirectory 'x265.pc')
    Require-Rejected $prefix 'metadata still links'
}
Write-Output 'PASS: 8-bit profile accepted; legacy, combined, incomplete, and stale depth profiles rejected.'
Write-Output "Codec profile fixture artifacts: $testRoot"

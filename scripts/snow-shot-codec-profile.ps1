# The Windows application records SDR; standalone recording libraries can opt
# into a different dependency profile with software HDR encoding.
function Assert-SnowShotX265Profile {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Prefix)

    $capabilityPath = Join-Path $Prefix 'share/x265/snow-encoder-capabilities.json'
    if (-not (Test-Path -LiteralPath $capabilityPath -PathType Leaf)) {
        throw "The audited x265 encoder capabilities were not found: $capabilityPath. Rebuild the Snow Shot dependency profile."
    }
    $capability = Get-Content -LiteralPath $capabilityPath -Raw | ConvertFrom-Json
    if ($capability.schemaVersion -ne 2 -or
        $capability.bitDepth8 -ne $true -or
        $capability.bitDepth10 -ne $false -or
        $capability.bitDepth12 -ne $false -or
        $capability.dynamicDepthLoading -ne $false -or
        $capability.singlePublicApi -ne $true) {
        throw 'The Windows Snow Shot x265 profile must provide only 8-bit software HEVC encoding.'
    }

    foreach ($directory in @('lib', 'debug/lib')) {
        foreach ($name in @('x265-main10.lib', 'x265-main12.lib')) {
            $archive = Join-Path $Prefix "$directory/$name"
            if (Test-Path -LiteralPath $archive) {
                throw "An unsupported x265 depth archive remains in the Snow Shot prefix: $archive"
            }
        }
        $metadata = Join-Path $Prefix "$directory/pkgconfig/x265.pc"
        if ((Test-Path -LiteralPath $metadata -PathType Leaf) -and
            (Get-Content -LiteralPath $metadata -Raw) -match 'x265[-_]main(?:10|12)') {
            throw "Static x265 metadata still links an unsupported software bit depth: $metadata"
        }
    }
}

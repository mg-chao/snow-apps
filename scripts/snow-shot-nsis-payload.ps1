# Shared by the Full and Mini packaging entry points. The archive and decoder
# remain build artifacts; neither is part of the installed ownership manifest.
. (Join-Path $PSScriptRoot 'snow-shot-installer-payload.ps1')

function New-SnowShotNsisPayload {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$BuildDirectory,
        [Parameter(Mandatory)][string]$Stage,
        [Parameter(Mandatory)][string]$Archive,
        [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64'
    )

    $cache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt')
    if ($cache -notcontains 'SNOW_SHOT_USE_7Z_PAYLOAD:BOOL=ON') {
        return [pscustomobject]@{ CPackDefinitions = ''; Report = $null }
    }
    $encoders = @($cache | ForEach-Object {
        if ($_ -match '^SNOW_SHOT_PAYLOAD_ENCODER:FILEPATH=(.+)$') { $Matches[1] }
    })
    if ($encoders.Count -ne 1 -or -not (Test-Path -LiteralPath $encoders[0] -PathType Leaf)) {
        throw 'The configured installer payload encoder is unavailable. Configure the release build again.'
    }
    $extractor = Join-Path $BuildDirectory 'installer-payload/Release/snow-shot-payload-extractor.exe'
    if (-not (Test-Path -LiteralPath $extractor -PathType Leaf)) {
        throw "The installer payload extractor was not built: $extractor. Build snow-shot-payload-extractor before packaging with -SkipBuild."
    }
    $payload = New-SnowShotInstallerPayload -Stage $Stage -Archive $Archive `
        -Architecture $Architecture -Encoder $encoders[0]
    $defines = [ordered]@{
        SNOW_SHOT_PAYLOAD_ARCHIVE = [IO.Path]::GetFullPath($payload.Archive)
        SNOW_SHOT_PAYLOAD_EXTRACTOR = [IO.Path]::GetFullPath($extractor)
        SNOW_SHOT_PAYLOAD_SHA256 = $payload.Sha256
    }
    $lines = foreach ($name in $defines.Keys) {
        $value = [string]$defines[$name]
        if ($value -match '["\r\n]') { throw "Unsupported character in installer payload definition: $name" }
        # File source operands are consumed at compile time; NSIS's runtime
        # dollar escaping would change the actual filename here.
        '!define {0} "{1}"' -f $name, $value
    }
    $definitions = "`n" + ($lines -join "`n") + "`n"
    # Bracket quoting preserves NSIS variables and native Windows paths in CMake.
    $equals = '='
    while ($definitions.Contains("]$equals]")) { $equals += '=' }
    # Headers use these definitions to share one embedded archive between the
    # legacy-upgrade bootstrap and the main installation section.
    $cmake = "`nstring(PREPEND CPACK_NSIS_DEFINES [$equals[$definitions]$equals])`n"
    return [pscustomobject]@{
        CPackDefinitions = $cmake
        Report = [ordered]@{
            Format = '7z'
            Bytes = $payload.Bytes
            Sha256 = $payload.Sha256
            Methods = $payload.Methods
            EncoderVersion = $payload.EncoderVersion
            EncoderSha256 = $payload.EncoderSha256
            FileCount = $payload.FileCount
            UnpackedBytes = $payload.UnpackedBytes
            ExtractorBytes = (Get-Item -LiteralPath $extractor).Length
            ExtractorSha256 = (Get-FileHash -LiteralPath $extractor -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}

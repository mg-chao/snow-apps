[CmdletBinding()]
param(
    [string]$ComponentsHeader = "",
    [string]$LinkMap = "",
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [switch]$ComponentsOnly
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot 'snow-shot-codec-profile.ps1')
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot "package-snow-shot.ps1"), [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw "The packaging script contains syntax errors." }

# Import only the shared policy function, without executing release actions.
$definition = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Get-SnowShotExpectedFfmpegComponents'
}, $true))
if ($definition.Count -ne 1) { throw 'Expected one FFmpeg component policy function.' }
. ([scriptblock]::Create($definition[0].Extent.Text))
$components = Get-SnowShotExpectedFfmpegComponents -Architecture $Architecture
if ([string]::IsNullOrWhiteSpace($ComponentsHeader)) {
    $ComponentsHeader = Join-Path $repoRoot (
        ".tools/vcpkg/installed/static/$Architecture-windows-static/share/ffmpeg/snow-shot-config-components.h")
}
$actual = @(Get-Content -LiteralPath $ComponentsHeader | ForEach-Object {
    if ($_ -match '^#define CONFIG_(.+)_(BSF|DECODER|ENCODER|HWACCEL|PARSER|DEMUXER|MUXER|PROTOCOL|FILTER|INDEV|OUTDEV) 1$') {
        "ff_$($Matches[1].ToLowerInvariant())_$($Matches[2].ToLowerInvariant())"
    }
} | Sort-Object)
if ($actual.Count -eq 0) { throw "No enabled FFmpeg components found in $ComponentsHeader." }
$expected = @(foreach ($kind in $components.Keys) {
    foreach ($name in $components[$kind]) {
        "ff_$($name.ToLowerInvariant())_$($kind.ToLowerInvariant())"
    }
})
$expected = @($expected | Sort-Object)
$difference = @(Compare-Object -ReferenceObject $actual -DifferenceObject $expected)
if ($difference.Count -gt 0) {
    throw "FFmpeg dependency audit differs from the compiled component set: $($difference | Out-String)"
}
$prefix = Split-Path (Split-Path (Split-Path $ComponentsHeader -Parent) -Parent) -Parent
Assert-SnowShotX265Profile -Prefix $prefix
if ($ComponentsOnly) {
    Write-Output "FFmpeg component contract verified: $($actual.Count) compiled components and 8-bit software HEVC."
    return
}
if ([string]::IsNullOrWhiteSpace($LinkMap)) {
    $preset = if ($Architecture -eq 'arm64') { 'snow-shot-msvc-arm64-release' } else { 'snow-shot-msvc-release' }
    $LinkMap = Join-Path $repoRoot "build/$preset/snow_shot/Release/snow_shot.map"
}
if (Select-String -LiteralPath $LinkMap -Pattern 'x265_(?:10|12)bit|x265-main(?:10|12)\.lib' -Quiet) {
    throw 'The Snow Shot link map retains an unsupported software HEVC bit depth.'
}
$linked = @(Select-String -LiteralPath $LinkMap -Pattern (
    '^\s+[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+(ff_[A-Za-z0-9_]+_(?:bsf|decoder|encoder|hwaccel|parser|demuxer|muxer|protocol)|ff_(?:af|asrc|asink|vf|vsrc|vsink|avf|vaf)_[A-Za-z0-9_]+)\s+[0-9A-Fa-f]+\s{2,}\S'
) | ForEach-Object {
    $registration = $_.Matches[0].Groups[1].Value
    switch ($registration) {
        'ff_asrc_abuffer' { 'ff_abuffer_filter' }
        'ff_asink_abuffer' { 'ff_abuffersink_filter' }
        'ff_af_atempo' { 'ff_atempo_filter' }
        'ff_vsrc_buffer' { 'ff_buffer_filter' }
        'ff_vsink_buffer' { 'ff_buffersink_filter' }
        default { $registration }
    }
} | Sort-Object)
$expected = @($expected + @(
    'ff_abuffer_filter', 'ff_abuffersink_filter', 'ff_buffer_filter', 'ff_buffersink_filter') |
    Sort-Object)
if (@(Compare-Object $linked $expected).Count -gt 0) {
    throw "The link-map audit does not match the optimized executable registrations."
}
Write-Output "FFmpeg contract verified: $($actual.Count) compiled components and $($linked.Count) linked registrations."

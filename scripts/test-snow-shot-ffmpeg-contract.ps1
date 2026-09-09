[CmdletBinding()]
param([string]$ComponentsHeader = "")

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot "package-snow-shot.ps1"), [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw "The packaging script contains syntax errors." }

function Read-LiteralAssignment {
    param([string]$Name)
    $assignments = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and
        $node.Left.VariablePath.UserPath -eq $Name
    }, $true))
    if ($assignments.Count -ne 1) { throw "Expected one literal assignment for $Name." }
    $literal = $assignments[0].Right.Find({
        param($node)
        $node -is [System.Management.Automation.Language.HashtableAst] -or
        $node -is [System.Management.Automation.Language.ArrayExpressionAst]
    }, $true)
    if ($null -eq $literal) { throw "Expected a literal collection for $Name." }
    return $literal.SafeGetValue()
}

$components = Read-LiteralAssignment "expectedFfmpegComponents"
$registrations = Read-LiteralAssignment "expectedFfmpegRegistrations"
if ([string]::IsNullOrWhiteSpace($ComponentsHeader)) {
    $ComponentsHeader = Join-Path $repoRoot (
        ".tools/vcpkg/installed/static/x64-windows-static/share/ffmpeg/snow-shot-config-components.h")
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
foreach ($audit in @(
    @{ Name = "dependency"; Symbols = $expected },
    @{ Name = "registration configuration"; Symbols = @(
        $registrations
        $components.PARSER | ForEach-Object { "ff_$($_.ToLowerInvariant())_parser" }
    ) | Sort-Object }
)) {
    $difference = @(Compare-Object -ReferenceObject $actual -DifferenceObject $audit.Symbols)
    if ($difference.Count -gt 0) {
        throw "FFmpeg $($audit.Name) audit differs from the compiled component set: $($difference | Out-String)"
    }
}
$linkMap = Join-Path $repoRoot "build/snow-shot-msvc-release/snow_shot/Release/snow_shot.map"
$linked = @(Select-String -LiteralPath $linkMap -Pattern (
    '^\s+[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+(ff_[A-Za-z0-9_]+_(?:bsf|decoder|encoder|hwaccel|parser|demuxer|muxer|protocol))\s+[0-9A-Fa-f]+\s{2,}\S'
) | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object)
if (@(Compare-Object $linked @($registrations | Sort-Object)).Count -gt 0) {
    throw "The link-map audit does not match the optimized executable registrations."
}
Write-Output "FFmpeg contract verified: $($actual.Count) compiled components and $($linked.Count) linked registrations."

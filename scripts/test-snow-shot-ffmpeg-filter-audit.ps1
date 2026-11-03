#Requires -Version 7.2
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$testRoot = Join-Path $repoRoot ('build/ffmpeg-filter-audit-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $testRoot
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'package-snow-shot.ps1'), [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'The packaging script must parse successfully.' }
foreach ($name in @('Assert-ExactStringSet', 'Get-SnowShotExpectedFfmpegComponents')) {
    $function = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (-not $function) { throw "Missing packaging audit function: $name" }
    Invoke-Expression $function.Extent.Text
}

# Execute the actual optimized-executable audit against synthetic MSVC link maps.
$auditStatements = [Collections.Generic.List[string]]::new()
$collect = $false
foreach ($statement in $ast.EndBlock.Statements) {
    if ($statement -is [Management.Automation.Language.AssignmentStatementAst] -and
        $statement.Left.Extent.Text -eq '$ffmpegRegistrationPattern') { $collect = $true }
    if ($collect) { $auditStatements.Add($statement.Extent.Text) }
    if ($collect -and $statement.Extent.Text -match
        '^Assert-ExactStringSet -Description "Linked FFmpeg component registrations"') { break }
}
if ($auditStatements.Count -ne 5) { throw 'The FFmpeg registration audit was not found.' }
$audit = [scriptblock]::Create($auditStatements -join "`n")
$Architecture = 'x64'
$linkMapPath = Join-Path $testRoot 'fixture.map'
$symbols = @(foreach ($entry in (Get-SnowShotExpectedFfmpegComponents).GetEnumerator()) {
    foreach ($name in $entry.Value) {
        if ($entry.Key -eq 'FILTER') {
            switch ($name) {
                'ABUFFER' { 'ff_asrc_abuffer' }
                'ABUFFERSINK' { 'ff_asink_abuffer' }
                'ATEMPO' { 'ff_af_atempo' }
            }
        } else { "ff_$($name.ToLowerInvariant())_$($entry.Key.ToLowerInvariant())" }
    }
})
$symbols += @('ff_asrc_abuffer', 'ff_asink_abuffer', 'ff_vsrc_buffer', 'ff_vsink_buffer')
function Test-FilterAudit {
    param([string]$Name, [string[]]$Registrations, [bool]$ShouldPass)
    $lines = @($Registrations | ForEach-Object { " 0002:00000000 $_ 0000000140001000  avfilter.lib:filter.obj" })
    [IO.File]::WriteAllLines($linkMapPath, $lines, [Text.UTF8Encoding]::new($false))
    $failure = $null
    try { & $audit } catch { $failure = $_.Exception.Message }
    if ($ShouldPass -and $failure) { throw "Valid $Name audit failed: $failure" }
    if (-not $ShouldPass -and -not $failure) { throw "Invalid $Name audit was accepted." }
    if ($failure -and $failure -notlike 'Linked FFmpeg component registrations*') {
        throw "Unexpected $Name failure: $failure"
    }
    Write-Output "PASS: $Name"
}
try {
    Test-FilterAudit actual-audio-filter-symbols $symbols $true
    Test-FilterAudit unexpected-ffmpeg-gif-encoder @($symbols + 'ff_gif_encoder') $false
    Test-FilterAudit missing-tempo @($symbols | Where-Object { $_ -ne 'ff_af_atempo' }) $false
    Test-FilterAudit missing-audio-source @($symbols | Where-Object { $_ -ne 'ff_asrc_abuffer' }) $false
    Test-FilterAudit unexpected-video-filter @($symbols + 'ff_vf_scale') $false
    Test-FilterAudit unexpected-audio-video-filter @($symbols + 'ff_avf_avectorscope') $false
} finally {
    $resolved = [IO.Path]::GetFullPath($testRoot)
    $allowed = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build')).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe FFmpeg filter fixture cleanup.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

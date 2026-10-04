# Execute the production Full and Mini import audits with deterministic PE inspection fixtures.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'package-snow-shot.ps1'))
$start = $source.IndexOf('$debugRuntimeImports = ')
$end = $source.IndexOf('Write-Output "PE dependency audit:', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Full PE audit was not found.' }
$fullAudit = [scriptblock]::Create($source.Substring($start, $end - $start))
$policyStart = $source.IndexOf('$allowedSystemImports = @(')
$policyEnd = $source.IndexOf('$allowedLocalImports = ', $policyStart)
$policy = [scriptblock]::Create($source.Substring($policyStart, $policyEnd - $policyStart))
$miniSource = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'package-snow-shot-mini.ps1'))
$miniStart = $miniSource.IndexOf('    foreach ($binary in @(Get-ChildItem')
$miniEnd = $miniSource.IndexOf("    & (Join-Path `$PSScriptRoot 'collect-snow-shot-symbols.ps1')", $miniStart)
if ($miniStart -lt 0 -or $miniEnd -le $miniStart) { throw 'Mini PE audit was not found.' }
$miniAudit = [scriptblock]::Create($miniSource.Substring($miniStart, $miniEnd - $miniStart))
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('snow-pe-audit-' + [guid]::NewGuid().ToString('N'))
$windowsSystemDirectory = Join-Path $fixture 'system'
$stagedBinDirectory = Join-Path $fixture 'bin'
$miniInstall = $fixture
$script:DumpbinPath = 'Invoke-FixtureDumpbin'
function Invoke-FixtureDumpbin {
    $global:LASTEXITCODE = 0
    return @($script:FixtureImports | ForEach-Object { "    $_" })
}
function Invoke-FixtureAudit([string]$Edition) {
    if ($Edition -ceq 'Full') { . $fullAudit }
    else { . $policy; . $miniAudit }
}
function Require-Rejection([string]$Edition, [string]$Message) {
    try { Invoke-FixtureAudit $Edition } catch { return }
    throw $Message
}
try {
    $null = New-Item -ItemType Directory -Path $windowsSystemDirectory, $stagedBinDirectory
    [IO.File]::WriteAllText((Join-Path $stagedBinDirectory 'snow_shot.exe'), 'PE fixture')
    $stagedBinaries = @(Get-Item -LiteralPath (Join-Path $stagedBinDirectory 'snow_shot.exe'))
    foreach ($name in @('mscms.dll', 'kernel32.dll', 'unexpected.dll', 'ucrtbased.dll', 'vcruntime140.dll')) {
        [IO.File]::WriteAllText((Join-Path $windowsSystemDirectory $name), 'system fixture')
    }
    foreach ($edition in @('Full', 'Mini')) {
        foreach ($imports in @(@('mscms.dll'), @('MSCMS.DLL', 'kernel32.dll'), @('api-ms-win-core-file-l1-1-0.dll'))) {
            $script:FixtureImports = $imports
            Invoke-FixtureAudit $edition
        }
        foreach ($name in @('unexpected.dll', 'ucrtbased.dll', 'vcruntime140.dll')) {
            $script:FixtureImports = @($name)
            Require-Rejection $edition "$edition accepted a disallowed import: $name"
        }
    }
    Remove-Item -LiteralPath (Join-Path $windowsSystemDirectory 'mscms.dll')
    $script:FixtureImports = @('mscms.dll')
    foreach ($edition in @('Full', 'Mini')) {
        Require-Rejection $edition "$edition accepted a missing system color-management library."
    }
    Write-Output 'PASS: Full and Mini permit system color management and reject missing, unknown, and CRT dependencies.'
} finally {
    if (Test-Path -LiteralPath $fixture) {
        $resolved = [IO.Path]::GetFullPath($fixture)
        $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Unsafe PE test fixture cleanup path.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

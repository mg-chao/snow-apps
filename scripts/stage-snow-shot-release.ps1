[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [string]$BuildDirectory,
    [string]$InstallDirectory,
    [string]$OcrRuntimeArchive,
    [ValidateRange(1, 256)][int]$Parallelism = 4,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
if ($Architecture -eq 'arm64') {
    if (-not $PSBoundParameters.ContainsKey('BuildDirectory')) { $BuildDirectory = 'build/snow-shot-msvc-arm64-release' }
    if (-not $PSBoundParameters.ContainsKey('InstallDirectory')) { $InstallDirectory = 'artifacts/windows-arm64/snow-shot' }
}

# Keep this entry point as a thin, named-parameter wrapper.  A
# ValueFromRemainingArguments parameter cannot preserve a named switch such as
# -SkipBuild: PowerShell binds it as the first positional argument instead,
# making the package script interpret "-SkipBuild" as BuildDirectory.
$packageScript = Join-Path $PSScriptRoot "package-snow-shot.ps1"
$forwardedParameters = @{
    Architecture = $Architecture
    BuildDirectory = $BuildDirectory
    InstallDirectory = $InstallDirectory
    Parallelism = $Parallelism
    OcrRuntimeArchive = $OcrRuntimeArchive
}
if ($SkipBuild) {
    $forwardedParameters.SkipBuild = $true
}

& $packageScript @forwardedParameters
exit $LASTEXITCODE

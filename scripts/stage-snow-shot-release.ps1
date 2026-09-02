[CmdletBinding()]
param(
    [string]$BuildDirectory = "build\snow-shot-msvc-release",
    [string]$InstallDirectory = "artifacts\snow-shot",
    [ValidateRange(1, 256)][int]$Parallelism = 4,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

# Keep this entry point as a thin, named-parameter wrapper.  A
# ValueFromRemainingArguments parameter cannot preserve a named switch such as
# -SkipBuild: PowerShell binds it as the first positional argument instead,
# making the package script interpret "-SkipBuild" as BuildDirectory.
$packageScript = Join-Path $PSScriptRoot "package-snow-shot.ps1"
$forwardedParameters = @{
    BuildDirectory = $BuildDirectory
    InstallDirectory = $InstallDirectory
    Parallelism = $Parallelism
}
if ($SkipBuild) {
    $forwardedParameters.SkipBuild = $true
}

& $packageScript @forwardedParameters
exit $LASTEXITCODE

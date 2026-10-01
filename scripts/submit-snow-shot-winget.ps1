#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Tag,
    [ValidateSet('Full', 'Mini')][string]$Edition = 'Full',
    [Parameter(Mandatory)][string]$ManifestDirectory,
    [string]$WingetCreate = 'wingetcreate.exe'
)
. (Join-Path $PSScriptRoot 'snow-shot-winget.ps1')
Submit-SnowShotWingetManifest -Edition $Edition -Tag $Tag -ManifestDirectory $ManifestDirectory -WingetCreate $WingetCreate

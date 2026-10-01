#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Tag,
    [ValidateSet('Full', 'Mini')][string]$Edition = 'Full',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../build/winget')
)
. (Join-Path $PSScriptRoot 'snow-shot-winget.ps1')
New-SnowShotWingetManifest -Edition $Edition -Tag $Tag -OutputDirectory $OutputDirectory

#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$Tag,
    [string]$OutputPath = (Join-Path $PSScriptRoot '../build/scoop/snowshot.json')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-scoop.ps1')
New-SnowShotScoopManifest -Tag $Tag -OutputPath $OutputPath

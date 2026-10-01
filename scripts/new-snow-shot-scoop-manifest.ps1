#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$Tag,
    [ValidateSet('Full', 'Mini')][string]$Edition = 'Full',
    [string]$OutputPath = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-scoop.ps1')
if (-not $OutputPath) { $product = Get-SnowShotEdition $Edition; $OutputPath = Join-Path $PSScriptRoot "../build/scoop/$($product.Scoop).json" }
New-SnowShotScoopManifest -Edition $Edition -Tag $Tag -OutputPath $OutputPath

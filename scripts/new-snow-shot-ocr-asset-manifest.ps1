#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OcrRuntimeArchive,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'arm64',
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-ocr-release-runtime.ps1')
New-SnowOcrAssetManifest -ArchivePath $OcrRuntimeArchive -Architecture $Architecture -OutputPath $Output

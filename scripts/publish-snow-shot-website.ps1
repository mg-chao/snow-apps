[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Version,
    [string]$WebsiteDirectory = 'D:/snow-apps-site',
    [string]$ServerHost = '120.79.232.67',
    [string]$ServerUser = 'root',
    [ValidateRange(1, 65535)][int]$ServerPort = 22,
    [string]$IdentityFile,
    [string]$KnownHostsFile,
    [string]$RemoteWebRoot = '/var/www/html',
    [string]$PublicBaseUrl = 'https://snowshot.top'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$sourceVersion = [regex]::Match([IO.File]::ReadAllText((Join-Path $repo 'CMakeLists.txt')),
    'set\(SNOW_SHOT_VERSION "([^"]+)"\)').Groups[1].Value
if (-not $sourceVersion) { throw 'Cannot read SNOW_SHOT_VERSION.' }
if (-not $Version) { $Version = $sourceVersion }
if ($Version -cne $sourceVersion) { throw 'The website target must match SNOW_SHOT_VERSION.' }
$workflow = Join-Path $WebsiteDirectory 'scripts/publish-release.ps1'
if (-not (Test-Path -LiteralPath $workflow -PathType Leaf)) { throw "Website release workflow is missing: $workflow" }
$settings = @{ Version = $Version; ServerHost = $ServerHost; ServerUser = $ServerUser;
    ServerPort = $ServerPort; IdentityFile = $IdentityFile; KnownHostsFile = $KnownHostsFile;
    RemoteWebRoot = $RemoteWebRoot; PublicBaseUrl = $PublicBaseUrl }
& $workflow @settings -WhatIf:$WhatIfPreference

# Copy to publish-snow-shot-release.local.ps1 (ignored by Git), then fill local settings.
[CmdletBinding()]
param([ValidateSet('Publish', 'Verify', 'Rollback')][string]$Operation = 'Publish', [switch]$SkipBuild, [switch]$AuditOnly, [switch]$WhatIf)
$releaseSettings = @{
    ServerHost = 'YOUR_SSH_HOST'
    ServerUser = 'YOUR_SSH_USER'
    IdentityFile = 'C:/private/ssh-key'
    KnownHostsFile = 'C:/private/known_hosts'
    SigningKeyPath = 'C:/private/snow-shot-release/private.pem'
    RemoteWebRoot = '/var/www/html'
    PublicBaseUrl = 'https://YOUR_PUBLIC_HOST'
}
& "$PSScriptRoot/publish-snow-shot-release.ps1" @releaseSettings -Operation $Operation -SkipBuild:$SkipBuild -AuditOnly:$AuditOnly -WhatIf:$WhatIf

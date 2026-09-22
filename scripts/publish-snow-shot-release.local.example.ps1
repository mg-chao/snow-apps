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
    # With MacHost configured, both platforms package concurrently and publish together.
    MacHost = 'YOUR_MAC_SSH_HOST'
    MacUser = 'YOUR_MAC_SSH_USER'
    MacProjectDirectory = '/Users/YOUR_USER/workspaces/snow-apps'
    # Optional; otherwise use your local OpenSSH config/agent and known_hosts.
    # MacIdentityFile = 'C:/private/mac-ssh-key'
    # MacKnownHostsFile = 'C:/private/known_hosts'
}
& "$PSScriptRoot/publish-snow-shot-release.ps1" @releaseSettings -Operation $Operation -SkipBuild:$SkipBuild -AuditOnly:$AuditOnly -WhatIf:$WhatIf

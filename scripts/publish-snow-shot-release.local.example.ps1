# Copy to publish-snow-shot-release.local.ps1 (ignored by Git), then fill local settings.
[CmdletBinding()]
param([ValidateSet('Publish', 'Verify')][string]$Operation = 'Publish', [switch]$SkipBuild, [switch]$AuditOnly, [switch]$SkipGitee, [switch]$DeployWebsite, [string]$ReleaseNotesPath, [switch]$WhatIf)
$releaseSettings = @{
    GitHubRepository = 'mg-chao/snow-apps'
    SigningKeyPath = 'C:/private/snow-shot-release/private.pem'
    # With MacHost configured, both platforms package concurrently and publish together.
    MacHost = 'YOUR_MAC_SSH_HOST'
    MacUser = 'YOUR_MAC_SSH_USER'
    MacProjectDirectory = '/Users/YOUR_USER/workspaces/snow-apps'
    # Full for both architectures; Mini is packaged for ARM64 only.
    MacArchitectures = @('arm64', 'x64')
    WebsiteDirectory = 'D:/snow-apps-site'
    # Optional; otherwise use your local OpenSSH config/agent and known_hosts.
    # MacIdentityFile = 'C:/private/mac-ssh-key'
    # MacKnownHostsFile = 'C:/private/known_hosts'
}
# GITEE_TOKEN is read locally; never store it in this file or GitHub release workflows.
& "$PSScriptRoot/publish-snow-shot-release.ps1" @releaseSettings -Operation $Operation -SkipBuild:$SkipBuild -AuditOnly:$AuditOnly -SkipGitee:$SkipGitee -DeployWebsite:$DeployWebsite -ReleaseNotesPath $ReleaseNotesPath -WhatIf:$WhatIf

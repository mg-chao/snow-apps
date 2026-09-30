# GitHub release publication helpers. Dot-source for deterministic command-mocked tests.
function Invoke-SnowGitHub([string[]]$Arguments) {
    $output = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw "GitHub command failed: $($Arguments[0])" }
    return $output
}

function Get-SnowGitHubRelease([string]$Repository, [string]$Version) {
    $pages = (Invoke-SnowGitHub @('api', "repos/$Repository/releases?per_page=100", '--paginate', '--slurp')) |
        ConvertFrom-Json -AsHashtable
    $matches = @($pages | ForEach-Object { $_ } | Where-Object { $_.tag_name -ceq "v${Version}_snow-shot" })
    if ($matches.Count -gt 1) { throw 'Ambiguous GitHub release tag.' }
    if ($matches.Count -eq 1) { return $matches[0] }
    return $null
}

function Get-SnowGitHubAsset([string]$Repository, [string]$Version, [string]$Name, [string]$Directory) {
    $null = New-Item -ItemType Directory -Force -Path $Directory
    Invoke-SnowGitHub @('release', 'download', "v${Version}_snow-shot", '--repo', $Repository,
        '--pattern', $Name, '--dir', $Directory, '--clobber') | Out-Null
    return Join-Path $Directory $Name
}

function Test-SnowGitHubBytes([string]$Expected, [string]$Downloaded) {
    if ((Get-Item -LiteralPath $Expected).Length -ne (Get-Item -LiteralPath $Downloaded).Length -or
        (Get-FileHash -LiteralPath $Expected -Algorithm SHA256).Hash -cne
        (Get-FileHash -LiteralPath $Downloaded -Algorithm SHA256).Hash) {
        throw "GitHub asset differs from audited bytes: $([IO.Path]::GetFileName($Expected)). Increase the version; assets are never overwritten."
    }
}

function Publish-SnowGitHubRelease([string]$Repository, [string]$Version, [hashtable]$Assets,
    [string]$VerificationDirectory, [switch]$VerifyOnly, [string]$NotesPath) {
    $release = Get-SnowGitHubRelease $Repository $Version
    $tag = "v${Version}_snow-shot"
    $prerelease = ($Version -split '\+')[0].Contains('-')
    if ($NotesPath) {
        $notes = [IO.File]::ReadAllText($NotesPath)
        if ($release -and -not $release.draft -and $release.body.Trim() -cne $notes.Trim()) {
            throw 'Published GitHub release notes differ from the local release.'
        }
    }
    if ($release -and [bool]$release.prerelease -ne $prerelease) { throw 'GitHub prerelease classification does not match the version.' }
    # Validate every existing asset before uploading anything, including CI-created drafts.
    foreach ($name in $Assets.Keys) {
        $existing = @(if ($release) { $release.assets | Where-Object { $_.name -ceq $name } })
        if ($existing.Count -gt 1) { throw "Duplicate GitHub asset: $name" }
        if ($existing.Count -eq 1) {
            $download = Get-SnowGitHubAsset $Repository $Version $name $VerificationDirectory
            Test-SnowGitHubBytes $Assets[$name] $download
        } elseif ($VerifyOnly -or ($release -and -not $release.draft)) {
            throw "Published GitHub release is missing $name."
        }
    }
    if ($VerifyOnly) {
        if (-not $release -or $release.draft) { throw 'Expected a published GitHub release.' }
        return
    }
    if (-not $release) {
        $flags = if ($prerelease) { @('--prerelease') } else { @() }
        $notesFlags = if ($NotesPath) { @('--notes-file', $NotesPath) } else { @('--generate-notes') }
        Invoke-SnowGitHub (@('release', 'create', $tag, '--repo', $Repository, '--verify-tag', '--draft',
            '--title', "Snow Shot $Version") + $notesFlags + $flags) | Out-Null
        $release = Get-SnowGitHubRelease $Repository $Version
        if (-not $release -or -not $release.draft) { throw 'Could not confirm the GitHub draft.' }
    }
    foreach ($name in $Assets.Keys) {
        if (@($release.assets | Where-Object { $_.name -ceq $name }).Count -eq 0) {
            Invoke-SnowGitHub @('release', 'upload', $tag, $Assets[$name], '--repo', $Repository) | Out-Null
        }
    }
    # Download every asset again before exposing the release to update clients.
    foreach ($name in $Assets.Keys) {
        $download = Get-SnowGitHubAsset $Repository $Version $name $VerificationDirectory
        Test-SnowGitHubBytes $Assets[$name] $download
    }
    if ($release.draft) {
        if ($NotesPath) { Invoke-SnowGitHub @('release', 'edit', $tag, '--repo', $Repository, '--notes-file', $NotesPath) | Out-Null }
        $latest = if ($prerelease) { '--latest=false' } else { '--latest' }
        Invoke-SnowGitHub @('release', 'edit', $tag, '--repo', $Repository, '--draft=false', $latest) | Out-Null
    }
    Write-Output "Published and verified GitHub release $Repository $tag."
}

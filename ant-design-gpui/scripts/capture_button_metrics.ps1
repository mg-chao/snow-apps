# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Execute upstream Button styles in headless Edge and save independent computed-style fixtures.
.EXAMPLE
./scripts/capture_button_metrics.ps1 -Upstream E:/ant-design -PnpmStore E:/.pnpm-store/v10
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Upstream,
    [Parameter(Mandatory = $true)][string]$PnpmStore,
    [string]$EdgePath = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    [string]$OutputPath
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $crateRoot
$captureRoot = Join-Path $repositoryRoot 'build/ant-design-gpui/button-reference'
if (-not $OutputPath) { $OutputPath = Join-Path $crateRoot 'fixtures/button/reference.json' }
if (-not (Test-Path -LiteralPath $EdgePath -PathType Leaf)) {
    throw "Headless Edge was not found: $EdgePath"
}
$generator = Join-Path $PSScriptRoot 'generate_button_reference.mjs'
& node $generator --upstream $Upstream --pnpm-store $PnpmStore --output $captureRoot
if ($LASTEXITCODE -ne 0) { throw 'Upstream Button reference generation failed.' }
$sourceProvenance = Get-Content -LiteralPath (Join-Path $captureRoot 'provenance.json') -Raw |
    ConvertFrom-Json -AsHashtable
$cases = [System.Collections.Generic.List[object]]::new()
$captureId = [Guid]::NewGuid().ToString('N')
foreach ($theme in @('default', 'dark', 'compact', 'dark-compact')) {
    foreach ($state in @('rest', 'hover', 'active')) {
        $label = "$theme-$state"
        $htmlPath = Join-Path $captureRoot "button-reference-$theme.html"
        $domPath = Join-Path $captureRoot "metrics-$label.dom.html"
        $errorPath = Join-Path $captureRoot "metrics-$label.stderr.txt"
        $profilePath = Join-Path $captureRoot "headless-metrics-$captureId-$label"
        $url = ([Uri]::new($htmlPath)).AbsoluteUri
        if ($state -ne 'rest') { $url += "?state=$state" }
        $arguments = @(
            '--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars',
            '--disable-extensions', '--disable-background-networking',
            '--disable-component-update', '--force-device-scale-factor=1',
            '--window-size=1180,1040', '--virtual-time-budget=1000', '--dump-dom',
            ('--user-data-dir="' + $profilePath + '"'), $url
        )
        # The profile belongs solely to this process; no signed-in browser state is read.
        $process = Start-Process -FilePath $EdgePath -ArgumentList $arguments -PassThru `
            -WindowStyle Hidden -RedirectStandardOutput $domPath -RedirectStandardError $errorPath
        try {
            $deadline = [DateTime]::UtcNow.AddSeconds(20)
            $match = $null
            do {
                Start-Sleep -Milliseconds 100
                if (Test-Path -LiteralPath $domPath) {
                    $html = Get-Content -LiteralPath $domPath -Raw
                    if ($html) {
                        $candidate = [regex]::Match(
                            $html, '<script type="application/json" id="reference-metrics">([^<]+)</script>')
                        if ($candidate.Success) { $match = $candidate }
                    }
                }
                $process.Refresh()
            } while ($null -eq $match -and [DateTime]::UtcNow -lt $deadline -and -not $process.HasExited)
            if ($null -eq $match) {
                throw "Headless Edge did not emit metrics for $label. See $errorPath"
            }
            $buttons = @($match.Groups[1].Value | ConvertFrom-Json -AsHashtable)
            if ($buttons.Count -lt 50) { throw "Incomplete Button metrics for $label" }
            $cases.Add([ordered]@{ theme = $theme; state = $state; buttons = $buttons })
            Write-Host "Captured original computed styles: $label ($($buttons.Count) controls)"
        } finally {
            $process.Refresh()
            if (-not $process.HasExited) {
                # Headless Edge can leave its main process alive after printing DOM.
                # Kill only the process tree created above for this capture.
                $process.Kill($true)
                $process.WaitForExit(5000) | Out-Null
            }
        }
    }
}
$fixture = [ordered]@{
    provenance = [ordered]@{
        schemaVersion = 1
        upstream = $sourceProvenance.upstream
        renderer = [ordered]@{
            name = 'Microsoft Edge (Chromium)'
            version = (Get-Item -LiteralPath $EdgePath).VersionInfo.ProductVersion
            platform = 'Windows'
            scaleFactor = 1
            viewport = @(1180, 1040)
            motion = 'disabled'
        }
        generator = [ordered]@{
            path = 'scripts/generate_button_reference.mjs'
            sha256 = (Get-FileHash -LiteralPath $generator -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        capture = [ordered]@{
            path = 'scripts/capture_button_metrics.ps1'
            sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        packages = $sourceProvenance.packages
        sources = $sourceProvenance.sources
        themeFixtureSha256 = $sourceProvenance.fixtureSha256
        notes = $sourceProvenance.notes
    }
    cases = $cases.ToArray()
}
$parentPath = Split-Path -Parent $OutputPath
[System.IO.Directory]::CreateDirectory($parentPath) | Out-Null
$json = ($fixture | ConvertTo-Json -Depth 50) -replace "`r`n", "`n"
[System.IO.File]::WriteAllText($OutputPath, $json + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "Saved Button reference fixture: $OutputPath"

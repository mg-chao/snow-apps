# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Execute live rc-trigger placement in headless Edge and save independent geometry fixtures.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Upstream,
    [string]$EdgePath = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    [string]$OutputPath
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$captureRoot = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/popup-placement-reference'
if (-not $OutputPath) { $OutputPath = Join-Path $crateRoot 'fixtures/popup/placement.json' }
if (-not (Test-Path -LiteralPath $EdgePath -PathType Leaf)) { throw "Headless Edge was not found: $EdgePath" }
$generator = Join-Path $PSScriptRoot 'generate_popup_placement.mjs'
& node $generator --upstream $Upstream --output $captureRoot
if ($LASTEXITCODE -ne 0) { throw 'Upstream popup reference generation failed.' }
$sourceProvenance = Get-Content -LiteralPath (Join-Path $captureRoot 'provenance.json') -Raw | ConvertFrom-Json -AsHashtable
$cases = [System.Collections.Generic.List[object]]::new()
$captureId = [Guid]::NewGuid().ToString('N')
foreach ($case in $sourceProvenance.batches) {
    $label = $case.name
    $htmlPath = Join-Path $captureRoot "$label.html"
    $domPath = Join-Path $captureRoot "metrics-$label.dom.html"
    $errorPath = Join-Path $captureRoot "metrics-$label.stderr.txt"
    $profilePath = Join-Path $captureRoot "headless-metrics-$captureId-$label"
    $arguments = @(
        '--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars',
        '--disable-extensions', '--disable-background-networking', '--disable-component-update',
        '--force-device-scale-factor=1', '--window-size=400,407', '--virtual-time-budget=5000',
        '--dump-dom', ('--user-data-dir="' + $profilePath + '"'), ([Uri]::new($htmlPath)).AbsoluteUri
    )
    $process = Start-Process -FilePath $EdgePath -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $domPath -RedirectStandardError $errorPath
    try {
        if (-not $process.WaitForExit(30000)) { throw "Capture timeout: $label" }
        # Drain redirected output before parsing. Reading a file while its old
        # contents are being replaced can combine two otherwise valid dumps.
        $process.WaitForExit()
        $html = Get-Content -LiteralPath $domPath -Raw
        $match = [regex]::Match($html, '<script(?=[^>]*\bid="reference-placement")[^>]*>([^<]+)</script>')
        if (-not $match.Success) { throw "Headless Edge did not emit metrics for $label. See $errorPath" }
        $popups = @($match.Groups[1].Value | ConvertFrom-Json -AsHashtable)
        if ($popups.Count -ne $case.count) { throw "Incomplete popup metrics for $label" }
        foreach ($popup in $popups) { $cases.Add($popup) }
        Write-Host "Captured live placement: $label ($($popups.Count) controls)"
    } finally {
        $process.Refresh()
        if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit(5000) | Out-Null }
    }
}
$fixture = [ordered]@{
    provenance = [ordered]@{
        schemaVersion = 1
        upstream = $sourceProvenance.upstream
        renderer = [ordered]@{
            name = 'Microsoft Edge (Chromium)'
            version = (Get-Item -LiteralPath $EdgePath).VersionInfo.ProductVersion
            platform = 'Windows'; scaleFactor = 1; requestedWindowSize = @(400, 407); motion = 'disabled'
        }
        generator = [ordered]@{ path = 'scripts/generate_popup_placement.mjs'; sha256 = (Get-FileHash -LiteralPath $generator -Algorithm SHA256).Hash.ToLowerInvariant() }
        capture = [ordered]@{ path = 'scripts/capture_popup_placement.ps1'; sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant() }
        packages = $sourceProvenance.packages
        sources = $sourceProvenance.sources
        packageLockSha256 = $sourceProvenance.packageLockSha256
        bundleSha256 = $sourceProvenance.bundleSha256
        notes = $sourceProvenance.notes
    }
    cases = $cases.ToArray()
}
[System.IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
$json = ($fixture | ConvertTo-Json -Depth 50) -replace "`r`n", "`n"
[System.IO.File]::WriteAllText($OutputPath, $json + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "Saved live popup placement fixture: $OutputPath"

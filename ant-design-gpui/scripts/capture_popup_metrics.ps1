# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Execute original popup styles in headless Edge and save independent computed-style fixtures.
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
$captureRoot = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui/popup-reference'
if (-not $OutputPath) { $OutputPath = Join-Path $crateRoot 'fixtures/popup/reference.json' }
if (-not (Test-Path -LiteralPath $EdgePath -PathType Leaf)) { throw "Headless Edge was not found: $EdgePath" }
$generator = Join-Path $PSScriptRoot 'generate_popup_reference.mjs'
& node $generator --upstream $Upstream --pnpm-store $PnpmStore --output $captureRoot
if ($LASTEXITCODE -ne 0) { throw 'Upstream popup reference generation failed.' }
$sourceProvenance = Get-Content -LiteralPath (Join-Path $captureRoot 'provenance.json') -Raw | ConvertFrom-Json -AsHashtable
$cases = [System.Collections.Generic.List[object]]::new()
$captureId = [Guid]::NewGuid().ToString('N')
foreach ($case in $sourceProvenance.cases) {
    $label = $case.label
    $htmlPath = Join-Path $captureRoot "popup-reference-$label.html"
    $domPath = Join-Path $captureRoot "metrics-$label.dom.html"
    $errorPath = Join-Path $captureRoot "metrics-$label.stderr.txt"
    $profilePath = Join-Path $captureRoot "headless-metrics-$captureId-$label"
    $arguments = @(
        '--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars',
        '--disable-extensions', '--disable-background-networking', '--disable-component-update',
        '--force-device-scale-factor=1', '--window-size=1180,1040', '--virtual-time-budget=1000',
        '--dump-dom', ('--user-data-dir="' + $profilePath + '"'), ([Uri]::new($htmlPath)).AbsoluteUri
    )
    $process = Start-Process -FilePath $EdgePath -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $domPath -RedirectStandardError $errorPath
    try {
        if (-not $process.WaitForExit(30000)) { throw "Capture timeout: $label" }
        # Drain redirection before reading a dump that replaces an earlier capture.
        $process.WaitForExit()
        $html = Get-Content -LiteralPath $domPath -Raw
        $match = [regex]::Match($html, '<script type="application/json" id="reference-metrics">([^<]+)</script>')
        if (-not $match.Success) { throw "Headless Edge did not emit metrics for $label. See $errorPath" }
        $popups = @($match.Groups[1].Value | ConvertFrom-Json -AsHashtable)
        if ($popups.Count -ne $case.count) { throw "Incomplete popup metrics for $label" }
        $cases.Add([ordered]@{ theme = $case.theme; wireframe = $case.wireframe; popups = $popups })
        Write-Host "Captured original popup styles: $label ($($popups.Count) controls)"
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
            platform = 'Windows'; scaleFactor = 1; viewport = @(1180, 1040); motion = 'disabled'
        }
        generator = [ordered]@{ path = 'scripts/generate_popup_reference.mjs'; sha256 = (Get-FileHash -LiteralPath $generator -Algorithm SHA256).Hash.ToLowerInvariant() }
        capture = [ordered]@{ path = 'scripts/capture_popup_metrics.ps1'; sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant() }
        packages = $sourceProvenance.packages
        sources = $sourceProvenance.sources
        themeFixtureSha256 = $sourceProvenance.fixtureSha256
        notes = $sourceProvenance.notes
    }
    cases = $cases.ToArray()
}
[System.IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
$json = ($fixture | ConvertTo-Json -Depth 50) -replace "`r`n", "`n"
[System.IO.File]::WriteAllText($OutputPath, $json + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "Saved popup reference fixture: $OutputPath"

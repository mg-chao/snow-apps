# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Capture original Ant Design popup styles at the same DPI as the native capture.
Run generate_popup_reference.mjs and the popup_visual example first.
#>
[CmdletBinding()]
param(
    [string]$NativePath,
    [string]$ReferencePath,
    [string]$VisualPath,
    [string]$EdgePath = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    [string]$CasePattern = '*',
    [switch]$All
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path (Split-Path -Parent $crateRoot) 'build/ant-design-gpui'
if (-not $NativePath) { $NativePath = Join-Path $buildRoot 'popup-native' }
if (-not $ReferencePath) { $ReferencePath = Join-Path $buildRoot 'popup-reference' }
$visualRoot = if ($VisualPath) { [System.IO.Path]::GetFullPath($VisualPath) } else { Join-Path $ReferencePath 'visual' }
[System.IO.Directory]::CreateDirectory($visualRoot) | Out-Null
$manifest = Get-Content -LiteralPath (Join-Path $NativePath 'native-metrics.json') -Raw | ConvertFrom-Json
if ($All) {
    # All cases share the native capture window's DPI. This also allows browser
    # capture to proceed while the native renderer completes its own matrix.
    $scale = $manifest[0].scale
    $source = Get-Content -LiteralPath (Join-Path $ReferencePath 'provenance.json') -Raw | ConvertFrom-Json
    $manifest = foreach ($group in $source.cases) {
        foreach ($direction in @('ltr', 'rtl')) {
            foreach ($component in @('tooltip', 'popover', 'popconfirm')) {
                foreach ($placement in @('top', 'topLeft', 'topRight', 'bottom', 'bottomLeft', 'bottomRight', 'left', 'leftTop', 'leftBottom', 'right', 'rightTop', 'rightBottom')) {
                    [pscustomobject]@{ name = "$($group.label)-$direction-$component-$placement"; scale = $scale }
                }
            }
        }
    }
}
$cases = [System.Collections.Generic.List[object]]::new()
$captureId = [Guid]::NewGuid().ToString('N')
foreach ($case in $manifest | Where-Object { $_.name -like $CasePattern }) {
    $name = $case.name
    $htmlPath = Join-Path $ReferencePath "$name.html"
    if (-not (Test-Path -LiteralPath $htmlPath -PathType Leaf)) { throw "Missing generated reference: $htmlPath" }
    $imagePath = Join-Path $visualRoot "$name.png"
    $domPath = Join-Path $visualRoot "$name.dom.html"
    $errorPath = Join-Path $visualRoot "$name.stderr.txt"
    $profilePath = Join-Path $visualRoot "profile-$captureId-$name"
    $scale = $case.scale.ToString([System.Globalization.CultureInfo]::InvariantCulture)
    $arguments = @(
        '--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars',
        '--disable-extensions', '--disable-background-networking', '--disable-component-update',
        "--force-device-scale-factor=$scale", '--window-size=800,500', '--virtual-time-budget=1000',
        '--dump-dom', ('--screenshot="' + $imagePath + '"'),
        ('--user-data-dir="' + $profilePath + '"'), ([Uri]::new($htmlPath)).AbsoluteUri
    )
    $process = Start-Process -FilePath $EdgePath -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $domPath -RedirectStandardError $errorPath
    try {
        if (-not $process.WaitForExit(30000)) { throw "Capture timeout: $name" }
        $html = Get-Content -LiteralPath $domPath -Raw
        $match = [regex]::Match($html, '<script id="visual-metrics" type="application/json">([^<]+)</script>')
        if (-not $match.Success -or -not (Test-Path -LiteralPath $imagePath)) { throw "Incomplete capture: $name. See $errorPath" }
        $metric = $match.Groups[1].Value | ConvertFrom-Json -AsHashtable
        $metric.scale = $case.scale
        $cases.Add($metric)
        if ($cases.Count % 24 -eq 0) { Write-Host "Captured $($cases.Count) reference screenshots" }
    } finally {
        $process.Refresh()
        if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit(5000) | Out-Null }
    }
}
$report = [ordered]@{
    renderer = (Get-Item -LiteralPath $EdgePath).VersionInfo.ProductVersion
    cases = $cases.ToArray()
}
$json = ($report | ConvertTo-Json -Depth 12) -replace "`r`n", "`n"
[System.IO.File]::WriteAllText((Join-Path $visualRoot 'browser-metrics.json'), $json + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "Captured $($cases.Count) matching browser screenshots in $visualRoot"

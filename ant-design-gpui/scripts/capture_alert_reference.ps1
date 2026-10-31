# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
# Capture the actual React Alert pages emitted by generate_alert_reference.mjs.
param(
    [string]$Reference = (Join-Path $PSScriptRoot '../../build/ant-design-gpui/alert-reference'),
    [string]$Output = (Join-Path $PSScriptRoot '../../build/ant-design-gpui/alert-browser'),
    [string]$Browser = 'C:/Program Files/Google/Chrome/Application/chrome.exe'
)
$ErrorActionPreference = 'Stop'
$referenceRoot = [IO.Path]::GetFullPath($Reference)
$outputRoot = [IO.Path]::GetFullPath($Output)
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
if (-not (Test-Path -LiteralPath $Browser -PathType Leaf)) { throw "Browser not found: $Browser" }
$records = [Collections.Generic.List[object]]::new()
$visualCases = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../fixtures/alert/visual-cases.json') -Raw | ConvertFrom-Json -AsHashtable
foreach ($scene in @('basic', 'advanced')) {
foreach ($theme in @('default', 'dark', 'compact', 'dark-compact')) {
    foreach ($direction in @('ltr', 'rtl')) {
        foreach ($scale in @(1.0, 1.5, 2.0)) {
            $percent = [int]($scale * 100)
            $states = if ($scene -eq 'basic' -and $direction -eq 'ltr' -and $scale -eq 1.0) { @('rest', 'hover', 'focus') } else { @('rest') }
            foreach ($state in $states) {
            $suffix = if ($state -eq 'rest') { '' } else { "-$state" }
            $name = "$scene-$theme-$direction-$percent$suffix"
            $page = Join-Path $referenceRoot "alert-visual-$scene-$theme-$direction.html"
            if (-not (Test-Path -LiteralPath $page)) { throw "Generate reference pages first: $page" }
            $image = Join-Path $outputRoot "$name.png"
            $dom = Join-Path $outputRoot "$name.dom.html"
            $errorLog = Join-Path $outputRoot "$name.stderr.txt"
            $profile = Join-Path $outputRoot ("profile-$name-" + [Guid]::NewGuid().ToString('N'))
            $url = ([Uri]::new($page)).AbsoluteUri + "?state=$state"
            $arguments = @(
                '--headless', '--disable-gpu', '--no-first-run', '--hide-scrollbars',
                '--disable-extensions', '--disable-background-networking', '--disable-component-update',
                "--force-device-scale-factor=$scale", '--window-size=740,1000', '--virtual-time-budget=1000',
                ('--screenshot="' + $image + '"'), '--dump-dom',
                ('--user-data-dir="' + $profile + '"'), $url
            )
            $process = Start-Process -FilePath $Browser -ArgumentList $arguments -WindowStyle Hidden `
                -PassThru -RedirectStandardOutput $dom -RedirectStandardError $errorLog
            try {
                $deadline = [DateTime]::UtcNow.AddSeconds(25)
                do {
                    Start-Sleep -Milliseconds 100
                    $process.Refresh()
                    $ready = (Test-Path -LiteralPath $image) -and (Test-Path -LiteralPath $dom)
                    if ($ready) {
                        $html = Get-Content -LiteralPath $dom -Raw
                        if ($html) { $match = [regex]::Match($html, '<script type="application/json" id="reference-metrics">([^<]+)</script>') }
                        $ready = $html -and $match.Success
                    }
                } while (-not $ready -and [DateTime]::UtcNow -lt $deadline -and -not $process.HasExited)
                if (-not $ready) { throw "Incomplete Alert capture: $name; see $errorLog" }
                $alerts = @($match.Groups[1].Value | ConvertFrom-Json -AsHashtable)
                if ($alerts.Count -ne $visualCases[$scene].Count) { throw "Incomplete real Alert rows for $name" }
                $records.Add([ordered]@{ name = $name; scale = $scale; rows = $alerts })
                Write-Host "Captured browser Alert $name"
            } finally {
                $process.Refresh()
                if (-not $process.HasExited) {
                    # Terminate only this fresh, isolated capture process tree.
                    $process.Kill($true)
                    $process.WaitForExit(5000) | Out-Null
                }
            }
            }
        }
    }
}
}
$sourceProvenance = Get-Content -LiteralPath (Join-Path $referenceRoot 'provenance.json') -Raw | ConvertFrom-Json -AsHashtable
$metadata = [ordered]@{
    provenance = $sourceProvenance
    renderer = [ordered]@{
        name = [IO.Path]::GetFileName($Browser)
        version = (Get-Item -LiteralPath $Browser).VersionInfo.ProductVersion
        viewport = @(740, 1000)
        scales = @(1.0, 1.5, 2.0)
    }
    capture = [ordered]@{
        path = 'scripts/capture_alert_reference.ps1'
        sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    cases = $records.ToArray()
}
$json = ($metadata | ConvertTo-Json -Depth 50) -replace "`r`n", "`n"
[IO.File]::WriteAllText((Join-Path $outputRoot 'browser-metrics.json'), $json + "`n", [Text.UTF8Encoding]::new($false))

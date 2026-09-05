[CmdletBinding()]
param(
    [string]$QtBin = "",
    [string]$OutputDirectory = "",
    [int]$ScreenIndex = 0,
    [int]$Captures = 12,
    [int]$SettleMilliseconds = 500,
    [int]$TimeoutMilliseconds = 30000,
    [switch]$SkipBuild,
    [ValidateSet("single-repaint", "posted-update", "native-update", "native-invalidate", "native-invalidate-suppressed")]
    [string[]]$Strategies = @("native-update", "native-invalidate", "native-invalidate-suppressed")
)

$ErrorActionPreference = "Stop"
$shot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $shot ("build\capture-reveal-ablation\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$runner = Join-Path $shot "scripts\run-capture-startup-perf.ps1"
$summaries = @()
for ($index = 0; $index -lt $Strategies.Count; ++$index) {
    $strategy = $Strategies[$index]
    $strategyOutput = Join-Path $OutputDirectory $strategy
    $arguments = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $runner,
        "-OutputDirectory", $strategyOutput,
        "-ScreenIndex", $ScreenIndex,
        "-Captures", $Captures,
        "-SettleMilliseconds", $SettleMilliseconds,
        "-TimeoutMilliseconds", $TimeoutMilliseconds,
        "-RevealStrategy", $strategy
    )
    if (![string]::IsNullOrWhiteSpace($QtBin)) {
        $arguments += @("-QtBin", $QtBin)
    }
    if ($SkipBuild -or $index -gt 0) {
        $arguments += "-SkipBuild"
    }

    & powershell @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "The $strategy reveal benchmark failed"
    }

    $report = Get-Content -LiteralPath (Join-Path $strategyOutput "report.json") -Raw | ConvertFrom-Json
    $steady = $report.groups | Where-Object { $_.id -eq "steady" }
    if ($null -eq $steady) {
        throw "The $strategy report has no steady-state group"
    }
    $summaries += [pscustomobject]@{
        strategy = $strategy
        samples = $steady.count
        end_to_end_mean_ms = $steady.metrics.end_to_end.mean_ms
        end_to_end_p95_ms = $steady.metrics.end_to_end.p95_ms
        native_to_composited_mean_ms = $steady.metrics.native_to_composited.mean_ms
        native_to_composited_p95_ms = $steady.metrics.native_to_composited.p95_ms
        sync_reveal_mean_ms = $steady.metrics.overlay_sync_reveal_total.mean_ms
        sync_reveal_p95_ms = $steady.metrics.overlay_sync_reveal_total.p95_ms
        surface_commit_mean_ms = $steady.metrics.overlay_surface_commit.mean_ms
        first_frame_ok_mean = $steady.metrics.reveal_first_frame_ok.mean_ratio
        settled_ok_mean = $steady.metrics.reveal_settled_ok.mean_ratio
        canvas_paint_events_mean = $steady.counters.'presentation.window.canvas.paint_events'.mean_count
        full_canvas_paint_events_mean = $steady.counters.'presentation.window.canvas.full_paint_events'.mean_count
        update_request_events_mean = $steady.counters.'presentation.window.update_request_events'.mean_count
        surface_commit_requests_mean = $steady.counters.'presentation.window.surface_commit_requests'.mean_count
        redraw_suppress_requests_mean = $steady.counters.'presentation.window.redraw_suppress_requests'.mean_count
        redraw_restore_requests_mean = $steady.counters.'presentation.window.redraw_restore_requests'.mean_count
        update_filter_install_requests_mean = $steady.counters.'presentation.window.update_filter_install_requests'.mean_count
        update_filter_remove_requests_mean = $steady.counters.'presentation.window.update_filter_remove_requests'.mean_count
        update_filter_drain_requests_mean = $steady.counters.'presentation.window.update_filter_drain_requests'.mean_count
        update_requests_suppressed_mean = $steady.counters.'presentation.window.update_requests_suppressed'.mean_count
    }
}

$ordered = @($summaries | Sort-Object native_to_composited_mean_ms)
$ordered | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory "summary.json") -Encoding utf8
$ordered | Export-Csv -LiteralPath (Join-Path $OutputDirectory "summary.csv") -NoTypeInformation -Encoding utf8
$ordered | Format-Table -AutoSize
Write-Host "Reveal ablation summary: $(Join-Path $OutputDirectory 'summary.json')"

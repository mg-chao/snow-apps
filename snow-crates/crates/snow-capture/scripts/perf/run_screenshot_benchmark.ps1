# Runs the screenshot_benchmark example once per capture backend (dxgi, wgc,
# gdi) and merges the three single-row CSVs into one comparison CSV.
#
# Each backend runs in its own process so memory measurements stay isolated.
# The example (and the instrumentation it measures) only builds with the
# snow-capture `stage-timing` feature; production builds never enable it.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File run_screenshot_benchmark.ps1
#   powershell -ExecutionPolicy Bypass -File run_screenshot_benchmark.ps1 -Backends dxgi,gdi
#   powershell -ExecutionPolicy Bypass -File run_screenshot_benchmark.ps1 -MeasureFrames 100

param(
    [string]$Backends = "dxgi,wgc,gdi",
    [int]$WarmupFrames = 10,
    [int]$MeasureFrames = 50,
    [int]$ColdFrames = 10,
    [int]$SettleMs = 500,
    [switch]$Prewarm,
    [string]$OutputDir = "target/perf",
    [string]$CombinedCsv = "screenshot-benchmark.csv"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$crateRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))

$outputPath = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    [System.IO.Path]::GetFullPath($OutputDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $crateRoot $OutputDir))
}
if (-not (Test-Path $outputPath)) {
    New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
}

$backendList = $Backends -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ }
if (-not $backendList) {
    throw "no backends selected; pass -Backends dxgi,wgc,gdi"
}

$commonArgs = @(
    "--warmup", "$WarmupFrames",
    "--frames", "$MeasureFrames",
    "--cold-frames", "$ColdFrames",
    "--settle-ms", "$SettleMs"
)
if ($Prewarm) {
    $commonArgs += "--prewarm"
}

$allHeaders = New-Object 'System.Collections.Generic.List[string]'
$rowMaps = New-Object 'System.Collections.Generic.List[hashtable]'

foreach ($backend in $backendList) {
    $rowPath = Join-Path $outputPath "screenshot-benchmark-$backend.csv"
    if (Test-Path $rowPath) {
        Remove-Item $rowPath -Force
    }

    Write-Host "=== running screenshot_benchmark: $backend ==="
    Push-Location $crateRoot
    try {
        & cargo run --release --features stage-timing --example screenshot_benchmark -- --backend $backend @commonArgs --csv $rowPath
        if ($LASTEXITCODE -ne 0) {
            throw "screenshot_benchmark failed for backend $backend (exit $LASTEXITCODE)"
        }
    }
    finally {
        Pop-Location
    }

    if (-not (Test-Path $rowPath)) {
        throw "expected benchmark row at $rowPath but it was not written"
    }

    $lines = Get-Content $rowPath | Where-Object { $_.Trim() }
    if ($lines.Count -lt 2) {
        throw "benchmark row at $rowPath is missing header or data"
    }
    $headerFields = $lines[0].Split(",")
    $rowFields = $lines[1].Split(",")
    if ($headerFields.Count -ne $rowFields.Count) {
        throw "benchmark row at $rowPath has $($headerFields.Count) headers but $($rowFields.Count) values"
    }

    $rowMap = @{}
    for ($i = 0; $i -lt $headerFields.Count; $i++) {
        $rowMap[$headerFields[$i]] = $rowFields[$i]
        if (-not $allHeaders.Contains($headerFields[$i])) {
            $allHeaders.Add($headerFields[$i])
        }
    }
    $rowMaps.Add($rowMap)
}

$combinedPath = Join-Path $outputPath $CombinedCsv
$combinedLines = New-Object 'System.Collections.Generic.List[string]'
$combinedLines.Add(($allHeaders -join ","))
foreach ($rowMap in $rowMaps) {
    $values = foreach ($header in $allHeaders) {
        if ($rowMap.ContainsKey($header)) { $rowMap[$header] } else { "" }
    }
    $combinedLines.Add(($values -join ","))
}
Set-Content -Path $combinedPath -Value $combinedLines

Write-Host ""
Write-Host "combined csv written to $combinedPath"

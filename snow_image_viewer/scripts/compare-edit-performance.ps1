param(
    [Parameter(Mandatory = $true)]
    [string]$Baseline,
    [Parameter(Mandatory = $true)]
    [string]$Candidate,
    [ValidateSet("source-raster-identity", "cpu-lanczos-4x", "opaque-jpeg-fast-path")]
    [string[]]$AcceptanceClaims = @()
)

$ErrorActionPreference = "Stop"
$schema = "snow-image-viewer.edit-mode-performance.v1"

function Read-Report([string]$Path) {
    $resolved = Resolve-Path -LiteralPath $Path
    $report = Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json
    if ($report.schema -ne $schema) {
        throw "'$resolved' is not a supported edit performance report."
    }
    if ($null -eq $report.scenarios) {
        throw "'$resolved' has no scenario set."
    }
    return $report
}

function Combined-Memory($Scenario) {
    if ($null -ne $Scenario.parent_current_rss_bytes) {
        return [double]$Scenario.parent_current_rss_bytes + [double]$Scenario.worker_current_rss_bytes
    }
    return [double]$Scenario.parent_peak_rss_bytes + [double]$Scenario.worker_peak_rss_bytes
}

function Delta-Percent([double]$Before, [double]$After) {
    if ($Before -eq 0) { return [double]::NaN }
    return (($After - $Before) / $Before) * 100.0
}

function Scenario-Names($Report) {
    return @($Report.scenarios.PSObject.Properties.Name | Sort-Object)
}

function Find-Timing($Scenario, [string]$Name) {
    if ($null -eq $Scenario -or $null -eq $Scenario.timings) { return $null }
    return $Scenario.timings.PSObject.Properties[$Name].Value
}

$baselineReport = Read-Report $Baseline
$candidateReport = Read-Report $Candidate
$baselineScenarios = Scenario-Names $baselineReport
$candidateScenarios = Scenario-Names $candidateReport
if (($baselineScenarios -join "`n") -ne ($candidateScenarios -join "`n")) {
    throw "Scenario mismatch. Baseline: [$($baselineScenarios -join ', ')]; candidate: [$($candidateScenarios -join ', ')]."
}

$rows = @()
$gateFailures = @()
$pngBackendChanged = [string]$baselineReport.benchmark.png_compression_backend -ne
                     [string]$candidateReport.benchmark.png_compression_backend
$changedEffortCodecs = @(
    $candidateReport.benchmark.changed_effort_codecs |
        Where-Object { ![string]::IsNullOrWhiteSpace([string]$_) }
)
$claims = @(
    @($candidateReport.benchmark.acceptance_claims) + @($AcceptanceClaims) |
        Where-Object { ![string]::IsNullOrWhiteSpace([string]$_) } |
        Sort-Object -Unique
)
$claimsSet = [System.Collections.Generic.HashSet[string]]::new(
    [string[]]$claims, [System.StringComparer]::OrdinalIgnoreCase)
$pngBaselineEncode = 0.0
$pngCandidateEncode = 0.0
$codecBaselineEncode = @{}
$codecCandidateEncode = @{}
foreach ($scenarioName in $baselineScenarios) {
    $beforeScenario = $baselineReport.scenarios.$scenarioName
    $afterScenario = $candidateReport.scenarios.$scenarioName
    $beforeMemory = Combined-Memory $beforeScenario
    $afterMemory = Combined-Memory $afterScenario
    if ([double]$beforeScenario.artifact_bytes -gt 0) {
        $sizeDelta = Delta-Percent ([double]$beforeScenario.artifact_bytes) ([double]$afterScenario.artifact_bytes)
        if ($sizeDelta -gt 25.0) {
            $gateFailures += "${scenarioName}: artifact size grew by $([math]::Round($sizeDelta, 1))% (limit 25%)."
        }
    }
    $requestBefore = Find-Timing $beforeScenario "edit.request_to_artifact_ready"
    $requestAfter = Find-Timing $afterScenario "edit.request_to_artifact_ready"
    $encodeBefore = Find-Timing $beforeScenario "exact.encode"
    $encodeAfter = Find-Timing $afterScenario "exact.encode"
    if ($claimsSet.Contains("opaque-jpeg-fast-path") -and
        $scenarioName -eq "identity.jpeg.cold-full-pipeline" -and
        $beforeScenario.alpha_content -eq "opaque" -and
        $afterScenario.alpha_content -eq "opaque") {
        $prepareBefore = Find-Timing $beforeScenario "exact.prepare_export"
        $prepareAfter = Find-Timing $afterScenario "exact.prepare_export"
        if ($null -eq $prepareBefore -or $null -eq $prepareAfter) {
            $gateFailures += "${scenarioName}: opaque JPEG preparation timing is missing."
        } elseif ([double]$prepareAfter.median_nanoseconds -gt [double]$prepareBefore.median_nanoseconds * 0.10) {
            $gateFailures += "${scenarioName}: opaque JPEG preparation improved by less than 90%."
        }
        if ($null -eq $requestBefore -or $null -eq $requestAfter) {
            $gateFailures += "${scenarioName}: artifact-readiness timing is missing."
        } elseif ([double]$requestAfter.median_nanoseconds -gt [double]$requestBefore.median_nanoseconds * 0.40) {
            $gateFailures += "${scenarioName}: request-to-artifact readiness improved by less than 60%."
        }
    }
    if ($claimsSet.Contains("cpu-lanczos-4x") -and
        $scenarioName -match '^(50-percent|12\.5-percent)\..*\.cpu-fallback$') {
        $resizeBefore = Find-Timing $beforeScenario "exact.direct_mapped_transform"
        $resizeAfter = Find-Timing $afterScenario "exact.direct_mapped_transform"
        if ($null -eq $resizeBefore -or $null -eq $resizeAfter) {
            $gateFailures += "${scenarioName}: CPU resize timing is missing."
        } elseif ([double]$resizeAfter.median_nanoseconds -gt [double]$resizeBefore.median_nanoseconds * 0.25) {
            $gateFailures += "${scenarioName}: CPU Lanczos fallback is less than 4x faster."
        }
    }
    if ($pngBackendChanged -and $scenarioName -like "*.png.*" -and
        $null -ne $encodeBefore -and $null -ne $encodeAfter) {
        $pngBaselineEncode += [double]$encodeBefore.median_nanoseconds
        $pngCandidateEncode += [double]$encodeAfter.median_nanoseconds
        if ([double]$encodeAfter.median_nanoseconds -gt [double]$encodeBefore.median_nanoseconds * 1.10) {
            $gateFailures += "${scenarioName}: the changed PNG backend regressed encode latency by more than 10%."
        }
    }
    foreach ($codec in $changedEffortCodecs) {
        if ($scenarioName -notlike "*.$codec.*") { continue }
        if ($null -eq $encodeBefore -or $null -eq $encodeAfter) {
            $gateFailures += "${scenarioName}: changed effort default lacks encode timing."
            continue
        }
        if (!$codecBaselineEncode.ContainsKey($codec)) {
            $codecBaselineEncode[$codec] = 0.0
            $codecCandidateEncode[$codec] = 0.0
        }
        $codecBaselineEncode[$codec] += [double]$encodeBefore.median_nanoseconds
        $codecCandidateEncode[$codec] += [double]$encodeAfter.median_nanoseconds
        if ([double]$encodeAfter.median_nanoseconds -gt [double]$encodeBefore.median_nanoseconds * 1.10) {
            $gateFailures += "${scenarioName}: changed effort default regressed latency by more than 10%."
        }
        if ($null -eq $beforeScenario.psnr_db -or $null -eq $afterScenario.psnr_db -or
            $null -eq $beforeScenario.ssim -or $null -eq $afterScenario.ssim) {
            $gateFailures += "${scenarioName}: changed effort default lacks PSNR/SSIM measurements."
        } else {
            if ([double]$afterScenario.psnr_db -lt [double]$beforeScenario.psnr_db - 0.1) {
                $gateFailures += "${scenarioName}: PSNR loss exceeds 0.1 dB."
            }
            if ([double]$afterScenario.ssim -lt [double]$beforeScenario.ssim - 0.001) {
                $gateFailures += "${scenarioName}: SSIM loss exceeds 0.001."
            }
        }
    }
    $beforeStages = @($beforeScenario.timings.PSObject.Properties.Name | Sort-Object)
    $afterStages = @($afterScenario.timings.PSObject.Properties.Name | Sort-Object)
    $comparableStages = @($beforeStages | Where-Object { $afterStages -contains $_ })
    foreach ($stage in $comparableStages) {
        $before = $beforeScenario.timings.$stage
        $after = $afterScenario.timings.$stage
        $rows += [pscustomobject]@{
            Scenario = $scenarioName
            Stage = $stage
            ColdBaselineMs = [math]::Round($before.samples_nanoseconds[0] / 1e6, 3)
            ColdCandidateMs = [math]::Round($after.samples_nanoseconds[0] / 1e6, 3)
            MedianBaselineMs = [math]::Round($before.median_nanoseconds / 1e6, 3)
            MedianCandidateMs = [math]::Round($after.median_nanoseconds / 1e6, 3)
            MedianDeltaPercent = [math]::Round((Delta-Percent $before.median_nanoseconds $after.median_nanoseconds), 1)
            P95BaselineMs = [math]::Round($before.p95_nanoseconds / 1e6, 3)
            P95CandidateMs = [math]::Round($after.p95_nanoseconds / 1e6, 3)
            P95DeltaPercent = [math]::Round((Delta-Percent $before.p95_nanoseconds $after.p95_nanoseconds), 1)
            BaselineMemoryMiB = [math]::Round($beforeMemory / 1MB, 2)
            CandidateMemoryMiB = [math]::Round($afterMemory / 1MB, 2)
            SizeDeltaPercent = [math]::Round((Delta-Percent $beforeScenario.artifact_bytes $afterScenario.artifact_bytes), 1)
        }
        if ($scenarioName -like "*.exact-artifact-hit" -and
            $stage -eq "edit.request_to_artifact_ready" -and
            [double]$after.median_nanoseconds -ge 2000000.0) {
            $gateFailures += "${scenarioName}: artifact cache-hit median is $([math]::Round([double]$after.median_nanoseconds / 1e6, 3)) ms (limit <2 ms)."
        }
    }
}

if ($pngBackendChanged) {
    if ($pngBaselineEncode -le 0 -or $pngCandidateEncode -gt $pngBaselineEncode * 0.80) {
        $gateFailures += "PNG backend: aggregate median encode latency improved by less than 20%."
    }
    $pngBefore = $baselineReport.scenarios.'identity.png.cold-full-pipeline'
    $pngAfter = $candidateReport.scenarios.'identity.png.cold-full-pipeline'
    $pngRequestBefore = Find-Timing $pngBefore "edit.request_to_artifact_ready"
    $pngRequestAfter = Find-Timing $pngAfter "edit.request_to_artifact_ready"
    if ($null -eq $pngRequestBefore -or $null -eq $pngRequestAfter -or
        [double]$pngRequestAfter.median_nanoseconds -gt [double]$pngRequestBefore.median_nanoseconds * 0.80) {
        $gateFailures += "identity.png.cold-full-pipeline: full-size PNG artifact readiness improved by less than 20%."
    }
}
foreach ($codec in $changedEffortCodecs) {
    if (!$codecBaselineEncode.ContainsKey($codec) -or
        [double]$codecCandidateEncode[$codec] -gt [double]$codecBaselineEncode[$codec] * 0.80) {
        $gateFailures += "$codec effort default: aggregate median encode latency improved by less than 20%."
    }
}

if ($claimsSet.Contains("source-raster-identity")) {
    $sourceBefore = $baselineReport.scenarios.'identity.png.cold-full-pipeline'
    $sourceAfter = $candidateReport.scenarios.'identity.png.cold-full-pipeline'
    $sourceBeforeTiming = Find-Timing $sourceBefore "edit.request_to_artifact_ready"
    $sourceAfterTiming = Find-Timing $sourceAfter "edit.request_to_artifact_ready"
    if ($null -eq $sourceBefore -or $null -eq $sourceAfter) {
        $gateFailures += "source-raster-identity: identity PNG cold-pipeline scenarios are missing."
    } else {
        if ([string]$sourceAfter.provenance -ne "source_exact") {
            $gateFailures += "source-raster-identity: provenance is not source_exact."
        }
        if ([double]$sourceAfter.readback_bytes -ne 0) {
            $gateFailures += "source-raster-identity: GPU readback bytes are not zero."
        }
        if ([double]$sourceAfter.preview_cache_bytes -ne 0) {
            $gateFailures += "source-raster-identity: identity preview allocated a CPU preview cache."
        }
        if ($null -eq $sourceAfter.timings.'exact.source_raster_reuse') {
            $gateFailures += "source-raster-identity: source-raster reuse timing is missing."
        }
        if ($null -ne $sourceAfter.timings.'exact.gpu_readback' -or
            $null -ne $sourceAfter.timings.'exact.raw_buffer_prepare') {
            $gateFailures += "source-raster-identity: removed GPU readback stages are still present."
        }
        $alphaTiming = Find-Timing $sourceAfter "exact.classify_alpha"
        if ($null -eq $alphaTiming -or [double]$alphaTiming.median_nanoseconds -ne 0) {
            $gateFailures += "source-raster-identity: verified alpha metadata was not reused."
        }
        if ($null -eq $sourceBeforeTiming -or $null -eq $sourceAfterTiming -or
            [double]$sourceAfterTiming.median_nanoseconds -gt
                [double]$sourceBeforeTiming.median_nanoseconds * 0.80) {
            $gateFailures += "source-raster-identity: artifact readiness improved by less than 20%."
        }
    }
}

$rows | Format-Table -AutoSize
if ($gateFailures.Count -gt 0) {
    throw "Performance acceptance gates failed:`n - $($gateFailures -join "`n - ")"
}
Write-Host "All applicable performance acceptance gates passed."

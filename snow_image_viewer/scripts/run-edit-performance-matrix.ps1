param(
    [Parameter(Mandatory = $true)]
    [string]$Viewer,
    [Parameter(Mandatory = $true)]
    [string]$Image,
    [Parameter(Mandatory = $true)]
    [string]$Output,
    [ValidateRange(10, 1000)]
    [int]$Iterations = 10,
    [ValidateRange(1, 100)]
    [int]$Warmups = 5,
    [ValidateSet("png", "jpeg", "webp-lossy", "webp-lossless", "avif", "heif", "jxl")]
    [string[]]$Formats = @("png", "jpeg", "webp-lossy", "webp-lossless", "avif", "heif", "jxl"),
    [ValidateSet("identity", "50-percent", "12.5-percent")]
    [string[]]$Ratios = @("identity", "50-percent", "12.5-percent"),
    [ValidateSet("stripped", "preserved")]
    [string[]]$MetadataModes = @("stripped", "preserved"),
    [ValidateSet("source-raster-identity", "cpu-lanczos-4x", "opaque-jpeg-fast-path")]
    [string[]]$AcceptanceClaims = @()
)

$ErrorActionPreference = "Stop"
$viewerPath = (Resolve-Path -LiteralPath $Viewer).Path
$imagePath = (Resolve-Path -LiteralPath $Image).Path
$outputPath = [System.IO.Path]::GetFullPath($Output)
$outputDirectory = Split-Path -Parent $outputPath
if ($outputDirectory -and !(Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory | Out-Null
}

$allRatios = @(
    @{ Name = "identity"; Scale = "1" },
    @{ Name = "50-percent"; Scale = "0.5" },
    @{ Name = "12.5-percent"; Scale = "0.125" }
)
$selectedRatios = @($allRatios | Where-Object { $Ratios -contains $_.Name })
$selectedFormats = @($Formats)
$metadataCapableFormats = @("png", "webp-lossy", "webp-lossless", "avif", "heif", "jxl")
$scenarioKinds = @("cold-full-pipeline", "base-raster-reuse", "exact-artifact-hit",
                   "preview-only-cache-recovery")
$scenarios = [ordered]@{}
$skippedFormats = [System.Collections.Generic.HashSet[string]]::new()
$pngCompressionBackend = $null
$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("snow-edit-benchmark-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null

function Invoke-Benchmark([string]$Format, [string]$Scale, [string]$Mode,
                          [string]$MetadataMode) {
    $reportPath = Join-Path $temporaryRoot ("report-" + [guid]::NewGuid().ToString("N") + ".json")
    $arguments = @(
        "--edit-performance-test",
        "--edit-performance-output", $reportPath,
        "--edit-performance-iterations", $Iterations,
        "--edit-performance-warmups", $Warmups,
        "--edit-performance-format", $Format,
        "--edit-performance-scale", $Scale
    )
    if ($Mode -eq "cpu") { $arguments += "--edit-performance-cpu" }
    if ($Mode -eq "rapid") { $arguments += "--edit-performance-rapid-superseding" }
    if ($MetadataMode -eq "preserved") {
        $arguments += "--edit-performance-preserve-metadata"
    }
    $arguments += $imagePath
    $quotedArguments = foreach ($argument in $arguments) {
        if ($argument -notmatch '[\s"]') { $argument; continue }
        $escaped = [regex]::Replace($argument, '(\\*)"', '$1$1\"')
        $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
        '"' + $escaped + '"'
    }
    $process = Start-Process -FilePath $viewerPath -ArgumentList ($quotedArguments -join ' ') -Wait -PassThru -WindowStyle Hidden
    $exitCode = $process.ExitCode
    if ($exitCode -eq 3) { return $null }
    if ($exitCode -ne 0) {
        throw "Edit benchmark failed for format '$Format', scale '$Scale', mode '$Mode', metadata '$MetadataMode' (exit $exitCode)."
    }
    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    if ($report.schema -ne "snow-image-viewer.edit-mode-performance.v1" -or !$report.succeeded) {
        throw "Edit benchmark returned an invalid or failed report for '$Format' at '$Scale'."
    }
    $expectedPreservation = $MetadataMode -eq "preserved"
    if ([bool]$report.request.preserve_metadata -ne $expectedPreservation) {
        throw "Edit benchmark returned the wrong metadata policy for '$Format' at '$Scale'."
    }
    return $report
}

try {
    foreach ($format in $selectedFormats) {
        $formatAvailable = $true
        foreach ($ratio in $selectedRatios) {
            if (!$formatAvailable) { break }
            $applicableMetadataModes = if ($metadataCapableFormats -contains $format) {
                @($MetadataModes)
            } else {
                @($MetadataModes | Where-Object { $_ -eq "stripped" })
            }
            foreach ($metadataMode in $applicableMetadataModes) {
                Write-Host "Benchmarking $format at $($ratio.Name) with metadata $metadataMode..."
                $standard = Invoke-Benchmark $format $ratio.Scale "standard" $metadataMode
                if ($null -eq $standard) {
                    $null = $skippedFormats.Add($format)
                    $formatAvailable = $false
                    break
                }
                $reportedBackend = [string]$standard.benchmark.png_compression_backend
                if ([string]::IsNullOrWhiteSpace($reportedBackend)) {
                    throw "Edit benchmark did not report its PNG compression backend."
                }
                if ($null -eq $pngCompressionBackend) {
                    $pngCompressionBackend = $reportedBackend
                } elseif ($pngCompressionBackend -ne $reportedBackend) {
                    throw "Edit benchmark reported inconsistent PNG compression backends ('$pngCompressionBackend' and '$reportedBackend')."
                }
                $scenarioPrefix = if ($metadataMode -eq "preserved") {
                    "$($ratio.Name).$format.preserved-metadata"
                } else {
                    "$($ratio.Name).$format"
                }
                foreach ($kind in $scenarioKinds) {
                    $key = "$scenarioPrefix.$kind"
                    $scenarios[$key] = $standard.scenarios.$kind
                    $scenarios[$key].name = $key
                    $scenarios[$key].metadata_policy = $metadataMode
                }

                $cpu = Invoke-Benchmark $format $ratio.Scale "cpu" $metadataMode
                $cpuKey = "$scenarioPrefix.cpu-fallback"
                $scenarios[$cpuKey] = $cpu.scenarios.'cpu-fallback'
                $scenarios[$cpuKey].name = $cpuKey
                $scenarios[$cpuKey].metadata_policy = $metadataMode

                $rapid = Invoke-Benchmark $format $ratio.Scale "rapid" $metadataMode
                $rapidKey = "$scenarioPrefix.rapid-superseding-request"
                $scenarios[$rapidKey] = $rapid.scenarios.'rapid-superseding-request'
                $scenarios[$rapidKey].name = $rapidKey
                $scenarios[$rapidKey].metadata_policy = $metadataMode
            }
        }
    }

    $report = [ordered]@{
        schema = "snow-image-viewer.edit-mode-performance.v1"
        succeeded = $true
        benchmark = [ordered]@{
            report_version = 1
            iterations = $Iterations
            warmup_iterations = $Warmups
            ratios = @($selectedRatios.Name)
            metadata_modes = @($MetadataModes)
            formats_requested = $selectedFormats
            formats_skipped = @($skippedFormats | Sort-Object)
            scenario_kinds = @($scenarioKinds + "cpu-fallback" + "rapid-superseding-request")
            png_compression_backend = $pngCompressionBackend
            acceptance_claims = @($AcceptanceClaims)
        }
        image = [ordered]@{ path = $imagePath }
        scenarios = $scenarios
    }
    $json = $report | ConvertTo-Json -Depth 100
    [System.IO.File]::WriteAllText($outputPath, $json + [Environment]::NewLine,
                                  [System.Text.UTF8Encoding]::new($false))
    Write-Host "Edit performance matrix: $outputPath"
} finally {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
}

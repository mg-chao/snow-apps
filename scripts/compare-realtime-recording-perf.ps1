[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Baseline,
    [Parameter(Mandatory)][string]$Candidate,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet(5, 10)][int]$Pairs = 5,
    [ValidateRange(1, 10)][int]$StartPair = 1,
    [int]$DurationSeconds = 30,
    [int]$WarmupSeconds = 5,
    [int]$Fps = 30,
    [string]$Backend = "auto",
    [string]$BaselineBackend = "",
    [string]$CandidateBackend = "",
    [ValidateRange(0, 64)][int]$BaselineThreads = 0,
    [ValidateRange(0, 64)][int]$CandidateThreads = 0,
    [ValidateRange(0, 4)][int]$BaselineResizeThreads = 0,
    [ValidateRange(0, 4)][int]$CandidateResizeThreads = 0,
    [switch]$BaselineAlignCapture,
    [switch]$CandidateAlignCapture,
    [switch]$BaselineFreeRunningCapture,
    [switch]$CandidateFreeRunningCapture,
    [string]$Scenario = "all-effects",
    [string]$Workload = "continuous",
    [switch]$Audio,
    [switch]$PreferHardware
)

$ErrorActionPreference = "Stop"
if (($BaselineAlignCapture -and $BaselineFreeRunningCapture) -or
    ($CandidateAlignCapture -and $CandidateFreeRunningCapture)) { throw 'Choose one pacing override per variant.' }
if ($StartPair -gt $Pairs) { throw "StartPair cannot exceed Pairs." }
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$env:Path = "$(Join-Path $repoRoot '.tools/vcpkg/installed/dynamic/x64-windows/bin');$env:Path"
$Baseline = (Resolve-Path -LiteralPath $Baseline).Path
$Candidate = (Resolve-Path -LiteralPath $Candidate).Path
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$manifest = [ordered]@{
    schema_version = 1
    baseline = $Baseline
    baseline_sha256 = (Get-FileHash -LiteralPath $Baseline -Algorithm SHA256).Hash
    candidate = $Candidate
    candidate_sha256 = (Get-FileHash -LiteralPath $Candidate -Algorithm SHA256).Hash
    pairs = $Pairs
    duration_seconds = $DurationSeconds
    warmup_seconds = $WarmupSeconds
    fps = $Fps
    backend = $Backend
    baseline_backend = $BaselineBackend
    candidate_backend = $CandidateBackend
    baseline_threads = $BaselineThreads
    candidate_threads = $CandidateThreads
    baseline_resize_threads = $BaselineResizeThreads
    candidate_resize_threads = $CandidateResizeThreads
    baseline_align_capture = [bool]$BaselineAlignCapture
    candidate_align_capture = [bool]$CandidateAlignCapture
    baseline_free_running_capture = [bool]$BaselineFreeRunningCapture
    candidate_free_running_capture = [bool]$CandidateFreeRunningCapture
    scenario = $Scenario
    workload = $Workload
    audio = [bool]$Audio
    prefer_hardware = [bool]$PreferHardware
}
$manifest | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $OutputDirectory "comparison-manifest.json")
Write-Warning "Paired recording benchmarks take over the primary display and inject input."
for ($pair = $StartPair; $pair -le $Pairs; $pair++) {
    $order = if ($pair % 2 -eq 1) { @("baseline", "candidate") } else { @("candidate", "baseline") }
    foreach ($variant in $order) {
        $executable = if ($variant -eq "baseline") { $Baseline } else { $Candidate }
        $destination = Join-Path $OutputDirectory "$variant-$pair"
        $selectedBackend = if ($variant -eq "baseline") { $BaselineBackend } else { $CandidateBackend }
        if ([string]::IsNullOrWhiteSpace($selectedBackend)) { $selectedBackend = $Backend }
        $threads = if ($variant -eq "baseline") { $BaselineThreads } else { $CandidateThreads }
        $arguments = @("--duration-seconds", $DurationSeconds, "--warmup-seconds", $WarmupSeconds,
            "--samples", 1, "--fps", $Fps, "--backend", $selectedBackend, "--clarity", "1080p",
            "--scenario", $Scenario, "--workload", $Workload, "--output", $destination)
        if ($Audio) { $arguments += "--audio" }
        if ($threads -gt 0) { $arguments += @("--encode-threads", $threads) }
        $resizeThreads = if ($variant -eq "baseline") { $BaselineResizeThreads } else { $CandidateResizeThreads }
        if ($resizeThreads -gt 0) { $arguments += @("--resize-threads", $resizeThreads) }
        if (($variant -eq "baseline" -and $BaselineAlignCapture) -or
            ($variant -eq "candidate" -and $CandidateAlignCapture)) { $arguments += "--align-capture" }
        if (($variant -eq "baseline" -and $BaselineFreeRunningCapture) -or
            ($variant -eq "candidate" -and $CandidateFreeRunningCapture)) { $arguments += "--free-running-capture" }
        if ($PreferHardware) { $arguments += "--prefer-hardware" }
        Write-Host "Pair $pair/$Pairs $variant"
        & $executable @arguments *> (Join-Path $OutputDirectory "$variant-$pair.log")
        if ($LASTEXITCODE -ne 0) {
            if (-not (Test-Path (Join-Path $destination "realtime-recording-benchmark.csv"))) {
                throw "Benchmark failed before producing measurements: $variant-$pair"
            }
            Write-Warning "Validation failed for $variant-$pair; preserve its failure and continue the paired measurement."
        }
        $row = Import-Csv (Join-Path $destination "realtime-recording-benchmark.csv")
        Write-Host "$variant-$pair useful_fps=$($row.useful_fps) cpu=$($row.cpu_percent) memory=$($row.private_peak_mib)MiB"
    }
}
& python (Join-Path $PSScriptRoot "compare-recording-results.py") $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw "Benchmark result analysis failed." }

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Baseline,
    [string]$Candidate = "build/windows-msvc-performance/snow_draw_engine_qt/Release/snow-canvas-smart-erase-benchmark.exe",
    [string]$OutputDirectory = "build/smart-erase-perf/measurements",
    [string[]]$Scenarios = @(
        "texture", "4k-text", "8k-text", "large-region", "long-pen",
        "nonrepeating-texture", "nonrepeat-medium", "nonrepeat-large",
        "nonrepeat-long-pen", "diagonal-pen", "source-edge", "fractional-scale", "double-scale"
    ),
    [ValidateSet("532", "533", "544", "555")]
    [string[]]$Schedules = @("532", "533", "544", "555"),
    [ValidateSet(1, 2)]
    [int]$Jobs = 1,
    [switch]$ParallelVoting
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
Push-Location $repoRoot
try {
    $baselinePath = (Resolve-Path -LiteralPath $Baseline).Path
    $candidatePath = (Resolve-Path -LiteralPath $Candidate).Path
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    Get-FileHash -Algorithm SHA256 -LiteralPath $baselinePath, $candidatePath |
        Export-Csv -NoTypeInformation -Path (Join-Path $OutputDirectory "executables.csv")
    $index = 0
    foreach ($scenario in $Scenarios) {
        # Fresh processes make peak working set meaningful; no PNG comparison or
        # image saving runs in these measurements. Rotate order to reduce drift.
        $variants = @("baseline") + $Schedules
        for ($offset = 0; $offset -lt $variants.Count; ++$offset) {
            $variant = $variants[($index + $offset) % $variants.Count]
            $arguments = @("--scenario", $scenario, "--warmup", "1", "--repeat", "7", "--jobs", "$Jobs")
            $executable = $candidatePath
            if ($variant -eq "baseline") {
                $executable = $baselinePath
            } else {
                $arguments += @("--schedule", $variant)
                if ($ParallelVoting) { $arguments += "--parallel-voting" }
                else { $arguments += "--serial-voting" }
            }
            $stem = Join-Path $OutputDirectory "$scenario-$variant"
            & $executable @arguments 1> "$stem.csv" 2> "$stem.log"
            if ($LASTEXITCODE -ne 0) {
                throw "Smart Erase benchmark failed: $scenario / $variant (exit $LASTEXITCODE)."
            }
            Get-Content -LiteralPath "$stem.log"
        }
        ++$index
    }
} finally {
    Pop-Location
}

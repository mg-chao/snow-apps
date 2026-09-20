[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Baseline,
    [string]$Candidate = "build/windows-msvc-performance/snow_draw_engine_qt/Release/snow-canvas-smart-erase-benchmark.exe",
    [string]$OutputDirectory = "build/smart-erase-policy-perf",
    [string[]]$Scenarios = @(
        "texture", "4k-text", "8k-text", "large-region", "long-pen",
        "nonrepeating-texture", "nonrepeat-medium", "nonrepeat-large",
        "nonrepeat-long-pen", "diagonal-pen", "source-edge", "fractional-scale", "double-scale"
    ),
    # Adaptive policies require the preserved experimental executable. They are
    # retained here to reproduce qualification of rejected candidates too.
    [ValidateSet("baseline", "default", "reference", "crop", "adaptive-32-1", "adaptive-32-2", "adaptive-64-1", "adaptive-64-2")]
    [string[]]$Policies = @("baseline", "default"),
    [ValidateSet(1, 2)]
    [int]$Jobs = 1
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
        for ($offset = 0; $offset -lt $Policies.Count; ++$offset) {
            $policy = $Policies[($index + $offset) % $Policies.Count]
            $arguments = @("--scenario", $scenario, "--warmup", "1", "--repeat", "7", "--jobs", "$Jobs")
            $executable = $candidatePath
            if ($policy -eq "baseline") {
                $executable = $baselinePath
            } elseif ($policy -like "adaptive-*") {
                $parts = $policy.Split('-')
                $budget = [int]$parts[1] * 1024
                $arguments += @("--policy", "adaptive", "--search-budget", "$budget", "--refinement-passes", $parts[2])
            } else {
                $arguments += @("--policy", $policy)
            }
            $stem = Join-Path $OutputDirectory "$scenario-$policy"
            & $executable @arguments 1> "$stem.csv" 2> "$stem.log"
            if ($LASTEXITCODE -ne 0) {
                throw "Smart Erase benchmark failed: $scenario / $policy (exit $LASTEXITCODE)."
            }
            Get-Content -LiteralPath "$stem.log"
        }
        ++$index
    }
} finally {
    Pop-Location
}

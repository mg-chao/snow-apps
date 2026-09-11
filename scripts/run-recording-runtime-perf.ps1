[CmdletBinding()]
param(
    [string]$BaselineExecutable = '',
    [string]$CandidateExecutable = '',
    [string]$OutputDirectory = '',
    [int]$Samples = 7,
    [int]$Frames = 120,
    [string[]]$Cases = @('effects:1920:1080:60', 'animated:1280:720:60', 'motion:1920:1080:60', 'static:3840:2160:60'),
    [switch]$Hardware,
    [switch]$BuildOnly,
    [switch]$KeepMedia
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$env:FFMPEG_DIR = Join-Path $repoRoot '.tools/vcpkg/installed/dynamic/x64-windows'
$env:VCPKGRS_TRIPLET = 'x64-windows'
$env:VCPKGRS_DYNAMIC = '1'
$env:Path = "$(Join-Path $env:FFMPEG_DIR 'bin');$env:Path"
if (-not $CandidateExecutable) {
    & cargo build --manifest-path "$repoRoot/snow-crates/Cargo.toml" --release --target x86_64-pc-windows-msvc `
        --target-dir "$repoRoot/build/windows-msvc-performance/cargo" -p snow-recording-runtime `
        --features recording-benchmark --example recording_runtime_benchmark
    if ($LASTEXITCODE -ne 0) { throw 'Runtime benchmark build failed' }
    $CandidateExecutable = "$repoRoot/build/windows-msvc-performance/cargo/x86_64-pc-windows-msvc/release/examples/recording_runtime_benchmark.exe"
}
if ($BuildOnly) { Write-Output $CandidateExecutable; return }
if (-not $BaselineExecutable) { throw 'A preserved baseline executable is required' }
if ($Samples -lt 7 -or $Frames -lt 2) { throw 'At least seven samples and two frames are required' }
$CandidateExecutable = (Resolve-Path $CandidateExecutable).Path
$BaselineExecutable = (Resolve-Path $BaselineExecutable).Path
if (-not $OutputDirectory) { $OutputDirectory = "$repoRoot/build/windows-msvc-performance/recording-runtime-$(Get-Date -Format yyyyMMdd-HHmmss)" }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path
@{
    revision = (& git -C $repoRoot rev-parse HEAD)
    preset = 'windows-msvc-performance'; profile = 'release'; fixture = 'screen-pattern-v1'
    baseline = $BaselineExecutable; candidate = $CandidateExecutable
    baselineHash = (Get-FileHash $BaselineExecutable).Hash; candidateHash = (Get-FileHash $CandidateExecutable).Hash
    cpu = @(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
    gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)
    samples = $Samples; frames = $Frames; hardware = [bool]$Hardware
} | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 "$OutputDirectory/environment.json"
function Get-Median([double[]]$Values) {
    $ordered = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if ($ordered.Count % 2) { return $ordered[$middle] }
    return ($ordered[$middle - 1] + $ordered[$middle]) / 2
}
$summaries = @()
foreach ($case in $Cases) {
    $parts = $case.Split(':')
    if ($parts.Count -ne 4) { throw "Invalid case: $case" }
    $caseName = $case.Replace(':', '-')
    $rows = @()
    for ($sample = 0; $sample -le $Samples; $sample++) {
        $order = if ($sample % 2) { @('baseline','candidate') } else { @('candidate','baseline') }
        foreach ($mode in $order) {
            $directory = "$OutputDirectory/$caseName/$sample-$mode"
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
            $executable = if ($mode -eq 'baseline') { $BaselineExecutable } else { $CandidateExecutable }
            $arguments = @($directory, $parts[0], $parts[1], $parts[2], $parts[3], $Frames)
            if ($Hardware) { $arguments += '--hardware' }
            & $executable @arguments > "$directory/native.log" 2>&1
            if ($LASTEXITCODE -ne 0) { throw "Replay failed: $directory" }
            $row = Import-Csv "$directory/replay.csv"
            $row | Add-Member sample $sample
            $row | Add-Member mode $mode
            if ($sample -gt 0) { $rows += $row }
            if (-not $KeepMedia) {
                Get-ChildItem -LiteralPath $directory -File | Where-Object { $_.Extension -in @('.mp4','.apng') } |
                    ForEach-Object { Remove-Item -LiteralPath $_.FullName }
            }
        }
    }
    $rows | Export-Csv -NoTypeInformation "$OutputDirectory/$caseName/samples.csv"
    $base = @($rows | Where-Object mode -eq baseline)
    $candidate = @($rows | Where-Object mode -eq candidate)
    $improvements = @(for ($i = 0; $i -lt $Samples; $i++) {
        $before = [double]$base[$i].process_ms + [double]$base[$i].finish_ms
        $after = [double]$candidate[$i].process_ms + [double]$candidate[$i].finish_ms
        100 * (1 - $after / $before)
    })
    $random = [Random]::new(1729)
    $bootstrap = @(for ($i = 0; $i -lt 10000; $i++) {
        Get-Median @(for ($j = 0; $j -lt $Samples; $j++) { $improvements[$random.Next($Samples)] })
    }) | Sort-Object
    $median = Get-Median $improvements
    $entry = [pscustomobject]@{
        case = $case; improvement_percent = $median
        ci95_low = $bootstrap[249]; ci95_high = $bootstrap[9749]
        p95_change_percent = 100 * ((Get-Median @($candidate | ForEach-Object {[double]$_.p95_ms})) /
            (Get-Median @($base | ForEach-Object {[double]$_.p95_ms})) - 1)
        cpu_improvement_percent = 100 * (1 - (Get-Median @($candidate | ForEach-Object {[double]$_.cpu_ms})) /
            (Get-Median @($base | ForEach-Object {[double]$_.cpu_ms})))
        passes_improvement_gate = $median -ge 5 -and $bootstrap[249] -gt 0
    }
    $summaries += $entry
    $entry | Format-List
}
$summaries | Export-Csv -NoTypeInformation "$OutputDirectory/comparison.csv"

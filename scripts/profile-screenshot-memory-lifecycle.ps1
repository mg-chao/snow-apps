[CmdletBinding()]
param(
    [string]$BenchmarkPath =
        "build/windows-msvc-performance/snow_shot/Release/snow-shot-screenshot-memory-footprint-benchmark.exe",
    [string]$ApplicationPath =
        "build/windows-msvc-performance/snow_shot/Release/snow_shot.exe",
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [int]$ScreenIndex = 0,
    [int]$ColdDelaySeconds = 15,
    [int]$ActiveDelaySeconds = 15,
    [int]$PostReleaseDelaySeconds = 60,
    [int]$BenchmarkColdStartMinimumWaitMilliseconds = 60000,
    [int]$BenchmarkStageMinimumWaitMilliseconds = 120000,
    [int]$BenchmarkTimeoutMilliseconds = 180000,
    [switch]$CaptureHeapSnapshots,
    [switch]$CaptureMemoryRegionsWithHeapSnapshots,
    [switch]$CaptureDebuggerState
)

$ErrorActionPreference = "Stop"

$workspace = (Get-Location).Path
$benchmark = [IO.Path]::GetFullPath($BenchmarkPath, $workspace)
$application = [IO.Path]::GetFullPath($ApplicationPath, $workspace)
$output = [IO.Path]::GetFullPath($OutputDirectory, $workspace)
if (Test-Path -LiteralPath $output) {
    throw "Output directory already exists: $output"
}
[IO.Directory]::CreateDirectory($output) | Out-Null

$memoryProbe = Join-Path $PSScriptRoot "inspect-process-memory-regions.ps1"
$threadProbe = Join-Path $PSScriptRoot "inspect-process-threads.ps1"
$debugger = "C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2606.22001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe"
$lifecycle = Join-Path $output "app-traces\sample-001-lifecycle.jsonl"
$started = Get-Date
$benchmarkProcess = $null
$applicationProcess = $null
$heapRecordingStarted = $false
$heapSnapshotEnabled = $false
$arguments = @(
    "--app", $application,
    "--output", $output,
    "--scenario", "screenshot-window",
    "--samples", "1",
    "--screen-index", $ScreenIndex.ToString(),
    "--poll-ms", "250",
    "--cold-start-min-wait-ms", $BenchmarkColdStartMinimumWaitMilliseconds.ToString(),
    "--stage-min-wait-ms", $BenchmarkStageMinimumWaitMilliseconds.ToString(),
    "--stability-window", "8",
    "--stability-range-kib", "1536",
    "--final-idle-tolerance-kib", "3072",
    "--timeout-ms", $BenchmarkTimeoutMilliseconds.ToString()
)

function Wait-Until([scriptblock]$Condition, [string]$Description, [int]$TimeoutSeconds = 240) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $Condition) {
            return
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for $Description"
}

function Lifecycle-Contains([string]$EventName) {
    if (-not (Test-Path -LiteralPath $lifecycle)) {
        return $false
    }
    try {
        $stream = [IO.FileStream]::new(
            $lifecycle, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $reader = [IO.StreamReader]::new($stream)
            try {
                $needle = '"event":"{0}"' -f $EventName
                while (-not $reader.EndOfStream) {
                    if ($reader.ReadLine().Contains($needle)) {
                        return $true
                    }
                }
            } finally {
                $reader.Dispose()
            }
        } finally {
            $stream.Dispose()
        }
        return $false
    } catch [Exception] {
        return $false
    }
}

function Capture-State([string]$Name, [int]$DelaySeconds) {
    if ($DelaySeconds -gt 0) {
        Start-Sleep -Seconds $DelaySeconds
    }
    if (-not $CaptureHeapSnapshots -or $CaptureMemoryRegionsWithHeapSnapshots) {
        & $memoryProbe -ProcessId $applicationProcess.Id -Top 200 `
            -OutputPath (Join-Path $output "$Name-regions.json")
    }
    & $threadProbe -ProcessId $applicationProcess.Id `
        -OutputPath (Join-Path $output "$Name-threads.json")
    if ($CaptureHeapSnapshots) {
        $script:heapRecordingStarted = $true
        & wpr -start HeapSnapshot -filemode
        if ($LASTEXITCODE -ne 0) {
            $script:heapRecordingStarted = $false
            throw "Could not start the $Name heap snapshot recording."
        }
        try {
            & wpr -singlesnapshot heap $applicationProcess.Id
            if ($LASTEXITCODE -ne 0) {
                throw "Could not capture the $Name heap snapshot."
            }
            $heapTrace = Join-Path $output "$Name-heap-snapshot.etl"
            & wpr -stop $heapTrace "Snow Shot $Name heap snapshot" -skipPdbGen
            if ($LASTEXITCODE -ne 0) {
                throw "Could not save the $Name heap snapshot recording."
            }
            $script:heapRecordingStarted = $false
        } catch {
            & wpr -cancel
            $script:heapRecordingStarted = $false
            throw
        }
    }
    Write-Host "Captured $Name state"
}

function Stop-LaunchedProcess([Diagnostics.Process]$Process) {
    if ($null -eq $Process) {
        return
    }
    try {
        if (-not $Process.HasExited) {
            Stop-Process -Id $Process.Id -ErrorAction Stop
            $Process.WaitForExit(5000) | Out-Null
        }
    } catch [ArgumentException] {
    }
}

$exitCode = 1
try {
    Write-Host "Starting controlled lifecycle profile"
    if ($CaptureHeapSnapshots) {
        & wpr -HeapTracingConfig snow_shot.exe disable
        & wpr -snapshotconfig heap -name snow_shot.exe disable
        & wpr -snapshotconfig heap -name snow_shot.exe enable
        if ($LASTEXITCODE -ne 0) {
            throw "Could not enable heap snapshots for snow_shot.exe."
        }
        $heapSnapshotEnabled = $true
    }

    $benchmarkProcess =
        Start-Process -FilePath $benchmark -ArgumentList $arguments -PassThru -NoNewWindow

    Wait-Until {
        @(Get-Process -Name snow_shot -ErrorAction SilentlyContinue | Where-Object {
            $_.StartTime -ge $started
        }).Count -eq 1
    } "the benchmark application"
    $applicationProcess = Get-Process -Name snow_shot | Where-Object {
        $_.StartTime -ge $started
    } | Select-Object -First 1
    Write-Host "Application process: $($applicationProcess.Id)"

    Wait-Until { Lifecycle-Contains "app_ready" } "app_ready"
    Capture-State -Name "cold" -DelaySeconds $ColdDelaySeconds

    Wait-Until { Lifecycle-Contains "capture_interaction_ready" } "capture_interaction_ready"
    Capture-State -Name "active" -DelaySeconds $ActiveDelaySeconds
    if ($CaptureDebuggerState -and (Test-Path -LiteralPath $debugger)) {
        & $debugger -p $applicationProcess.Id -logo (Join-Path $output "active-stacks.txt") `
            -c ".reload;~* k;.detach;q"
    }

    Wait-Until { Lifecycle-Contains "capture_released" } "capture_released" 300
    Capture-State -Name "post" -DelaySeconds $PostReleaseDelaySeconds
    if (-not $CaptureHeapSnapshots -or $CaptureMemoryRegionsWithHeapSnapshots) {
        & $memoryProbe -ProcessId $applicationProcess.Id -Top 200 -SamplePrivateData `
            -SampleMinimumPrivateWorkingSetMiB 1 `
            -OutputPath (Join-Path $output "post-regions-sampled.json")
    }

    if ($CaptureDebuggerState -and (Test-Path -LiteralPath $debugger)) {
        $post = Get-Content (Join-Path $output "post-regions.json") -Raw | ConvertFrom-Json
        $addresses = @($post.TopAllocations | Where-Object {
            $_.Type -eq "Private" -and $_.PrivateWorkingSetMiB -ge 1
        } | Select-Object -First 12 -ExpandProperty AllocationBase)
        $addressCommands = ($addresses | ForEach-Object {
            "!address $_;!heap -x $_"
        }) -join ";"
        $commands =
            ".reload;!address -summary;!heap -s;!heap -stat;$addressCommands;~* k;.detach;q"
        & $debugger -p $applicationProcess.Id -logo (Join-Path $output "post-debugger.txt") `
            -c $commands
    }
    Write-Host "Captured stabilized post-release state"

    $benchmarkProcess.WaitForExit()
    $exitCode = $benchmarkProcess.ExitCode
    Write-Host "Benchmark exit code: $exitCode"
} finally {
    if ($heapSnapshotEnabled) {
        & wpr -snapshotconfig heap -name snow_shot.exe disable
        $heapSnapshotEnabled = $false
    }
    if ($heapRecordingStarted) {
        & wpr -cancel
        $heapRecordingStarted = $false
    }
    if ($CaptureHeapSnapshots) {
        & wpr -HeapTracingConfig snow_shot.exe disable
    }
    Stop-LaunchedProcess $applicationProcess
    Stop-LaunchedProcess $benchmarkProcess
}
exit $exitCode

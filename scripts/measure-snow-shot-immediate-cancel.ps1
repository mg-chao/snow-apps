[CmdletBinding()]
param(
    [string]$Executable = "build/windows-msvc-performance/snow_shot/Release/snow_shot.exe",
    [double]$PrivateLimitMiB = 32,
    [int]$TimeoutSeconds = 90,
    [int]$BaselineMinimumWaitSeconds = 20,
    [int]$PostCancelMinimumWaitSeconds = 15,
    [switch]$KeepProcess
)

$ErrorActionPreference = "Stop"
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$instanceId = "memory-" + [Guid]::NewGuid().ToString("N").Substring(0, 16)
$baseArguments = @(
    "--e2e-allow-overlay-capture",
    "--e2e-instance-id=$instanceId"
)

function Get-MemorySample {
    param([int]$ProcessId)

    $process = Get-Process -Id $ProcessId -ErrorAction Stop
    [pscustomobject]@{
        Timestamp = [DateTime]::UtcNow
        PrivateBytes = [int64]$process.PrivateMemorySize64
        WorkingSetBytes = [int64]$process.WorkingSet64
        ThreadCount = $process.Threads.Count
        HandleCount = $process.HandleCount
    }
}

function Wait-ForStableMemory {
    param(
        [int]$ProcessId,
        [int]$MinimumWaitSeconds
    )

    $timer = [Diagnostics.Stopwatch]::StartNew()
    $window = [Collections.Generic.Queue[object]]::new()
    $peakPrivateBytes = [int64]0
    $peakWorkingSetBytes = [int64]0
    while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $sample = Get-MemorySample -ProcessId $ProcessId
        $window.Enqueue($sample)
        while ($window.Count -gt 20) {
            [void]$window.Dequeue()
        }
        $peakPrivateBytes = [Math]::Max($peakPrivateBytes, $sample.PrivateBytes)
        $peakWorkingSetBytes = [Math]::Max($peakWorkingSetBytes, $sample.WorkingSetBytes)

        if ($timer.Elapsed.TotalSeconds -ge $MinimumWaitSeconds -and $window.Count -eq 20) {
            $privateValues = @($window | ForEach-Object { $_.PrivateBytes })
            $workingSetValues = @($window | ForEach-Object { $_.WorkingSetBytes })
            $privateRange = ($privateValues | Measure-Object -Maximum).Maximum -
                ($privateValues | Measure-Object -Minimum).Minimum
            $workingSetRange = ($workingSetValues | Measure-Object -Maximum).Maximum -
                ($workingSetValues | Measure-Object -Minimum).Minimum
            if ($privateRange -lt 1MB -and $workingSetRange -lt 1MB) {
                return [pscustomobject]@{
                    Sample = $sample
                    PeakPrivateBytes = $peakPrivateBytes
                    PeakWorkingSetBytes = $peakWorkingSetBytes
                    ElapsedSeconds = [Math]::Round($timer.Elapsed.TotalSeconds, 2)
                }
            }
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Memory did not converge within $TimeoutSeconds seconds."
}

$primary = $null
try {
    $primary = Start-Process -FilePath $executablePath -ArgumentList $baseArguments `
        -PassThru -WindowStyle Hidden
    $baseline = Wait-ForStableMemory -ProcessId $primary.Id `
        -MinimumWaitSeconds $BaselineMinimumWaitSeconds

    $requestArguments = $baseArguments + "--e2e-immediate-capture-cancel"
    $request = Start-Process -FilePath $executablePath -ArgumentList $requestArguments `
        -PassThru -WindowStyle Hidden
    if (-not $request.WaitForExit(10000)) {
        Stop-Process -Id $request.Id -Force -ErrorAction SilentlyContinue
        throw "The E2E capture/cancel request did not reach the primary process."
    }
    if ($request.ExitCode -ne 0) {
        throw "The E2E capture/cancel request exited with code $($request.ExitCode)."
    }

    $postCancel = Wait-ForStableMemory -ProcessId $primary.Id `
        -MinimumWaitSeconds $PostCancelMinimumWaitSeconds
    $oneMiB = 1MB
    $result = [ordered]@{
        Executable = $executablePath
        ProcessId = $primary.Id
        BaselinePrivateMiB = [Math]::Round($baseline.Sample.PrivateBytes / $oneMiB, 2)
        PostCancelPrivateMiB = [Math]::Round($postCancel.Sample.PrivateBytes / $oneMiB, 2)
        PrivateDeltaMiB = [Math]::Round(
            ($postCancel.Sample.PrivateBytes - $baseline.Sample.PrivateBytes) / $oneMiB,
            2
        )
        BaselineWorkingSetMiB = [Math]::Round($baseline.Sample.WorkingSetBytes / $oneMiB, 2)
        PostCancelWorkingSetMiB = [Math]::Round(
            $postCancel.Sample.WorkingSetBytes / $oneMiB,
            2
        )
        PostCancelThreads = $postCancel.Sample.ThreadCount
        PostCancelHandles = $postCancel.Sample.HandleCount
        PostCancelConvergenceSeconds = $postCancel.ElapsedSeconds
        PrivateLimitMiB = $PrivateLimitMiB
        Passed = $postCancel.Sample.PrivateBytes -lt ($PrivateLimitMiB * $oneMiB)
    }
    [pscustomobject]$result | ConvertTo-Json

    if (-not $result.Passed) {
        throw "Post-cancel private memory is $($result.PostCancelPrivateMiB) MiB; " +
            "the limit is less than $PrivateLimitMiB MiB."
    }
}
finally {
    if (-not $KeepProcess -and $null -ne $primary -and -not $primary.HasExited) {
        Stop-Process -Id $primary.Id -Force -ErrorAction SilentlyContinue
    }
}

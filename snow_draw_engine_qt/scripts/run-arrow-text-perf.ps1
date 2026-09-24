[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Executables,
    [string]$OutputDirectory = "$PSScriptRoot/../../build/windows-msvc-performance/arrow-text-perf",
    [ValidateRange(20, 1000000)]
    [int]$Iterations = 400,
    [ValidateRange(1, 100)]
    [int]$Rounds = 6,
    [long]$CpuAffinity = 4
)

$ErrorActionPreference = "Stop"
$paths = @($Executables | ForEach-Object { (Resolve-Path -LiteralPath $_).Path })
$names = @($paths | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) })
if (@($names | Select-Object -Unique).Count -ne $names.Count) {
    throw "Benchmark executable basenames must be unique."
}
$destination = [IO.Directory]::CreateDirectory($OutputDirectory).FullName
for ($round = 0; $round -lt $Rounds; ++$round) {
    $order = @($paths)
    if ($round % 2 -ne 0) {
        [Array]::Reverse($order)
    }
    foreach ($path in $order) {
        $name = [IO.Path]::GetFileNameWithoutExtension($path)
        $stem = Join-Path $destination "$name-$round"
        Write-Host "Round $($round + 1)/${Rounds}: $name"
        $process = Start-Process -FilePath $path -ArgumentList "$Iterations", "1" `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput "$stem.csv" `
            -RedirectStandardError "$stem.err"
        try {
            $process.ProcessorAffinity = [IntPtr]$CpuAffinity
            $process.PriorityClass = "High"
            $process.WaitForExit()
            if ($process.ExitCode -ne 0) {
                throw "$name failed with exit code $($process.ExitCode); see $stem.err"
            }
        }
        finally {
            $process.Dispose()
        }
    }
}

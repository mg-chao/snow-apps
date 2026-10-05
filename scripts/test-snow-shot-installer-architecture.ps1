#Requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$compiler = Join-Path ${env:ProgramFiles(x86)} 'NSIS/makensis.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'NSIS is required for architecture tests.' }
$root = Join-Path $repo ("build/installer-architecture-tests-" + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
try {
    foreach ($case in @(
        @{ Name = 'x64'; Architecture = 'x64'; Arguments = @(); Accepted = $true },
        @{ Name = 'arm64'; Architecture = 'arm64'; Arguments = @(); Accepted =
            ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString() -eq 'Arm64' -and
             [Environment]::OSVersion.Version.Build -ge 22000) },
        @{ Name = 'arm64-win10'; Architecture = 'arm64'; Arguments = @('/DSIMULATE_ARM64', '/DSIMULATE_WIN10'); Accepted = $false },
        @{ Name = 'arm64-win11'; Architecture = 'arm64'; Arguments = @('/DSIMULATE_ARM64', '/DSIMULATE_WIN11'); Accepted = $true }
    )) {
        $architecture = $case.Architecture
        $marker = Join-Path $root "$($case.Name)-existing-user-data.txt"
        [IO.File]::WriteAllText($marker, 'preserved')
        $output = Join-Path $root "$($case.Name).exe"
        $arguments = $case.Arguments
        & $compiler /V2 @arguments "/DOUTPUT=$output" "/DMARKER=$marker" "/DPACKAGING=$repo/snow_shot/packaging" `
            "/DSNOW_SHOT_INSTALLER_ARCHITECTURE=$architecture" "$repo/snow_shot/tests/installer_architecture_tests.nsi"
        if ($LASTEXITCODE -ne 0) { throw 'Architecture fixture compilation failed.' }
        $process = Start-Process -FilePath $output -ArgumentList '/S' -PassThru -WindowStyle Hidden
        try {
            if (-not $process.WaitForExit(20000)) { $process.Kill(); throw 'Architecture installer fixture timed out.' }
            $accepted = $case.Accepted
            $expectedCode = if ($accepted) { 0 } else { 14 }
            $expectedData = if ($accepted) { 'accepted' } else { 'preserved' }
            if ($process.ExitCode -ne $expectedCode -or [IO.File]::ReadAllText($marker) -cne $expectedData) {
                throw 'The installer architecture guard must reject before modifying the existing installation.'
            }
        } finally { $process.Dispose() }
        Write-Output "PASS: $($case.Name) architecture guard"
    }
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    $allowed = [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe installer test cleanup.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

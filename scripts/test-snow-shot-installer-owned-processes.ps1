[CmdletBinding()]
param([ValidateSet('x64', 'arm64')][string]$Architecture = 'x64')

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$testRoot = Join-Path $repoRoot "build/installer-owned-processes-tests-$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $testRoot
$compiler = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
if ((Get-SnowWindowsHostArchitecture) -cne $Architecture) {
    throw 'Installer behavior tests require the matching native Windows host.'
}
$null = Add-SnowMsvcToolsToPath -Architecture $Architecture
$fixture = Join-Path $testRoot 'headless.exe'
& cl /nologo /std:c++20 /W4 /WX /O2 /MT /DUNICODE /D_UNICODE "/Fe:$fixture" `
    "/Fo:$testRoot/headless.obj" "$repoRoot/snow_shot/tests/installer_headless_fixture.cpp" `
    /link /SUBSYSTEM:WINDOWS shell32.lib
if ($LASTEXITCODE -ne 0) { throw 'Headless fixture compilation failed.' }
$updater = Join-Path $testRoot 'updater.exe'
& cl /nologo /std:c++20 /W4 /WX /O2 /MT /DUNICODE /D_UNICODE "/Fe:$updater" `
    "/Fo:$testRoot/updater.obj" "$repoRoot/snow_shot/tests/installer_updater_stub.cpp" `
    /link /SUBSYSTEM:WINDOWS shell32.lib
if ($LASTEXITCODE -ne 0) { throw 'Updater fixture compilation failed.' }

function Run-Fixture([string]$Path, [string[]]$Arguments = @('/S')) {
    $process = Start-Process -FilePath $Path -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(45000)) {
        $process.Kill()
        $process.WaitForExit()
        throw 'Installer fixture timed out.'
    }
    return $process.ExitCode
}

function Start-Headless([string]$Path, [string]$HeldFile) {
    $options = @{FilePath=$Path; WindowStyle='Hidden'; PassThru=$true}
    if ($HeldFile) { $options.ArgumentList = @('--hold-file', "`"$HeldFile`"") }
    $process = Start-Process @options
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        try {
            $ready = [Threading.EventWaitHandle]::OpenExisting("Local\SnowShotInstallerTest-$($process.Id)")
            $ready.Dispose()
            return $process
        }
        catch [Threading.WaitHandleCannotBeOpenedException] { Start-Sleep -Milliseconds 20 }
    }
    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    throw 'Headless fixture did not become ready.'
}

foreach ($edition in 'Full', 'Mini') {
    $appName = if ($edition -eq 'Mini') { 'snow_shot_mini' } else { 'snow_shot' }
    $updaterName = if ($edition -eq 'Mini') { 'snow-shot-mini-updater' } else { 'snow-shot-updater' }
    $mcpName = if ($edition -eq 'Mini') { 'snow-shot-mini-mcp' } else { 'snow-shot-mcp' }
    $helpers = @($mcpName, $updaterName, 'crashpad_handler')
    if ($edition -eq 'Full') { $helpers += 'snow-ocr-process' }
    foreach ($helper in $helpers) {
        $destination = Join-Path $testRoot "$edition $helper 安"
        $binaries = Join-Path $destination 'bin'
        $null = New-Item -ItemType Directory -Path $binaries
        $helperPath = Join-Path $binaries "$helper.exe"
        $appPath = Join-Path $binaries "$appName.exe"
        $sentinel = Join-Path $destination 'user-data.txt'
        [IO.File]::WriteAllText($sentinel, 'preserve user data')
        $otherDir = Join-Path $testRoot "unrelated $edition $helper"
        $null = New-Item -ItemType Directory -Path $otherDir
        $otherPath = Join-Path $otherDir "$helper.exe"
        Copy-Item -LiteralPath $fixture -Destination $otherPath
        $installers = @{}
        foreach ($answer in 'silent', 'declined', 'closeApp') {
            $output = Join-Path $testRoot "$edition-$helper-$answer.exe"
            $arguments = @('/V2', "/DOUTPUT=$output", "/DDESTINATION=$destination",
                "/DPAYLOAD=$fixture", "/DPACKAGING=$repoRoot/snow_shot/packaging",
                "/DSNOW_SHOT_INSTALLER_EXECUTABLE=$appName", "/DSNOW_SHOT_INSTALLER_UPDATER=$updaterName",
                "/DSNOW_SHOT_INSTALLER_MCP=$mcpName", "/DHELPER=$helper")
            if ($answer -ne 'silent') { $arguments += "/DANSWER=$answer" }
            & $compiler @arguments "$repoRoot/snow_shot/tests/installer_owned_processes_tests.nsi"
            if ($LASTEXITCODE -ne 0) { throw 'Owned process installer compilation failed.' }
            $installers[$answer] = $output
        }
        foreach ($answer in 'silent', 'declined', 'closeApp') {
            if ((Run-Fixture $installers[$answer]) -ne 0) { throw 'Fixture setup failed.' }
            Remove-Item -LiteralPath $appPath
            Copy-Item -LiteralPath $fixture -Destination $helperPath
            if ($helper -ne $updaterName) {
                Copy-Item -LiteralPath $updater -Destination (Join-Path $binaries "$updaterName.exe")
            }
            $first = Start-Headless $helperPath
            $second = Start-Headless $helperPath
            $other = Start-Headless $otherPath
            try {
                $result = Run-Fixture (Join-Path $destination 'uninstall.exe') @('/S', "_?=$destination")
                if ($answer -eq 'closeApp') {
                    if ($result -ne 0 -or -not $first.WaitForExit(5000) -or -not $second.WaitForExit(5000) -or
                        (Test-Path -LiteralPath $helperPath)) {
                        throw "Accepted uninstall must close both $helper processes and remove the payload; exit $result."
                    }
                }
                elseif ($result -ne 10 -or $first.HasExited -or $second.HasExited -or
                    -not (Test-Path -LiteralPath $helperPath) -or
                    (Test-Path -LiteralPath (Join-Path $destination 'snow-shot-updater-ran.txt'))) {
                    throw "Refused/silent uninstall must preserve running $helper and stop before cleanup; exit $result."
                }
                if ($other.HasExited -or [IO.File]::ReadAllText($sentinel) -cne 'preserve user data') {
                    throw 'Uninstall affected another installation or user data.'
                }
            }
            finally {
                foreach ($process in $first, $second, $other) {
                    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
                    $process.Dispose()
                }
            }
        }
        # Preserve the existing Restart Manager contract: acceptance also closes
        # a resource holder reading this installation's file, even if its image
        # lives elsewhere. Only the forced fallback is restricted to our image.
        if ($helper -eq $mcpName) {
            if ((Run-Fixture $installers.closeApp) -ne 0) { throw 'Resource holder fixture setup failed.' }
            Copy-Item -LiteralPath $fixture -Destination $helperPath
            Copy-Item -LiteralPath $updater -Destination (Join-Path $binaries "$updaterName.exe")
            $holder = Start-Headless $otherPath $helperPath
            try {
                $result = Run-Fixture (Join-Path $destination 'uninstall.exe') @('/S', "_?=$destination")
                if ($result -ne 0 -or -not $holder.WaitForExit(5000) -or
                    (Test-Path -LiteralPath $appPath) -or (Test-Path -LiteralPath $helperPath)) {
                    throw "Restart Manager must close holders of the installation file before accepted cleanup; exit $result."
                }
            }
            finally {
                if (-not $holder.HasExited) { $holder.Kill(); $holder.WaitForExit() }
                $holder.Dispose()
            }
        }
        $setup = Join-Path $testRoot "$edition-$helper-setup.exe"
        & $compiler /V2 "/DOUTPUT=$setup" "/DDESTINATION=$destination" "/DPAYLOAD=$fixture" `
            "/DPACKAGING=$repoRoot/snow_shot/packaging" "/DSNOW_SHOT_INSTALLER_EXECUTABLE=$appName" `
            "/DSNOW_SHOT_INSTALLER_UPDATER=$updaterName" "/DSNOW_SHOT_INSTALLER_MCP=$mcpName" `
            "/DHELPER=$helper" /DCHECK_SETUP "$repoRoot/snow_shot/tests/installer_owned_processes_tests.nsi"
        if ($LASTEXITCODE -ne 0) { throw 'Setup guard compilation failed.' }
        Copy-Item -LiteralPath $fixture -Destination $helperPath
        $process = Start-Headless $helperPath
        try {
            if ((Run-Fixture $setup) -ne 0 -or -not $process.WaitForExit(5000) -or
                (Test-Path -LiteralPath (Join-Path $destination 'restart-required.txt'))) {
                throw 'Setup must close helpers without requesting a main application restart.'
            }
        }
        finally {
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            $process.Dispose()
        }
        Write-Output "PASS: $edition $helper is protected before cleanup, closes when accepted, and leaves other installations/data intact."
    }
}

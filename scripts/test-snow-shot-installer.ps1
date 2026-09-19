[CmdletBinding()]
param([switch]$ReproduceOnly)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$compiler = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
if (-not (Test-Path -LiteralPath $compiler)) { throw "NSIS is required for installer tests." }
$testRoot = Join-Path $repoRoot "build\installer-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $testRoot | Out-Null
$fixture = Join-Path $testRoot "fixture.exe"
$destination = Join-Path $testRoot ("installed app " + [char]0x5b89)
New-Item -ItemType Directory -Path $destination | Out-Null
$installed = Join-Path $destination "snow_shot.exe"

function Compile-Installer {
    param([string]$Output, [switch]$Guard, [string]$Answer)
    $payload = Join-Path $repoRoot "snow_shot\tests\installer_process_fixture.cpp"
    $arguments = @("/V2", "/DOUTPUT=$Output", "/DDESTINATION=$destination", "/DPAYLOAD=$payload")
    if ($Guard) { $arguments += "/DGUARD=$repoRoot\snow_shot\packaging\RunningApplication.nsh" }
    if ($Answer) { $arguments += "/DANSWER=$Answer" }
    & $compiler @arguments "$repoRoot\snow_shot\tests\installer_running_app_tests.nsi"
    if ($LASTEXITCODE -ne 0) { throw "Installer test compilation failed." }
}

function Run-Installer {
    param([string]$Path)
    $process = Start-Process -FilePath $Path -ArgumentList "/S" -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(20000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Installer test timed out."
    }
    return $process.ExitCode
}

function Run-Uninstaller {
    param([string]$Destination)
    $uninstaller = Join-Path $Destination "uninstall.exe"
    $process = Start-Process -FilePath $uninstaller -ArgumentList "/S", "_?=$Destination" `
        -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(20000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Uninstaller test timed out."
    }
    return $process.ExitCode
}

function Reset-OwnedInstallation {
    param([string]$Installer, [string]$Destination, [string]$Updater)
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    if ((Run-Installer $Installer) -ne 0) { throw "Owned cleanup installation failed." }
    Set-Content -LiteralPath (Join-Path $Destination "snow-shot-installation.json") -Value '{"schema":1}'
    if ($Updater) {
        Copy-Item -LiteralPath $Updater -Destination (Join-Path $Destination "bin\snow-shot-updater.exe")
    }
}

function Start-Fixture {
    param([string]$Path)
    $process = Start-Process -FilePath $Path -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        try {
            $ready = [System.Threading.EventWaitHandle]::OpenExisting("Local\SnowShotInstallerTest-$($process.Id)")
            $ready.Dispose()
            return $process
        }
        catch [System.Threading.WaitHandleCannotBeOpenedException] {
            Start-Sleep -Milliseconds 20
        }
    }
    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    throw "Fixture did not become ready."
}

. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
$null = Set-SnowBuildEnvironment -Preset "windows-msvc-debug"
& cl /nologo /std:c++20 /W4 /WX /O2 /MT /DUNICODE /D_UNICODE "/Fe:$fixture" "/Fo:$testRoot\fixture.obj" `
    "$repoRoot\snow_shot\tests\installer_process_fixture.cpp" /link /SUBSYSTEM:WINDOWS user32.lib
if ($LASTEXITCODE -ne 0) { throw "Fixture compilation failed." }
Copy-Item -LiteralPath $fixture -Destination $installed
$baseline = Join-Path $testRoot "baseline.exe"
Compile-Installer -Output $baseline
$app = Start-Fixture $installed
try {
    if ($app.HasExited) { throw "Fixture exited before the test." }
    $result = Run-Installer $baseline
    if ($result -eq 0) { throw "Expected the unguarded installer to fail on the running executable." }
    Write-Output "Reproduced: unguarded extraction fails with exit code $result while the executable is running."
    if (-not $ReproduceOnly) {
        $guarded = Join-Path $testRoot "guarded.exe"
        Compile-Installer -Output $guarded -Guard
        $result = Run-Installer $guarded
        if ($result -ne 10) { throw "Expected running-app refusal (10), got $result." }
        if ($app.HasExited) { throw "Silent setup must not terminate the running application." }
        Write-Output "PASS: silent setup refuses to overwrite a running executable and leaves it running."
        $declined = Join-Path $testRoot "declined.exe"
        Compile-Installer -Output $declined -Guard -Answer "declined"
        if ((Run-Installer $declined) -ne 10 -or $app.HasExited) {
            throw "Declining must stop setup and leave the application running."
        }
        if ((Get-FileHash $installed).Hash -ne (Get-FileHash $fixture).Hash) {
            throw "Refusing setup must leave the installed executable unchanged."
        }
        Write-Output "PASS: declining leaves the running application and installed file unchanged."
        $accepted = Join-Path $testRoot "accepted.exe"
        Compile-Installer -Output $accepted -Guard -Answer "closeApp"
        $secondApp = Start-Fixture $installed
        $otherApp = Start-Fixture $fixture
        try {
            $result = Run-Installer $accepted
            if ($result -ne 0 -or -not $app.WaitForExit(5000) -or -not $secondApp.WaitForExit(5000)) {
                throw "Accepting must close all file holders and complete setup; exit code $result."
            }
            if ($otherApp.HasExited) { throw "A copy running from another directory must be preserved." }
        }
        finally {
            foreach ($process in @($secondApp, $otherApp)) {
                if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            }
        }
        Write-Output "PASS: accepting closes all holders, completes extraction, and preserves other installations."
    }
}
finally {
    if (-not $app.HasExited) { $app.Kill(); $app.WaitForExit() }
}
if (-not $ReproduceOnly) {
    $result = Run-Installer $guarded
    if ($result -ne 0) { throw "Expected installation to succeed after exit, got $result." }
    Write-Output "PASS: setup succeeds once the installed application exits."

    Copy-Item -LiteralPath $fixture -Destination $installed
    $app = Start-Fixture $installed
    try {
        $uninstaller = Join-Path $destination "uninstall.exe"
        $process = Start-Process -FilePath $uninstaller -ArgumentList "/S", "_?=$destination" `
            -WindowStyle Hidden -PassThru
        if (-not $process.WaitForExit(20000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Uninstaller test timed out."
        }
        if ($process.ExitCode -ne 10 -or $app.HasExited) {
            throw "Silent uninstall must refuse to remove a running application."
        }
        Write-Output "PASS: standalone silent uninstall protects the running application."
    }
    finally {
        if (-not $app.HasExited) { $app.Kill(); $app.WaitForExit() }
    }

    Move-Item -LiteralPath $installed -Destination "$installed.saved"
    if ((Run-Installer $guarded) -ne 0) { throw "Fresh installation must succeed." }
    $payload = Join-Path $repoRoot "snow_shot\tests\installer_process_fixture.cpp"
    if ((Get-FileHash $installed).Hash -ne (Get-FileHash $payload).Hash) {
        throw "Successful setup must actually extract the new payload."
    }
    Write-Output "PASS: fresh installation extracts the expected payload."

    # Restart Manager rejects a registered directory. Exercise a real API
    # error and ensure setup does not continue as if no application was running.
    Move-Item -LiteralPath $installed -Destination "$installed.payload"
    New-Item -ItemType Directory -Path $installed | Out-Null
    if ((Run-Installer $guarded) -ne 11) { throw "Detection errors must stop setup with exit code 11." }
    Write-Output "PASS: Restart Manager detection errors stop setup."

    # A partial installation with missing components must still uninstall,
    # while a helper that ran and failed must keep blocking the uninstall.
    $ownedDestination = Join-Path $testRoot ("owned cleanup " + [char]0x5b89)
    $stubSource = Join-Path $repoRoot "snow_shot\tests\installer_updater_stub.cpp"
    $stubWorking = Join-Path $testRoot "updater-stub.exe"
    $stubFailing = Join-Path $testRoot "updater-stub-failing.exe"
    & cl /nologo /std:c++20 /W4 /WX /O2 /MT /DUNICODE /D_UNICODE "/Fe:$stubWorking" `
        "/Fo:$testRoot\updater-stub.obj" $stubSource /link /SUBSYSTEM:WINDOWS shell32.lib
    if ($LASTEXITCODE -ne 0) { throw "Update helper stub compilation failed." }
    & cl /nologo /std:c++20 /W4 /WX /O2 /MT /DUNICODE /D_UNICODE /DSNOW_SHOT_UPDATER_STUB_FAIL `
        "/Fe:$stubFailing" "/Fo:$testRoot\updater-stub-failing.obj" $stubSource /link /SUBSYSTEM:WINDOWS shell32.lib
    if ($LASTEXITCODE -ne 0) { throw "Failing update helper stub compilation failed." }
    $ownedInstaller = Join-Path $testRoot "owned-cleanup.exe"
    & $compiler /V2 "/DOUTPUT=$ownedInstaller" "/DDESTINATION=$ownedDestination" "/DPAYLOAD=$fixture" `
        "/DGUARD=$repoRoot\snow_shot\packaging\RunningApplication.nsh" `
        "/DOWNED_CLEANUP=$repoRoot\snow_shot\packaging\OwnedCleanup.nsh" `
        "$repoRoot\snow_shot\tests\installer_owned_cleanup_tests.nsi"
    if ($LASTEXITCODE -ne 0) { throw "Owned cleanup test compilation failed." }
    $ownedApp = Join-Path $ownedDestination "bin\snow_shot.exe"
    $ownedManifest = Join-Path $ownedDestination "snow-shot-installation.json"
    $ownedMarker = Join-Path $ownedDestination "snow-shot-updater-ran.txt"

    Reset-OwnedInstallation $ownedInstaller $ownedDestination $null
    $result = Run-Uninstaller $ownedDestination
    if ($result -ne 0 -or (Test-Path -LiteralPath $ownedApp) -or (Test-Path -LiteralPath $ownedManifest)) {
        throw "A missing update helper must not block uninstallation; exit code $result."
    }
    Write-Output "PASS: silent uninstall completes when the update helper is missing."

    Reset-OwnedInstallation $ownedInstaller $ownedDestination $stubWorking
    $result = Run-Uninstaller $ownedDestination
    if ($result -ne 0 -or -not (Test-Path -LiteralPath $ownedMarker)) {
        throw "Uninstall must run the update helper when it is present; exit code $result."
    }
    Write-Output "PASS: uninstall still runs the update helper when it is present."

    Reset-OwnedInstallation $ownedInstaller $ownedDestination $stubFailing
    $result = Run-Uninstaller $ownedDestination
    if ($result -ne 12 -or -not (Test-Path -LiteralPath $ownedApp)) {
        throw "A failing update helper must still block uninstallation; exit code $result."
    }
    Write-Output "PASS: uninstall still blocks when the update helper fails."

    Reset-OwnedInstallation $ownedInstaller $ownedDestination $stubWorking
    Remove-Item -LiteralPath $ownedManifest
    $result = Run-Uninstaller $ownedDestination
    if ($result -ne 0 -or -not (Test-Path -LiteralPath $ownedMarker)) {
        throw "Uninstall must still run the update helper without a manifest; exit code $result."
    }
    Write-Output "PASS: uninstall still runs the update helper without a manifest."

    Reset-OwnedInstallation $ownedInstaller $ownedDestination $stubWorking
    Remove-Item -LiteralPath $ownedApp
    $result = Run-Uninstaller $ownedDestination
    if ($result -ne 0 -or (Test-Path -LiteralPath $ownedManifest)) {
        throw "A missing application binary must not block uninstallation; exit code $result."
    }
    Write-Output "PASS: uninstall completes when the application binary is missing."

    Reset-OwnedInstallation $ownedInstaller $ownedDestination $stubWorking
    Set-Content -LiteralPath (Join-Path $ownedDestination "bin\snow-shot-updater.exe") -Value "not executable"
    $result = Run-Uninstaller $ownedDestination
    if ($result -ne 0 -or (Test-Path -LiteralPath $ownedApp)) {
        throw "An unlaunchable update helper must not block uninstallation; exit code $result."
    }
    Write-Output "PASS: uninstall completes when the update helper cannot start."

    $cpackBuild = Join-Path $testRoot "cpack"
    & cmake -S "$repoRoot\snow_shot\tests\installer_packaging" -B $cpackBuild
    if ($LASTEXITCODE -ne 0) { throw "Installer integration configuration failed." }
    & cpack --config "$cpackBuild\CPackConfig.cmake" -G NSIS -B $cpackBuild
    if ($LASTEXITCODE -ne 0) { throw "CPack installer compilation failed." }
    $scriptPath = Get-ChildItem -LiteralPath "$cpackBuild\_CPack_Packages" -Recurse -Filter project.nsi
    $generated = Get-Content -LiteralPath $scriptPath.FullName -Raw
    $init = [regex]::Match($generated, '(?s)Function \.onInit\r?\n.*?FunctionEnd').Value
    if ($init.IndexOf('Call SnowShotEnsureAppClosed') -lt 0 -or
        $init.IndexOf('Call SnowShotEnsureAppClosed') -gt $init.IndexOf('ExecWait')) {
        throw "The running-app check must precede the old uninstaller in .onInit."
    }
    $core = [regex]::Match($generated, '(?s)Section "-Core installation".*?SectionEnd').Value
    if ($core.IndexOf('Call SnowShotEnsureAppClosed') -lt 0 -or
        $core.IndexOf('Call SnowShotEnsureAppClosed') -gt $core.IndexOf('File /r')) {
        throw "The destination check must precede file extraction."
    }
    $uninstall = [regex]::Match($generated, '(?s)Section "Uninstall".*?SectionEnd').Value
    if ($uninstall.IndexOf('Call un.SnowShotEnsureAppClosed') -lt 0 -or
        $uninstall.IndexOf('Call un.SnowShotEnsureAppClosed') -gt $uninstall.IndexOf('Delete "')) {
        throw "The uninstall check must precede file deletion."
    }
    Write-Output "PASS: CPack compiles the guard and checks the old installation before launching its uninstaller."
}
Write-Output "Installer test artifacts: $testRoot"

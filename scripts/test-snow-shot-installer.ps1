[CmdletBinding()]
param([switch]$ReproduceOnly, [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64')

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
    return Wait-FixtureReady $process
}

function Wait-FixtureReady {
    param([Diagnostics.Process]$Process)
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not $Process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        try {
            $ready = [System.Threading.EventWaitHandle]::OpenExisting("Local\SnowShotInstallerTest-$($Process.Id)")
            $ready.Dispose()
            return $Process
        }
        catch [System.Threading.WaitHandleCannotBeOpenedException] {
            Start-Sleep -Milliseconds 20
        }
    }
    if (-not $Process.HasExited) { $Process.Kill(); $Process.WaitForExit() }
    throw "Fixture did not become ready."
}

function Wait-RestartedFixture {
    param([string]$Directory, [string]$Executable)
    $marker = Join-Path $Directory 'snow-shot-desktop-launch.txt'
    if (-not (Test-Path -LiteralPath $marker)) { throw 'Successful update did not request a desktop launch.' }
    $process = Get-Process -Id ([int][IO.File]::ReadAllText($marker))
    if ($process.Path -ine (Join-Path $Directory "bin\$Executable.exe")) {
        throw 'Restart launched the wrong installation.'
    }
    return Wait-FixtureReady $process
}

function Compile-RestartInstaller {
    param([string]$Output, [string]$Destination, [string]$Executable, [string]$UpdaterName,
        [string]$Payload, [string]$Updater, [string]$Previous, [switch]$FailInstall)
    $arguments = @('/V2', "/DOUTPUT=$Output", "/DDESTINATION=$Destination",
        "/DPACKAGING=$repoRoot\snow_shot\packaging", "/DPAYLOAD=$Payload", "/DUPDATER=$Updater",
        "/DSNOW_SHOT_INSTALLER_EXECUTABLE=$Executable", "/DSNOW_SHOT_INSTALLER_UPDATER=$UpdaterName")
    if ($Previous) { $arguments += "/DPREVIOUS_DESTINATION=$Previous" }
    if ($FailInstall) { $arguments += '/DFAIL_INSTALL' }
    & $compiler @arguments "$repoRoot\snow_shot\tests\installer_restart_tests.nsi"
    if ($LASTEXITCODE -ne 0) { throw 'Restart installer compilation failed.' }
}

. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
if ((Get-SnowWindowsHostArchitecture) -cne $Architecture) { throw 'Installer behavior tests require the matching native Windows host.' }
$null = Add-SnowMsvcToolsToPath -Architecture $Architecture
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
        if ($result -ne 0 -or -not $app.WaitForExit(5000)) {
            throw "Silent setup must close the running application and install; exit code $result."
        }
        $payload = Join-Path $repoRoot "snow_shot\tests\installer_process_fixture.cpp"
        if ((Get-FileHash $installed).Hash -ne (Get-FileHash $payload).Hash) {
            throw "Silent setup must extract the replacement payload after closing the application."
        }
        Write-Output "PASS: silent setup closes the running application and installs the replacement."
        Copy-Item -LiteralPath $fixture -Destination $installed
        $app = Start-Fixture $installed
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

    $updatedFixture = Join-Path $testRoot 'updated-fixture.exe'
    & cl /nologo /std:c++20 /W4 /WX /O2 /MT /DUNICODE /D_UNICODE /DSNOW_SHOT_INSTALLER_TEST_UPDATED `
        "/Fe:$updatedFixture" "/Fo:$testRoot\updated-fixture.obj" `
        "$repoRoot\snow_shot\tests\installer_process_fixture.cpp" /link /SUBSYSTEM:WINDOWS user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Updated fixture compilation failed.' }
    foreach ($edition in 'Full', 'Mini') {
        $executable = if ($edition -eq 'Mini') { 'snow_shot_mini' } else { 'snow_shot' }
        $updaterName = if ($edition -eq 'Mini') { 'snow-shot-mini-updater' } else { 'snow-shot-updater' }
        $updater = Join-Path $testRoot "desktop-updater-$edition.exe"
        $defines = @()
        if ($edition -eq 'Mini') { $defines += '/DSNOW_SHOT_UPDATER_STUB_MINI' }
        & cl /nologo /std:c++20 /W4 /WX /O2 /MT /DUNICODE /D_UNICODE @defines "/Fe:$updater" `
            "/Fo:$testRoot\desktop-updater-$edition.obj" $stubSource /link /SUBSYSTEM:WINDOWS shell32.lib
        if ($LASTEXITCODE -ne 0) { throw 'Desktop updater fixture compilation failed.' }
        $restartDestination = Join-Path $testRoot ("restart $edition " + [char]0x5b89)
        $restartInstalled = Join-Path $restartDestination "bin\$executable.exe"
        $marker = Join-Path $restartDestination 'snow-shot-desktop-launch.txt'
        $setup = Join-Path $testRoot "restart-$edition.exe"
        $options = @{ Executable = $executable; UpdaterName = $updaterName;
            Payload = $updatedFixture; Updater = $updater }
        Compile-RestartInstaller -Output $setup -Destination $restartDestination @options
        if ((Run-Installer $setup) -ne 0 -or (Test-Path -LiteralPath $marker)) {
            throw 'Fresh silent installation must succeed without launching the application.'
        }

        Copy-Item -LiteralPath $fixture -Destination $restartInstalled -Force
        $first = Start-Fixture $restartInstalled
        $second = Start-Fixture $restartInstalled
        $otherDirectory = Join-Path $testRoot "other $edition"
        $null = New-Item -ItemType Directory -Path $otherDirectory
        $otherInstalled = Join-Path $otherDirectory "$executable.exe"
        Copy-Item -LiteralPath $fixture -Destination $otherInstalled
        $other = Start-Fixture $otherInstalled
        $crashPath = Join-Path $restartDestination 'bin\crashpad_handler.exe'
        Copy-Item -LiteralPath $fixture -Destination $crashPath
        $crash = Start-Fixture $crashPath
        $restarted = $null
        try {
            if ((Run-Installer $setup) -ne 0 -or -not $first.WaitForExit(5000) -or
                -not $second.WaitForExit(5000) -or -not $crash.WaitForExit(5000) -or $other.HasExited) {
                throw 'Silent upgrade must close all affected holders and preserve other installations.'
            }
            if ((Get-FileHash $restartInstalled).Hash -ne (Get-FileHash $updatedFixture).Hash) {
                throw 'Silent upgrade did not install the updated executable.'
            }
            $restarted = Wait-RestartedFixture $restartDestination $executable
        } finally {
            foreach ($process in @($first, $second, $crash, $other, $restarted)) {
                if ($process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            }
        }
        Remove-Item -LiteralPath $marker
        if ((Run-Installer $setup) -ne 0 -or (Test-Path -LiteralPath $marker)) {
            throw 'Silent upgrade of a stopped application must not launch it.'
        }
        $crash = Start-Fixture $crashPath
        try {
            if ((Run-Installer $setup) -ne 0 -or -not $crash.WaitForExit(5000) -or
                (Test-Path -LiteralPath $marker)) {
                throw 'Closing only Crashpad must not launch the main application.'
            }
        } finally {
            if (-not $crash.HasExited) { $crash.Kill(); $crash.WaitForExit() }
        }

        $failed = Join-Path $testRoot "failed-restart-$edition.exe"
        Compile-RestartInstaller -Output $failed -Destination $restartDestination -FailInstall @options
        $first = Start-Fixture $restartInstalled
        try {
            if ((Run-Installer $failed) -ne 21 -or -not $first.WaitForExit(5000) -or
                (Test-Path -LiteralPath $marker)) {
                throw 'Failed installation must not launch an application from the incomplete installation.'
            }
        } finally {
            if (-not $first.HasExited) { $first.Kill(); $first.WaitForExit() }
        }

        $movedDestination = Join-Path $testRoot "moved $edition"
        $moved = Join-Path $testRoot "moved-restart-$edition.exe"
        Compile-RestartInstaller -Output $moved -Destination $movedDestination -Previous $restartDestination @options
        $first = Start-Fixture $restartInstalled
        $restarted = $null
        try {
            if ((Run-Installer $moved) -ne 0 -or -not $first.WaitForExit(5000) -or
                (Test-Path -LiteralPath $restartInstalled)) {
                throw 'Upgrade must close the previous installation before its silent uninstaller runs.'
            }
            $restarted = Wait-RestartedFixture $movedDestination $executable
        } finally {
            foreach ($process in @($first, $restarted)) {
                if ($process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            }
        }
        Write-Output "PASS: $edition silent upgrade restarts the updated app; fresh, stopped, Crashpad-only, failed, and moved installations behave correctly."
    }

    $cpackBuild = Join-Path $testRoot "cpack"
    & cmake -S "$repoRoot\snow_shot\tests\installer_packaging" -B $cpackBuild "-DSNOW_WINDOWS_ARCHITECTURE=$Architecture"
    if ($LASTEXITCODE -ne 0) { throw "Installer integration configuration failed." }
    & cpack --config "$cpackBuild\CPackConfig.cmake" -G NSIS -B $cpackBuild
    if ($LASTEXITCODE -ne 0) { throw "CPack installer compilation failed." }
    $scriptPath = Get-ChildItem -LiteralPath "$cpackBuild\_CPack_Packages" -Recurse -Filter project.nsi
    $generated = Get-Content -LiteralPath $scriptPath.FullName -Raw
    $init = [regex]::Match($generated, '(?s)Function \.onInit\r?\n.*?FunctionEnd').Value
    if ($init.IndexOf('Call SnowShotEnsureMainAppClosed') -lt 0 -or
        $init.IndexOf('Call SnowShotEnsureMainAppClosed') -gt $init.IndexOf('ExecWait')) {
        throw "The running-app check must precede the old uninstaller in .onInit."
    }
    if ($init.IndexOf('Call SnowShotPrepareUpgradeHelper') -lt 0 -or
        $init.IndexOf('Call SnowShotPrepareUpgradeHelper') -gt $init.IndexOf('ExecWait') -or
        -not $init.Contains('Call SnowShotRestoreUpgradeHelper')) {
        throw 'Legacy upgrades must prepare the bundled helper before uninstall and restore it on failure.'
    }
    $core = [regex]::Match($generated, '(?s)Section "-Core installation".*?SectionEnd').Value
    if ($core.IndexOf('Call SnowShotEnsureMainAppClosed') -lt 0 -or
        $core.IndexOf('Call SnowShotEnsureMainAppClosed') -gt $core.IndexOf('File /r')) {
        throw "The destination check must precede file extraction."
    }
    if (-not $generated.Contains('!include "' + $repoRoot + '\snow_shot\packaging\InstallerLaunch.nsh"')) {
        throw 'CPack must use the shared successful-install restart callback.'
    }
    $uninstall = [regex]::Match($generated, '(?s)Section "Uninstall".*?SectionEnd').Value
    if ($uninstall.IndexOf('Call un.SnowShotEnsureAppClosed') -lt 0 -or
        $uninstall.IndexOf('Call un.SnowShotEnsureAppClosed') -gt $uninstall.IndexOf('Delete "')) {
        throw "The uninstall check must precede file deletion."
    }
    Write-Output "PASS: CPack compiles the guard and checks the old installation before launching its uninstaller."
}
Write-Output "Installer test artifacts: $testRoot"

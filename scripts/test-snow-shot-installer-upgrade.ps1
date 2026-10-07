[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$HelperPath,
    [ValidateSet('Full', 'Mini')][string]$Edition = 'Full',
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
if ((Get-SnowWindowsHostArchitecture) -cne $Architecture) { throw 'Upgrade tests require the matching native host.' }
$null = Add-SnowMsvcToolsToPath -Architecture $Architecture
$helper = (Resolve-Path -LiteralPath $HelperPath).Path
$compiler = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
$root = Join-Path $repo "build\installer-upgrade-tests-$([guid]::NewGuid().ToString('N'))"
$executable = if ($Edition -eq 'Mini') { 'snow_shot_mini' } else { 'snow_shot' }
$updater = if ($Edition -eq 'Mini') { 'snow-shot-mini-updater' } else { 'snow-shot-updater' }
$recordName = if ($Edition -eq 'Mini') { 'snow-shot-mini-installation.json' } else { 'snow-shot-installation.json' }
$markerName = if ($Edition -eq 'Mini') { '__mini_data_directory' } else { '__data_directory' }
$product = if ($Edition -eq 'Mini') { 'snow-shot-mini' } else { 'snow-shot' }
$bundled = Join-Path $root 'bundled'
$null = New-Item -ItemType Directory -Path (Join-Path $bundled 'bin')
Copy-Item -LiteralPath $helper -Destination (Join-Path $bundled "bin/$updater.exe")
$legacy = Join-Path $root 'legacy-updater.exe'
$defines = @('/DSNOW_SHOT_UPDATER_STUB_REMOVE_INSTALLED')
if ($Edition -eq 'Mini') { $defines += '/DSNOW_SHOT_UPDATER_STUB_MINI' }
& cl /nologo /std:c++20 /W4 /WX /O2 /MT /DUNICODE /D_UNICODE @defines "/Fe:$legacy" "/Fo:$root/legacy.obj" `
    "$repo/snow_shot/tests/installer_updater_stub.cpp" /link /SUBSYSTEM:WINDOWS shell32.lib
if ($LASTEXITCODE) { throw 'Legacy helper fixture compilation failed.' }

function Compile-Installer([string]$Name, [string]$Destination, [string]$UpdaterPath, [string[]]$Options = @()) {
    $output = Join-Path $root "$Name.exe"
    & $compiler /V2 "/DOUTPUT=$output" "/DDESTINATION=$Destination" "/DUPDATER=$UpdaterPath" `
        "/DPAYLOAD=$repo\snow_shot\tests\installer_process_fixture.cpp" "/DPACKAGING=$repo\snow_shot\packaging" `
        "/DBUNDLED_DIRECTORY=$bundled" "/DSNOW_SHOT_INSTALLER_EXECUTABLE=$executable" `
        "/DSNOW_SHOT_INSTALLER_UPDATER=$updater" @Options "$repo/snow_shot/tests/installer_upgrade_tests.nsi" | Out-Host
    if ($LASTEXITCODE) { throw "Installer fixture compilation failed: $Name" }
    return $output
}
function Run-Installer([string]$Path) {
    $process = Start-Process -FilePath $Path -ArgumentList '/S' -WindowStyle Hidden -PassThru
    try {
        if (-not $process.WaitForExit(20000)) { $process.Kill(); $process.WaitForExit(); throw 'Upgrade timed out.' }
        return $process.ExitCode
    } finally { $process.Dispose() }
}
function Write-InstallationRecord([string]$Destination, [string]$Data) {
    $files = @("bin/$executable.exe", "bin/$updater.exe") | ForEach-Object {
        $path = Join-Path $Destination $_
        @{ path = $_; size = (Get-Item -LiteralPath $path).Length; sha256 = (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() }
    }
    $record = @{ schema = 1; platform = "windows-$Architecture"; product = $product; variant = 'online'; version = '1.2.3'; files = @($files) }
    [IO.File]::WriteAllText((Join-Path $Destination $recordName), ($record | ConvertTo-Json -Depth 4))
    [IO.File]::WriteAllText((Join-Path $Destination "bin/$markerName"), $Data)
}

foreach ($case in 'in-place', 'moved', 'refused') {
    $previous = Join-Path $root ("$case old app " + [char]0x5b89)
    $destination = if ($case -eq 'moved') { Join-Path $root 'new app directory' } else { $previous }
    $data = Join-Path $root ("$case custom configuration " + [char]0x914d)
    $null = New-Item -ItemType Directory -Path $data
    $config = Join-Path $data 'config.json'
    [IO.File]::WriteAllText($config, 'preserve custom configuration')
    $options = @('/DBLOCK_HELPER_COPY')
    if ($case -eq 'refused') { $options += '/DREFUSE_UNINSTALL' }
    $old = Compile-Installer "$case-old" $previous $legacy $options
    if ((Run-Installer $old) -ne 0) { throw 'Legacy fixture installation failed.' }
    Write-InstallationRecord $previous $data
    $originalHash = (Get-FileHash -LiteralPath (Join-Path $previous "bin/$updater.exe")).Hash

    if ($case -eq 'in-place') {
        $baseline = Compile-Installer 'baseline' $destination $helper @("/DPREVIOUS_DESTINATION=$previous")
        if ((Run-Installer $baseline) -ne 22) { throw 'Expected legacy self-deletion to stop the upgrade.' }
        Write-Output 'Reproduced: the old helper cannot delete its running executable and blocks the upgrade.'
    }
    $setup = Compile-Installer "$case-fixed" $destination $helper @("/DPREVIOUS_DESTINATION=$previous", '/DBOOTSTRAP')
    $result = Run-Installer $setup
    if ($case -eq 'refused') {
        if ($result -ne 22 -or (Get-FileHash -LiteralPath (Join-Path $previous "bin/$updater.exe")).Hash -cne $originalHash -or
            -not (Test-Path -LiteralPath (Join-Path $previous "bin/$executable.exe"))) {
            throw 'A refused uninstall must restore the original helper and preserve the old application.'
        }
    } else {
        if ($result -ne 0 -or (Get-FileHash -LiteralPath (Join-Path $destination "bin/$updater.exe")).Hash -cne (Get-FileHash -LiteralPath $helper).Hash) {
            throw "Legacy upgrade failed: $case, exit code $result"
        }
        if ($case -eq 'moved' -and (Test-Path -LiteralPath (Join-Path $previous "bin/$executable.exe"))) {
            throw 'The previous installation was not removed.'
        }
    }
    if ([IO.File]::ReadAllText($config) -cne 'preserve custom configuration') { throw 'Upgrade changed custom configuration.' }
    Write-Output "PASS: $Edition $case upgrade preserves custom configuration and handles the previous helper correctly."
}
Write-Output "Upgrade test artifacts: $root"

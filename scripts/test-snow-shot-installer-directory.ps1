[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$compiler = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
if (-not (Test-Path -LiteralPath $compiler)) { throw "NSIS is required for installer tests." }
$testId = [guid]::NewGuid().ToString('N')
$testRoot = Join-Path $repoRoot "build\installer-directory-tests-$testId"
$registryKey = "Software\SnowShotInstallerTests\$testId"
$defaultDirectory = Join-Path $testRoot "default"
$customDirectory = Join-Path $testRoot ("custom app " + [char]0x5b89)
$overrideDirectory = Join-Path $testRoot "explicit destination"
New-Item -ItemType Directory -Path $testRoot | Out-Null

$cases = @(
    @{ Name = 'fresh'; Expected = $defaultDirectory },
    @{ Name = 'custom-upgrade'; Saved = $customDirectory; Expected = $customDirectory },
    @{ Name = 'default-upgrade'; Saved = $defaultDirectory; Expected = $defaultDirectory },
    @{ Name = 'empty-entry'; Saved = ''; Expected = $defaultDirectory },
    @{ Name = 'explicit-destination'; Saved = $customDirectory; Expected = $overrideDirectory;
        Override = $overrideDirectory }
)
foreach ($case in $cases) {
    $installer = Join-Path $testRoot "$($case.Name).exe"
    $arguments = @('/V2', "/DOUTPUT=$installer", "/DPACKAGING=$repoRoot\snow_shot\packaging",
        "/DREGISTRY_KEY=$registryKey", "/DDEFAULT_DIRECTORY=$defaultDirectory",
        "/DEXPECTED_DIRECTORY=$($case.Expected)")
    if ($case.ContainsKey('Saved')) { $arguments += "/DSAVED_DIRECTORY=$($case.Saved)" }
    & $compiler @arguments "$repoRoot\snow_shot\tests\installer_directory_tests.nsi"
    if ($LASTEXITCODE -ne 0) { throw "Directory test compilation failed." }
    $runArguments = @('/S')
    # NSIS requires /D last, without quotes, even when the path contains spaces.
    if ($case.Override) { $runArguments += "/D=$($case.Override)" }
    $process = Start-Process -FilePath $installer -ArgumentList $runArguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(20000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Directory test timed out: $($case.Name)"
    }
    if ($process.ExitCode -ne 0) {
        throw "Directory test failed: $($case.Name), exit code $($process.ExitCode)"
    }
    Write-Output "PASS: $($case.Name)"
}

$cpackBuild = Join-Path $testRoot 'cpack'
& cmake -S "$repoRoot\snow_shot\tests\installer_packaging" -B $cpackBuild
if ($LASTEXITCODE -ne 0) { throw "Installer integration configuration failed." }
& cpack --config "$cpackBuild\CPackConfig.cmake" -G NSIS -B $cpackBuild
if ($LASTEXITCODE -ne 0) { throw "CPack installer compilation failed." }
$scriptPath = Get-ChildItem -LiteralPath "$cpackBuild\_CPack_Packages" -Recurse -Filter project.nsi
$generated = Get-Content -LiteralPath $scriptPath.FullName -Raw
$init = [regex]::Match($generated, '(?s)Function \.onInit\r?\n.*?FunctionEnd').Value
$restore = $init.IndexOf('!insertmacro SnowShotRestoreInstallDirectory')
if ($restore -lt 0 -or $restore -lt $init.IndexOf('!insertmacro SnowShotLanguageContext') -or
    $restore -gt $init.IndexOf('ExecWait')) {
    throw 'Directory restoration must follow registry context selection and precede the old uninstaller.'
}
if (-not $init.Contains('"Software\Snow Apps\SnowShotInstallerTest"')) {
    throw 'Directory restoration must use the registry key written by CPack.'
}
Write-Output 'PASS: CPack compiles directory restoration before upgrade removal.'
Write-Output "Installer directory test artifacts: $testRoot"

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
. (Join-Path $PSScriptRoot 'snow-shot-nsis-payload.ps1')
if ((Get-SnowWindowsHostArchitecture) -cne $Architecture) {
    throw 'Installer payload integration tests require a matching native Windows host.'
}
$null = Add-SnowMsvcToolsToPath -Architecture $Architecture
$compiler = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw 'NSIS is required.' }
$extractor = Join-Path $BuildDirectory 'installer-payload/Release/snow-shot-payload-extractor.exe'
if (-not (Test-Path -LiteralPath $extractor -PathType Leaf)) { throw "Build the extractor first: $extractor" }
$identity = 'SnowShotPayloadTest-' + [guid]::NewGuid().ToString('N')
$testRoot = Join-Path $repoRoot "build/nsis-payload-tests-$identity"
$source = Join-Path $testRoot 'source'
New-Item -ItemType Directory -Path $source -Force | Out-Null
$registrations = [Collections.Generic.List[string]]::new()

function Assert-WorkspaceTestPath([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path)
    $prefix = [IO.Path]::GetFullPath($testRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing a test operation outside its workspace directory: $resolved"
    }
    return $resolved
}

function Invoke-CheckedTool([string]$Tool, [string[]]$Arguments, [string]$Log,
    [switch]$ExpectFailure) {
    & $Tool @Arguments *> $Log
    $result = $LASTEXITCODE
    if (($ExpectFailure -and $result -eq 0) -or (-not $ExpectFailure -and $result -ne 0)) {
        throw "Unexpected tool exit code $result. See $Log"
    }
}

function Invoke-Fixture([string]$Executable, [string]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    # NSIS /D= and _?= consume the unquoted remainder, including spaces.
    # ArgumentList would quote the whole token and NSIS would ignore /D=.
    $start.Arguments = $Arguments
    $process = [Diagnostics.Process]::Start($start)
    try {
        if (-not $process.WaitForExit(30000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw "Installer fixture timed out: $Executable"
        }
        return $process.ExitCode
    } finally { $process.Dispose() }
}

function Get-TreeManifest([string]$Directory, [string[]]$Exclude = @()) {
    return @(Get-ChildItem -LiteralPath $Directory -File -Recurse -Force | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($Directory, $_.FullName).Replace('\', '/')
        if ($relative -notin $Exclude) {
            '{0}|{1}|{2}' -f $relative, $_.Length,
                (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    } | Sort-Object -CaseSensitive)
}

function Assert-NoFixtureRegistration([string]$Key) {
    foreach ($hive in @('HKCU', 'HKLM')) {
        foreach ($path in @("${hive}:\Software\$identity\$Key",
            "${hive}:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$Key")) {
            if (Test-Path -LiteralPath $path) { throw "Failed extraction registered an installation: $path" }
        }
    }
}

function New-VariantConfig([string]$Path, [string]$Base, [string]$Stage, [string]$Output,
    [string]$Name, [string]$Key, [string]$Definitions) {
    $basePath = $Base.Replace('\', '/')
    $stagePath = $Stage.Replace('\', '/')
    $outputPath = $Output.Replace('\', '/')
    # Mini's real generator uses its normal registry leaf. Only this fixture's
    # unique vendor/leaf is substituted; installed files and payload hooks match.
    $oldMiniKey = "Software\$identity\SnowShotMini"
    $newMiniKey = "Software\$identity\$Key"
    @"
include([==[$basePath]==])
set(CPACK_INSTALL_CMAKE_PROJECTS "")
set(CPACK_INSTALLED_DIRECTORIES [==[$stagePath;/]==])
set(CPACK_PACKAGE_DIRECTORY [==[$outputPath]==])
set(CPACK_PACKAGE_FILE_NAME "$Name")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "$Key")
set(CPACK_PACKAGE_EXECUTABLES "")
set(CPACK_CREATE_DESKTOP_LINKS "")
string(REPLACE [==[$oldMiniKey]==] [==[$newMiniKey]==] CPACK_NSIS_DEFINES "`${CPACK_NSIS_DEFINES}")
$Definitions
"@ | Set-Content -LiteralPath $Path -Encoding utf8NoBOM
}

try {
    # Keep native fixture tools inside the isolated directory; no app process,
    # updater logic, real product registry key, shortcut, or elevation is used.
    $stubSource = Join-Path $source 'updater.cpp'
    '#include <windows.h>' + "`n" +
        'int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return 0; }' |
        Set-Content -LiteralPath $stubSource -Encoding utf8NoBOM
    $updater = Join-Path $testRoot 'updater.exe'
    Invoke-CheckedTool cl @('/nologo', '/std:c++20', '/W4', '/WX', '/O1', '/MT',
        '/DUNICODE', '/D_UNICODE', "/Fe:$updater", "/Fo:$testRoot/updater.obj", $stubSource,
        '/link', '/SUBSYSTEM:WINDOWS') (Join-Path $testRoot 'compile-updater.log')

    $fixtureReadme = Join-Path $source 'snow_shot/packaging'
    New-Item -ItemType Directory -Path $fixtureReadme -Force | Out-Null
    'Isolated installer payload fixture.' | Set-Content (Join-Path $fixtureReadme 'README-mini.txt')
    $repoCmake = $repoRoot.Replace('\', '/')
    $fixtureInstallRoot = $testRoot.Replace('\', '/')
    @"
cmake_minimum_required(VERSION 4.2)
project(SnowShotNsisPayloadTests VERSION 1.0.0 LANGUAGES NONE)
add_custom_target(snow_shot_mini)
set(SNOW_SHOT_VERSION 1.0.0)
set(SNOW_SHOT_VERSION_NUMERIC 1.0.0)
set(SNOW_SHOT_VENDOR "$identity")
set(SNOW_SHOT_COPYRIGHT "Isolated test fixture")
set(SNOW_WINDOWS_ARCHITECTURE $Architecture)
set(SNOW_WINDOWS_PLATFORM windows-$Architecture)
set(CPACK_PACKAGE_NAME snow-shot-payload-fixture)
set(CPACK_PACKAGE_VENDOR "$identity")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "$identity-Full")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "$identity")
set(CPACK_NSIS_DISPLAY_NAME "Snow Shot payload fixture")
set(CPACK_NSIS_PACKAGE_NAME "Snow Shot payload fixture")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall Payload Fixture")
set(CPACK_NSIS_INSTALL_ROOT "$fixtureInstallRoot")
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_VERBATIM_VARIABLES ON)
include("$repoCmake/cmake/SnowShotPackageCompression.cmake")
snow_shot_apply_nsis_compression(PAYLOAD)
include("$repoCmake/cmake/SnowShotInstaller.cmake")
include(CPack)
include("$repoCmake/cmake/SnowShotMiniPackage.cmake")
"@ | Set-Content -LiteralPath (Join-Path $source 'CMakeLists.txt') -Encoding utf8NoBOM
    $configuration = Join-Path $testRoot 'configuration'
    Invoke-CheckedTool cmake @('-S', $source, '-B', $configuration) (Join-Path $testRoot 'configure.log')

    # Instrument only copies generated for this fixture. Production template
    # inclusion, payload macro, ownership cleanup, file inventory, and codec
    # directives remain the real implementation.
    foreach ($directory in @('snow-shot-nsis', 'snow-shot-mini-nsis')) {
        $template = Join-Path $configuration "$directory/NSIS.template.in"
        $text = [IO.File]::ReadAllText($template)
        $text = $text.Replace('RequestExecutionLevel admin', 'RequestExecutionLevel user')
        $text = $text.Replace('!insertmacro SnowShotLanguageContext', 'SetShellVarContext current')
        $text = $text.Replace('SetShellVarContext all', 'SetShellVarContext current')
        $text = [regex]::Replace($text,
            '(?s)!insertmacro MUI_STARTMENU_WRITE_BEGIN Application.*?!insertmacro MUI_STARTMENU_WRITE_END', '')
        $text = [regex]::Replace($text,
            '(?s)!insertmacro MUI_STARTMENU_GETFOLDER Application \$MUI_TEMP.*?secondStartMenuDeleteLoopDone:', '')
        $text = $text.Replace('@CPACK_NSIS_EXTRA_INSTALL_COMMANDS@', @'
@CPACK_NSIS_EXTRA_INSTALL_COMMANDS@
  FileOpen $1 "$INSTDIR\payload-install-complete.txt" w
  FileWrite $1 "complete"
  FileClose $1
'@)
        $defaultPath = (Join-Path $testRoot 'default installation').Replace('\', '/')
        $text = $text.Replace('$DOCUMENTS\@CPACK_PACKAGE_INSTALL_DIRECTORY@', $defaultPath)
        $text = $text.Replace('$PROGRAMFILES\@CPACK_PACKAGE_INSTALL_DIRECTORY@', $defaultPath)
        if ($text -match 'CreateShortCut|CreateDirectory "\$SMPROGRAMS') {
            throw 'Fixture must not create shortcuts.'
        }
        $bytes = [IO.File]::ReadAllBytes($template)
        if ($bytes.Length -lt 6 -or [Convert]::ToHexString($bytes[0..2]) -cne 'EFBBBF' -or
            [Convert]::ToHexString($bytes[3..5]) -ceq 'EFBBBF') {
            throw 'Production NSIS template must declare UTF-8 for Unicode uninstall paths.'
        }
        [IO.File]::WriteAllText($template, $text, [Text.UTF8Encoding]::new($true))
    }

    foreach ($edition in @('Full', 'Mini')) {
        $caseRoot = Join-Path $testRoot $edition
        $stage = Join-Path $caseRoot ('stage ' + [char]0x5b89)
        $output = Join-Path $caseRoot 'output'
        New-Item -ItemType Directory -Path (Join-Path $stage 'bin'),
            (Join-Path $stage ('share/' + [char]0x6587)), $output -Force | Out-Null
        $application = if ($edition -eq 'Mini') { 'snow_shot_mini.exe' } else { 'snow_shot.exe' }
        $updaterName = if ($edition -eq 'Mini') { 'snow-shot-mini-updater.exe' } else { 'snow-shot-updater.exe' }
        Copy-Item -LiteralPath $extractor -Destination (Join-Path $stage "bin/$application")
        Copy-Item -LiteralPath $updater -Destination (Join-Path $stage "bin/$updaterName")
        [IO.File]::WriteAllBytes((Join-Path $stage 'empty.txt'), [byte[]]::new(0))
        [IO.File]::WriteAllText((Join-Path $stage ('share/' + [char]0x6587 + '/unicode.txt')),
            "Unicode payload $([char]0x5b89)$([char]0x5168) and literal `$value.", [Text.UTF8Encoding]::new($false))
        $expected = Get-TreeManifest $stage
        $archive = Join-Path $caseRoot ('payload $literal ' + [char]0x5b89 + '.7z')
        $payload = New-SnowShotNsisPayload -BuildDirectory $BuildDirectory -Stage $stage `
            -Archive $archive -Architecture $Architecture
        if (-not $payload.Report -or $payload.Report.FileCount -ne $expected.Count) {
            throw "$edition did not create the enabled payload with the exact file inventory."
        }
        $baseName = if ($edition -eq 'Mini') { 'CPackSnowShotMiniConfig.cmake' } else { 'CPackConfig.cmake' }
        $base = Join-Path $configuration $baseName
        $key = "$identity-$edition"
        $registrations.Add($key)
        $config = Join-Path $caseRoot 'package.cmake'
        $name = "payload-$edition"
        New-VariantConfig $config $base $stage $output $name $key $payload.CPackDefinitions
        Invoke-CheckedTool cpack @('--config', $config, '-G', 'NSIS', '-C', 'Release') `
            (Join-Path $caseRoot 'cpack.log')
        $installer = Join-Path $output "$name.exe"
        $generated = @(Get-ChildItem -LiteralPath $output -Recurse -Filter project.nsi)
        if ($generated.Count -ne 1) { throw 'Expected exactly one generated CPack NSIS program.' }
        $programBytes = [IO.File]::ReadAllBytes($generated[0].FullName)
        if ([Convert]::ToHexString($programBytes[0..2]) -cne 'EFBBBF' -or
            [Convert]::ToHexString($programBytes[3..5]) -ceq 'EFBBBF') {
            throw 'CPack must preserve the NSIS template UTF-8 BOM.'
        }
        $program = [IO.File]::ReadAllText($generated[0].FullName)
        $core = [regex]::Match($program, '(?s)Section "-Core installation".*?SectionEnd').Value
        if (-not $core.Contains('!insertmacro SnowShotInstallPayload') -or
            $core.IndexOf('!insertmacro SnowShotInstallPayload') -gt $core.IndexOf('WriteRegStr SHCTX')) {
            throw 'Payload extraction must precede installation registration.'
        }
        $uninstall = [regex]::Match($program, '(?s)Section "Uninstall".*?SectionEnd').Value
        foreach ($file in @(Get-ChildItem -LiteralPath $stage -File -Recurse)) {
            $relative = [IO.Path]::GetRelativePath($stage, $file.FullName).Replace('/', '\')
            if (-not $uninstall.Contains('Delete "$INSTDIR\' + $relative + '"')) {
                throw "CPack lost its generated uninstall deletion: $relative"
            }
        }
        if (-not $uninstall.Contains('!insertmacro SnowShotUninstallOwnedCleanup')) {
            throw 'Payload packages must retain the shared owned-file uninstall guard.'
        }
        $preprocessed = Join-Path $caseRoot 'preprocessed.nsi'
        Invoke-CheckedTool $compiler @('/PPO', '/V1', $generated[0].FullName) $preprocessed
        $expanded = [IO.File]::ReadAllText($preprocessed)
        $expandedCore = [regex]::Match($expanded, '(?s)Section "-Core installation".*?SectionEnd').Value
        $resources = [regex]::Match($expanded,
            '(?s)Function SnowShotPreparePayloadResources\r?\n.*?FunctionEnd').Value
        $fileCommands = [regex]::Matches($resources, '(?m)^\s*File(?:\s|$)')
        if ($fileCommands.Count -ne 2 -or
            -not $expandedCore.Contains('Call SnowShotPreparePayloadResources') -or
            $expanded -match '(?m)^\s*File\s+/r\b' -or
            $expanded.Contains('File /oname=snow-shot-installer-updater.exe')) {
            throw 'Payload must compile one shared extractor/archive pair, without the staged File tree or a second updater copy.'
        }
        if ($expanded -notmatch '(?m)^\s*SetCompressor\s+lzma\s*$' -or
            $expanded -match '(?im)^\s*SetCompressor\s+/SOLID' -or
            $expanded -notmatch '(?m)^\s*SetCompressorDictSize\s+32\s*$') {
            throw 'Payload bootstrap must use non-solid LZMA with the bounded dictionary.'
        }
        # SetCompress off must result in one literal, contiguous copy of the
        # archive in the executable, proving it was not recompressed or doubled.
        $archiveText = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($archive))
        $installerText = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($installer))
        $position = $installerText.IndexOf($archiveText, [StringComparison]::Ordinal)
        if ($position -lt 0 -or $installerText.IndexOf($archiveText, $position + 1,
            [StringComparison]::Ordinal) -ge 0) {
            throw 'The installer must embed exactly one unmodified payload archive.'
        }

        foreach ($trailingSeparator in @($false, $true)) {
            $destination = Join-Path $caseRoot ('installed ' + [char]0x5b89 + "-$trailingSeparator")
            $null = Assert-WorkspaceTestPath $destination
            $argument = '/D=' + $destination + $(if ($trailingSeparator) { '\' } else { '' })
            $exit = Invoke-Fixture $installer ('/S ' + $argument)
            if ($exit -ne 0) { throw "$edition installation failed with exit $exit (trailing separator: $trailingSeparator)." }
            $uninstallerName = if ($edition -eq 'Mini') { 'Uninstall Snow Shot Mini.exe' } else { 'Uninstall Payload Fixture.exe' }
            $actual = Get-TreeManifest $destination @($uninstallerName, 'payload-install-complete.txt')
            if (($actual -join "`n") -cne ($expected -join "`n")) {
                throw "$edition installed files differ from the exact staged inventory."
            }
            if (-not (Test-Path -LiteralPath (Join-Path $destination 'payload-install-complete.txt')) -or
                -not (Test-Path -LiteralPath "HKCU:\Software\$identity\$key")) {
                throw 'Successful extraction must reach completion and register the isolated installation.'
            }
            Remove-Item -LiteralPath (Join-Path $destination 'payload-install-complete.txt')
            $exit = Invoke-Fixture (Join-Path $destination $uninstallerName) ("/S _?=$destination")
            if ($exit -ne 0) { throw "$edition generated uninstaller failed with exit $exit." }
            if (@(Get-ChildItem -LiteralPath $destination -File -Recurse -ErrorAction SilentlyContinue |
                Where-Object Name -ne $uninstallerName).Count -ne 0) {
                throw "$edition generated uninstaller left staged payload files behind."
            }
            Assert-NoFixtureRegistration $key
        }

        $badConfig = Join-Path $caseRoot 'package-bad-hash.cmake'
        $badDefinitions = $payload.CPackDefinitions.Replace($payload.Report.Sha256, ('0' * 64))
        New-VariantConfig $badConfig $base $stage $output "$name-bad-hash" $key $badDefinitions
        Invoke-CheckedTool cpack @('--config', $badConfig, '-G', 'NSIS', '-C', 'Release') `
            (Join-Path $caseRoot 'cpack-bad-hash.log')
        $failedDestination = Join-Path $caseRoot 'failed installation'
        $exit = Invoke-Fixture (Join-Path $output "$name-bad-hash.exe") ("/S /D=$failedDestination")
        if ($exit -ne 14) { throw "Extraction integrity failure must exit14; received $exit." }
        Assert-NoFixtureRegistration $key
        if (Test-Path -LiteralPath $failedDestination) {
            if (@(Get-ChildItem -LiteralPath $failedDestination -File -Recurse).Count -ne 0) {
                throw 'Failed extraction must precede payload writes, uninstaller creation, and completion sentinel.'
            }
        }
        if (($expected -join "`n") -cne ((Get-TreeManifest $stage) -join "`n")) {
            throw 'Installer integration tests changed the input stage.'
        }
        Write-Output "PASS: $edition exact Unicode/empty-file install and uninstall, trailing separators, extraction failure14, non-solid LZMA, and one payload copy."
    }
    $missingConfig = Join-Path $testRoot 'missing-payload.cmake'
    $fullStage = Join-Path $testRoot ('Full/stage ' + [char]0x5b89)
    $missingOutput = Join-Path $testRoot 'missing-payload'
    New-VariantConfig $missingConfig (Join-Path $configuration 'CPackConfig.cmake') `
        $fullStage $missingOutput 'missing-payload' "$identity-Missing" ''
    Invoke-CheckedTool cpack @('--config', $missingConfig, '-G', 'NSIS') `
        (Join-Path $testRoot 'missing-payload.log') -ExpectFailure
    $missingLogs = @(Get-ChildItem -LiteralPath $missingOutput -Recurse -Filter NSISOutput.log)
    if ($missingLogs.Count -ne 1 -or
        -not ([IO.File]::ReadAllText($missingLogs[0].FullName)).Contains('Create the installer payload')) {
        throw 'Missing payload failure must explain the required payload configuration.'
    }
    Write-Output 'PASS: payload-enabled direct CPack fails closed without an archive definition.'
} finally {
    # Keys are unique to this run and only HKCU fixture keys can be written.
    foreach ($key in $registrations) {
        foreach ($path in @("HKCU:\Software\$identity\$key",
            "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$key")) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        }
    }
    $vendor = "HKCU:\Software\$identity"
    if ((Test-Path -LiteralPath $vendor) -and @(Get-ChildItem -LiteralPath $vendor).Count -eq 0) {
        Remove-Item -LiteralPath $vendor
    }
    Write-Output "Installer payload test artifacts: $testRoot"
}

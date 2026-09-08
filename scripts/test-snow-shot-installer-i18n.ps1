[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$compiler = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
if (-not (Test-Path -LiteralPath $compiler)) { throw "NSIS is required for installer tests." }
$testId = [guid]::NewGuid().ToString('N')
$testRoot = Join-Path $repoRoot "build\installer-i18n-tests-$testId"
$registryKey = "Software\SnowShotInstallerTests\$testId"
$registryPath = "HKCU:\$registryKey"
$packaging = Join-Path $repoRoot "snow_shot\packaging"
New-Item -ItemType Directory -Path $testRoot | Out-Null

function Read-Catalog {
    param([string]$Locale, [int]$Language)
    $catalog = [ordered]@{}
    $defines = @{}
    foreach ($line in Get-Content -LiteralPath "$packaging\i18n\$Locale.nsh" -Encoding utf8) {
        if ($line -match '^!define (SnowShotUninstallShortcut\d+) "(.+)"$') {
            if ($defines.ContainsKey($Matches[1])) { throw "Duplicate installer define: $line" }
            $defines[$Matches[1]] = $Matches[2]
            continue
        }
        if ($line -notmatch '^LangString (\w+) (\d+) "(.+)"$' -or [int]$Matches[2] -ne $Language) {
            throw "Invalid or empty translation in ${Locale}: $line"
        }
        if ($catalog.Contains($Matches[1])) { throw "Duplicate translation in ${Locale}: $line" }
        $catalog[$Matches[1]] = $Matches[3]
    }
    foreach ($key in @($catalog.Keys)) {
        if ($catalog[$key] -match '^\$\{(\w+)\}$') {
            if (-not $defines.ContainsKey($Matches[1])) { throw "Unknown installer define: $key" }
            $catalog[$key] = $defines[$Matches[1]]
        }
    }
    return $catalog
}

function Run-Installer {
    param([string]$Path, [string[]]$Arguments)
    $process = Start-Process -FilePath $Path -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(20000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Silent language test timed out: $Path"
    }
    if ($process.ExitCode -ne 0) { throw "Language test failed with exit code $($process.ExitCode)." }
}

$locales = [ordered]@{ en_US = 1033; zh_CN = 2052; zh_TW = 1028 }
$catalogs = @{}
foreach ($locale in $locales.Keys) { $catalogs[$locale] = Read-Catalog $locale $locales[$locale] }
$english = $catalogs.en_US
foreach ($locale in $locales.Keys) {
    $catalog = $catalogs[$locale]
    if (@(Compare-Object @($english.Keys) @($catalog.Keys)).Count -ne 0) {
        throw "Installer translation keys differ for $locale."
    }
    foreach ($key in $english.Keys) {
        $expected = @([regex]::Matches($english[$key], '\$(?:[0-9]|\\[nr])') | ForEach-Object Value | Sort-Object)
        $actual = @([regex]::Matches($catalog[$key], '\$(?:[0-9]|\\[nr])') | ForEach-Object Value | Sort-Object)
        if (($expected -join '|') -cne ($actual -join '|')) { throw "Placeholder mismatch: $locale/$key" }
    }
}
Write-Output "PASS: all three installer catalogs have complete, nonempty translations and matching placeholders."

$installer = Join-Path $testRoot "language-test.exe"
& $compiler /V2 "/DOUTPUT=$installer" "/DDESTINATION=$testRoot" "/DPACKAGING=$packaging" `
    "/DREGISTRY_KEY=$registryKey" "$repoRoot\snow_shot\tests\installer_language_tests.nsi"
if ($LASTEXITCODE -ne 0) { throw "Language test compilation failed." }
try {
    foreach ($locale in $locales.Keys) {
        if (Test-Path -LiteralPath $registryPath) { Remove-Item -LiteralPath $registryPath -Recurse -Force }
        $language = $locales[$locale]
        $shortcuts = @('Uninstall.lnk') + @($locales.Keys | ForEach-Object {
            $catalogs[$_].SnowShotUninstallShortcut + '.lnk'
        })
        foreach ($shortcut in $shortcuts + @('unrelated.lnk')) {
            New-Item -ItemType File -Path (Join-Path $testRoot $shortcut) -Force | Out-Null
        }
        Run-Installer $installer @('/S', "/LANG=$language")
        foreach ($shortcut in $shortcuts) {
            if (Test-Path -LiteralPath (Join-Path $testRoot $shortcut)) {
                throw "Stale localized shortcut remains: $shortcut"
            }
        }
        if (-not (Test-Path -LiteralPath "$testRoot\unrelated.lnk")) {
            throw "Shortcut cleanup must preserve unrelated files."
        }
        $lines = @(Get-Content -LiteralPath "$testRoot\install.txt" -Encoding unicode)
        $expected = @("$language", $catalogs[$locale].SnowShotLanguageTitle,
            $catalogs[$locale].SnowShotClosePrompt, $catalogs[$locale].SnowShotUninstallShortcut)
        if (($lines[0..3] -join '|') -cne ($expected -join '|')) {
            throw "Installer did not resolve $locale strings correctly."
        }
        if ($locale -ne 'en_US' -and $lines[4] -eq $englishNextButton) {
            throw "Standard wizard buttons must also be translated for $locale."
        }
        if ($locale -eq 'en_US') { $englishNextButton = $lines[4] }
        $saved = (Get-ItemProperty -LiteralPath $registryPath).InstallerLanguage
        if ($saved -ne "$language") { throw "Silent installation did not persist its language." }
        Run-Installer "$testRoot\uninstall.exe" @('/S', "_?=$testRoot")
        if ((Get-Content "$testRoot\uninstall.txt" -Raw -Encoding unicode) -cne
            (Get-Content "$testRoot\install.txt" -Raw -Encoding unicode)) {
            throw "Uninstaller did not restore $locale."
        }
        Run-Installer $installer @('/S', '/LANG=1033')
        if ((Get-Content "$testRoot\install.txt" -Encoding unicode)[0] -ne "$language") {
            throw "Setup did not reuse the saved language for $locale."
        }
        Write-Output "PASS: $locale strings, buttons, silent selection, persistence, uninstall restoration, and shortcut cleanup."
    }
    Remove-Item -LiteralPath $registryPath -Recurse -Force
    Run-Installer $installer @('/S', '/LANG=1041')
    $fallback = @(Get-Content "$testRoot\install.txt" -Encoding unicode)
    if ($fallback[1] -cne $english.SnowShotLanguageTitle -or $fallback[4] -cne $englishNextButton) {
        throw "An unsupported language must fall back to English."
    }
    Write-Output "PASS: unsupported languages fall back to English."
}
finally {
    if (Test-Path -LiteralPath $registryPath) { Remove-Item -LiteralPath $registryPath -Recurse -Force }
}

$cpackBuild = Join-Path $testRoot "cpack"
& cmake -S "$repoRoot\snow_shot\tests\installer_packaging" -B $cpackBuild
if ($LASTEXITCODE -ne 0) { throw "Installer integration configuration failed." }
& cpack --config "$cpackBuild\CPackConfig.cmake" -G NSIS -B $cpackBuild
if ($LASTEXITCODE -ne 0) { throw "Localized CPack installer compilation failed." }
$scriptPath = Get-ChildItem -LiteralPath "$cpackBuild\_CPack_Packages" -Recurse -Filter project.nsi
$generated = Get-Content -LiteralPath $scriptPath.FullName -Raw
$init = [regex]::Match($generated, '(?s)Function \.onInit\r?\n.*?FunctionEnd').Value
$optionsPage = [regex]::Match($generated, '(?s)Function InstallOptionsPage\r?\n.*?FunctionEnd').Value
if ($optionsPage.Contains('Abort') -or
    -not $optionsPage.Contains('MUI_INSTALLOPTIONS_DISPLAY "NSIS.InstallOptions.ini"') -or
    -not $optionsPage.Contains('"Field 1" "Text" "$(SnowShotDesktopIcon)"') -or
    $optionsPage.Contains('"State"')) {
    throw "The shortcut option must be visible, localized, and preserve the user's selection."
}
foreach ($setting in '"Settings" "NumFields" "1"', '"Field 1" "Type" "CheckBox"',
    '"Field 1" "State" "1"') {
    if (-not $init.Contains($setting)) { throw "Missing shortcut initialization: $setting" }
}
if ($init.Contains('0 noOptionsPage') -or
    -not $generated.Contains('MUI_INSTALLOPTIONS_READ $INSTALL_DESKTOP "NSIS.InstallOptions.ini" "Field 1" "State"') -or
    $generated -notmatch 'StrCmp "\$INSTALL_DESKTOP" "1" 0 \+2\r?\n\s*CreateShortCut "\$DESKTOP\\Snow Shot.lnk" "\$INSTDIR\\bin\\snow_shot.exe"' -or
    -not $generated.Contains('Delete "$DESKTOP\Snow Shot.lnk"')) {
    throw "Desktop shortcut creation must honor the checkbox and support uninstall cleanup."
}
Write-Output "PASS: shortcut-only options default on, preserve selection, and gate desktop shortcut creation."
if ($init.IndexOf('!insertmacro MUI_LANGDLL_DISPLAY') -lt 0 -or
    $init.IndexOf('!insertmacro MUI_LANGDLL_DISPLAY') -gt $init.IndexOf('Call SnowShotEnsureAppClosed')) {
    throw "Language selection must precede the running-app and upgrade prompts."
}
$uninit = [regex]::Match($generated, '(?s)Function un\.onInit\r?\n.*?FunctionEnd').Value
if ($uninit.IndexOf('!insertmacro MUI_UNGETLANGUAGE') -lt $uninit.IndexOf('SetShellVarContext all')) {
    throw "Uninstaller language restoration must follow registry context selection."
}
foreach ($text in 'Welcome to Snow Shot Setup', 'installation complete', 'is already installed.',
    'Uninstall failed.', '!insertmacro MUI_LANGUAGE "Afrikaans"', 'SnowShotLanguageFallback') {
    if ($generated.Contains($text)) { throw "Unlocalized or unsupported installer text remains: $text" }
}
foreach ($key in 'UpgradePrompt', 'OptionsTitle', 'PathDescription', 'PathNone', 'PathAll',
    'PathCurrent', 'DesktopIcon', 'UninstallShortcut', 'RegistryEntry', 'UninstallFailed') {
    if (-not $generated.Contains('$(SnowShot' + $key + ')')) { throw "Missing localized use: $key" }
}
# CPack discards successful compiler output, so check the generated script directly.
$compileOutput = & $compiler /V2 $scriptPath.FullName 2>&1
if ($LASTEXITCODE -ne 0) { throw "Generated installer compilation failed: $compileOutput" }
if (($compileOutput -join "`n") -match 'warning 6040|LangString .*not set') {
    throw "NSIS reported missing translations."
}
Write-Output "PASS: CPack compiles complete language tables and localizes prompts before upgrade handling."
Write-Output "Installer language test artifacts: $testRoot"

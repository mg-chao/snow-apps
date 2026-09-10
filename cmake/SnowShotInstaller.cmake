# CPack runs the old uninstaller in .onInit, before its public pre-install hook.
# Generate a minimally extended copy of the installed CPack template so the
# previous version is protected even when its own uninstaller has no guard.
file(READ "${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in" _snow_nsis_template)
# Fail visibly on CPack template drift instead of shipping partially translated UI.
function(snow_shot_nsis_replace original replacement)
    string(FIND "${_snow_nsis_template}" "${original}" _snow_nsis_position)
    if(_snow_nsis_position EQUAL -1)
        message(FATAL_ERROR "The CPack NSIS template changed; missing installer hook: ${original}")
    endif()
    string(REPLACE "${original}" "${replacement}" _snow_nsis_template "${_snow_nsis_template}")
    set(_snow_nsis_template "${_snow_nsis_template}" PARENT_SCOPE)
endfunction()
set(_snow_nsis_init "Function .onInit\n")
string(FIND "${_snow_nsis_template}" "${_snow_nsis_init}" _snow_nsis_init_position)
if(_snow_nsis_init_position EQUAL -1)
    message(FATAL_ERROR "The CPack NSIS .onInit hook changed; review the running-app guard.")
endif()
string(REPLACE "${_snow_nsis_init}" [=[Function .onInit
  !insertmacro SnowShotLanguageContext
  !insertmacro MUI_LANGDLL_DISPLAY
  Push $0
  ReadRegStr $0 HKLM "Software\@CPACK_PACKAGE_VENDOR@\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@" ""
  StrCmp $0 "" +5
    Push "$0\bin\@SNOW_SHOT_EXECUTABLE_NAME@.exe"
    Call SnowShotEnsureAppClosed
    Push "$0\bin\crashpad_handler.exe"
    Call SnowShotEnsureAppClosed
  ReadRegStr $0 HKCU "Software\@CPACK_PACKAGE_VENDOR@\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@" ""
  StrCmp $0 "" +5
    Push "$0\bin\@SNOW_SHOT_EXECUTABLE_NAME@.exe"
    Call SnowShotEnsureAppClosed
    Push "$0\bin\crashpad_handler.exe"
    Call SnowShotEnsureAppClosed
  Pop $0
]=] _snow_nsis_template "${_snow_nsis_template}")
# CPack does not carry project variables into its template configuration.
string(REPLACE "@SNOW_SHOT_EXECUTABLE_NAME@" "${SNOW_SHOT_EXECUTABLE_NAME}"
    _snow_nsis_template "${_snow_nsis_template}")
# Restrict the wizard to languages with complete Snow Shot catalogs.
string(REGEX MATCHALL "!insertmacro MUI_LANGUAGE \"[A-Za-z]+\"[^\n]*" _snow_nsis_languages
    "${_snow_nsis_template}")
if(NOT _snow_nsis_languages)
    message(FATAL_ERROR "The CPack NSIS language declarations changed.")
endif()
foreach(_snow_nsis_language IN LISTS _snow_nsis_languages)
    string(REPLACE "${_snow_nsis_language}" "" _snow_nsis_template "${_snow_nsis_template}")
endforeach()
snow_shot_nsis_replace(";Languages" ";Languages\n  !insertmacro SnowShotInstallerLanguages")
snow_shot_nsis_replace("!insertmacro MUI_RESERVEFILE_INSTALLOPTIONS"
    "!insertmacro MUI_RESERVEFILE_INSTALLOPTIONS\n  !insertmacro MUI_RESERVEFILE_LANGDLL")
string(FIND "${_snow_nsis_template}" "Function un.onInit\n" _snow_nsis_uninit_start)
if(_snow_nsis_uninit_start EQUAL -1)
    message(FATAL_ERROR "The CPack NSIS un.onInit hook changed.")
endif()
string(SUBSTRING "${_snow_nsis_template}" ${_snow_nsis_uninit_start} -1 _snow_nsis_uninit)
string(FIND "${_snow_nsis_uninit}" "FunctionEnd" _snow_nsis_uninit_end)
if(_snow_nsis_uninit_end EQUAL -1)
    message(FATAL_ERROR "The CPack NSIS un.onInit terminator changed.")
endif()
math(EXPR _snow_nsis_uninit_length "${_snow_nsis_uninit_end} + 11")
string(SUBSTRING "${_snow_nsis_uninit}" 0 ${_snow_nsis_uninit_length} _snow_nsis_uninit)
string(REPLACE "FunctionEnd" "  !insertmacro MUI_UNGETLANGUAGE\nFunctionEnd"
    _snow_nsis_localized_uninit "${_snow_nsis_uninit}")
snow_shot_nsis_replace("${_snow_nsis_uninit}" "${_snow_nsis_localized_uninit}")
snow_shot_nsis_replace([=["$1 is already installed. $\n$\nDo you want to uninstall the old version before installing the new one?"]=]
    [=["$(SnowShotUpgradePrompt)"]=])
snow_shot_nsis_replace([=["Uninstall failed."]=] [=["$(SnowShotUninstallFailed)"]=])
snow_shot_nsis_replace([=["Install Options" "Choose options for installing @CPACK_NSIS_PACKAGE_NAME@"]=]
    [=["$(SnowShotOptionsTitle)" "$(SnowShotOptionsDescription)"]=])
snow_shot_nsis_replace("Function InstallOptionsPage\n" [=[Function InstallOptionsPage
  !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Field 1" "Text" "$(SnowShotPathDescription)"
  !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Field 2" "Text" "$(SnowShotPathNone)"
  !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Field 3" "Text" "$(SnowShotPathAll)"
  !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Field 4" "Text" "$(SnowShotPathCurrent)"
  !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Field 5" "Text" "$(SnowShotDesktopIcon)"
]=])
# Extract options even when PATH changes are disabled. Initialize the default only
# once, so returning to the page does not overwrite an unchecked shortcut choice.
snow_shot_nsis_replace([=[  StrCmp "@CPACK_NSIS_MODIFY_PATH@" "ON" 0 noOptionsPage]=] "")
snow_shot_nsis_replace([=[    !insertmacro MUI_INSTALLOPTIONS_EXTRACT "NSIS.InstallOptions.ini"]=]
    [=[    !insertmacro MUI_INSTALLOPTIONS_EXTRACT "NSIS.InstallOptions.ini"
    !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Field 5" "State" "1"]=])
if(NOT CPACK_NSIS_MODIFY_PATH)
    # Keep CPack's state fields for registry bookkeeping, but display only the
    # shortcut checkbox. Fields beyond NumFields remain readable in the INI.
    snow_shot_nsis_replace([=["Field 5"]=] [=["Field 1"]=])
    snow_shot_nsis_replace([=[    !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Field 1" "State" "1"]=]
        [=[    !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Settings" "NumFields" "1"
    !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Field 1" "Type" "CheckBox"
    !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Field 1" "Bottom" "12"
    !insertmacro MUI_INSTALLOPTIONS_WRITE "NSIS.InstallOptions.ini" "Field 1" "State" "1"]=])
endif()
snow_shot_nsis_replace([=["Set install registry entry: '$1' to '$0'"]=]
    [=["$(SnowShotRegistryEntry)"]=])
snow_shot_nsis_replace([=["Selected environment for all users"]=] [=["$(SnowShotEnvironmentAll)"]=])
snow_shot_nsis_replace([=["Selected environment for current user only."]=]
    [=["$(SnowShotEnvironmentCurrent)"]=])
snow_shot_nsis_replace([=["Download failed: $1"]=] [=["$(SnowShotDownloadFailed)"]=])
snow_shot_nsis_replace([=[$STARTMENU_FOLDER\Uninstall.lnk]=]
    [=[$STARTMENU_FOLDER\$(SnowShotUninstallShortcut).lnk]=])
snow_shot_nsis_replace([=[CreateShortCut "$SMPROGRAMS\$STARTMENU_FOLDER\$(SnowShotUninstallShortcut).lnk"]=]
    [=[!insertmacro SnowShotDeleteUninstallShortcuts "$SMPROGRAMS\$STARTMENU_FOLDER"
  CreateShortCut "$SMPROGRAMS\$STARTMENU_FOLDER\$(SnowShotUninstallShortcut).lnk"]=])
snow_shot_nsis_replace([=[Delete "$SMPROGRAMS\$MUI_TEMP\Uninstall.lnk"]=]
    [=[!insertmacro SnowShotDeleteUninstallShortcuts "$SMPROGRAMS\$MUI_TEMP"]=])
# Future self-updates can add files that this uninstaller did not know at build time.
# Run a copy of the installed helper before CPack's original file deletion list.
snow_shot_nsis_replace("@CPACK_NSIS_DELETE_FILES@" [=[
  Push "$INSTDIR\bin\snow_shot.exe"
  Call un.SnowShotEnsureAppClosed
  Push "$INSTDIR\bin\crashpad_handler.exe"
  Call un.SnowShotEnsureAppClosed
  IfFileExists "$INSTDIR\snow-shot-installation.json" 0 snowOwnedDone
    InitPluginsDir
    ClearErrors
    CopyFiles /SILENT "$INSTDIR\bin\snow-shot-updater.exe" "$PLUGINSDIR\snow-shot-updater.exe"
    IfErrors snowOwnedFailed
    ExecWait '"$PLUGINSDIR\snow-shot-updater.exe" --uninstall --target "$INSTDIR"' $0
    IfErrors snowOwnedFailed
    StrCmp $0 0 snowOwnedDone
snowOwnedFailed:
    SetErrorLevel 12
    Quit
snowOwnedDone:
@CPACK_NSIS_DELETE_FILES@
]=])
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/snow-shot-nsis")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/snow-shot-nsis/NSIS.template.in" "${_snow_nsis_template}")
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_BINARY_DIR}/snow-shot-nsis")
set(_snow_nsis_guard "${CMAKE_CURRENT_LIST_DIR}/../snow_shot/packaging/RunningApplication.nsh")
cmake_path(NATIVE_PATH _snow_nsis_guard NORMALIZE _snow_nsis_guard_native)
set(_snow_nsis_localization "${CMAKE_CURRENT_LIST_DIR}/../snow_shot/packaging/InstallerLanguages.nsh")
cmake_path(NATIVE_PATH _snow_nsis_localization NORMALIZE _snow_nsis_localization_native)
string(APPEND CPACK_NSIS_DEFINES "\nUnicode true\n!include \"${_snow_nsis_guard_native}\"\n"
    "!include \"${_snow_nsis_localization_native}\"\n"
    "!define MUI_LANGDLL_REGISTRY_ROOT SHCTX\n"
    "!define MUI_LANGDLL_REGISTRY_KEY \"Software\\${CPACK_PACKAGE_VENDOR}\\${CPACK_PACKAGE_INSTALL_REGISTRY_KEY}\"\n"
    "!define MUI_LANGDLL_REGISTRY_VALUENAME \"InstallerLanguage\"\n")
# MUI normally saves on the visible progress page; also persist silent installs.
string(APPEND CPACK_NSIS_EXTRA_INSTALL_COMMANDS "\n!insertmacro MUI_LANGDLL_SAVELANGUAGE\n")
set(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS
    "Push \"$INSTDIR\\bin\\${SNOW_SHOT_EXECUTABLE_NAME}.exe\"\nCall SnowShotEnsureAppClosed\nPush \"$INSTDIR\\bin\\crashpad_handler.exe\"\nCall SnowShotEnsureAppClosed")
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
    "Push \"$INSTDIR\\bin\\${SNOW_SHOT_EXECUTABLE_NAME}.exe\"\nCall un.SnowShotEnsureAppClosed\nPush \"$INSTDIR\\bin\\crashpad_handler.exe\"\nCall un.SnowShotEnsureAppClosed")

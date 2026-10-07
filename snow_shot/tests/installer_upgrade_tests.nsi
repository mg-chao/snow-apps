Unicode true
Name "Snow Shot legacy installer upgrade tests"
OutFile "${OUTPUT}"
RequestExecutionLevel user
!define INST_DIR "${BUNDLED_DIRECTORY}"
!include "${PACKAGING}\RunningApplication.nsh"
!include "${PACKAGING}\OwnedCleanup.nsh"
!include "${PACKAGING}\UpgradeHelper.nsh"

Function .onInit
!ifdef PREVIOUS_DESTINATION
  Push "${PREVIOUS_DESTINATION}\bin\${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe"
  Call SnowShotEnsureMainAppClosed
  Push "${PREVIOUS_DESTINATION}\bin\crashpad_handler.exe"
  Call SnowShotEnsureAppClosed
!ifdef BOOTSTRAP
  Push "${PREVIOUS_DESTINATION}"
  Call SnowShotPrepareUpgradeHelper
!endif
  ClearErrors
  ExecWait '"${PREVIOUS_DESTINATION}\uninstall.exe" /S /SNOWUPGRADE _?=${PREVIOUS_DESTINATION}' $0
  IfErrors oldUninstallFailed
  StrCmp $0 0 oldUninstallDone
oldUninstallFailed:
!ifdef BOOTSTRAP
  Call SnowShotRestoreUpgradeHelper
!endif
  SetErrorLevel 22
  Quit
oldUninstallDone:
!endif
FunctionEnd

Section
  StrCpy $INSTDIR "${DESTINATION}"
  SetOutPath "$INSTDIR\bin"
  File /oname=${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe "${PAYLOAD}"
  File /oname=${SNOW_SHOT_INSTALLER_UPDATER}.exe "${UPDATER}"
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
!ifdef REFUSE_UNINSTALL
  SetErrorLevel 23
  Quit
!endif
!ifdef BLOCK_HELPER_COPY
  ; Force the same isolation failure that makes 1.2.3/1.2.4 use the installed
  ; helper. A Windows sharing violation is deterministic and needs no UI.
  InitPluginsDir
  System::Call 'kernel32::CreateFileW(w "$PLUGINSDIR\${SNOW_SHOT_INSTALLER_UPDATER}.exe", i 0x40000000, i 0, p 0, i 2, i 0, p 0) p.r9'
!endif
  !insertmacro SnowShotUninstallOwnedCleanup
!ifdef BLOCK_HELPER_COPY
  System::Call 'kernel32::CloseHandle(p r9)'
!endif
  Delete "$INSTDIR\bin\${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe"
  Delete "$INSTDIR\bin\${SNOW_SHOT_INSTALLER_UPDATER}.exe"
  Delete "$INSTDIR\snow-shot-installation.json"
  Delete "$INSTDIR\snow-shot-mini-installation.json"
SectionEnd

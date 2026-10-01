Unicode true
Name "Snow Shot installer restart tests"
OutFile "${OUTPUT}"
RequestExecutionLevel user
!include "${PACKAGING}\RunningApplication.nsh"
!include "${PACKAGING}\InstallerLaunch.nsh"

; Exercise the .onInit guard and old uninstaller before checking the final
; destination, including an upgrade that moves to a different directory.
Function .onInit
!ifdef PREVIOUS_DESTINATION
  Push "${PREVIOUS_DESTINATION}\bin\${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe"
  Call SnowShotEnsureMainAppClosed
  Push "${PREVIOUS_DESTINATION}\bin\crashpad_handler.exe"
  Call SnowShotEnsureAppClosed
  ClearErrors
  ExecWait '"${PREVIOUS_DESTINATION}\uninstall.exe" /S _?=${PREVIOUS_DESTINATION}' $0
  IfErrors oldUninstallFailed
  StrCmp $0 0 oldUninstallDone
oldUninstallFailed:
  SetErrorLevel 22
  Quit
oldUninstallDone:
!endif
FunctionEnd

Section
  StrCpy $INSTDIR "${DESTINATION}"
  Push "$INSTDIR\bin\${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe"
  Call SnowShotEnsureMainAppClosed
  Push "$INSTDIR\bin\crashpad_handler.exe"
  Call SnowShotEnsureAppClosed
!ifdef FAIL_INSTALL
  SetErrorLevel 21
  Abort
!endif
  SetOutPath "$INSTDIR\bin"
  File /oname=${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe "${PAYLOAD}"
  File /oname=${SNOW_SHOT_INSTALLER_UPDATER}.exe "${UPDATER}"
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  Push "$INSTDIR\bin\${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe"
  Call un.SnowShotEnsureAppClosed
  Push "$INSTDIR\bin\crashpad_handler.exe"
  Call un.SnowShotEnsureAppClosed
  Delete "$INSTDIR\bin\${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe"
  Delete "$INSTDIR\bin\${SNOW_SHOT_INSTALLER_UPDATER}.exe"
SectionEnd

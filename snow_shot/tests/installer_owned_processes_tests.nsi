Unicode true
Name "Snow Shot installer owned processes tests"
OutFile "${OUTPUT}"
RequestExecutionLevel user
!pragma warning disable 6010 ; The fixture exercises one entry point at a time.
!ifdef ANSWER
!macro SnowShotConfirmClose Prefix
  StrCmp "${ANSWER}" "closeApp" closeApp declined
!macroend
!endif
!include "${PACKAGING}\RunningApplication.nsh"
!include "${PACKAGING}\OwnedCleanup.nsh"

Section
  StrCpy $INSTDIR "${DESTINATION}"
!ifdef CHECK_SETUP
  Push "$INSTDIR"
  Call SnowShotEnsureInstallationClosed
!endif
  SetOutPath "$INSTDIR\bin"
  File /oname=${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe "${PAYLOAD}"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  StrCmp $SnowShotRestartRequired 1 0 +3
    FileOpen $0 "$INSTDIR\restart-required.txt" w
    FileClose $0
SectionEnd

Section "Uninstall"
  !insertmacro SnowShotUninstallOwnedCleanup
  ClearErrors
  Delete "$INSTDIR\bin\${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe"
  Delete "$INSTDIR\bin\${HELPER}.exe"
  Delete "$INSTDIR\bin\${SNOW_SHOT_INSTALLER_UPDATER}.exe"
  Delete "$INSTDIR\snow-shot-updater-ran.txt"
  Delete "$INSTDIR\snow-shot-installation.json"
  IfErrors 0 +2
    SetErrorLevel 20
SectionEnd

Unicode true
Name "Snow Shot installer owned cleanup tests"
OutFile "${OUTPUT}"
RequestExecutionLevel user
!include "LogicLib.nsh"
!include "${GUARD}"
!include "${OWNED_CLEANUP}"

Section
  StrCpy $INSTDIR "${DESTINATION}"
  SetOutPath "$INSTDIR\bin"
  File /oname=snow_shot.exe "${PAYLOAD}"
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  !insertmacro SnowShotUninstallOwnedCleanup
  Delete "$INSTDIR\bin\snow_shot.exe"
  Delete "$INSTDIR\bin\snow-shot-updater.exe"
  Delete "$INSTDIR\snow-shot-installation.json"
  RMDir "$INSTDIR\bin"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd

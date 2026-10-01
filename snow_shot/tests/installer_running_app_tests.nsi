Unicode true
Name "Snow Shot installer running application tests"
OutFile "${OUTPUT}"
RequestExecutionLevel user
!include "LogicLib.nsh"
!ifdef ANSWER
!macro SnowShotConfirmClose Prefix
  StrCmp "${ANSWER}" "closeApp" closeApp declined
!macroend
!endif
!ifdef GUARD
  !include "${GUARD}"
!endif
Section
  StrCpy $INSTDIR "${DESTINATION}"
!ifdef GUARD
  Push "$INSTDIR\snow_shot.exe"
  Call SnowShotEnsureMainAppClosed
!endif
  SetOutPath "$INSTDIR"
  File /oname=snow_shot.exe "${PAYLOAD}"
  IfErrors 0 +2
    SetErrorLevel 20
!ifdef GUARD
  WriteUninstaller "$INSTDIR\uninstall.exe"
!endif
SectionEnd
!ifdef GUARD
Section "Uninstall"
  Push "$INSTDIR\snow_shot.exe"
  Call un.SnowShotEnsureAppClosed
  Delete "$INSTDIR\snow_shot.exe"
SectionEnd
!endif

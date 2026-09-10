!ifndef SNOW_SHOT_INSTALL_DIRECTORY_INCLUDED
!define SNOW_SHOT_INSTALL_DIRECTORY_INCLUDED
!include "LogicLib.nsh"

; Run after selecting SHCTX and before the previous uninstaller deletes its
; registry entry. CPack preserves non-default $INSTDIR values during .onInit.
; Use the same /D override detection as CPack's directory initialization.
!macro SnowShotRestoreInstallDirectory RegistryKey DefaultDirectory
  ${If} $INSTDIR == "${DefaultDirectory}"
    Push $0
    ReadRegStr $0 SHCTX "${RegistryKey}" ""
    ${If} $0 != ""
      StrCpy $INSTDIR $0
    ${EndIf}
    Pop $0
  ${EndIf}
!macroend
!endif

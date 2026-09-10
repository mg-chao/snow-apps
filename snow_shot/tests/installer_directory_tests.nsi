Unicode true
!include "${PACKAGING}\InstallDirectory.nsh"
Name "Snow Shot installer directory test"
OutFile "${OUTPUT}"
RequestExecutionLevel user
InstallDir "${DEFAULT_DIRECTORY}"
SilentInstall silent

Function .onInit
  SetShellVarContext current
  ; Seed the same value written by CPack, in an isolated per-test key.
  !ifdef SAVED_DIRECTORY
    WriteRegStr SHCTX "${REGISTRY_KEY}" "" "${SAVED_DIRECTORY}"
  !endif
  StrCpy $0 "preserved register"
  !insertmacro SnowShotRestoreInstallDirectory "${REGISTRY_KEY}" "${DEFAULT_DIRECTORY}"
  ; An upgrade can now delete the previous install's registry entry.
  DeleteRegKey SHCTX "${REGISTRY_KEY}"
  ${If} $INSTDIR != "${EXPECTED_DIRECTORY}"
    SetErrorLevel 1
    Quit
  ${EndIf}
  ${If} $0 != "preserved register"
    SetErrorLevel 2
    Quit
  ${EndIf}
  SetErrorLevel 0
  Quit
FunctionEnd

Section
SectionEnd

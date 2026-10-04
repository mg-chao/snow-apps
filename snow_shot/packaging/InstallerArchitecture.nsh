!ifndef SNOW_SHOT_INSTALLER_ARCHITECTURE_INCLUDED
!define SNOW_SHOT_INSTALLER_ARCHITECTURE_INCLUDED
!include "x64.nsh"
!include "WinVer.nsh"
!include "${__FILEDIR__}\InstallerStrings.nsh"
!ifndef SNOW_SHOT_INSTALLER_ARCHITECTURE
!define SNOW_SHOT_INSTALLER_ARCHITECTURE "x64"
!endif

; Check before closing applications, invoking the old uninstaller, or extracting
; files. ARM64 replaces the same edition's x64 installation on an ARM64 OS.
!macro SnowShotCheckArchitecture
!if "${SNOW_SHOT_INSTALLER_ARCHITECTURE}" == "arm64"
  ${IfNot} ${IsNativeARM64}
    MessageBox MB_OK|MB_ICONSTOP "$(SnowShotArm64Required)" /SD IDOK
    SetErrorLevel 14
    Quit
  ${EndIf}
  ${IfNot} ${AtLeastWin11}
    MessageBox MB_OK|MB_ICONSTOP "$(SnowShotArm64Required)" /SD IDOK
    SetErrorLevel 14
    Quit
  ${EndIf}
!else if "${SNOW_SHOT_INSTALLER_ARCHITECTURE}" == "x64"
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "$(SnowShot64BitRequired)" /SD IDOK
    SetErrorLevel 14
    Quit
  ${EndIf}
!else
  !error "Unsupported Snow Shot installer architecture"
!endif
!macroend
!endif

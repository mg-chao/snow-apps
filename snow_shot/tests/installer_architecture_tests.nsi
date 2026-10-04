Unicode true
!include "MUI2.nsh"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "TradChinese"
!include "${PACKAGING}\InstallerArchitecture.nsh"
!ifdef SIMULATE_ARM64
  !define /redef IsNativeARM64 '"" LogicLib_AlwaysTrue ""'
!endif
!ifdef SIMULATE_WIN10
  !define /redef AtLeastWin11 '"" LogicLib_AlwaysFalse ""'
!endif
!ifdef SIMULATE_WIN11
  !define /redef AtLeastWin11 '"" LogicLib_AlwaysTrue ""'
!endif
Name "Snow Shot architecture test"
OutFile "${OUTPUT}"
RequestExecutionLevel user
SilentInstall silent

Function .onInit
  !insertmacro SnowShotCheckArchitecture
FunctionEnd

Section
  FileOpen $0 "${MARKER}" w
  FileWrite $0 "accepted"
  FileClose $0
SectionEnd

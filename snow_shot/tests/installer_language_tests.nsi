Unicode true
Name "Snow Shot"
OutFile "${OUTPUT}"
RequestExecutionLevel user
!include "MUI.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "${PACKAGING}\InstallerStrings.nsh"
!include "${PACKAGING}\InstallerLanguages.nsh"
!define MUI_LANGDLL_REGISTRY_ROOT HKCU
!define MUI_LANGDLL_REGISTRY_KEY "${REGISTRY_KEY}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "InstallerLanguage"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro SnowShotInstallerLanguages

!macro ReportLanguage Filename
  FileOpen $0 "${DESTINATION}\${Filename}" w
  FileWriteUTF16LE $0 "$LANGUAGE$\r$\n$(SnowShotLanguageTitle)$\r$\n$(SnowShotClosePrompt)$\r$\n$(SnowShotUninstallShortcut)$\r$\n$(^NextBtn)$\r$\n"
  FileClose $0
!macroend

Function .onInit
  ${GetParameters} $0
  ${GetOptions} $0 "/LANG=" $LANGUAGE
  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

Section
  !insertmacro SnowShotDeleteUninstallShortcuts "${DESTINATION}"
  !insertmacro ReportLanguage "install.txt"
  !insertmacro MUI_LANGDLL_SAVELANGUAGE
  ClearErrors
  WriteUninstaller "${DESTINATION}\uninstall.exe"
  IfErrors 0 +3
    SetErrorLevel 20
    Quit
SectionEnd

Function un.onInit
  StrCpy $LANGUAGE 1033
  !insertmacro MUI_UNGETLANGUAGE
FunctionEnd

Section "Uninstall"
  !insertmacro ReportLanguage "uninstall.txt"
SectionEnd

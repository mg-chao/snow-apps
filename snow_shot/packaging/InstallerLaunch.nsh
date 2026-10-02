!ifndef SNOW_SHOT_INSTALLER_LAUNCH_INCLUDED
!define SNOW_SHOT_INSTALLER_LAUNCH_INCLUDED
!ifndef SNOW_SHOT_INSTALLER_UPDATER
!define SNOW_SHOT_INSTALLER_UPDATER "snow-shot-updater"
!endif

; Use the desktop shell through the installed helper so an elevated installer
; does not restart Snow Shot with its own administrator token.
Function SnowShotLaunchDesktop
  ClearErrors
  ExecWait '"$INSTDIR\bin\${SNOW_SHOT_INSTALLER_UPDATER}.exe" --launch-desktop --target "$INSTDIR"' $0
  IfErrors snowDesktopFailed
  StrCmp $0 0 snowDesktopDone
snowDesktopFailed:
  DetailPrint "$(SnowShotDesktopLaunchFailed)"
  MessageBox MB_OK|MB_ICONEXCLAMATION "$(SnowShotDesktopLaunchFailed)" /SD IDOK
snowDesktopDone:
FunctionEnd

; MUI's finish-page checkbox handles interactive launches. Silent setup has
; no finish page, and only restarts an app it closed after installation succeeds.
Function .onInstSuccess
  IfSilent 0 snowRestartDone
  StrCmp $SnowShotRestartRequired 1 0 snowRestartDone
  Call SnowShotLaunchDesktop
snowRestartDone:
FunctionEnd
!endif

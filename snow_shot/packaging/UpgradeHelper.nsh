!ifndef SNOW_SHOT_UPGRADE_HELPER_INCLUDED
!define SNOW_SHOT_UPGRADE_HELPER_INCLUDED

Var SnowShotUpgradeHelperRoot
Var SnowShotUpgradeHelperBackup
Var SnowShotUpgradeBootstrap

; Old uninstallers launch the helper installed alongside them. Bootstrap that
; entry point with this package's helper so fixes also apply to existing copies.
; The native helper owns locking, backup, and atomic replacement.
Function SnowShotPrepareUpgradeHelper
  Exch $0
  Push $1
  Push $2
  StrCpy $SnowShotUpgradeHelperRoot ""
  StrCpy $SnowShotUpgradeHelperBackup ""
  IfFileExists "$0\bin\${SNOW_SHOT_INSTALLER_UPDATER}.exe" 0 snowUpgradeHelperDone
  InitPluginsDir
  StrCpy $1 $OUTDIR
  ClearErrors
  SetOutPath "$PLUGINSDIR"
!ifdef SNOW_SHOT_PAYLOAD_ARCHIVE
  Call SnowShotPreparePayloadResources
  ClearErrors
  ExecWait '"$PLUGINSDIR\snow-shot-payload-extractor.exe" "$PLUGINSDIR\snow-shot-payload.7z" "$PLUGINSDIR\payload-bootstrap\." "${SNOW_SHOT_PAYLOAD_SHA256}" "bin/${SNOW_SHOT_INSTALLER_UPDATER}.exe"' $2
  IfErrors snowUpgradePayloadFailed
  StrCmp $2 0 0 snowUpgradePayloadFailed
  StrCpy $SnowShotUpgradeBootstrap "$PLUGINSDIR\payload-bootstrap\bin\${SNOW_SHOT_INSTALLER_UPDATER}.exe"
!else
  File /oname=snow-shot-installer-updater.exe "${INST_DIR}\bin\${SNOW_SHOT_INSTALLER_UPDATER}.exe"
  StrCpy $2 0
  IfErrors 0 +2
    StrCpy $2 1
  StrCmp $2 0 0 snowUpgradeHelperFailed
  StrCpy $SnowShotUpgradeBootstrap "$PLUGINSDIR\snow-shot-installer-updater.exe"
!endif
  SetOutPath "$1"
  ClearErrors
  ExecWait '"$SnowShotUpgradeBootstrap" --prepare-installer-upgrade --target "$0" --backup "$PLUGINSDIR\previous-updater.exe"' $2
  IfErrors snowUpgradeHelperFailed
  StrCmp $2 0 0 snowUpgradeHelperFailed
  StrCpy $SnowShotUpgradeHelperRoot $0
  StrCpy $SnowShotUpgradeHelperBackup "$PLUGINSDIR\previous-updater.exe"
  Goto snowUpgradeHelperDone
!ifdef SNOW_SHOT_PAYLOAD_ARCHIVE
snowUpgradePayloadFailed:
  MessageBox MB_OK|MB_ICONSTOP "$(SnowShotPayloadExtractionFailed)" /SD IDOK
  SetErrorLevel 14
  Quit
!endif
snowUpgradeHelperFailed:
  MessageBox MB_OK|MB_ICONSTOP "$(SnowShotStartupCleanupFailed)" /SD IDOK
  SetErrorLevel 12
  Quit
snowUpgradeHelperDone:
  Pop $2
  Pop $1
  Pop $0
  ClearErrors
FunctionEnd

; Restore the original entry point when the previous uninstall fails. The
; native helper checks that another operation has not changed it in the meantime.
Function SnowShotRestoreUpgradeHelper
  Push $0
  StrCmp $SnowShotUpgradeHelperRoot "" snowUpgradeRestoreDone
  ClearErrors
  ExecWait '"$SnowShotUpgradeBootstrap" --restore-installer-upgrade --target "$SnowShotUpgradeHelperRoot" --backup "$SnowShotUpgradeHelperBackup"' $0
  IfErrors snowUpgradeRestoreFailed
  StrCmp $0 0 snowUpgradeRestoreDone
snowUpgradeRestoreFailed:
  MessageBox MB_OK|MB_ICONSTOP "$(SnowShotStartupCleanupFailed)" /SD IDOK
  SetErrorLevel 14
  Quit
snowUpgradeRestoreDone:
  StrCpy $SnowShotUpgradeHelperRoot ""
  StrCpy $SnowShotUpgradeHelperBackup ""
  Pop $0
FunctionEnd
!endif

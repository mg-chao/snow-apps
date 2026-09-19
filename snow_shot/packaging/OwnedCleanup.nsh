!ifndef SNOW_SHOT_OWNED_CLEANUP_INCLUDED
!define SNOW_SHOT_OWNED_CLEANUP_INCLUDED
!include "FileFunc.nsh"

; The update helper tracks files that self-updates added after this
; uninstaller was built, plus the startup registrations. Run a copy of it
; before CPack's original file deletion list, so the copy is not deleted
; underneath itself. If the copy cannot be isolated, run the installed
; helper instead: file deletion has not started yet. Missing components of
; a partial installation are skipped like any other missing file: the guard
; functions finish without prompting when their target does not exist, and
; a missing or unlaunchable helper leaves nothing to run and removes
; nothing. Only a helper that ran and failed blocks the uninstallation,
; because its failure (running application, active update, access denied)
; is recoverable before files are removed.
!macro SnowShotUninstallOwnedCleanup
  Push "$INSTDIR\bin\snow_shot.exe"
  Call un.SnowShotEnsureAppClosed
  Push "$INSTDIR\bin\crashpad_handler.exe"
  Call un.SnowShotEnsureAppClosed
  IfFileExists "$INSTDIR\bin\snow-shot-updater.exe" 0 snowOwnedDone
    InitPluginsDir
    StrCpy $4 "$INSTDIR\bin\snow-shot-updater.exe"
    ClearErrors
    CopyFiles /SILENT "$INSTDIR\bin\snow-shot-updater.exe" "$PLUGINSDIR\snow-shot-updater.exe"
    IfErrors snowOwnedRun
    StrCpy $4 "$PLUGINSDIR\snow-shot-updater.exe"
  snowOwnedRun:
    StrCpy $1 ""
    ${GetParameters} $2
    ClearErrors
    ${GetOptions} $2 "/SNOWUPGRADE" $3
    IfErrors +2
      StrCpy $1 "--upgrade"
    ClearErrors
    ExecWait '"$4" --uninstall --target "$INSTDIR" $1' $0
    IfErrors snowOwnedDone
    StrCmp $0 0 snowOwnedDone snowOwnedFailed
snowOwnedFailed:
    MessageBox MB_OK|MB_ICONSTOP "$(SnowShotStartupCleanupFailed)" /SD IDOK
    SetErrorLevel 12
    Quit
snowOwnedDone:
!macroend
!endif

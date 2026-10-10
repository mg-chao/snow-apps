!ifndef SNOW_SHOT_RUNNING_APPLICATION_INCLUDED
!define SNOW_SHOT_RUNNING_APPLICATION_INCLUDED
!include "LogicLib.nsh"

!include "${__FILEDIR__}\InstallerStrings.nsh"

!ifndef SNOW_SHOT_INSTALLER_EXECUTABLE
!define SNOW_SHOT_INSTALLER_EXECUTABLE "snow_shot"
!endif
!ifndef SNOW_SHOT_INSTALLER_UPDATER
!define SNOW_SHOT_INSTALLER_UPDATER "snow-shot-updater"
!endif
!ifndef SNOW_SHOT_INSTALLER_MCP
!searchreplace SNOW_SHOT_INSTALLER_MCP "${SNOW_SHOT_INSTALLER_UPDATER}" "-updater" "-mcp"
!endif

Var SnowShotAppWasClosed
Var SnowShotRestartRequired

!ifmacrondef SnowShotConfirmClose
!macro SnowShotConfirmClose Prefix
!if "${Prefix}" == ""
  ; Silent setup authorizes closing the affected installation. Standalone
  ; silent uninstall still requires the application to have exited already.
  IfSilent closeApp
!else
  IfSilent declined
!endif
  MessageBox MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2 "$(SnowShotClosePrompt)" /SD IDNO IDYES closeApp IDNO declined
!macroend
!endif

; Windowless helpers without a console can be classified as RmCritical. A
; failed RmShutdown does not authorize killing arbitrary resource holders.
; Use its process list, then verify both creation time and full image path
; through the same process handle before terminating this executable only.
!macro SnowShotCloseExecutableHoldersFunction Prefix
Function ${Prefix}SnowShotCloseExecutableHolders
  Push $3
  Push $4
  Push $5
  Push $6
  Push $7
  Push $8
  Push $9
  Push $R0
  Push $R1
  Push $R2
  Push $R3
  Push $R4
  System::Call 'rstrtmgr::RmGetList(i r1, *i .r3, *i 0, p 0, *i .r4) i.r2'
  StrCmp $2 0 done
  StrCmp $2 234 0 done
  ; RM_PROCESS_INFO: RM_UNIQUE_PROCESS (12), WCHAR[256], WCHAR[64], four DWORDs.
  IntOp $4 $3 * 668
  System::Alloc $4
  Pop $4
  StrCmp $4 0 allocationFailed
  System::Call 'rstrtmgr::RmGetList(i r1, *i .r3, *i r3, p r4, *i .r5) i.r2'
  StrCmp $2 0 0 freeList
  StrCpy $5 $4
  StrCpy $6 $3
nextHolder:
  StrCmp $6 0 freeList
  System::Call '*$5(i .r7, l .r8)'
  ; PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE.
  System::Call 'kernel32::OpenProcess(i 0x101001, i 0, i r7) p.r9'
  StrCmp $9 0 openFailed
  System::Call 'kernel32::WaitForSingleObject(p r9, i 0) i.rR4'
  StrCmp $R4 0 closeHandle
  System::Call 'kernel32::GetProcessTimes(p r9, *l .rR0, *l .rR1, *l .rR2, *l .rR3) i.rR4'
  StrCmp $R4 0 handleFailed
  System::Int64Op $8 = $R0
  Pop $R4
  StrCmp $R4 1 0 closeHandle
  System::Call 'kernel32::QueryFullProcessImageNameW(p r9, i 0, w .rR1, *i ${NSIS_MAX_STRLEN}) i.rR4'
  StrCmp $R4 0 handleFailed
  StrCmp $R1 $0 0 closeHandle
  System::Call 'kernel32::TerminateProcess(p r9, i 0) i.rR4'
  StrCmp $R4 0 handleFailed
  System::Call 'kernel32::WaitForSingleObject(p r9, i 30000) i.r2'
  StrCmp $2 0 closeHandle
  System::Call 'kernel32::CloseHandle(p r9)'
  Goto freeList
handleFailed:
  System::Call 'kernel32::GetLastError() i.r2'
  System::Call 'kernel32::CloseHandle(p r9)'
  Goto freeList
openFailed:
  System::Call 'kernel32::GetLastError() i.r2'
  StrCmp $2 87 0 freeList ; ERROR_INVALID_PARAMETER: the process already exited.
  StrCpy $2 0
  Goto advanceHolder
closeHandle:
  System::Call 'kernel32::CloseHandle(p r9)'
advanceHolder:
  IntOp $5 $5 + 668
  IntOp $6 $6 - 1
  Goto nextHolder
allocationFailed:
  StrCpy $2 14 ; ERROR_OUTOFMEMORY
  Goto done
freeList:
  System::Free $4
done:
  Pop $R4
  Pop $R3
  Pop $R2
  Pop $R1
  Pop $R0
  Pop $9
  Pop $8
  Pop $7
  Pop $6
  Pop $5
  Pop $4
  Pop $3
FunctionEnd
!macroend
!insertmacro SnowShotCloseExecutableHoldersFunction ""
!insertmacro SnowShotCloseExecutableHoldersFunction "un."

; Windows Restart Manager identifies holders of this exact executable path.
; https://learn.microsoft.com/windows/win32/rstmgr/using-restart-manager
; The System plug-in is bundled with NSIS; no external binary plug-in is needed.
!macro SnowShotRunningApplicationFunction Prefix
Function ${Prefix}SnowShotEnsureAppClosed
  Exch $0
  Push $1
  Push $2
  Push $3
  Push $4
  Push $5
  Push $6
  StrCpy $6 0
  IfFileExists "$0" 0 finished
startSession:
  System::Call 'rstrtmgr::RmStartSession(*i .r1, i 0, w .r2) i.r2'
  StrCmp $2 0 0 failed
  System::Call '*(&w${NSIS_MAX_STRLEN} r0) p.r3'
  System::Call '*(p r3) p.r4'
  System::Call 'rstrtmgr::RmRegisterResources(i r1, i 1, p r4, i 0, p 0, i 0, p 0) i.r2'
  System::Free $4
  System::Free $3
  StrCmp $2 0 0 sessionFailed

  System::Call 'rstrtmgr::RmGetList(i r1, *i .r5, *i 0, p 0, *i .r4) i.r2'
  StrCmp $2 0 sessionFinished
  StrCmp $2 234 0 sessionFailed ; ERROR_MORE_DATA means there are file holders.
  StrCmp $6 1 sessionFailed
  !insertmacro SnowShotConfirmClose "${Prefix}"
declined:
  System::Call 'rstrtmgr::RmEndSession(i r1)'
  SetErrorLevel 10
  Quit

closeApp:
  ; RmForceShutdown first requests shutdown and terminates unresponsive apps.
  System::Call 'rstrtmgr::RmShutdown(i r1, i 1, p 0) i.r2'
  StrCmp $2 0 +2
    Call ${Prefix}SnowShotCloseExecutableHolders
  StrCmp $2 0 0 sessionFailed
  ; A session retains stopped processes for RmRestart. Use a fresh session
  ; to check for current file holders rather than treating that list as live.
  System::Call 'rstrtmgr::RmEndSession(i r1)'
  StrCpy $6 1
  Goto startSession
sessionFinished:
  System::Call 'rstrtmgr::RmEndSession(i r1)'
  Goto finished
sessionFailed:
  System::Call 'rstrtmgr::RmEndSession(i r1)'
failed:
  DetailPrint "$(SnowShotCloseFailed) ($2)"
  MessageBox MB_OK|MB_ICONSTOP "$(SnowShotCloseFailed)" /SD IDOK
  SetErrorLevel 11
  Quit
finished:
!if "${Prefix}" == ""
  StrCpy $SnowShotAppWasClosed $6
!endif
  Pop $6
  Pop $5
  Pop $4
  Pop $3
  Pop $2
  Pop $1
  Pop $0
FunctionEnd
!macroend

!insertmacro SnowShotRunningApplicationFunction ""
!insertmacro SnowShotRunningApplicationFunction "un."

; Only closing the main executable requires a restart. Crashpad or another
; file holder must not cause a previously stopped application to be launched.
; Keep this intent across .onInit, the old uninstaller, and destination checks.
Function SnowShotEnsureMainAppClosed
  Call SnowShotEnsureAppClosed
  StrCmp $SnowShotAppWasClosed 1 0 +2
    StrCpy $SnowShotRestartRequired 1
FunctionEnd

; All installer entry points use the same shipped executable inventory. Check
; even when the main app is missing: MCP clients outlive the UI and can keep
; the payload mapped after a previous partial uninstall or self-update.
!macro SnowShotInstallationClosedFunction Prefix
Function ${Prefix}SnowShotEnsureInstallationClosed
  Exch $0
  Push "$0\bin\${SNOW_SHOT_INSTALLER_EXECUTABLE}.exe"
!if "${Prefix}" == ""
  Call SnowShotEnsureMainAppClosed
!else
  Call un.SnowShotEnsureAppClosed
!endif
  Push "$0\bin\${SNOW_SHOT_INSTALLER_MCP}.exe"
  Call ${Prefix}SnowShotEnsureAppClosed
  Push "$0\bin\${SNOW_SHOT_INSTALLER_UPDATER}.exe"
  Call ${Prefix}SnowShotEnsureAppClosed
!if "${SNOW_SHOT_INSTALLER_EXECUTABLE}" == "snow_shot"
  Push "$0\bin\snow-ocr-process.exe"
  Call ${Prefix}SnowShotEnsureAppClosed
!endif
  Push "$0\bin\crashpad_handler.exe"
  Call ${Prefix}SnowShotEnsureAppClosed
  Pop $0
FunctionEnd
!macroend
!insertmacro SnowShotInstallationClosedFunction ""
!insertmacro SnowShotInstallationClosedFunction "un."
!endif

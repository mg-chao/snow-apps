!ifndef SNOW_SHOT_INSTALLER_PAYLOAD_INCLUDED
!define SNOW_SHOT_INSTALLER_PAYLOAD_INCLUDED

!ifdef SNOW_SHOT_PAYLOAD_ARCHIVE
; Share the same embedded archive with the upgrade bootstrap. Extracting only
; its updater entry avoids bundling another copy of the updater executable.
Function SnowShotPreparePayloadResources
  Push $0
  StrCpy $0 $OUTDIR
  InitPluginsDir
  IfFileExists "$PLUGINSDIR\snow-shot-payload.7z" 0 snowPayloadCopyResources
  IfFileExists "$PLUGINSDIR\snow-shot-payload-extractor.exe" snowPayloadResourcesReady
snowPayloadCopyResources:
  SetOutPath "$PLUGINSDIR"
  SetCompress auto
  File /oname=snow-shot-payload-extractor.exe "${SNOW_SHOT_PAYLOAD_EXTRACTOR}"
  ; A solid outer NSIS stream would recompress this archive regardless of
  ; SetCompress. Payload packages deliberately use non-solid outer LZMA.
  SetCompress off
  File /oname=snow-shot-payload.7z "${SNOW_SHOT_PAYLOAD_ARCHIVE}"
  SetCompress auto
  SetOutPath "$0"
snowPayloadResourcesReady:
  Pop $0
FunctionEnd
!endif

!macro SnowShotInstallPayload
  Call SnowShotPreparePayloadResources
  DetailPrint "$(SnowShotExtractingPayload)"
  ClearErrors
  ; The suffix also prevents a trailing install-directory backslash from
  ; escaping the closing quote in CommandLineToArgvW.
  ExecWait '"$PLUGINSDIR\snow-shot-payload-extractor.exe" "$PLUGINSDIR\snow-shot-payload.7z" "$INSTDIR\." "${SNOW_SHOT_PAYLOAD_SHA256}"' $0
  IfErrors snowPayloadExtractionFailed
  StrCmp $0 0 snowPayloadExtracted
snowPayloadExtractionFailed:
  MessageBox MB_OK|MB_ICONSTOP "$(SnowShotPayloadExtractionFailed)" /SD IDOK
  SetErrorLevel 14
  Abort
snowPayloadExtracted:
  SetOutPath "$INSTDIR"
!macroend

!endif

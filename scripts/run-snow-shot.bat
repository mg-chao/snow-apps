@echo off
setlocal
set "script_dir=%~dp0"
where pwsh.exe >nul 2>nul
if %ERRORLEVEL%==0 (
    pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%script_dir%run-snow-shot.ps1" %*
) else (
    powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%script_dir%run-snow-shot.ps1" %*
)
set "exit_code=%ERRORLEVEL%"
endlocal & exit /b %exit_code%

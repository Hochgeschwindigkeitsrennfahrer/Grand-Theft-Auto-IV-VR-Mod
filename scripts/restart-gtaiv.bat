@echo off
cd /d "%~dp0"
REM Always -NoPause: window closes by itself. For a pinable icon use:
REM   powershell -File "%~dp0install-restart-shortcut.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0restart-gtaiv.ps1" -NoPause %*
exit /b %ERRORLEVEL%

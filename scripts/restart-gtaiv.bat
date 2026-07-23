@echo off
cd /d "%~dp0"
echo Starte GTA IV Quick Restart...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0restart-gtaiv.ps1" %*
if errorlevel 1 (
  echo.
  echo Skript fehlgeschlagen.
  pause
)

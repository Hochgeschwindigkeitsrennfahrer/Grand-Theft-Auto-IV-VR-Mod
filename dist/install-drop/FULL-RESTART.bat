@echo off
REM Real process restart for gtaiv-dxvk-vr (Steam app 12210)
cd /d "%~dp0"
taskkill /F /IM GTAIV.exe >nul 2>&1
taskkill /F /IM PlayGTAIV.exe >nul 2>&1
taskkill /F /IM RockstarErrorHandler.exe >nul 2>&1
timeout /t 2 /nobreak >nul
set STEAM=C:\Program Files (x86)\Steam\steam.exe
if not exist "%STEAM%" set STEAM=%ProgramFiles(x86)%\Steam\steam.exe
start "" "%STEAM%" -applaunch 12210

@echo off
REM gtaiv-dxvk-vr — Quick Start
REM 1) Start SteamVR first  2) run this bat  3) put headset on
cd /d "%~dp0"

if not exist "GTAIV.exe" (
  echo ERROR: Put this bat inside your GTAIV folder ^(next to GTAIV.exe^).
  echo Typical: C:\Program Files ^(x86^)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV
  pause
  exit /b 1
)

if not exist "gtaiv_dxvk_vr.asi" (
  echo ERROR: gtaiv_dxvk_vr.asi missing. Copy the release drop-into-GTAIV files first.
  pause
  exit /b 1
)

if not exist "gtaiv_dxvk_vr.stereo" (
  echo 909> gtaiv_dxvk_vr.stereo
)

echo Starting SteamVR if needed...
start "" "steam://rungameid/250820" >nul 2>&1
timeout /t 3 /nobreak >nul

set STEAM=C:\Program Files (x86)\Steam\steam.exe
if not exist "%STEAM%" set STEAM=%ProgramFiles(x86)%\Steam\steam.exe
echo Launching GTA IV CE ^(Steam app 12210^)...
start "" "%STEAM%" -applaunch 12210
echo.
echo Headset on. Log file: gtaiv_dxvk_vr.log
echo F3 = VR menu. Full restart in menu runs FULL-RESTART.bat
echo.
pause

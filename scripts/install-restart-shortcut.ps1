# Creates a pinable Desktop + Start Menu shortcut for GTA IV Quick Restart.
# Target is wscript.exe (real .exe → Windows allows Pin to taskbar) + silent .vbs.
# Icon = GTAIV.exe. No CMD window, no "Enter to close".
param(
  [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
)

$ErrorActionPreference = "Stop"
$scripts = $PSScriptRoot
$vbs = Join-Path $scripts "Restart-GTAIV.vbs"
$wscript = Join-Path $env:WINDIR "System32\wscript.exe"
$gtaExe = Join-Path $GameDir "GTAIV.exe"

if (-not (Test-Path $vbs)) { throw "Missing: $vbs" }
if (-not (Test-Path $wscript)) { throw "Missing: $wscript" }

$icon = if (Test-Path $gtaExe) { "$gtaExe,0" } else { "$wscript,0" }

function New-RestartShortcut([string]$lnkPath) {
  $dir = Split-Path $lnkPath -Parent
  if (-not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
  }
  $w = New-Object -ComObject WScript.Shell
  $s = $w.CreateShortcut($lnkPath)
  $s.TargetPath = $wscript
  $s.Arguments = "//nologo `"$vbs`""
  $s.WorkingDirectory = $scripts
  $s.WindowStyle = 7  # Minimized (wscript itself is silent)
  $s.IconLocation = $icon
  $s.Description = "GTA IV Quick Restart (kill + DirectExe / RGLess, no console)"
  $s.Save()
  Write-Host "OK: $lnkPath"
}

$desktop = Join-Path ([Environment]::GetFolderPath("Desktop")) "GTA IV Quick Restart.lnk"
$startMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\GTA IV Quick Restart.lnk"

New-RestartShortcut $desktop
New-RestartShortcut $startMenu

Write-Host ""
Write-Host "Pin to taskbar:"
Write-Host "  1. Open Start Menu → search 'GTA IV Quick Restart'"
Write-Host "  2. Right-click → Pin to taskbar"
Write-Host "  (Desktop icon: right-click → Show more options → Pin to taskbar)"
Write-Host ""
Write-Host "Runs silent (no CMD, no Enter). Same as: restart-gtaiv.ps1 -NoPause -DirectExe"

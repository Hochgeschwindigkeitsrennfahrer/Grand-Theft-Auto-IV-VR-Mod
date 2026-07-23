# Deploy eine gebaute x86-d3d9.dll nach GTAIV.
# Nutzung:
#   .\scripts\deploy.ps1 -GameDir "C:\...\Grand Theft Auto IV\GTAIV" -DllPath ".\path\to\d3d9.dll"

param(
  [Parameter(Mandatory = $true)]
  [string] $GameDir,

  [Parameter(Mandatory = $true)]
  [string] $DllPath,

  [switch] $SkipBackup
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $GameDir)) {
  throw "GameDir nicht gefunden: $GameDir"
}
if (-not (Test-Path $DllPath)) {
  throw "DllPath nicht gefunden: $DllPath"
}

$exe = Join-Path $GameDir "GTAIV.exe"
if (-not (Test-Path $exe)) {
  Write-Warning "GTAIV.exe nicht in GameDir — bist du im richtigen Ordner?"
}

$dest = Join-Path $GameDir "d3d9.dll"
$bak = Join-Path $GameDir "d3d9.dll.gtaiv-dxvk-vr.bak"

if ((Test-Path $dest) -and -not $SkipBackup) {
  if (-not (Test-Path $bak)) {
    Copy-Item -LiteralPath $dest -Destination $bak -Force
    Write-Host "Backup: $bak"
  } else {
    Write-Host "Backup existiert bereits: $bak"
  }
}

Copy-Item -LiteralPath $DllPath -Destination $dest -Force
Write-Host "Deployed: $dest"

# dxvk.conf bewusst NICHT automatisch kopieren — verunreinigt FusionFix-Vulkan-Tests.
# Bei Bedarf manuell aus config\dxvk.conf.example anlegen.

Write-Host ""
Write-Host "Checkliste:"
Write-Host "  [ ] DLL ist Win32/x86 (nicht x64)"
Write-Host "  [ ] FusionFix Vulkan/DXVK-Toggle AUS"
Write-Host "  [ ] SteamVR laeuft (fuer OpenVR-Tests)"
Write-Host "  [ ] Singleplayer / offline"
Write-Host "  [ ] Bei Problemen Backup zurueck: copy `"$bak`" `"$dest`""

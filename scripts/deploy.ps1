# Deploy a built x86 d3d9.dll to GTAIV.
# Usage:
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
  throw "GameDir not found: $GameDir"
}
if (-not (Test-Path $DllPath)) {
  throw "DllPath not found: $DllPath"
}

$exe = Join-Path $GameDir "GTAIV.exe"
if (-not (Test-Path $exe)) {
  Write-Warning "GTAIV.exe not in GameDir — are you in the right folder?"
}

$dest = Join-Path $GameDir "d3d9.dll"
$bak = Join-Path $GameDir "d3d9.dll.gtaiv-dxvk-vr.bak"

if ((Test-Path $dest) -and -not $SkipBackup) {
  if (-not (Test-Path $bak)) {
    Copy-Item -LiteralPath $dest -Destination $bak -Force
    Write-Host "Backup: $bak"
  } else {
    Write-Host "Backup already exists: $bak"
  }
}

Copy-Item -LiteralPath $DllPath -Destination $dest -Force
Write-Host "Deployed: $dest"

# Intentionally do NOT auto-copy dxvk.conf — pollutes FusionFix Vulkan tests.
# If needed, create manually from config\dxvk.conf.example.

Write-Host ""
Write-Host "Checklist:"
Write-Host "  [ ] DLL is Win32/x86 (not x64)"
Write-Host "  [ ] FusionFix Vulkan/DXVK toggle OFF"
Write-Host "  [ ] SteamVR running (for OpenVR tests)"
Write-Host "  [ ] Singleplayer / offline"
Write-Host "  [ ] If problems, restore backup: copy `"$bak`" `"$dest`""

# Undo RGLess install from a backup created by install-rgless.ps1
param(
  [string] $BackupDir = "",
  [string] $GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BackupRoot = Join-Path $Root "backups\rgless"

if (-not $BackupDir) {
  $latest = Join-Path $BackupRoot "LATEST.txt"
  if (Test-Path $latest) {
    $BackupDir = (Get-Content $latest -Raw).Trim()
  }
}

if (-not $BackupDir -or -not (Test-Path $BackupDir)) {
  Write-Host "No backup found under: $BackupRoot"
  Write-Host "Existing backups:"
  Get-ChildItem $BackupRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $($_.FullName)" }
  Write-Host "Usage: .\scripts\restore-rgless.ps1 -BackupDir `"<path>`""
  exit 2
}

$manifestPath = Join-Path $BackupDir "MANIFEST.json"
if (-not (Test-Path $manifestPath)) {
  throw "MANIFEST.json missing in $BackupDir"
}

$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
if ($manifest.gameDir) { $GameDir = [string]$manifest.gameDir }

$running = @(Get-Process -Name "GTAIV", "PlayGTAIV" -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
  Write-Host "Closing GTA..."
  $running | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
}

Write-Host "=== RGLess Restore ==="
Write-Host "Backup: $BackupDir"
Write-Host "Game:   $GameDir"

$PreDir = Join-Path $BackupDir "pre-replace"

# 1) Delete files that RGLess newly added
foreach ($rel in @($manifest.addedFiles)) {
  if (-not $rel) { continue }
  $path = Join-Path $GameDir $rel
  if (Test-Path $path) {
    Remove-Item -LiteralPath $path -Force
    Write-Host "  del $rel"
  }
}

# 2) Restore replaced originals
if (Test-Path $PreDir) {
  $backs = @(Get-ChildItem $PreDir -Recurse -File)
  foreach ($b in $backs) {
    $rel = $b.FullName.Substring($PreDir.Length).TrimStart("\", "/")
    $dest = Join-Path $GameDir $rel
    $destDir = Split-Path $dest -Parent
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    Copy-Item -LiteralPath $b.FullName -Destination $dest -Force
    Write-Host "  restore $rel"
  }
}

# Optional: remove empty save folder created by us (do NOT delete if user has progress there)
$gameSave = Join-Path $GameDir "save"
Write-Host ""
Write-Host "Note: Folder $gameSave was NOT deleted (in case saves are there)."
Write-Host "Documents saves were only copied during install — they remain under Documents."
Write-Host ""
Write-Host "OK — RGLess rolled back."
Write-Host "Then test Steam launch (not -DirectExe)."
Write-Host "If broken: Steam → Properties → Verify files, then re-deploy ASI/DXVK/FusionFix."

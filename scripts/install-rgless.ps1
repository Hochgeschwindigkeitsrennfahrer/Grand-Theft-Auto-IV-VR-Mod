# Install RGLess with full rollback support.
# 1) Download RGLess zip (TJGM guide) into: vendor\rgless\RGLess.zip
# 2) Close GTA
# 3) Run: .\scripts\install-rgless.ps1
#
# Restore later: .\scripts\restore-rgless.ps1
param(
  [string] $GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV",
  [string] $ZipPath = ""
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$DropDir = Join-Path $Root "vendor\rgless"
$BackupRoot = Join-Path $Root "backups\rgless"
$GuideUrl = "https://tjgm.dev/guides/gtaiv/Removing-Rockstar-Games-Launcher-from-GTA-IV/"

New-Item -ItemType Directory -Force -Path $DropDir | Out-Null
New-Item -ItemType Directory -Force -Path $BackupRoot | Out-Null

if (-not $ZipPath) {
  $candidates = @(
    (Join-Path $DropDir "RGLess.zip"),
    (Join-Path $DropDir "rgless.zip")
  ) + @(Get-ChildItem $DropDir -File -ErrorAction SilentlyContinue | Where-Object {
      $_.Extension -in ".zip", ".7z" -or $_.Name -match "rgless"
    } | ForEach-Object { $_.FullName })
  $ZipPath = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}

if (-not $ZipPath -or -not (Test-Path $ZipPath)) {
  Write-Host ""
  Write-Host "RGLess zip missing."
  Write-Host "1) Open guide: $GuideUrl"
  Write-Host "2) Download zip (Mediafire / Mega / Internet Archive)"
  Write-Host "3) Save as: $DropDir\RGLess.zip"
  Write-Host "4) Run this script again"
  Write-Host ""
  try { Start-Process $GuideUrl } catch {}
  try { Start-Process explorer.exe $DropDir } catch {}
  exit 2
}

if (-not (Test-Path (Join-Path $GameDir "GTAIV.exe"))) {
  throw "GTAIV.exe not found in: $GameDir"
}

$running = @(Get-Process -Name "GTAIV", "PlayGTAIV" -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
  Write-Host "Closing GTA..."
  $running | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$BackupDir = Join-Path $BackupRoot $stamp
$PreDir = Join-Path $BackupDir "pre-replace"
$ExtractDir = Join-Path $BackupDir "rgless-extract"
$SavesBackup = Join-Path $BackupDir "saves-documents"
New-Item -ItemType Directory -Force -Path $PreDir, $ExtractDir, $SavesBackup | Out-Null

Write-Host "=== RGLess Install (reversible) ==="
Write-Host "Zip:     $ZipPath"
Write-Host "Game:    $GameDir"
Write-Host "Backup:  $BackupDir"

# Extract zip (Expand-Archive needs .zip)
$tmpZip = $ZipPath
if ([IO.Path]::GetExtension($ZipPath) -ieq ".7z") {
  throw "Please provide as .zip (or extract 7z first to vendor\rgless\extracted)."
}

Expand-Archive -LiteralPath $tmpZip -DestinationPath $ExtractDir -Force

# If zip has a single top folder, use that as content root
$contentRoot = $ExtractDir
$top = @(Get-ChildItem $ExtractDir -Force)
if ($top.Count -eq 1 -and $top[0].PSIsContainer) {
  $contentRoot = $top[0].FullName
}

$payloadFiles = @(Get-ChildItem $contentRoot -Recurse -File)
if ($payloadFiles.Count -eq 0) {
  throw "Zip contains no files: $ZipPath"
}

$added = New-Object System.Collections.Generic.List[string]
$replaced = New-Object System.Collections.Generic.List[string]

foreach ($f in $payloadFiles) {
  $rel = $f.FullName.Substring($contentRoot.Length).TrimStart("\", "/")
  $dest = Join-Path $GameDir $rel
  $destDir = Split-Path $dest -Parent
  if (-not (Test-Path $destDir)) {
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
  }

  if (Test-Path $dest) {
    $bak = Join-Path $PreDir $rel
    $bakDir = Split-Path $bak -Parent
    New-Item -ItemType Directory -Force -Path $bakDir | Out-Null
    Copy-Item -LiteralPath $dest -Destination $bak -Force
    $replaced.Add($rel) | Out-Null
  } else {
    $added.Add($rel) | Out-Null
  }

  Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
  Write-Host ("  + {0}" -f $rel)
}

# Backup Documents saves (do not delete originals)
$docsGta = Join-Path $env:USERPROFILE "Documents\Rockstar Games\GTA IV"
if (Test-Path $docsGta) {
  Write-Host "Backing up Documents saves..."
  Copy-Item -LiteralPath $docsGta -Destination (Join-Path $SavesBackup "GTA IV") -Recurse -Force
}

# Copy profile saves into RGLess path: save\GTA IV\Profiles\<id>\
$gameSave = Join-Path $GameDir "save\GTA IV"
$gameProfiles = Join-Path $gameSave "Profiles"
New-Item -ItemType Directory -Force -Path $gameProfiles | Out-Null
$docsProfiles = Join-Path $docsGta "Profiles"
if (Test-Path $docsProfiles) {
  foreach ($prof in @(Get-ChildItem $docsProfiles -Directory -ErrorAction SilentlyContinue)) {
    $dest = Join-Path $gameProfiles $prof.Name
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item -Path (Join-Path $prof.FullName "*") -Destination $dest -Force
    Write-Host ("Profile copied: {0} -> {1}" -f $prof.Name, $dest)
  }
  foreach ($s in @(Get-ChildItem $docsProfiles -Recurse -File -Filter "SGTA*" -ErrorAction SilentlyContinue)) {
    Copy-Item $s.FullName (Join-Path $gameSave $s.Name) -Force
  }
} elseif (Test-Path $docsGta) {
  Copy-Item -LiteralPath $docsGta -Destination $gameSave -Recurse -Force
  Write-Host "Mirrored Documents GTA IV to game\save (fallback)."
}

$manifest = [ordered]@{
  createdUtc     = (Get-Date).ToUniversalTime().ToString("o")
  gameDir        = $GameDir
  zipPath        = (Resolve-Path $ZipPath).Path
  backupDir      = $BackupDir
  replacedFiles  = @($replaced)
  addedFiles     = @($added)
  notes          = @(
    "Restore with: .\scripts\restore-rgless.ps1 -BackupDir `"$BackupDir`"",
    "Documents saves were COPIED not deleted.",
    "Steam 'Verify integrity' is nuclear fallback (re-deploy ASI/DXVK/FusionFix after)."
  )
}
$manifestPath = Join-Path $BackupDir "MANIFEST.json"
$manifest | ConvertTo-Json -Depth 6 | Set-Content -Path $manifestPath -Encoding UTF8

# Pointer to latest backup for easy restore
Set-Content -Path (Join-Path $BackupRoot "LATEST.txt") -Value $BackupDir -Encoding UTF8

Write-Host ""
Write-Host "OK - RGLess installed."
Write-Host "Backup: $BackupDir"
Write-Host "Restore: .\scripts\restore-rgless.ps1"
Write-Host "Start test: .\scripts\restart-gtaiv.ps1 -DirectExe"
Write-Host ""
Write-Host "Note: Original saves in Documents are preserved (copy only)."

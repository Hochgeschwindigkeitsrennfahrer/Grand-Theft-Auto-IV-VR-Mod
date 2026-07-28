# Install newest compatible DXVK-gplasync x32 A/B (Ph42oN weekly 3.0-gplasync).
# Precautions: stock 3.0.2 must already be backed up; easy rollback via restore-stock-dxvk.ps1.
# Async helps SHADER STUTTER only - not stereo flicker. Keep stereo=169 for flicker A/B.
$ErrorActionPreference = "Stop"
$Game = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
$Repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$Gpl = Join-Path $Repo "thirdparty\dxvk-gplasync\artifacts-main\dxvk-gplasync-main\x32\d3d9.dll"
$Fallback271 = Join-Path $Repo "thirdparty\dxvk-gplasync\v2.7.1-1\dxvk-gplasync-v2.7.1-1\x32\d3d9.dll"
$StockGame = Join-Path $Game "d3d9.dll.stock-302"
$StockRepo = Join-Path $Repo "backups\dxvk-stock-302\d3d9.dll.stock-302"
$CacheSrc = Join-Path $Repo "inspo\dxvk cache\GTAIV.dxvk-cache"

if (-not (Test-Path $Game)) { throw "GTAIV folder missing: $Game" }

# Prefer weekly v3.0-gplasync (interop + async); fall back to packaged 2.7.1-1.
$src = $null
$label = ""
if (Test-Path $Gpl) {
  $src = $Gpl
  $label = "v3.0-gplasync (Ph42oN weekly artifact x32)"
} elseif (Test-Path $Fallback271) {
  $src = $Fallback271
  $label = "v2.7.1-1-gplasync (packaged release x32)"
} else {
  throw "No gplasync x32 d3d9.dll under thirdparty/dxvk-gplasync"
}

# Ensure stock backup exists before overwrite.
if (-not (Test-Path $StockGame)) {
  if (Test-Path $StockRepo) {
    Copy-Item -Force $StockRepo $StockGame
  } else {
    $live = Join-Path $Game "d3d9.dll"
    if ((Test-Path $live) -and ((Get-Item $live).Length -gt 7000000)) {
      Copy-Item -Force $live $StockGame
      New-Item -ItemType Directory -Force -Path (Split-Path $StockRepo) | Out-Null
      Copy-Item -Force $live $StockRepo
      Write-Host "Created stock backup from live d3d9.dll"
    } else {
      throw "No stock-302 backup - aborting (run backup first)."
    }
  }
}

Copy-Item -Force $src (Join-Path $Game "d3d9.dll")
Write-Host "Installed $label"
Write-Host "  from: $src"
Write-Host "  SHA256: $((Get-FileHash (Join-Path $Game 'd3d9.dll') -Algorithm SHA256).Hash)"

$conf = @"
# gtaiv-dxvk-vr gplasync A/B - shader stutter only. Not a stereo flicker fix.
# Rollback: scripts\restore-stock-dxvk.ps1
dxvk.enableAsync = true
"@
Set-Content -Path (Join-Path $Game "dxvk.conf") -Value $conf -Encoding ASCII
Write-Host "Wrote dxvk.conf (enableAsync=true)"

$cacheDst = Join-Path $Game "GTAIV.dxvk-cache"
if (Test-Path $CacheSrc) {
  if (Test-Path $cacheDst) {
    $bak = Join-Path $Game ("GTAIV.dxvk-cache.bak-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
    Copy-Item -Force $cacheDst $bak
    Write-Host "Backed up existing cache -> $bak"
  }
  Copy-Item -Force $CacheSrc $cacheDst
  Write-Host "Installed FusionFix cache ($((Get-Item $cacheDst).Length) bytes)"
} else {
  Write-Host "WARN: no inspo cache at $CacheSrc"
}

$stamp = Join-Path $Game "gtaiv_dxvk_vr.dxvk-build"
Set-Content -Path $stamp -Value $label -Encoding ASCII
Write-Host "OK - gplasync A/B ready. If black/crash/DEVICE_LOST: run restore-stock-dxvk.ps1"
Write-Host "Test order: flat window 1 min, then SteamVR Mode 169."

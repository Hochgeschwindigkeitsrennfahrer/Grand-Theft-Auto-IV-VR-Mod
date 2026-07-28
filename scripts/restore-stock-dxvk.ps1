# Restore stock DXVK 3.0.2 d3d9.dll (VR-proven). Undo gplasync A/B.
$ErrorActionPreference = "Stop"
$Game = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
$Repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$StockRepo = Join-Path $Repo "backups\dxvk-stock-302\d3d9.dll.stock-302"
$StockGame = Join-Path $Game "d3d9.dll.stock-302"

$src = $null
if (Test-Path $StockGame) { $src = $StockGame }
elseif (Test-Path $StockRepo) { $src = $StockRepo }
else { throw "No stock backup found at $StockGame or $StockRepo" }

Copy-Item -Force $src (Join-Path $Game "d3d9.dll")
Write-Host "Restored stock d3d9.dll from: $src"
Write-Host "SHA256: $((Get-FileHash (Join-Path $Game 'd3d9.dll') -Algorithm SHA256).Hash)"

$conf = Join-Path $Game "dxvk.conf"
if (Test-Path $conf) {
  Write-Host "Note: dxvk.conf still present (enableAsync ignored by stock). Keep or delete manually."
}
Write-Host "OK - stock DXVK 3.0.2 restored. Launch flat first, then VR."

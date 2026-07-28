# Pack Mode 170 drag-and-drop install (no FusionFix update assets / no Rockstar files).
param(
  [switch] $SkipBuild
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
$OutAsi = Join-Path $Root "out-asi"
$Pre = Join-Path $Root "prebuilt\mode170"
$Drop = Join-Path $Pre "drop-into-GTAIV"
$ZipOut = Join-Path $Root "dist\gtaiv-dxvk-vr-mode170-install.zip"
$BuildId = "20260728-mode170-clean"

if (-not $SkipBuild) {
  & (Join-Path $Root "scripts\build-asi.ps1")
}

$asi = Join-Path $OutAsi "gtaiv_dxvk_vr.asi"
$ovr = Join-Path $OutAsi "openvr_api.dll"
$stockCandidates = @(
  (Join-Path $Root "backups\dxvk-stock-302\d3d9.dll.stock-302"),
  (Join-Path $GameDir "d3d9.dll.stock-302"),
  (Join-Path $Root "dist\gtaiv-dxvk-vr-drop\drop-into-GTAIV\d3d9.dll.stock-302"),
  (Join-Path $Root "dist\gtaiv-dxvk-vr-drop\drop-into-GTAIV\d3d9.dll")
)
$d3d9 = $null
foreach ($c in $stockCandidates) {
  if (Test-Path $c) { $d3d9 = $c; break }
}

foreach ($p in @($asi, $ovr)) {
  if (-not (Test-Path $p)) { throw "Missing required file: $p" }
}
if (-not $d3d9) { throw "Missing stock DXVK 3.0.2 d3d9.dll (place under backups/dxvk-stock-302/)" }

if (Test-Path $Drop) { Remove-Item -LiteralPath $Drop -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Drop | Out-Null

Copy-Item $asi (Join-Path $Drop "gtaiv_dxvk_vr.asi") -Force
Copy-Item $ovr (Join-Path $Drop "openvr_api.dll") -Force
Copy-Item $d3d9 (Join-Path $Drop "d3d9.dll") -Force
Copy-Item $d3d9 (Join-Path $Drop "d3d9.dll.stock-302") -Force

$configs = @{
  "gtaiv_dxvk_vr.stereo"      = "170"
  "gtaiv_dxvk_vr.vres"        = "2048"
  "gtaiv_dxvk_vr.eyefwd"      = "0"
  "gtaiv_dxvk_vr.fovadd"      = "0"
  "gtaiv_dxvk_vr.fpfov"       = "100 100 100"
  "gtaiv_dxvk_vr.camoff"      = "0 0 0"
  "gtaiv_dxvk_vr.vehcamoff"   = "0 -12 0"
  "gtaiv_dxvk_vr.mousesens"   = "50"
  "gtaiv_dxvk_vr.joysens"     = "20 30"
  "gtaiv_dxvk_vr.dualn"       = "2"
  "gtaiv_dxvk_vr.ipd"         = "3"
  "gtaiv_dxvk_vr.pedhide"     = "1"
  "gtaiv_dxvk_vr.stereoscale" = "130"
  "gtaiv_dxvk_vr.hudoff"      = "center 1.0 -0.10 0.10"
  "gtaiv_dxvk_vr.buildid"     = $BuildId
}
foreach ($kv in $configs.GetEnumerator()) {
  Set-Content -LiteralPath (Join-Path $Drop $kv.Key) -Value $kv.Value -Encoding ASCII
}

Copy-Item (Join-Path $Pre "INSTALL.txt") (Join-Path $Drop "gtaiv_dxvk_vr-INSTALL.txt") -Force
Copy-Item (Join-Path $Pre "CONFIG-GUIDE.txt") (Join-Path $Drop "gtaiv_dxvk_vr-CONFIG-GUIDE.txt") -Force
Copy-Item (Join-Path $Root "CREDITS.md") (Join-Path $Pre "CREDITS.md") -Force

@"
Third-party components in this Mode 170 pack
===========================================
- DXVK 3.0.2 (d3d9.dll) - https://github.com/doitsujin/dxvk
- OpenVR (openvr_api.dll) - https://github.com/ValveSoftware/openvr
- gtaiv_dxvk_vr.asi - this project (MIT scaffolding)

NOT included (install yourself):
- FusionFix - https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix
- Any Rockstar game assets

See CREDITS.md in the repo root for full credits and sources.
"@ | Set-Content -LiteralPath (Join-Path $Pre "THIRDPARTY.txt") -Encoding ASCII

New-Item -ItemType Directory -Force -Path (Join-Path $Root "dist") | Out-Null
if (Test-Path $ZipOut) { Remove-Item -LiteralPath $ZipOut -Force }

Write-Host "Compressing Mode 170 drop..."
Compress-Archive -Path (Join-Path $Pre "*") -DestinationPath $ZipOut -CompressionLevel Optimal -Force

$zipLen = (Get-Item $ZipOut).Length
Write-Host ("OK: {0} ({1:N1} MB)" -f $ZipOut, ($zipLen / 1MB))
Write-Host "Mode 170 drop ready under prebuilt\mode170\ and dist zip."

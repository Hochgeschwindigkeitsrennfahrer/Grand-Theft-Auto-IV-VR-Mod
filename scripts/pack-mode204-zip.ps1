# Pack Mode 204 friend zip: VR ASI + stock DXVK + OpenVR + FusionFix + shader cache.
param(
  [switch] $SkipBuild
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
$OutAsi = Join-Path $Root "out-asi"
$Pre = Join-Path $Root "prebuilt\mode204"
$Stage = Join-Path $Root "dist\gtaiv-dxvk-vr-mode204-drop"
$Drop = Join-Path $Stage "drop-into-GTAIV"
$ZipOut = Join-Path $Root "dist\gtaiv-dxvk-vr-mode204-friend.zip"
$BuildId = "20260728-mode204-fusion"

if (-not $SkipBuild) {
  & (Join-Path $Root "scripts\build-asi.ps1")
}

$asi = Join-Path $OutAsi "gtaiv_dxvk_vr.asi"
$ovr = Join-Path $OutAsi "openvr_api.dll"
if (-not (Test-Path $asi)) { $asi = Join-Path $Pre "gtaiv_dxvk_vr.asi" }
if (-not (Test-Path $ovr)) { $ovr = Join-Path $Pre "openvr_api.dll" }

$stockCandidates = @(
  (Join-Path $Root "backups\dxvk-stock-302\d3d9.dll.stock-302"),
  (Join-Path $GameDir "d3d9.dll.stock-302"),
  (Join-Path $Root "prebuilt\mode170\drop-into-GTAIV\d3d9.dll.stock-302"),
  (Join-Path $Root "prebuilt\mode170\drop-into-GTAIV\d3d9.dll")
)
$d3d9 = $null
foreach ($c in $stockCandidates) {
  if (Test-Path $c) { $d3d9 = $c; break }
}

$dinput8 = Join-Path $GameDir "dinput8.dll"
$ceHook = Join-Path $GameDir "aCompleteEditionHook.asi"
$ffAsi = Join-Path $GameDir "plugins\GTAIV.EFLC.FusionFix.asi"
$ffUpdate = Join-Path $GameDir "update\GTAIV.EFLC.FusionFix"
$dxvkCache = Join-Path $GameDir "GTAIV.dxvk-cache"
$inspoCache = Join-Path $Root "inspo\dxvk cache\GTAIV.dxvk-cache"

foreach ($p in @($asi, $ovr, $dinput8, $ffAsi, $ffUpdate)) {
  if (-not (Test-Path $p)) { throw "Missing required file/folder: $p" }
}
if (-not $d3d9) { throw "Missing stock DXVK 3.0.2 d3d9.dll (place under backups/dxvk-stock-302/)" }

if (Test-Path $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Drop | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Drop "plugins") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Drop "update") | Out-Null

Copy-Item (Join-Path $Pre "INSTALL.txt") (Join-Path $Stage "INSTALL.txt") -Force
Copy-Item (Join-Path $Pre "CONFIG-GUIDE.txt") (Join-Path $Stage "CONFIG-GUIDE.txt") -Force
if (Test-Path (Join-Path $Pre "README.txt")) {
  Copy-Item (Join-Path $Pre "README.txt") (Join-Path $Stage "README.txt") -Force
}

# --- Our VR stack (stock DXVK, not live gplasync) ---
Copy-Item $asi (Join-Path $Drop "gtaiv_dxvk_vr.asi") -Force
Copy-Item $ovr (Join-Path $Drop "openvr_api.dll") -Force
Copy-Item $d3d9 (Join-Path $Drop "d3d9.dll") -Force
Copy-Item $d3d9 (Join-Path $Drop "d3d9.dll.stock-302") -Force

# Snapshot into prebuilt for git
Copy-Item $asi (Join-Path $Pre "gtaiv_dxvk_vr.asi") -Force
Copy-Item $ovr (Join-Path $Pre "openvr_api.dll") -Force
Set-Content -LiteralPath (Join-Path $Pre "gtaiv_dxvk_vr.stereo") -Value "204" -Encoding ASCII

# --- FusionFix loader + CE hook ---
Copy-Item $dinput8 (Join-Path $Drop "dinput8.dll") -Force
if (Test-Path $ceHook) {
  Copy-Item $ceHook (Join-Path $Drop "aCompleteEditionHook.asi") -Force
}

# --- FusionFix plugins ---
Copy-Item (Join-Path $GameDir "plugins\GTAIV.EFLC.FusionFix.asi") (Join-Path $Drop "plugins\") -Force
Copy-Item (Join-Path $GameDir "plugins\GTAIV.EFLC.FusionFix.ini") (Join-Path $Drop "plugins\") -Force
Copy-Item (Join-Path $GameDir "plugins\GTAIV.EFLC.FusionFix.cfg") (Join-Path $Drop "plugins\") -Force

# --- FusionFix update assets (~155 MB, includes shader-related content) ---
Copy-Item -LiteralPath $ffUpdate -Destination (Join-Path $Drop "update\GTAIV.EFLC.FusionFix") -Recurse -Force

# --- DXVK shader cache ---
$cacheSrc = $null
if (Test-Path $dxvkCache) { $cacheSrc = $dxvkCache }
elseif (Test-Path $inspoCache) { $cacheSrc = $inspoCache }
if ($cacheSrc) {
  Copy-Item $cacheSrc (Join-Path $Drop "GTAIV.dxvk-cache") -Force
}

# Mild async to reduce first-run stutter (friend-friendly)
@"
# gtaiv-dxvk-vr Mode 204 friend pack
dxvk.enableAsync = true
"@ | Set-Content -LiteralPath (Join-Path $Drop "dxvk.conf") -Encoding ASCII

# Luke Ross-style square aspect (inactive by default — rename to commandline.txt to use)
$squareCmd = @"
-windowed
-width 1440
-height 1440
-norestrictions
"@
Set-Content -LiteralPath (Join-Path $Drop "commandline.txt.vr-square") -Value $squareCmd.TrimEnd() -Encoding ASCII
# Also ship ready-to-use name so copy-all enables square; friend can rename off for widescreen
Set-Content -LiteralPath (Join-Path $Drop "commandline.txt") -Value $squareCmd.TrimEnd() -Encoding ASCII
Copy-Item (Join-Path $Drop "commandline.txt.vr-square") (Join-Path $Pre "commandline.txt.vr-square") -Force

# --- Mode 204 knobs (friend-safe vres; live often used 3884) ---
$configs = @{
  "gtaiv_dxvk_vr.stereo"      = "204"
  "gtaiv_dxvk_vr.vres"        = "2048"
  "gtaiv_dxvk_vr.eyefwd"      = "0"
  "gtaiv_dxvk_vr.fovadd"      = "0"
  "gtaiv_dxvk_vr.fpfov"       = "110 110 110"
  "gtaiv_dxvk_vr.camoff"      = "0 0 0"
  "gtaiv_dxvk_vr.vehcamoff"   = "0 -12 0"
  "gtaiv_dxvk_vr.mousesens"   = "50"
  "gtaiv_dxvk_vr.joysens"     = "20 30"
  "gtaiv_dxvk_vr.dualn"       = "2"
  "gtaiv_dxvk_vr.ipd"         = "2"
  "gtaiv_dxvk_vr.pedhide"     = "0"
  "gtaiv_dxvk_vr.stereoscale" = "130"
  "gtaiv_dxvk_vr.scale"       = "100"
  "gtaiv_dxvk_vr.hudoff"      = "center 1.0 -0.10 0.10"
  "gtaiv_dxvk_vr.buildid"     = $BuildId
}
foreach ($kv in $configs.GetEnumerator()) {
  Set-Content -LiteralPath (Join-Path $Drop $kv.Key) -Value $kv.Value -Encoding ASCII
}

Copy-Item (Join-Path $Stage "INSTALL.txt") (Join-Path $Drop "gtaiv_dxvk_vr-INSTALL.txt") -Force
Copy-Item (Join-Path $Stage "CONFIG-GUIDE.txt") (Join-Path $Drop "gtaiv_dxvk_vr-CONFIG-GUIDE.txt") -Force

@"
Third-party components in this Mode 204 pack
===========================================
- DXVK 3.0.2 (d3d9.dll) - https://github.com/doitsujin/dxvk
- OpenVR (openvr_api.dll) - https://github.com/ValveSoftware/openvr
- FusionFix (dinput8.dll, plugins, update) - https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix
- aCompleteEditionHook.asi - Complete Edition companion (if present)
- gtaiv_dxvk_vr.asi - this project

Do not redistribute Rockstar game assets. Respect FusionFix / DXVK / OpenVR licenses.
"@ | Set-Content -LiteralPath (Join-Path $Stage "THIRDPARTY.txt") -Encoding ASCII

New-Item -ItemType Directory -Force -Path (Join-Path $Root "dist") | Out-Null
if (Test-Path $ZipOut) { Remove-Item -LiteralPath $ZipOut -Force }

Write-Host "Compressing Mode 204 friend pack (FusionFix update is large)..."
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipOut -CompressionLevel Optimal -Force

$zipLen = (Get-Item $ZipOut).Length
Write-Host ("OK: {0} ({1:N1} MB)" -f $ZipOut, ($zipLen / 1MB))
Write-Host "Includes: Mode204 ASI + stock DXVK 3.0.2 + OpenVR + FusionFix + dxvk-cache"
Write-Host "Friend: unzip, read INSTALL.txt, copy drop-into-GTAIV\* into GTAIV folder"

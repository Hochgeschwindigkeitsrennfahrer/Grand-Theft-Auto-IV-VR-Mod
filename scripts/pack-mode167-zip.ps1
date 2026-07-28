# Pack Mode 167 drag-and-drop install zip (car sit-cam + HUD).
param(
  [switch] $SkipBuild
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
$OutAsi = Join-Path $Root "out-asi"
$Stage = Join-Path $Root "dist\gtaiv-dxvk-vr-drop"
$Drop = Join-Path $Stage "drop-into-GTAIV"
$ZipOut = Join-Path $Root "dist\gtaiv-dxvk-vr-mode167-install.zip"

if (-not $SkipBuild) {
  & (Join-Path $Root "scripts\build-asi.ps1")
}

$asi = Join-Path $OutAsi "gtaiv_dxvk_vr.asi"
$ovr = Join-Path $OutAsi "openvr_api.dll"
$d3d9 = Join-Path $GameDir "d3d9.dll"
$dinput8 = Join-Path $GameDir "dinput8.dll"
$ceHook = Join-Path $GameDir "aCompleteEditionHook.asi"
$ffAsi = Join-Path $GameDir "plugins\GTAIV.EFLC.FusionFix.asi"
$ffUpdate = Join-Path $GameDir "update\GTAIV.EFLC.FusionFix"

foreach ($p in @($asi, $ovr, $d3d9, $dinput8, $ffAsi, $ffUpdate)) {
  if (-not (Test-Path $p)) { throw "Missing required file/folder: $p" }
}

if (Test-Path $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Drop | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Drop "plugins") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Drop "update") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Drop "common\data") | Out-Null

Copy-Item (Join-Path $Root "dist\install-drop\INSTALL.txt") (Join-Path $Stage "INSTALL.txt") -Force
if (Test-Path (Join-Path $Root "dist\install-drop\CONFIG-GUIDE.txt")) {
  Copy-Item (Join-Path $Root "dist\install-drop\CONFIG-GUIDE.txt") (Join-Path $Stage "CONFIG-GUIDE.txt") -Force
}

Copy-Item $asi (Join-Path $Drop "gtaiv_dxvk_vr.asi") -Force
Copy-Item $ovr (Join-Path $Drop "openvr_api.dll") -Force
Copy-Item $d3d9 (Join-Path $Drop "d3d9.dll") -Force
Copy-Item $dinput8 (Join-Path $Drop "dinput8.dll") -Force
if (Test-Path $ceHook) {
  Copy-Item $ceHook (Join-Path $Drop "aCompleteEditionHook.asi") -Force
}

Copy-Item (Join-Path $GameDir "plugins\GTAIV.EFLC.FusionFix.asi") (Join-Path $Drop "plugins\") -Force
Copy-Item (Join-Path $GameDir "plugins\GTAIV.EFLC.FusionFix.ini") (Join-Path $Drop "plugins\") -Force
Copy-Item (Join-Path $GameDir "plugins\GTAIV.EFLC.FusionFix.cfg") (Join-Path $Drop "plugins\") -Force
Copy-Item -LiteralPath $ffUpdate -Destination (Join-Path $Drop "update\GTAIV.EFLC.FusionFix") -Recurse -Force

# Square VR commandline helper (user renames to commandline.txt)
$sq = Join-Path $GameDir "commandline.txt.vr-square"
if (Test-Path $sq) {
  Copy-Item $sq (Join-Path $Drop "commandline.txt.vr-square") -Force
}

# Stock hud bak so Mode167 can restore / re-inset cleanly
$hudBak = Join-Path $GameDir "common\data\hud.dat.gtaiv-dxvk-vr.bak"
$hud = Join-Path $GameDir "common\data\hud.dat"
if (Test-Path $hudBak) {
  Copy-Item $hudBak (Join-Path $Drop "common\data\hud.dat") -Force
  Copy-Item $hudBak (Join-Path $Drop "common\data\hud.dat.gtaiv-dxvk-vr.bak") -Force
} elseif (Test-Path $hud) {
  Copy-Item $hud (Join-Path $Drop "common\data\hud.dat") -Force
}

$configs = @{
  "gtaiv_dxvk_vr.stereo"      = "167"
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
  "gtaiv_dxvk_vr.buildid"     = "20260728-mode167-hudtune"
}
foreach ($kv in $configs.GetEnumerator()) {
  Set-Content -LiteralPath (Join-Path $Drop $kv.Key) -Value $kv.Value -Encoding ASCII
}

Copy-Item (Join-Path $Stage "INSTALL.txt") (Join-Path $Drop "gtaiv_dxvk_vr-INSTALL.txt") -Force
if (Test-Path (Join-Path $Stage "CONFIG-GUIDE.txt")) {
  Copy-Item (Join-Path $Stage "CONFIG-GUIDE.txt") (Join-Path $Drop "gtaiv_dxvk_vr-CONFIG-GUIDE.txt") -Force
}

@"
Third-party components in this pack
===================================
- DXVK 3.0.2 (d3d9.dll) - https://github.com/doitsujin/dxvk
- OpenVR (openvr_api.dll) - Valve
- FusionFix (dinput8.dll, plugins, update) - https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix
- aCompleteEditionHook.asi - Complete Edition companion (if present)

Do not redistribute Rockstar game assets. FusionFix is third-party; respect its license.
"@ | Set-Content -LiteralPath (Join-Path $Stage "THIRDPARTY.txt") -Encoding ASCII

if (Test-Path $ZipOut) { Remove-Item -LiteralPath $ZipOut -Force }

Write-Host "Compressing (FusionFix update assets are large)..."
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipOut -CompressionLevel Optimal -Force

$zipLen = (Get-Item $ZipOut).Length
Write-Host ("OK: {0} ({1:N1} MB)" -f $ZipOut, ($zipLen / 1MB))
Write-Host "Mode 167 = sit-cam + HUD inset (mission directions). User: unzip, read INSTALL.txt, copy drop-into-GTAIV\* into GTAIV"

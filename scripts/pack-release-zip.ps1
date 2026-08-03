# Pack a GitHub Release zip: everything needed to run gtaiv-dxvk-vr.
# Uses the playable Mode 909 ASI from prebuilt/mode909 (or live game dir).
param(
  [string] $BuildId = "20260803-909-hud4",
  [string] $Tag = ""
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
$Pre = Join-Path $Root "prebuilt\mode909"
$Stage = Join-Path $Root "dist\gtaiv-dxvk-vr-release"
$Drop = Join-Path $Stage "drop-into-GTAIV"
$ZipOut = Join-Path $Root "dist\gtaiv-dxvk-vr-release.zip"

function Pick([string[]]$paths) {
  foreach ($p in $paths) {
    if ($p -and (Test-Path -LiteralPath $p)) { return $p }
  }
  return $null
}

$asi = Pick @((Join-Path $Pre "gtaiv_dxvk_vr.asi"), (Join-Path $GameDir "gtaiv_dxvk_vr.asi"), (Join-Path $Root "out-asi\gtaiv_dxvk_vr.asi"))
$ovr = Pick @((Join-Path $Pre "openvr_api.dll"), (Join-Path $GameDir "openvr_api.dll"), (Join-Path $Root "out-asi\openvr_api.dll"))
$d3d9 = Pick @((Join-Path $GameDir "d3d9.dll.stock-302"), (Join-Path $GameDir "d3d9.dll"))
$dinput8 = Join-Path $GameDir "dinput8.dll"
$ffAsi = Join-Path $GameDir "plugins\GTAIV.EFLC.FusionFix.asi"
$ffUpdate = Join-Path $GameDir "update\GTAIV.EFLC.FusionFix"
$hud = Pick @((Join-Path $Pre "hud.dat"), (Join-Path $GameDir "common\data\hud.dat"))

foreach ($p in @($asi, $ovr, $d3d9, $dinput8, $ffAsi, $hud)) {
  if (-not $p -or -not (Test-Path $p)) { throw "Missing required file: $p" }
}

if (Test-Path $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Drop | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Drop "plugins") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Drop "update") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Drop "common\data") | Out-Null

# Docs
Copy-Item (Join-Path $Root "dist\install-drop\INSTALL.txt") (Join-Path $Stage "INSTALL.txt") -Force
Copy-Item (Join-Path $Root "dist\install-drop\CONFIG-GUIDE.txt") (Join-Path $Stage "CONFIG-GUIDE.txt") -Force -EA SilentlyContinue
Copy-Item (Join-Path $Root "dist\install-drop\QUICK-START.bat") (Join-Path $Stage "QUICK-START.bat") -Force
Copy-Item (Join-Path $Root "dist\install-drop\FULL-RESTART.bat") (Join-Path $Stage "FULL-RESTART.bat") -Force

# VR stack
Copy-Item $asi (Join-Path $Drop "gtaiv_dxvk_vr.asi") -Force
Copy-Item $ovr (Join-Path $Drop "openvr_api.dll") -Force
Copy-Item $d3d9 (Join-Path $Drop "d3d9.dll") -Force
Copy-Item $d3d9 (Join-Path $Drop "d3d9.dll.stock-302") -Force -EA SilentlyContinue

# FusionFix
Copy-Item $dinput8 (Join-Path $Drop "dinput8.dll") -Force
$ceHook = Join-Path $GameDir "aCompleteEditionHook.asi"
if (Test-Path $ceHook) { Copy-Item $ceHook (Join-Path $Drop "aCompleteEditionHook.asi") -Force }
Copy-Item $ffAsi (Join-Path $Drop "plugins\") -Force
foreach ($n in @("GTAIV.EFLC.FusionFix.ini", "GTAIV.EFLC.FusionFix.cfg")) {
  $p = Join-Path $GameDir "plugins\$n"
  if (Test-Path $p) { Copy-Item $p (Join-Path $Drop "plugins\") -Force }
}
if (Test-Path $ffUpdate) {
  Copy-Item -LiteralPath $ffUpdate -Destination (Join-Path $Drop "update\GTAIV.EFLC.FusionFix") -Recurse -Force
}

# HUD layout (stacked money/stars/ammo/weapon + radar inset)
Copy-Item $hud (Join-Path $Drop "common\data\hud.dat") -Force
$hudBak = Join-Path $GameDir "common\data\hud.dat.gtaiv-dxvk-vr.bak"
if (Test-Path $hudBak) {
  Copy-Item $hudBak (Join-Path $Drop "common\data\hud.dat.gtaiv-dxvk-vr.bak") -Force
}

# Sidecars
$configs = @{
  "gtaiv_dxvk_vr.stereo"      = "909"
  "gtaiv_dxvk_vr.vres"        = "2048"
  "gtaiv_dxvk_vr.eyefwd"      = "0"
  "gtaiv_dxvk_vr.fovadd"      = "0"
  "gtaiv_dxvk_vr.fpfov"       = "110 110 110"
  "gtaiv_dxvk_vr.camoff"      = "0 0 0"
  "gtaiv_dxvk_vr.mousesens"   = "50"
  "gtaiv_dxvk_vr.joysens"     = "20 30"
  "gtaiv_dxvk_vr.dualn"       = "2"
  "gtaiv_dxvk_vr.ipd"         = "3"
  "gtaiv_dxvk_vr.pedhide"     = "1"
  "gtaiv_dxvk_vr.stereoscale" = "115"
  "gtaiv_dxvk_vr.buildid"     = $BuildId
  "gtaiv_dxvk_vr.menu"        = "1"
}
foreach ($kv in $configs.GetEnumerator()) {
  Set-Content -LiteralPath (Join-Path $Drop $kv.Key) -Value $kv.Value -Encoding ASCII
}

# Bats into drop folder too
Copy-Item (Join-Path $Root "dist\install-drop\QUICK-START.bat") (Join-Path $Drop "QUICK-START.bat") -Force
Copy-Item (Join-Path $Root "dist\install-drop\FULL-RESTART.bat") (Join-Path $Drop "FULL-RESTART.bat") -Force
Copy-Item (Join-Path $Root "dist\install-drop\FULL-RESTART.bat") (Join-Path $Drop "gtaiv_dxvk_vr-FULL-RESTART.bat") -Force

# Square commandline template
@"
-windowed
-width 1440
-height 1440
-norestrictions
"@ | Set-Content -LiteralPath (Join-Path $Drop "commandline.txt.vr-square") -Encoding ASCII

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
Write-Host "Compressing..."
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipOut -CompressionLevel Optimal -Force
$zipLen = (Get-Item $ZipOut).Length
Write-Host ("OK: {0} ({1:N1} MB)" -f $ZipOut, ($zipLen / 1MB))
Write-Host "Tag hint: $Tag  BuildId: $BuildId"

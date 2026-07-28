# Restore VR ASI after FirstPerson A/B test (removes FP + ScriptHook CE pack).
# Does NOT delete FusionFix / d3d9 / openvr.
$ErrorActionPreference = "Stop"
$gdir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"

$off = Join-Path $gdir "gtaiv_dxvk_vr.asi.off"
$on = Join-Path $gdir "gtaiv_dxvk_vr.asi"
if (Test-Path $off) {
  if (Test-Path $on) { Remove-Item $on -Force }
  Rename-Item $off "gtaiv_dxvk_vr.asi"
  Write-Host "Restored gtaiv_dxvk_vr.asi"
} elseif (Test-Path $on) {
  Write-Host "VR ASI already active"
} else {
  Write-Host "WARNING: no gtaiv_dxvk_vr.asi or .asi.off found"
}

foreach ($f in @(
  "FirstPerson.asi",
  "FirstPerson.ini",
  "ScriptHook.dll",
  "aCompleteEditionHook.asi",
  "ScriptHookDotNet.asi",
  "ScriptHook.log",
  "ScriptHookDotNet.log"
)) {
  $p = Join-Path $gdir $f
  if (Test-Path $p) {
    Remove-Item $p -Force
    Write-Host "Removed $f"
  }
}

Set-Content (Join-Path $gdir "gtaiv_dxvk_vr.stereo") "120" -Encoding ASCII -NoNewline
Set-Content (Join-Path $gdir "gtaiv_dxvk_vr.dualn") "2" -Encoding ASCII -NoNewline
Set-Content (Join-Path $gdir "gtaiv_dxvk_vr.pedhide") "0" -Encoding ASCII -NoNewline
Write-Host "stereo=120 dualn=2 pedhide=0"
Write-Host "Done. Use GTA IV Quick Restart."

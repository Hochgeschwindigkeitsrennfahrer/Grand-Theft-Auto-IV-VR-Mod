# Add a "VR Render Mode" row to GTA IV's own pause menu (Controls page).
#
# See docs/menu-integration/NATIVE_MENU.md. This is the cheap half of
# docs/MENU_INTEGRATION.md section 3: the row itself is pure data, because
# FusionFix already ships a frontend_menus.xml in the update folder and its text
# hook makes label="..." accept a literal string instead of a GXT key.
#
# It patches the game's installed FusionFix XML, NOT anything in this repo — that
# file is Rockstar-derived and must not be committed (AGENTS.md rule 3).
#
# The row borrows PREF_LEDILLUMINATION (FusionFix's LightSyncRGB). Nothing new is
# appended to the preference table, so FusionFix's own rows cannot be disturbed.
# LightSyncRGB does nothing without Logitech hardware, which is why it is the
# pick docs/MENU_INTEGRATION.md recommends.
#
#   .\scripts\install-vr-menu-row.ps1
#   .\scripts\install-vr-menu-row.ps1 -Uninstall
#
# Rollback without this script: copy the .gtaiv-dxvk-vr.bak back over the xml.

param(
  [string] $GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV",
  [switch] $Uninstall
)

$ErrorActionPreference = "Stop"

$xml = Join-Path $GameDir "update\common\data\frontend_menus.xml"
$bak = "$xml.gtaiv-dxvk-vr.bak"
if (-not (Test-Path $xml)) { throw "not found: $xml  (is FusionFix installed?)" }

if (Get-Process -Name "GTAIV", "PlayGTAIV" -ErrorAction SilentlyContinue) {
  throw "GTA IV is running - close it first (the menu XML is read at startup)"
}

# The ASI follows the row by reading FusionFix's cfg; this file names the key and
# is what switches that path on. No file = the ASI ignores the pause menu.
$keyFile = Join-Path $GameDir "gtaiv_dxvk_vr.menukey"
$cfgKey = "LightSyncRGB"

if ($Uninstall) {
  if (Test-Path $keyFile) { Remove-Item $keyFile -Force; Write-Host "removed gtaiv_dxvk_vr.menukey (ASI stops following the row)" }
  if (-not (Test-Path $bak)) { throw "no backup at $bak - nothing to restore" }
  Copy-Item $bak $xml -Force
  Write-Host "restored the original frontend_menus.xml"
  exit 0
}

# Work on raw bytes: the anchors and the inserted text are pure ASCII, so a byte
# splice cannot disturb whatever encoding or BOM the file actually uses.
$bytes = [System.IO.File]::ReadAllBytes($xml)
$text = [System.Text.Encoding]::ASCII.GetString($bytes)

if ($text.Contains('label="VR Render Mode"')) {
  Set-Content -Path $keyFile -Value $cfgKey -Encoding Ascii -NoNewline
  Write-Host "row already present; refreshed gtaiv_dxvk_vr.menukey = $cfgKey"
  exit 0
}

$menuAnchor = '<menu HeaderText="MH_CON" enum="SCR_CONTROLS">'
$menuAt = $text.IndexOf($menuAnchor)
if ($menuAt -lt 0) { throw "could not find the SCR_CONTROLS menu block in $xml" }

# End of that menu = the first END_OF_MENU_OPTIONS after it. Insert just above.
$endAnchor = '<options action="END_OF_MENU_OPTIONS"'
$endAt = $text.IndexOf($endAnchor, $menuAt)
if ($endAt -lt 0) { throw "could not find the end of the SCR_CONTROLS menu block" }
$closeAt = $text.IndexOf('<', $menuAt + $menuAnchor.Length)
if ($endAt -gt $text.IndexOf('</menu>', $menuAt)) { throw "SCR_CONTROLS block looks malformed - aborting" }

if (-not (Test-Path $bak)) {
  [System.IO.File]::WriteAllBytes($bak, $bytes)
  Write-Host "backed up -> $(Split-Path $bak -Leaf)"
}

$insert = @"
<optionspc action="MENUOPT_NONE" label="" value="PREF_NULL" scaler="0" displayValue="MENU_DISPLAY_NONE" />
<optionspc action="MENUOPT_ADJUST" label="VR Render Mode" value="PREF_LEDILLUMINATION" scaler="2" displayValue="MENU_DISPLAY_ON_OFF" />

"@ -replace "`r`n", "`n"

$insertBytes = [System.Text.Encoding]::ASCII.GetBytes($insert)
$out = New-Object byte[] ($bytes.Length + $insertBytes.Length)
[Array]::Copy($bytes, 0, $out, 0, $endAt)
[Array]::Copy($insertBytes, 0, $out, $endAt, $insertBytes.Length)
[Array]::Copy($bytes, $endAt, $out, $endAt + $insertBytes.Length, $bytes.Length - $endAt)
[System.IO.File]::WriteAllBytes($xml, $out)

Set-Content -Path $keyFile -Value $cfgKey -Encoding Ascii -NoNewline

Write-Host "added 'VR Render Mode' to the Controls page (PREF_LEDILLUMINATION)"
Write-Host "old size $($bytes.Length) -> new size $($out.Length)"
Write-Host "wrote gtaiv_dxvk_vr.menukey = $cfgKey"
Write-Host ""
Write-Host "Restart the game. Pause > Controls > VR Render Mode now switches"
Write-Host "between the profiles numbered 0 and 1 in gtaiv_dxvk_vr.menumap."

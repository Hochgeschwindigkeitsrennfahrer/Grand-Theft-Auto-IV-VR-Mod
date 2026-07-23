# DXVK-Submodule initialisieren

# Bevorzugt: IronWolf tiw-rel (VR-Mailbox; wir nutzen OpenVR).
# Alternative später: vr-dx9-rel oder sd805/dxvk (L4D2VR+async).
# Nicht: IronWolf master (keine d3d9_vr.h).

Write-Host "Initializing dxvk submodule (TheIronWolfModding/dxvk @ tiw-rel-241-260612)..."
git submodule sync --recursive
git submodule update --init --recursive

if (-not (Test-Path "dxvk\.git") -and -not (Test-Path "dxvk\src")) {
  Write-Host "Submodule empty — cloning explicitly..."
  git submodule add -b tiw-rel-241-260612 https://github.com/TheIronWolfModding/dxvk.git dxvk 2>$null
  git submodule update --init --recursive
}

if (Test-Path "dxvk\src") {
  Write-Host "OK: dxvk\src vorhanden."
} else {
  Write-Host "FEHLER: dxvk scheint leer. Ausgabe/Netzwerk pruefen und Agenten Log pasten."
  exit 1
}

Write-Host ""
Write-Host "Weiter: docs/HOME_CURSOR.md Abschnitt 5 (x86-Build)."
Write-Host "Referenz L4D2VR: https://github.com/sd805/l4d2vr"
Write-Host "SteamVR muss fuer OpenVR/Reverb G2 laufen."

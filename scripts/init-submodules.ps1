# Initialize DXVK submodule

# Preferred: IronWolf tiw-rel (VR mailbox; we use OpenVR).
# Alternative later: vr-dx9-rel or sd805/dxvk (L4D2VR+async).
# Not: IronWolf master (no d3d9_vr.h).

Write-Host "Initializing dxvk submodule (TheIronWolfModding/dxvk @ tiw-rel-241-260612)..."
git submodule sync --recursive
git submodule update --init --recursive

if (-not (Test-Path "dxvk\.git") -and -not (Test-Path "dxvk\src")) {
  Write-Host "Submodule empty - cloning explicitly..."
  git submodule add -b tiw-rel-241-260612 https://github.com/TheIronWolfModding/dxvk.git dxvk 2>$null
  git submodule update --init --recursive
}

if (Test-Path "dxvk\src") {
  Write-Host "OK: dxvk\src present."
} else {
  Write-Host "ERROR: dxvk appears empty. Check output/network and paste agent log."
  exit 1
}

Write-Host ""
Write-Host "Next: docs/HOME_CURSOR.md section 5 (x86 build)."
Write-Host "L4D2VR reference: https://github.com/sd805/l4d2vr"
Write-Host "SteamVR must be running for OpenVR/Reverb G2."

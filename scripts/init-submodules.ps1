# Init DXVK submodule (run at home — needs network)
# Starting point: sd805/dxvk (used by L4D2VR). May later retarget openRBRVR fork.

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot\..

Write-Host "Initializing dxvk submodule (TheIronWolfModding/dxvk @ vr-dx9-rel)..."
git submodule sync --recursive
git submodule update --init --recursive

if (-not (Test-Path "dxvk\.git") -and -not (Test-Path "dxvk\src")) {
  Write-Host "Submodule empty — cloning explicitly..."
  git submodule add -b vr-dx9-rel https://github.com/TheIronWolfModding/dxvk.git dxvk 2>$null
  git submodule update --init --recursive
}

Write-Host "Done. Read docs/IRONWOLF_DXVK.md then docs/ARCHITECTURE.md."
Write-Host "L4D2VR (hooks reference): https://github.com/sd805/l4d2vr"
Write-Host "Optional later: openRBRVR DXVK for OpenXR-heavy 32-bit path."

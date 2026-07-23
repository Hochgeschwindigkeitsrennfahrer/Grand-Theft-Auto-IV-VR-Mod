# Init DXVK submodule (run at home — needs network)
# Starting point: sd805/dxvk (used by L4D2VR). May later retarget openRBRVR fork.

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot\..

Write-Host "Initializing dxvk submodule (sd805/dxvk, dxvk-async)..."
git submodule sync --recursive
git submodule update --init --recursive

if (-not (Test-Path "dxvk\.git") -and -not (Test-Path "dxvk\src")) {
  Write-Host "Submodule empty — cloning explicitly..."
  git submodule add -b dxvk-async https://github.com/sd805/dxvk.git dxvk 2>$null
  git submodule update --init --recursive
}

Write-Host "Done. Next: read docs/ARCHITECTURE.md and L4D2VR build notes (x86)."
Write-Host "Reference build flow: https://github.com/sd805/l4d2vr#build-instructions"

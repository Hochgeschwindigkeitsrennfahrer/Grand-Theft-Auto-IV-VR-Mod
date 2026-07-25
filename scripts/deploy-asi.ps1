param(
  [Parameter(Mandatory = $true)]
  [string] $GameDir,

  # After successful copy: stop any running GTA (if needed) was caller's job;
  # -Launch starts the game via restart-gtaiv.ps1.
  [switch] $Launch,
  [switch] $DirectExe
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$asi = Join-Path $Root "out-asi\gtaiv_dxvk_vr.asi"
$ovr = Join-Path $Root "out-asi\openvr_api.dll"
if (-not (Test-Path $asi)) { throw "Build missing - run .\scripts\build-asi.ps1" }
if (-not (Test-Path $GameDir)) { throw "GameDir not found: $GameDir" }

$destAsi = Join-Path $GameDir "gtaiv_dxvk_vr.asi"
try {
  Copy-Item -LiteralPath $asi -Destination $destAsi -Force
} catch {
  Write-Host "ASI locked - stopping GTAIV and retrying..."
  & "$PSScriptRoot\restart-gtaiv.ps1" -NoStart -NoPause
  Start-Sleep -Seconds 1
  Copy-Item -LiteralPath $asi -Destination $destAsi -Force
}

if (Test-Path $ovr) {
  Copy-Item -LiteralPath $ovr -Destination (Join-Path $GameDir "openvr_api.dll") -Force
  Write-Host "Deployed openvr_api.dll"
}
Write-Host "Deployed gtaiv_dxvk_vr.asi -> $GameDir"
Write-Host "Log: $(Join-Path $GameDir 'gtaiv_dxvk_vr.log')"

if ($Launch) {
  Write-Host "Launching GTA IV (DirectExe default — no Steam Cancel+Play)..."
  & "$PSScriptRoot\restart-gtaiv.ps1" -NoPause
} else {
  Write-Host "Checklist: SteamVR running, DXVK 3.0.2 d3d9.dll - or use -Launch / build-deploy-run.ps1"
}

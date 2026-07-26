# Automated open/close log proof between builds (agent/nap sessions).
# Usage: .\scripts\auto-log-proof.ps1 [-WaitSec 70] [-Mode 88]
param(
  [int] $Mode = 88,
  [int] $WaitSec = 70,
  [string] $GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
)
$ErrorActionPreference = "Continue"
$log = Join-Path $GameDir "gtaiv_dxvk_vr.log"
$stereo = Join-Path $GameDir "gtaiv_dxvk_vr.stereo"
$buildId = Join-Path $GameDir "gtaiv_dxvk_vr.buildid"
Write-Host "=== auto-log-proof mode=$Mode wait=${WaitSec}s ==="
if (Test-Path $buildId) { Write-Host ("buildid file: " + (Get-Content $buildId -Raw)) }
if (Test-Path $stereo) { Write-Host ("stereo file: " + (Get-Content $stereo -Raw)) }
$deadline = (Get-Date).AddSeconds($WaitSec)
$ok = $false
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 3
  if (-not (Test-Path $log)) { continue }
  $t = Get-Content $log -Raw -ErrorAction SilentlyContinue
  if ($null -eq $t) { continue }
  if ($t -match "StereoMode: $Mode" -and $t -match "Mode${Mode}:" -and
      ($t -match "FOVPROOF" -or $t -match "DRAWSCENE-ONLY") -and
      ($t -match "AppFPS")) {
    $ok = $true
    break
  }
}
Write-Host "PROOF_OK=$ok"
Select-String -Path $log -Pattern "ASI_BUILD|StereoMode:|Mode${Mode}:|DualN:|EyeRt:|DeviceReset|FOVPROOF|DRAWSCENE-ONLY|AppFPS|hold=|StereoDiff|StretchRect:|fill" |
  Select-Object -First 35 | ForEach-Object { $_.Line }
Write-Host "=== last AppFPS / dual ==="
Select-String -Path $log -Pattern "AppFPS|DRAWSCENE-ONLY|StereoDiff|hold=" |
  Select-Object -Last 12 | ForEach-Object { $_.Line }
if (-not $ok) { exit 2 }
exit 0

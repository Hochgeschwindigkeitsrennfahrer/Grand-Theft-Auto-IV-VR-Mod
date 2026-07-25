# Smoke-test one stereo mode: set file, restart, wait for banner+ES, scrape, kill.
param(
  [Parameter(Mandatory = $true)][int] $Mode,
  [int] $WaitSec = 55,
  [string] $Tag = "smoke",
  [string] $GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
)
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$log = Join-Path $GameDir "gtaiv_dxvk_vr.log"
$stereo = Join-Path $GameDir "gtaiv_dxvk_vr.stereo"
Set-Content -LiteralPath $stereo -Value "$Mode" -NoNewline -Encoding Ascii
$env:ASI_BUILD_TAG = "$Tag-m$Mode"
& "$Root\scripts\build-deploy-run.ps1" -SkipBuild | Out-Host
$deadline = (Get-Date).AddSeconds($WaitSec)
$ok = $false
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 2
  if (-not (Test-Path $log)) { continue }
  $t = Get-Content $log -Raw -ErrorAction SilentlyContinue
  if ($t -match "mode $Mode family" -or $t -match "StereoMode: $Mode" -or $t -match "Mode${Mode}:") {
    if ($t -match "es#\d{2,}" -or $t -match "DRAWSCENE-ONLY" -or $t -match "AER QUALITY" -or $t -match "FOVPROOF" -or $t -match "SHADER-CONST" -or $t -match "SAFER SPARSE" -or $t -match "HITCHCUT") {
      $ok = $true
      break
    }
  }
}
Write-Host "=== SMOKE mode=$Mode ok=$ok ==="
Select-String -Path $log -Pattern "ASI_BUILD|StereoMode:|Mode74: HITCHCUT|Mode75:|Mode76:|Mode77:|Mode78:|Mode79:|EXCEPTION|wrote stereo|StereoDiff|FOVPROOF|DRAWSCENE|AER QUALITY|SHADER-CONST|SAFER|AppFPS es/s" |
  Select-Object -First 25 | ForEach-Object { $_.Line }
Write-Host "=== last Mode74 es / MonoSubmit ==="
Select-String -Path $log -Pattern "Mode74: es#|Mode76AER:|Mode77:|Mode79: FOVPROOF|MonoSubmit #" |
  Select-Object -Last 8 | ForEach-Object { $_.Line }
& "$Root\scripts\restart-gtaiv.ps1" -NoStart -NoPause | Out-Null
if (-not $ok) { exit 2 }
exit 0

# Build ASI → kill GTA if running → deploy → start GTA IV (Steam).
# Default GameDir matches CURRENT-STATE. Use after stereo/cam test builds.
param(
  [string] $GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV",
  [switch] $DirectExe,   # pass through to restart-gtaiv.ps1
  [switch] $SkipBuild,   # only deploy + run (ASI already built)
  [switch] $NoStart      # deploy only
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

if (-not $SkipBuild) {
  Write-Host "=== build-asi ==="
  & "$PSScriptRoot\build-asi.ps1"
  if ($LASTEXITCODE -ne 0) { throw "build-asi failed: $LASTEXITCODE" }
}

Write-Host "=== stop GTA (so ASI can be overwritten) ==="
& "$PSScriptRoot\restart-gtaiv.ps1" -NoStart -NoPause
Start-Sleep -Seconds 1

Write-Host "=== deploy-asi ==="
& "$PSScriptRoot\deploy-asi.ps1" -GameDir $GameDir
if ($LASTEXITCODE -ne 0) { throw "deploy-asi failed: $LASTEXITCODE" }

if ($NoStart) {
  Write-Host "Deploy done (-NoStart)."
  exit 0
}

Write-Host "=== start GTA IV ==="
$restartArgs = @{ NoPause = $true }
if ($DirectExe) { $restartArgs.DirectExe = $true }
& "$PSScriptRoot\restart-gtaiv.ps1" @restartArgs

# Verify the new process actually loaded our ASI (mode line in a fresh log tail).
$log = Join-Path $GameDir "gtaiv_dxvk_vr.log"
Write-Host "Waiting for log proof (StereoMode / MonoSubmit)..."
$ok = $false
for ($i = 0; $i -lt 90; $i++) {
  Start-Sleep -Seconds 1
  if (-not (Test-Path -LiteralPath $log)) { continue }
  $tail = Get-Content -LiteralPath $log -Tail 30 -ErrorAction SilentlyContinue
  if ($tail -match 'StereoMode:|StereoRender:|MonoSubmit #') {
    $tail | Select-String -Pattern 'StereoMode:|StereoRender:|MonoSubmit #' | Select-Object -Last 5
    $ok = $true
    break
  }
}
if (-not $ok) {
  Write-Host "WARN: no ASI log yet — Steam may still be on LAUNCHING; click Play if needed."
}

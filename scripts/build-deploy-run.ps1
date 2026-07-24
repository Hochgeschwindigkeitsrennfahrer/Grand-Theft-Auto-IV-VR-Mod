# Build ASI -> verify artifact -> kill GTA -> deploy -> start -> wait for NEW ASI log.
param(
  [string] $GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV",
  [switch] $DirectExe,
  [switch] $SkipBuild,
  [switch] $NoStart
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

$outAsi = Join-Path $Root "out-asi\gtaiv_dxvk_vr.asi"
$destAsi = Join-Path $GameDir "gtaiv_dxvk_vr.asi"
$log = Join-Path $GameDir "gtaiv_dxvk_vr.log"
$buildIdPath = Join-Path $GameDir "gtaiv_dxvk_vr.buildid"

function Wait-FileUnlocked([string]$Path, [int]$Seconds = 30) {
  for ($i = 0; $i -lt $Seconds; $i++) {
    try {
      $fs = [System.IO.File]::Open($Path, 'OpenOrCreate', 'ReadWrite', 'None')
      $fs.Close()
      return $true
    } catch {
      Start-Sleep -Seconds 1
    }
  }
  return $false
}

if (-not $SkipBuild) {
  Write-Host "=== build-asi (blocking until cl finishes) ==="
  & "$PSScriptRoot\build-asi.ps1"
  if ($LASTEXITCODE -ne 0) { throw "build-asi failed: $LASTEXITCODE" }
}

if (-not (Test-Path -LiteralPath $outAsi)) { throw "Missing build output: $outAsi" }
$built = Get-Item -LiteralPath $outAsi
if ($built.Length -lt 50KB) { throw "ASI too small ($($built.Length) bytes) - build incomplete?" }
Write-Host ("Build OK: {0} bytes, time={1:HH:mm:ss}" -f $built.Length, $built.LastWriteTime)

Write-Host "=== stop GTA (must finish before deploy) ==="
& "$PSScriptRoot\restart-gtaiv.ps1" -NoStart -NoPause
Start-Sleep -Seconds 2
# Extra: ensure GTAIV.exe really gone
for ($i = 0; $i -lt 20; $i++) {
  $p = Get-Process -Name "GTAIV" -ErrorAction SilentlyContinue
  if (-not $p) { break }
  Write-Host "GTAIV still alive - force kill..."
  $p | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 1
}
if (-not (Wait-FileUnlocked $destAsi 45)) {
  Write-Host "WARN: dest ASI still locked; deploy may fail"
}

$buildId = Get-Date -Format "yyyyMMdd-HHmmss"
Set-Content -LiteralPath $buildIdPath -Value $buildId -NoNewline -Encoding Ascii
Write-Host "=== deploy-asi (buildId=$buildId) ==="
& "$PSScriptRoot\deploy-asi.ps1" -GameDir $GameDir
if ($LASTEXITCODE -ne 0) { throw "deploy-asi failed: $LASTEXITCODE" }

$deployed = Get-Item -LiteralPath $destAsi
if ($deployed.Length -ne $built.Length) {
  throw "Deploy size mismatch: out=$($built.Length) dest=$($deployed.Length)"
}
if ($deployed.LastWriteTime -lt $built.LastWriteTime.AddSeconds(-2)) {
  throw "Deployed ASI older than build output - copy failed?"
}
Write-Host ("Deploy verified: {0} bytes @ {1:HH:mm:ss}" -f $deployed.Length, $deployed.LastWriteTime)

# Remove stale log so wait cannot match old MonoSubmit lines
if (Test-Path -LiteralPath $log) {
  Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
}

if ($NoStart) {
  Write-Host "Deploy done (-NoStart). buildId=$buildId"
  exit 0
}

Write-Host "=== start GTA IV (only after deploy verified; auto-close SteamVR dashboard) ==="
$restartArgs = @{ NoPause = $true }
if ($DirectExe) { $restartArgs.DirectExe = $true }
# restart-gtaiv kills any leftover GTAIV, starts the game, then closes dashboard.
& "$PSScriptRoot\restart-gtaiv.ps1" @restartArgs

Write-Host "Waiting for NEW ASI session (ASI loaded + buildId=$buildId)..."
$ok = $false
for ($i = 0; $i -lt 120; $i++) {
  Start-Sleep -Seconds 1
  $gta = Get-Process -Name "GTAIV" -ErrorAction SilentlyContinue
  if (-not (Test-Path -LiteralPath $log)) { continue }
  $text = Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue
  if (-not $text) { continue }
  # LogInit truncates; require fresh "ASI loaded" - not stale high MonoSubmit counts
  if ($text -match 'ASI loaded' -and ($text -match [regex]::Escape($buildId) -or $text -match 'StereoMode:|StereoRender: mode 13|StereoTemporal')) {
    Write-Host "--- log proof ---"
    Get-Content -LiteralPath $log -TotalCount 25
    $ok = $true
    break
  }
  if ($gta -and ($i % 10) -eq 0) {
    Write-Host ("... waiting ({0}s) GTAIV pid={1}" -f ($i + 1), $gta.Id)
  }
}
if (-not $ok) {
  Write-Host "WARN: new ASI log not confirmed. Steam may be on LAUNCHING - click Play, then check log for 'ASI loaded'."
  Write-Host "Do NOT trust old MonoSubmit #NNNN from a previous session."
} else {
  Write-Host "OK: new ASI is running (buildId=$buildId)."
}

# Quick restart after GTA IV crash.
# Default: only clear game/crash processes, then Steam-launch. No dashboard hacks.
# See docs/STARTUP_SPEED.md for offline / RGLess / DirectExe.
param(
  [switch]$NoStart,
  [switch]$NoPause,
  [switch]$CloseDashboard,  # opt-in: toggle SteamVR dashboard AFTER game is up
  [switch]$KillRockstarStack,  # opt-in: also kill Launcher (can stick Steam on LAUNCHING)
  [switch]$DirectExe  # start GTAIV.exe directly (needs RGLess / offline auth; skips steam -applaunch)
)

$ErrorActionPreference = "Continue"
$steamAppId = 12210
$steamExe = "C:\Program Files (x86)\Steam\steam.exe"
if (-not (Test-Path $steamExe)) {
  $steamExe = Join-Path ${env:ProgramFiles(x86)} "Steam\steam.exe"
}
$gameDir = "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
$gtaExe = Join-Path $gameDir "GTAIV.exe"

function Stop-NamedProcesses([string[]]$names) {
  foreach ($name in $names) {
    $procs = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { continue }
    Write-Host ("  kill {0} ({1})" -f $name, ($procs.Id -join ","))
    $procs | Stop-Process -Force -ErrorAction SilentlyContinue
  }
}

function Invoke-DashboardToggle {
  try {
    Start-Process -FilePath "cmd.exe" -ArgumentList @(
      "/c", "start", "", "vrmonitor://debugcommands/system_dashboard_toggle"
    ) -WindowStyle Hidden -ErrorAction SilentlyContinue | Out-Null
  } catch {}
}

Write-Host "=== GTA IV Quick Restart ==="

# Safe set: clears crash dialog + game without breaking Steam/Rockstar auth pipeline
$killAlways = @(
  "GTAIV",
  "PlayGTAIV",
  "gtaEncoder",
  "RockstarErrorHandler"
)

Write-Host "Stopping GTA / crash dialog..."
Stop-NamedProcesses $killAlways

if ($KillRockstarStack) {
  Write-Host "Stopping Rockstar stack (-KillRockstarStack)..."
  Stop-NamedProcesses @(
    "Launcher",
    "LauncherPatcher",
    "RockstarSteamHelper"
  )
  # Intentionally NOT killing RockstarService / SocialClubHelper - Steam stays on LAUNCHING if those die mid-start.
}

Write-Host "Waiting 2s for Steam to clear 'running' state..."
Start-Sleep -Seconds 2

if ($NoStart) {
  Write-Host "Cleanup done (-NoStart)."
  if (-not $NoPause) { Read-Host "Enter to close" | Out-Null }
  exit 0
}

if ($DirectExe) {
  if (-not (Test-Path $gtaExe)) {
    Write-Host "ERROR: GTAIV.exe missing: $gtaExe"
    if (-not $NoPause) { Read-Host "Enter to close" | Out-Null }
    exit 1
  }
  Write-Host "Starting DIRECT: $gtaExe"
  Write-Host "(Without RGLess/offline auth the launcher may still appear - see docs/STARTUP_SPEED.md)"
  Start-Process -FilePath $gtaExe -WorkingDirectory $gameDir
} else {
  if (-not (Test-Path $steamExe)) {
    Write-Host "ERROR: steam.exe not found: $steamExe"
    if (-not $NoPause) { Read-Host "Enter to close" | Out-Null }
    exit 1
  }
  Write-Host "Starting: $steamExe -applaunch $steamAppId"
  Start-Process -FilePath $steamExe -ArgumentList @("-applaunch", "$steamAppId")
}

Write-Host "Waiting for game process (up to 45s)..."
$ready = $false
for ($i = 0; $i -lt 45; $i++) {
  Start-Sleep -Seconds 1
  $gta = Get-Process -Name "GTAIV" -ErrorAction SilentlyContinue
  if ($gta) {
    Write-Host ("GTAIV running (pid {0}) after {1}s" -f $gta.Id, ($i + 1))
    $ready = $true
    break
  }
}

if (-not $ready) {
  Write-Host "GTAIV not seen yet. If Steam stuck on LAUNCHING: Cancel, wait 5s, click Play once."
  Write-Host "Do NOT use -KillRockstarStack unless cleaning a crash dialog."
}

if ($CloseDashboard -and $ready) {
  Write-Host "Closing SteamVR dashboard (-CloseDashboard)..."
  Start-Sleep -Seconds 2
  Invoke-DashboardToggle
}

if (-not $NoPause) {
  Write-Host ""
  Read-Host "Enter to close" | Out-Null
}

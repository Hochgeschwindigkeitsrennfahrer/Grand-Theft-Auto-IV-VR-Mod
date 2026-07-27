# Quick restart after GTA IV crash.
# Default: kill game/crash processes, then use the normal Steam authentication route.
# SteamVR is untouched. Its dashboard URI requires an explicit opt-in and is refused
# unless SteamVR is already running. See docs/STARTUP_SPEED.md for DirectExe.
param(
  [switch]$NoStart,
  [switch]$NoPause,
  [switch]$NoCloseDashboard,  # legacy compatibility; default is already no toggle
  [switch]$CloseSteamVrDashboard,  # explicit OpenVR-fallback-only opt-in
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
  $steamVr = @(
    Get-Process -Name "vrmonitor", "vrserver", "vrcompositor" -ErrorAction SilentlyContinue
  )
  if ($steamVr.Count -eq 0) {
    Write-Host "SteamVR is absent; refusing the dashboard URI because it could start SteamVR."
    return
  }
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

if ($ready -and $CloseSteamVrDashboard -and -not $NoCloseDashboard) {
  Write-Host "Closing the dashboard of an already-running SteamVR session..."
  Start-Sleep -Seconds 2
  Invoke-DashboardToggle
} elseif ($ready) {
  Write-Host "SteamVR untouched (dashboard toggle requires -CloseSteamVrDashboard)."
}

if (-not $NoPause) {
  Write-Host ""
  Read-Host "Enter to close" | Out-Null
}

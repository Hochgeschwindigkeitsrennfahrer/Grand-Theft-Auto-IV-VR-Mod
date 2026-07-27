# GTA IV Quick Restart — reliable start (no Steam Cancel+Play dance).
#
# DEFAULT: kill game processes → start GTAIV.exe DIRECTLY (working dir = game dir)
# while Steam stays running. Verified on this CE machine: GTAIV appears in ~1s.
#
# Episode/DLC chooser (Rockstar intro → select game): that is stock CE UI.
# Skipping it needs FusionFix Skip Menu (plugins\GTAIV.EFLC.FusionFix.asi +
# SkipMenu=1 in GTAIV.EFLC.FusionFix.cfg) — DirectExe alone does NOT skip it.
#
# Steam -applaunch / steam://rungameid often leave Steam stuck on LAUNCHING
# (PlayGTAIV + Rockstar pipeline). Use -SteamLaunch only if DirectExe fails.
#
# ONE-CLICK for you:
#   Desktop / taskbar: "GTA IV Quick Restart" (run install-restart-shortcut.ps1 once)
#   Or double-click:  scripts\restart-gtaiv.bat
#   Or PowerShell:    .\scripts\restart-gtaiv.ps1 -NoPause
#
# Agent / deploy: prefer this script (or build-deploy-run.ps1) — DirectExe is default.
# See docs/STARTUP_SPEED.md for RGLess / offline notes.
param(
  [switch]$NoStart,
  [switch]$NoPause,
  [switch]$NoCloseDashboard,  # skip dashboard toggle after game is up
  [switch]$KillRockstarStack,  # opt-in: also kill Launcher (can stick Steam on LAUNCHING)
  [switch]$SteamLaunch,  # old path: steam.exe -applaunch 12210 (often sticks on LAUNCHING)
  [switch]$DirectExe  # kept for callers; DirectExe is now the DEFAULT (no-op unless -SteamLaunch)
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

function Wait-ProcessesGone([string[]]$names, [int]$seconds = 15) {
  for ($i = 0; $i -lt $seconds; $i++) {
    $left = @()
    foreach ($name in $names) {
      $left += @(Get-Process -Name $name -ErrorAction SilentlyContinue)
    }
    if ($left.Count -eq 0) { return $true }
    Start-Sleep -Milliseconds 500
  }
  return $false
}

function Invoke-DashboardToggle {
  try {
    Start-Process -FilePath "cmd.exe" -ArgumentList @(
      "/c", "start", "", "vrmonitor://debugcommands/system_dashboard_toggle"
    ) -WindowStyle Hidden -ErrorAction SilentlyContinue | Out-Null
  } catch {}
}

Write-Host "=== GTA IV Quick Restart (DirectExe default) ==="

# Safe set: clears crash dialog + game without breaking Steam/Rockstar auth pipeline
$killAlways = @(
  "GTAIV",
  "PlayGTAIV",
  "gtaEncoder",
  "RockstarErrorHandler"
)

Write-Host "Stopping GTA / crash dialog..."
Stop-NamedProcesses $killAlways
if (-not (Wait-ProcessesGone $killAlways 20)) {
  Write-Host "WARN: some game processes still alive — force kill again..."
  Stop-NamedProcesses $killAlways
  Start-Sleep -Seconds 1
}

if ($KillRockstarStack) {
  Write-Host "Stopping Rockstar stack (-KillRockstarStack)..."
  Stop-NamedProcesses @(
    "Launcher",
    "LauncherPatcher",
    "RockstarSteamHelper"
  )
  # Intentionally NOT killing RockstarService / SocialClubHelper - Steam stays on LAUNCHING if those die mid-start.
}

# Brief settle so Steam drops "running" bit (does not require killing Steam).
Write-Host "Waiting 2s for Steam to clear 'running' state..."
Start-Sleep -Seconds 2

if ($NoStart) {
  Write-Host "Cleanup done (-NoStart)."
  if (-not $NoPause) { Read-Host "Enter to close" | Out-Null }
  exit 0
}

$useSteam = [bool]$SteamLaunch
if ($useSteam) {
  if (-not (Test-Path $steamExe)) {
    Write-Host "ERROR: steam.exe not found: $steamExe"
    if (-not $NoPause) { Read-Host "Enter to close" | Out-Null }
    exit 1
  }
  Write-Host "Starting STEAM: $steamExe -applaunch $steamAppId"
  Write-Host "(If stuck on LAUNCHING: Cancel, wait 5s, re-run WITHOUT -SteamLaunch)"
  Start-Process -FilePath $steamExe -ArgumentList @("-applaunch", "$steamAppId")
} else {
  if (-not (Test-Path $gtaExe)) {
    Write-Host "ERROR: GTAIV.exe missing: $gtaExe"
    if (-not $NoPause) { Read-Host "Enter to close" | Out-Null }
    exit 1
  }
  # Ensure Steam is up (CE steam_api often wants it) but do NOT use -applaunch.
  $steamProc = Get-Process -Name "steam" -ErrorAction SilentlyContinue
  if (-not $steamProc) {
    if (Test-Path $steamExe) {
      Write-Host "Steam not running — starting Steam (no applaunch)..."
      Start-Process -FilePath $steamExe
      Start-Sleep -Seconds 5
    } else {
      Write-Host "WARN: Steam not found — launching GTAIV.exe anyway"
    }
  }
  Write-Host "Starting DIRECT: $gtaExe"
  Write-Host "WorkingDirectory: $gameDir"
  Start-Process -FilePath $gtaExe -WorkingDirectory $gameDir
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
  # PlayGTAIV alone = Steam path still bootstrapping — keep waiting
  $play = Get-Process -Name "PlayGTAIV" -ErrorAction SilentlyContinue
  if ($play -and ($i % 5) -eq 4) {
    Write-Host ("... PlayGTAIV still up (pid {0}) — waiting for GTAIV.exe" -f $play.Id)
  }
}

if (-not $ready) {
  Write-Host "GTAIV not seen yet."
  if ($useSteam) {
    Write-Host "Steam LAUNCHING stuck? Cancel in Steam, wait 5s, then:"
    Write-Host "  .\scripts\restart-gtaiv.ps1 -NoPause"
    Write-Host "(DirectExe is default — no Cancel+Play needed on this machine.)"
  } else {
    Write-Host "DirectExe failed. Is Rockstar login blocking? Try once:"
    Write-Host "  .\scripts\restart-gtaiv.ps1 -SteamLaunch -NoPause"
    Write-Host "Or see docs/STARTUP_SPEED.md (RGLess / offline)."
  }
}

if ($ready -and -not $NoCloseDashboard) {
  Write-Host "Closing SteamVR dashboard (after GTAIV up; use -NoCloseDashboard to skip)..."
  Start-Sleep -Seconds 2
  Invoke-DashboardToggle
}

if (-not $NoPause) {
  Write-Host ""
  Read-Host "Enter to close" | Out-Null
}

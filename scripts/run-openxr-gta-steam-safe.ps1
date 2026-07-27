[CmdletBinding()]
param(
  [string]$GameDir = "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV",
  [ValidateRange(120, 1800)]
  [uint32]$MaxSeconds = 900,
  [switch]$Authorized
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not $Authorized) {
  throw "Live launch is locked. Pass -Authorized for one supervised run."
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
$GtaExe = Join-Path $GameDir "GTAIV.exe"
$InstalledAsi = Join-Path $GameDir "gtaiv_dxvk_vr.asi"
$BackendFile = Join-Path $GameDir "gtaiv_dxvk_vr.backend"
$StereoFile = Join-Path $GameDir "gtaiv_dxvk_vr.stereo"
$DisableFile = Join-Path $GameDir "gtaiv_dxvk_vr.disable"
$InstalledOpenVr = Join-Path $GameDir "openvr_api.dll"
$GameLog = Join-Path $GameDir "gtaiv_dxvk_vr.log"
$CandidateAsi = Join-Path $Root "out-asi\gtaiv_dxvk_vr.asi"
$HostExe = Join-Path $Root "out-openxr\gtaiv_xr_host.exe"
$HostLog = Join-Path (Split-Path -Parent $HostExe) "gtaiv_xr_host.log"
$SteamExe = "C:\Program Files (x86)\Steam\steam.exe"
$SteamVrNames = @(
  "vrmonitor",
  "vrserver",
  "vrcompositor",
  "vrdashboard",
  "vrwebhelper"
)
$RunDirectory = Join-Path (
  Join-Path $Root "out-openxr\runs"
) (Get-Date -Format "yyyyMMdd-HHmmss-'steam-safe'")
$BackupDirectory = Join-Path $RunDirectory "backup"
New-Item -ItemType Directory -Path $BackupDirectory -Force | Out-Null
$StatusPath = Join-Path $RunDirectory "status.log"
$ResultPath = Join-Path $RunDirectory "result.txt"

function Write-Status([string]$Message) {
  $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $Message
  Write-Output $line
  Add-Content -LiteralPath $StatusPath -Value $line -Encoding UTF8
}

function Get-ByName([string[]]$Names) {
  $result = @()
  foreach ($name in $Names) {
    $result += @(Get-Process -Name $name -ErrorAction SilentlyContinue)
  }
  return @($result)
}

function Stop-ByName([string[]]$Names, [string]$Reason) {
  $processes = @(Get-ByName $Names)
  foreach ($process in ($processes | Sort-Object Id -Unique)) {
    Write-Status "STOP: $Reason $($process.ProcessName) pid=$($process.Id)"
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
  }
}

function Assert-And-Stop-SteamVr {
  $steamVr = @(Get-ByName $SteamVrNames)
  if ($steamVr.Count -eq 0) {
    return
  }
  foreach ($process in ($steamVr | Sort-Object Id -Unique)) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
  }
  $summary = ($steamVr | ForEach-Object {
    "{0}(pid={1})" -f $_.ProcessName, $_.Id
  }) -join ", "
  throw "STEAMVR SAFETY ABORT: $summary"
}

function Read-Text([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    return ""
  }
  try {
    return Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
  }
  catch {
    return ""
  }
}

function Get-PeMachine([string]$Path) {
  $bytes = [IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -lt 256) {
    throw "Invalid PE file: $Path"
  }
  $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
  return [BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

function Get-Sha256([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    return "MISSING"
  }
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Save-State([string]$Path, [string]$Name) {
  $exists = Test-Path -LiteralPath $Path -PathType Leaf
  if ($exists) {
    Copy-Item -LiteralPath $Path -Destination (Join-Path $BackupDirectory $Name) -Force
  }
  return [pscustomobject]@{
    Path = $Path
    Name = $Name
    Existed = $exists
    Hash = Get-Sha256 $Path
  }
}

function Restore-State($State) {
  if ($State.Existed) {
    Copy-Item `
      -LiteralPath (Join-Path $BackupDirectory $State.Name) `
      -Destination $State.Path `
      -Force
  }
  elseif (Test-Path -LiteralPath $State.Path -PathType Leaf) {
    Remove-Item -LiteralPath $State.Path -Force
  }
}

foreach ($required in @($GtaExe, $InstalledAsi, $CandidateAsi, $HostExe, $SteamExe)) {
  if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
    throw "Required file is missing: $required"
  }
}

if ((Get-PeMachine $GtaExe) -ne 0x014c) {
  throw "GTAIV.exe is not x86."
}
if ((Get-PeMachine $CandidateAsi) -ne 0x014c) {
  throw "Candidate ASI is not x86."
}
if ((Get-PeMachine $HostExe) -ne 0x8664) {
  throw "OpenXR host is not x64."
}

$runtime = (Get-ItemProperty `
  -Path "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1" `
  -Name ActiveRuntime `
  -ErrorAction Stop).ActiveRuntime
if ($runtime -notmatch "Oculus|Meta") {
  throw "Meta OpenXR is not active: $runtime"
}
$ovrService = Get-Service -Name OVRService -ErrorAction SilentlyContinue
if (-not $ovrService -or $ovrService.Status -ne "Running") {
  throw "Meta OVRService is not running."
}

Assert-And-Stop-SteamVr
$conflicts = @(Get-ByName @("GTAIV", "PlayGTAIV", "gtaiv_xr_host"))
if ($conflicts.Count -gt 0) {
  throw "GTA/host process already exists; refusing an ambiguous run."
}

$steamWasRunning = @(Get-ByName @("steam")).Count -gt 0
$states = @()
$hostProcess = $null
$outcome = "ABORTED"
$failure = ""
$gameStarted = $false
$hostFocused = $false
$backendOpenXr = $false
$mode54 = $false
$controllerReady = $false
$drawHooksReady = $false
$bridgeReady = $false
$verifiedWvp = $false
$producerTransaction = $false
$hostConnected = $false
$hostAcquired = $false

try {
  Write-Status "SAFETY: Meta OpenXR active; SteamVR absent"
  Write-Status "SAFETY: normal Steam authentication allowed; SteamVR polled every 100 ms"
  Write-Status "ARTIFACT: ASI sha256=$(Get-Sha256 $CandidateAsi)"
  Write-Status "ARTIFACT: host sha256=$(Get-Sha256 $HostExe)"

  $states = @(
    Save-State $InstalledAsi "gtaiv_dxvk_vr.asi"
    Save-State $BackendFile "gtaiv_dxvk_vr.backend"
    Save-State $StereoFile "gtaiv_dxvk_vr.stereo"
    Save-State $DisableFile "gtaiv_dxvk_vr.disable"
    Save-State $InstalledOpenVr "openvr_api.dll"
  )
  if (Test-Path -LiteralPath $GameLog -PathType Leaf) {
    Copy-Item -LiteralPath $GameLog -Destination (Join-Path $BackupDirectory "previous-game.log") -Force
    Remove-Item -LiteralPath $GameLog -Force
  }
  if (Test-Path -LiteralPath $HostLog -PathType Leaf) {
    Copy-Item -LiteralPath $HostLog -Destination (Join-Path $BackupDirectory "previous-host.log") -Force
    Remove-Item -LiteralPath $HostLog -Force
  }

  Copy-Item -LiteralPath $CandidateAsi -Destination $InstalledAsi -Force
  Set-Content -LiteralPath $BackendFile -Value "openxr" -Encoding ASCII -NoNewline
  Set-Content -LiteralPath $StereoFile -Value "54" -Encoding ASCII -NoNewline
  Set-Content -LiteralPath $DisableFile -Value "1" -Encoding ASCII -NoNewline
  if (Test-Path -LiteralPath $InstalledOpenVr -PathType Leaf) {
    Remove-Item -LiteralPath $InstalledOpenVr -Force
  }
  Write-Status "DEPLOYED: backend=openxr stereo=54"
  Write-Status "OPENVR HARD-BLOCKED: disable sentinel present; openvr_api.dll removed and backed up"

  Assert-And-Stop-SteamVr
  $hostProcess = Start-Process `
    -FilePath $HostExe `
    -ArgumentList @(
      "--game",
      "--timeout-ms",
      (([uint64]$MaxSeconds + 90) * 1000).ToString()
    ) `
    -WorkingDirectory (Split-Path -Parent $HostExe) `
    -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $RunDirectory "host.stdout.log") `
    -RedirectStandardError (Join-Path $RunDirectory "host.stderr.log") `
    -PassThru
  Write-Status "HOST: started pid=$($hostProcess.Id)"

  $focusDeadline = [DateTime]::UtcNow.AddSeconds(30)
  while ([DateTime]::UtcNow -lt $focusDeadline) {
    Assert-And-Stop-SteamVr
    $hostProcess.Refresh()
    if ($hostProcess.HasExited) {
      throw "OpenXR host exited before launch, exit=$($hostProcess.ExitCode)"
    }
    $hostText = Read-Text $HostLog
    if ($hostText -match "XRHost: session FOCUSED") {
      $hostFocused = $true
      break
    }
    if ($hostText -match "FATAL:") {
      throw "OpenXR host logged a fatal startup error."
    }
    Start-Sleep -Milliseconds 100
  }
  if (-not $hostFocused) {
    throw "Quest session did not reach FOCUSED."
  }
  Write-Status "HEADSET READY: Quest session FOCUSED; Touch actions ready"

  Assert-And-Stop-SteamVr
  Start-Process `
    -FilePath $SteamExe `
    -ArgumentList @("-applaunch", "12210") `
    -WorkingDirectory (Split-Path -Parent $SteamExe) `
    -WindowStyle Hidden | Out-Null
  Write-Status "AUTH: requested GTA IV app 12210 through regular Steam; no custom arguments"

  $launchDeadline = [DateTime]::UtcNow.AddSeconds(120)
  while ([DateTime]::UtcNow -lt $launchDeadline) {
    Assert-And-Stop-SteamVr
    $hostProcess.Refresh()
    if ($hostProcess.HasExited) {
      throw "OpenXR host exited while GTA authenticated, exit=$($hostProcess.ExitCode)"
    }

    $gameText = Read-Text $GameLog
    if ($gameText -match "ASI loaded" -and
        $gameText -match "Backend: OpenXR" -and
        $gameText -match "StereoMode: 54") {
      $gameStarted = $true
      $backendOpenXr = $true
      $mode54 = $true
      break
    }
    if ($gameText -match "Backend: OpenVR") {
      throw "ASI selected OpenVR instead of OpenXR."
    }
    if ($gameText -match "FAIL-CLOSED - openvr_api.dll was loaded") {
      throw "ASI detected an OpenVR isolation violation."
    }
    Start-Sleep -Milliseconds 100
  }
  if (-not $gameStarted) {
    throw "No fresh GTA OpenXR/Mode 54 ASI session appeared within 120 seconds."
  }
  Write-Status "GTA READY: fresh x86 ASI session backend=OpenXR stereo=54"
  Write-Status "HEADSET: GTA is live; inspect stereo, head tracking, menus, and Touch controls"

  $runDeadline = [DateTime]::UtcNow.AddSeconds($MaxSeconds)
  $gameGoneSince = $null
  while ([DateTime]::UtcNow -lt $runDeadline) {
    Assert-And-Stop-SteamVr
    $hostProcess.Refresh()
    if ($hostProcess.HasExited) {
      throw "OpenXR host exited during gameplay, exit=$($hostProcess.ExitCode)"
    }

    $gameProcesses = @(Get-ByName @("GTAIV"))
    if ($gameProcesses.Count -eq 0) {
      if ($null -eq $gameGoneSince) {
        $gameGoneSince = [DateTime]::UtcNow
      }
      elseif (([DateTime]::UtcNow - $gameGoneSince).TotalSeconds -ge 3) {
        $outcome = "GAME_EXITED"
        break
      }
    }
    else {
      $gameGoneSince = $null
    }

    $gameText = Read-Text $GameLog
    $hostText = Read-Text $HostLog

    if (-not $controllerReady -and
        $gameText -match "OpenXRController: Touch -> XInput ready") {
      $controllerReady = $true
      Write-Status "MARKER: Touch -> XInput controller bridge READY"
    }
    if (-not $drawHooksReady -and
        $gameText -match "StereoWvp: six-method draw interception READY") {
      $drawHooksReady = $true
      Write-Status "MARKER: six-method WVP interception READY"
    }
    if (-not $bridgeReady -and $gameText -match "OpenXRBridge: READY") {
      $bridgeReady = $true
      Write-Status "MARKER: x86/x64 GPU bridge READY"
    }
    if (-not $verifiedWvp -and
        $gameText -match "StereoWvp: pair=.*verified=1") {
      $verifiedWvp = $true
      Write-Status "MARKER: same-frame WVP stereo pair VERIFIED"
    }
    if (-not $producerTransaction -and
        $gameText -match "OpenXRBridge: stereo transaction=") {
      $producerTransaction = $true
      Write-Status "MARKER: GTA published stereo transaction"
    }
    if (-not $hostConnected -and
        $hostText -match "GameBridge: CONNECTED stereo") {
      $hostConnected = $true
      Write-Status "MARKER: host connected to GTA stereo"
    }
    if (-not $hostAcquired -and
        $hostText -match "GameBridge: stereo acquired transaction=") {
      $hostAcquired = $true
      Write-Status "MARKER: host acquired GTA stereo"
    }

    if ($gameText -match "Backend: OpenVR" -or
        $gameText -match "OpenVR: VR_Init" -or
        $hostText -match "FATAL:") {
      throw "Forbidden OpenVR or fatal OpenXR state was logged."
    }
    Start-Sleep -Milliseconds 100
  }

  if ($outcome -eq "ABORTED") {
    $outcome = "TIME_LIMIT"
  }
}
catch {
  $failure = $_.Exception.Message
  Write-Status "ABORT: $failure"
}
finally {
  Stop-ByName @("GTAIV", "PlayGTAIV") "end supervised GTA run"
  if ($hostProcess) {
    $liveHost = Get-Process -Id $hostProcess.Id -ErrorAction SilentlyContinue
    if ($liveHost) {
      Write-Status "STOP: OpenXR host pid=$($hostProcess.Id)"
      Stop-Process -Id $hostProcess.Id -Force -ErrorAction SilentlyContinue
    }
  }
  Stop-ByName $SteamVrNames "unexpected SteamVR cleanup"

  if (-not $steamWasRunning) {
    Stop-ByName @("steam", "steamwebhelper") "Steam started for this test"
  }

  if (Test-Path -LiteralPath $GameLog -PathType Leaf) {
    Copy-Item -LiteralPath $GameLog -Destination (Join-Path $RunDirectory "gtaiv_dxvk_vr.log") -Force
  }
  if (Test-Path -LiteralPath $HostLog -PathType Leaf) {
    Copy-Item -LiteralPath $HostLog -Destination (Join-Path $RunDirectory "gtaiv_xr_host.log") -Force
  }
  foreach ($state in $states) {
    Restore-State $state
  }

  $result = @(
    "outcome=$outcome"
    "failure=$failure"
    "runtime=$runtime"
    "gameStarted=$gameStarted"
    "hostFocused=$hostFocused"
    "backendOpenXr=$backendOpenXr"
    "mode54=$mode54"
    "controllerReady=$controllerReady"
    "drawHooksReady=$drawHooksReady"
    "bridgeReady=$bridgeReady"
    "verifiedWvp=$verifiedWvp"
    "producerTransaction=$producerTransaction"
    "hostConnected=$hostConnected"
    "hostAcquired=$hostAcquired"
  )
  Set-Content -LiteralPath $ResultPath -Value $result -Encoding UTF8
  Write-Status "RESULT: $outcome"
  Write-Status "RESTORED: original ASI/backend/stereo"
}

[CmdletBinding()]
param(
  [string]$GameDir = "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV",
  [ValidateRange(120, 1800)]
  [uint32]$MaxSeconds = 900,
  [ValidateSet(55, 56, 57, 58)]
  [uint32]$StereoMode = 55,
  [switch]$Authorized,
  [string]$RuntimeManifest = "",
  [switch]$ElliottCliProof,
  [switch]$StopWhenProofComplete,
  [ValidateRange(6, 30)]
  [uint32]$ElliottWalkSeconds = 6
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not $Authorized) {
  throw "Live launch is locked. Pass -Authorized for one supervised run."
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. "$PSScriptRoot\openxr-runtime-info.ps1"
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
$GtaExe = Join-Path $GameDir "GTAIV.exe"
$PlayGtaExe = Join-Path $GameDir "PlayGTAIV.exe"
$InstalledAsi = Join-Path $GameDir "gtaiv_dxvk_vr.asi"
$BackendFile = Join-Path $GameDir "gtaiv_dxvk_vr.backend"
$StereoFile = Join-Path $GameDir "gtaiv_dxvk_vr.stereo"
$DisableFile = Join-Path $GameDir "gtaiv_dxvk_vr.disable"
$InstalledOpenVr = Join-Path $GameDir "openvr_api.dll"
$GameLog = Join-Path $GameDir "gtaiv_dxvk_vr.log"
$DxvkLog = Join-Path $GameDir "GTAIV_d3d9.log"
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
$ElliottWalkReadyMarker =
  Join-Path $RunDirectory "elliott-walk-ready.marker"
$ElliottDataDirectory = Join-Path (
  [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::LocalApplicationData
  )
) "OpenXR-Simulator"
$ElliottControllerCommand = Join-Path `
  $ElliottDataDirectory "controller_pose_command.json"
$ElliottPoseSweepCommand = Join-Path `
  $ElliottDataDirectory "pose_sweep_command.json"
$ElliottCommandAck = Join-Path `
  $ElliottDataDirectory "command_ack.json"
$ElliottCommandSequence = [uint64]0
$ElliottLastAckFailure = ""

function Write-Status([string]$Message) {
  $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $Message
  Write-Output $line
  Add-Content -LiteralPath $StatusPath -Value $line -Encoding UTF8
}

function Write-StatusBestEffort([string]$Message) {
  try {
    Write-Status $Message
  }
  catch {
    # A missing/unwritable diagnostic path must never interrupt process
    # cleanup or restoration of the user's installed game state.
  }
}

function Get-ByName([string[]]$Names) {
  $result = @()
  foreach ($name in $Names) {
    $result += @(Get-Process -Name $name -ErrorAction SilentlyContinue)
  }
  return @($result)
}

function Test-CanonicalPathEqual([string]$Left, [string]$Right) {
  if ([string]::IsNullOrWhiteSpace($Left) -or
      [string]::IsNullOrWhiteSpace($Right)) {
    return $false
  }
  try {
    $fullLeft = [System.IO.Path]::GetFullPath($Left)
    $fullRight = [System.IO.Path]::GetFullPath($Right)
  }
  catch {
    return $false
  }
  return $fullLeft.Equals(
    $fullRight,
    [System.StringComparison]::OrdinalIgnoreCase
  )
}

function Test-PathEqualOrWithinRoot([string]$Path, [string]$Root) {
  if ([string]::IsNullOrWhiteSpace($Path) -or
      [string]::IsNullOrWhiteSpace($Root)) {
    return $false
  }
  try {
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullRoot = [System.IO.Path]::GetFullPath($Root)
  }
  catch {
    return $false
  }
  if ($fullPath.Equals(
      $fullRoot,
      [System.StringComparison]::OrdinalIgnoreCase
    )) {
    return $true
  }
  $rootPrefix = $fullRoot
  $lastRootCharacter = $rootPrefix[$rootPrefix.Length - 1]
  if ($lastRootCharacter -ne [System.IO.Path]::DirectorySeparatorChar -and
      $lastRootCharacter -ne [System.IO.Path]::AltDirectorySeparatorChar) {
    $rootPrefix += [System.IO.Path]::DirectorySeparatorChar
  }
  return $fullPath.StartsWith(
    $rootPrefix,
    [System.StringComparison]::OrdinalIgnoreCase
  )
}

function Stop-ByName([string[]]$Names, [string]$Reason) {
  $processes = @(Get-ByName $Names)
  foreach ($process in ($processes | Sort-Object Id -Unique)) {
    Write-StatusBestEffort (
      "STOP: $Reason $($process.ProcessName) pid=$($process.Id)"
    )
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
  }
  foreach ($process in ($processes | Sort-Object Id -Unique)) {
    try {
      [void]$process.WaitForExit(5000)
    }
    catch {
      # Process may already be gone or inaccessible. Restore-State retries
      # file operations below so cleanup still has a bounded second chance.
    }
  }
}

function Stop-ByExactPath(
  [string[]]$Names,
  [string]$ExpectedPath,
  [string]$Reason
) {
  $targets = @(Get-ByName $Names | Where-Object {
    $processPath = ""
    try {
      $processPath = $_.Path
    }
    catch {
      return $false
    }
    Test-CanonicalPathEqual $processPath $ExpectedPath
  })
  foreach ($process in ($targets | Sort-Object Id -Unique)) {
    Write-StatusBestEffort (
      "STOP: $Reason $($process.ProcessName) pid=$($process.Id) " +
      "path=$ExpectedPath"
    )
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
  }
  foreach ($process in ($targets | Sort-Object Id -Unique)) {
    try {
      [void]$process.WaitForExit(5000)
    }
    catch {
      # Process may already be gone or inaccessible. Restore-State retries
      # file operations below so cleanup still has a bounded second chance.
    }
  }
}

function Stop-StaleRockstarLauncherSession([string]$Reason) {
  $activeTitle = @(Get-ByName @("GTAIV", "PlayGTAIV"))
  if ($activeTitle.Count -ne 0) {
    throw (
      "Refusing Rockstar launcher cleanup while GTAIV/PlayGTAIV is active."
    )
  }
  $rockstarRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $env:ProgramFiles "Rockstar Games")
  )
  $candidates = @(
    Get-ByName @("Launcher", "SocialClubHelper")
  )
  $targets = @($candidates | Where-Object {
    $processPath = ""
    try {
      $processPath = $_.Path
    }
    catch {
      return $false
    }
    $processPath -and
      (Test-PathEqualOrWithinRoot $processPath $rockstarRoot)
  })
  foreach ($process in ($targets | Sort-Object Id -Unique)) {
    Write-StatusBestEffort (
      "STOP: $Reason $($process.ProcessName) pid=$($process.Id)"
    )
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
  }
  foreach ($process in ($targets | Sort-Object Id -Unique)) {
    try {
      [void]$process.WaitForExit(5000)
    }
    catch {
      # A process may already be gone or inaccessible.
    }
  }
}

function Assert-And-Stop-SteamVrAtLaunchBoundary {
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

function Start-AuditedGtaSteamAuthentication([string]$Reason) {
  Assert-And-Stop-SteamVrAtLaunchBoundary
  Start-Process `
    -FilePath $SteamExe `
    -ArgumentList @("-applaunch", "12210") `
    -WorkingDirectory (Split-Path -Parent $SteamExe) `
    -WindowStyle Hidden | Out-Null
  Write-Status "AUTH: $Reason; regular Steam app 12210 with no custom arguments"
}

function Read-Text([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    return ""
  }
  try {
    # Get-Content can write a partial value to the success stream before a
    # concurrent append raises an error. The catch fallback would then make
    # the caller receive Object[] { partialText, "" }. Capture ReadToEnd into
    # one local String and explicitly share with the live C++ log writer.
    $share =
      [System.IO.FileShare]::ReadWrite -bor
      [System.IO.FileShare]::Delete
    $stream = [System.IO.FileStream]::new(
      $Path,
      [System.IO.FileMode]::Open,
      [System.IO.FileAccess]::Read,
      $share
    )
    try {
      $reader = [System.IO.StreamReader]::new($stream, $true)
      try {
        [string]$snapshot = $reader.ReadToEnd()
      }
      finally {
        $reader.Dispose()
      }
      return $snapshot
    }
    finally {
      $stream.Dispose()
    }
  }
  catch {
    return ""
  }
}

function Get-MonotonicTransactionSequence(
  [string]$Text,
  [string]$Pattern,
  [int]$AfterIndex
) {
  $ids = @()
  $seen = @{}
  $last = [uint64]0
  $monotonic = $true
  foreach ($match in [regex]::Matches($Text, $Pattern)) {
    if ($match.Index -le $AfterIndex) {
      continue
    }
    $id = [uint64]$match.Groups[1].Value
    $key = $id.ToString()
    if ($seen.ContainsKey($key)) {
      continue
    }
    if ($last -gt 0 -and $id -le $last) {
      $monotonic = $false
    }
    $seen[$key] = $true
    $ids += $id
    $last = $id
  }
  return [pscustomobject]@{
    Count = $ids.Count
    Monotonic = $monotonic
    Ids = @($ids)
    First = if ($ids.Count -gt 0) { [uint64]$ids[0] } else { [uint64]0 }
    Latest = $last
  }
}

function Get-SustainedWorldRouteEvidence(
  [string]$GameText,
  [string]$HostText,
  [string]$ProducerPattern,
  [string]$HostPattern,
  [int]$GameAfterIndex,
  [int]$HostAfterIndex
) {
  $producer = Get-MonotonicTransactionSequence `
    -Text $GameText `
    -Pattern $ProducerPattern `
    -AfterIndex $GameAfterIndex
  $hostFrames = Get-MonotonicTransactionSequence `
    -Text $HostText `
    -Pattern $HostPattern `
    -AfterIndex $HostAfterIndex
  $sharedIds = @($producer.Ids | Where-Object {
    $hostFrames.Ids -contains $_
  })
  return [pscustomobject]@{
    Ready =
      $producer.Count -ge 3 -and
      $hostFrames.Count -ge 3 -and
      $producer.Monotonic -and
      $hostFrames.Monotonic -and
      $sharedIds.Count -ge 3
    ProducerCount = $producer.Count
    HostCount = $hostFrames.Count
    SharedCount = $sharedIds.Count
    ProducerFirst = $producer.First
    ProducerLatest = $producer.Latest
    HostFirst = $hostFrames.First
    HostLatest = $hostFrames.Latest
  }
}

function Get-OpenXrHostArguments(
  [uint32]$Mode,
  [uint64]$TimeoutMs
) {
  $arguments = @(
    "--game",
    "--timeout-ms",
    $TimeoutMs.ToString()
  )
  if ($Mode -eq 57) {
    $arguments += "--allow-temporal-stereo"
  }
  return @($arguments)
}

function Clear-ElliottCliProofCommands {
  if (-not (Test-Path -LiteralPath $ElliottDataDirectory -PathType Container)) {
    return
  }
  foreach ($path in @(
      $ElliottControllerCommand,
      "$ElliottControllerCommand.tmp",
      $ElliottPoseSweepCommand,
      "$ElliottPoseSweepCommand.tmp",
      $ElliottCommandAck,
      "$ElliottCommandAck.tmp"
    )) {
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
  }
}

function Write-ElliottJsonAtomic(
  [string]$Path,
  [System.Collections.IDictionary]$Payload
) {
  $temporaryPath = "$Path.tmp"
  Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
  $json = $Payload | ConvertTo-Json -Compress
  Set-Content `
    -LiteralPath $temporaryPath `
    -Value $json `
    -Encoding ASCII `
    -NoNewline
  Move-Item -LiteralPath $temporaryPath -Destination $Path -Force
}

function Wait-ElliottCommandAck(
  [string]$Command,
  [uint64]$Sequence,
  [int]$TimeoutMs
) {
  $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
  $script:ElliottLastAckFailure =
    "$Command acknowledgment timed out after ${TimeoutMs}ms"
  while ([DateTime]::UtcNow -lt $deadline) {
    if ($hostProcess) {
      $hostProcess.Refresh()
      if ($hostProcess.HasExited) {
        throw "OpenXR host exited while waiting for Elliott CLI acknowledgment."
      }
    }
    if ((Read-Text $GameLog) -match "DeviceReset: FAILED") {
      throw "GTA logged DeviceReset: FAILED."
    }
    if ((Read-Text $HostLog) -match "FATAL:") {
      throw "OpenXR host logged a fatal state during Elliott CLI input."
    }

    if (Test-Path -LiteralPath $ElliottCommandAck -PathType Leaf) {
      try {
        $ack = Get-Content `
          -LiteralPath $ElliottCommandAck `
          -Raw `
          -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
        $commandMatches = [string]$ack.command -eq $Command
        $sequenceMatches = $Command -ne "controller_state" -or
          [uint64]$ack.sequence -eq $Sequence
        if ($commandMatches -and $sequenceMatches) {
          if ([bool]$ack.success) {
            return $true
          }
          $script:ElliottLastAckFailure =
            "$Command sequence $Sequence was rejected"
          return $false
        }
      }
      catch {
        # The runtime writes the acknowledgment directly. A concurrent read
        # can see a partial JSON file, so retry on the next 100 ms watchdog tick.
      }
    }
    Start-Sleep -Milliseconds 100
  }
  return $false
}

function Send-ElliottControllerSnapshot(
  [ValidateSet(
    "NeutralBoth",
    "NeutralLeft",
    "NeutralRight",
    "PrimaryA",
    "MenuStart",
    "StickForward"
  )]
  [string]$Kind,
  [ValidateRange(1, 5)]
  [int]$Attempts = 3
) {
  for ($attempt = 1; $attempt -le $Attempts; ++$attempt) {
    ++$script:ElliottCommandSequence
    if ($script:ElliottCommandSequence -eq 0) {
      ++$script:ElliottCommandSequence
    }

    $payload = switch ($Kind) {
      "NeutralBoth" {
        [ordered]@{
          hand = 2
          sequence = $script:ElliottCommandSequence
          lease_ms = 1000
          neutral = $true
        }
        break
      }
      "NeutralLeft" {
        [ordered]@{
          hand = 0
          sequence = $script:ElliottCommandSequence
          lease_ms = 750
          neutral = $true
        }
        break
      }
      "NeutralRight" {
        [ordered]@{
          hand = 1
          sequence = $script:ElliottCommandSequence
          lease_ms = 750
          neutral = $true
        }
        break
      }
      "PrimaryA" {
        [ordered]@{
          hand = 1
          sequence = $script:ElliottCommandSequence
          lease_ms = 750
          neutral = $false
          stickX = 0.0
          stickY = 0.0
          trigger = 0.0
          squeeze = 0.0
          primary = $true
          secondary = $false
          menu = $false
          thumbClick = $false
        }
        break
      }
      "MenuStart" {
        [ordered]@{
          hand = 0
          sequence = $script:ElliottCommandSequence
          lease_ms = 750
          neutral = $false
          stickX = 0.0
          stickY = 0.0
          trigger = 0.0
          squeeze = 0.0
          primary = $false
          secondary = $false
          menu = $true
          thumbClick = $false
        }
        break
      }
      "StickForward" {
        [ordered]@{
          hand = 0
          sequence = $script:ElliottCommandSequence
          lease_ms = [uint32](($ElliottWalkSeconds + 3) * 1000)
          neutral = $false
          stickX = 0.0
          stickY = 0.8
          trigger = 0.0
          squeeze = 0.0
          primary = $false
          secondary = $false
          menu = $false
          thumbClick = $false
        }
        break
      }
    }

    Remove-Item `
      -LiteralPath $ElliottCommandAck `
      -Force `
      -ErrorAction SilentlyContinue
    Write-ElliottJsonAtomic $ElliottControllerCommand $payload
    if (Wait-ElliottCommandAck `
        -Command "controller_state" `
        -Sequence $script:ElliottCommandSequence `
        -TimeoutMs 2000) {
      Write-Status (
        "ELLIOTT CLI: {0} acknowledged sequence={1}" -f
        $Kind,
        $script:ElliottCommandSequence
      )
      return $script:ElliottCommandSequence
    }
    Write-Status (
      "ELLIOTT CLI RETRY: {0} attempt={1}/{2}: {3}" -f
      $Kind,
      $attempt,
      $Attempts,
      $script:ElliottLastAckFailure
    )
  }
  throw "ELLIOTT CLI INPUT FAILED: $Kind; $script:ElliottLastAckFailure"
}

function Send-ElliottPoseSweep(
  [bool]$Enabled,
  [ValidateRange(1, 5)]
  [int]$Attempts = 3
) {
  $payload = [ordered]@{
    enabled = $Enabled
    yaw_amp_deg = if ($Enabled) { 15.0 } else { 0.0 }
    pitch_amp_deg = if ($Enabled) { 8.0 } else { 0.0 }
    roll_amp_deg = if ($Enabled) { 5.0 } else { 0.0 }
    freq_hz = 0.25
  }
  for ($attempt = 1; $attempt -le $Attempts; ++$attempt) {
    Remove-Item `
      -LiteralPath $ElliottCommandAck `
      -Force `
      -ErrorAction SilentlyContinue
    Write-ElliottJsonAtomic $ElliottPoseSweepCommand $payload
    if (Wait-ElliottCommandAck `
        -Command "pose_sweep" `
        -Sequence 0 `
        -TimeoutMs 2000) {
      Write-Status "ELLIOTT CLI: pose sweep enabled=$Enabled acknowledged"
      return
    }
    Write-Status (
      "ELLIOTT CLI RETRY: pose sweep enabled={0} attempt={1}/{2}: {3}" -f
      $Enabled,
      $attempt,
      $Attempts,
      $script:ElliottLastAckFailure
    )
  }
  throw (
    "ELLIOTT CLI INPUT FAILED: pose sweep enabled={0}; {1}" -f
    $Enabled,
    $script:ElliottLastAckFailure
  )
}

function Get-SteamAppLaunchOptions([string]$AppId) {
  $userdata = Join-Path (Split-Path -Parent $SteamExe) "userdata"
  if (-not (Test-Path -LiteralPath $userdata -PathType Container)) {
    return @()
  }
  $escapedAppId = [regex]::Escape($AppId)
  $results = @()
  Get-ChildItem -LiteralPath $userdata -Directory -ErrorAction SilentlyContinue |
    ForEach-Object {
      $config = Join-Path $_.FullName "config\localconfig.vdf"
      if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
        return
      }
      $awaitingBlock = $false
      $inAppBlock = $false
      $depth = 0
      foreach ($line in Get-Content -LiteralPath $config -ErrorAction SilentlyContinue) {
        if (-not $inAppBlock) {
          if ($line -match ('^\s*"' + $escapedAppId + '"\s*\{\s*$')) {
            $inAppBlock = $true
            $depth = 1
            $awaitingBlock = $false
          }
          elseif ($line -match ('^\s*"' + $escapedAppId + '"\s*$')) {
            $awaitingBlock = $true
          }
          elseif ($awaitingBlock -and $line -match '^\s*\{\s*$') {
            $inAppBlock = $true
            $depth = 1
            $awaitingBlock = $false
          }
          elseif ($awaitingBlock -and $line.Trim().Length -gt 0) {
            $awaitingBlock = $false
          }
          continue
        }

        if ($depth -eq 1 -and
            $line -match '^\s*"LaunchOptions"\s*"(.*)"\s*$') {
          $results += $Matches[1]
        }
        $depth += ([regex]::Matches($line, '\{')).Count
        $depth -= ([regex]::Matches($line, '\}')).Count
        if ($depth -le 0) {
          $inAppBlock = $false
          $depth = 0
        }
      }
    }
  return @($results | Select-Object -Unique)
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
    $source = Join-Path $BackupDirectory $State.Name
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
      try {
        Copy-Item -LiteralPath $source -Destination $State.Path -Force
        return
      }
      catch {
        if ([DateTime]::UtcNow -ge $deadline) {
          throw
        }
        Start-Sleep -Milliseconds 100
      }
    } while ($true)
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

$runtimeInfo = if ($RuntimeManifest) {
  $resolvedManifest = (Resolve-Path -LiteralPath $RuntimeManifest).Path
  Get-OpenXrRuntimeInfo -ManifestPath $resolvedManifest
}
else {
  Get-ActiveOpenXrRuntimeInfo
}
$runtime = $runtimeInfo.ManifestPath
if (-not $runtimeInfo.IsDirectTestRuntime) {
  throw "Meta Quest Link, Meta XR Simulator, or Elliott OpenXR Simulator must be selected: $runtime"
}
if ($ElliottCliProof -and $runtimeInfo.Kind -ne "ElliottSimulator") {
  throw "-ElliottCliProof requires the Elliott OpenXR Simulator manifest."
}
if ($runtimeInfo.RequiresOvrService) {
  $ovrService = Get-Service -Name OVRService -ErrorAction SilentlyContinue
  if (-not $ovrService -or $ovrService.Status -ne "Running") {
    throw "Meta OVRService is not running. Start Meta Quest Link first."
  }
}
$inspectionTarget = if ($runtimeInfo.IsSimulator) { "SIMULATOR" } else { "HEADSET" }

Assert-And-Stop-SteamVrAtLaunchBoundary
$conflicts = @(Get-ByName @("GTAIV", "PlayGTAIV", "gtaiv_xr_host"))
if ($conflicts.Count -gt 0) {
  throw "GTA/host process already exists; refusing an ambiguous run."
}
Stop-StaleRockstarLauncherSession "stale preflight Rockstar session"

$steamWasRunning = @(Get-ByName @("steam")).Count -gt 0
$states = @()
$hostProcess = $null
$outcome = "ABORTED"
$failure = ""
$gameStarted = $false
$hostFocused = $false
$backendOpenXr = $false
$stereoModeReady = $false
$controllerReady = $false
$controllerInputConsumed = $false
$controllerReleaseConsumed = $false
$controllerAConsumed = $false
$controllerStartConsumed = $false
$controllerStickConsumed = $false
$controllerStickNeutralConsumed = $false
$controllerAConsumedPacket = [uint64]0
$controllerStartConsumedPacket = [uint64]0
$controllerStickConsumedPacket = [uint64]0
$controllerNeutralConsumedPacket = [uint64]0
$bridgeReady = $false
$worldCaptureReady = $false
$colorPathReady = $false
$srgbDecodeReady = $false
$swapchainFormatReady = $false
$swapchainFormat = [int64]-1
$stationaryUiQuad = $false
$stationaryUiProducer = $false
$stationaryUiHost = $false
$producerTransaction = $false
$hostConnected = $false
$hostAcquired = $false
$gamePoseMatched = $false
$hostSwapchainReuseReady = $false
$hostSwapchainFreshUpdatesReady = $false
$hostSwapchainFreshWorldUpdateSamples = 0
$hostSwapchainReuseFramesLowerBound = [uint64]0
$hostSwapchainUpdatesLowerBound = [uint64]0
$stereoCameraDistanceReady = $StereoMode -eq 55
$stereoCameraDistanceCm = [double]0
$stereoPixelDistinct = $StereoMode -eq 55
$stereoRawRgbReady = $StereoMode -eq 55
$stereoRawRgbAbsDiff = [int64]0
$stereoChangedPixels = [uint32]0
$stereoChangedTiles = [uint32]0
$stereoApplyGenerationFresh = $StereoMode -eq 55
$stereoApplyGenerationLeft = [uint32]0
$stereoApplyGenerationRight = [uint32]0
$stereoAcceptedPairOrdinal = [uint32]0
$mode56PrePauseFirstPairOrdinal = [uint32]0
$mode56PrePauseAcceptedPairSpan = [uint32]0
$mode56PrePauseProducerTransactions = 0
$mode56PrePauseHostTransactions = 0
$mode56PrePauseSharedTransaction = $false
$mode56PrePauseContinuityReady = $StereoMode -ne 56
$mode58PrePauseFirstPairOrdinal = [uint32]0
$mode58PrePauseAcceptedPairSpan = [uint32]0
$mode58PrePauseProducerTransactions = 0
$mode58PrePauseHostTransactions = 0
$mode58PrePauseSharedTransaction = $false
$mode58PrePauseContinuityReady = $StereoMode -ne 58
$parentDualFusedPairReady = $StereoMode -ne 58
$parentDualProducerReady = $StereoMode -ne 58
$parentDualProofComplete = $StereoMode -ne 58
$parentDualPairPose = [uint64]0
$parentDualPairSource = [uint64]0
$parentDualProducerTransaction = [uint64]0
$parentDualHostExactPoseReady = $StereoMode -ne 58
$firstPersonHardHookReady = $StereoMode -ne 58
$firstPersonGameplayReady = $StereoMode -ne 58
$firstPersonPedHideReady = $StereoMode -ne 58
$firstPersonPedHideSuccesses = [uint32]0
$firstPersonProofComplete = $StereoMode -ne 58
$temporalPairReady = $StereoMode -ne 57
$temporalPixelDistinct = $StereoMode -ne 57
$temporalHostAccepted = $StereoMode -ne 57
$temporalCapturePoseActive = $StereoMode -ne 57
$temporalBuildReplayReceiptReady = $StereoMode -ne 57
$temporalSplitStageReady = $StereoMode -ne 57
$temporalSplitReadbackReady = $StereoMode -ne 57
$temporalPairTransaction = [uint64]0
$temporalPairOrdinal = [uint64]0
$temporalBuildReplayProofPairOrdinal = [uint64]0
$temporalSourceFrameLeft = [uint64]0
$temporalSourceFrameRight = [uint64]0
$temporalPoseSequenceLeft = [uint64]0
$temporalPoseSequenceRight = [uint64]0
$temporalSplitPhaseGatePose = [uint64]0
$temporalSplitPhaseCurrentPose = [uint64]0
$temporalSplitPhaseGateCall = [uint64]0
$temporalSplitPhaseCurrentCall = [uint64]0
$temporalSplitPhaseEpoch = [uint64]0
$temporalSplitPhaseGapMs = [double]0
$stereoProofComplete = if ($StereoMode -eq 58) {
  $stereoCameraDistanceReady -and
  $stereoPixelDistinct -and
  $stereoApplyGenerationFresh -and
  $parentDualProofComplete -and
  $mode58PrePauseContinuityReady
}
else {
  $stereoCameraDistanceReady -and
  $stereoPixelDistinct -and
  $stereoRawRgbReady -and
  $stereoApplyGenerationFresh -and
  $temporalPairReady -and
  $temporalPixelDistinct -and
  $temporalHostAccepted -and
  $temporalCapturePoseActive -and
  $temporalBuildReplayReceiptReady -and
  $temporalSplitStageReady -and
  $temporalSplitReadbackReady
}
$controllerProofComplete = $false
$deviceResetObserved = $false
$deviceResetRecovered = $false
$deviceResetSince = $null
$postResetProducerFresh = $true
$postResetHostFresh = $true
$resetRecoveryProducerBaseline = [uint64]0
$resetRecoveryHostBaseline = [uint64]0
$orderedUiToWorld = $false
$continuousWorldFrames = $false
$producerFirstTransaction = [uint64]0
$producerLatestTransaction = [uint64]0
$hostFirstTransaction = [uint64]0
$hostLatestTransaction = [uint64]0
$producerStallTransaction = [uint64]0
$hostStallTransaction = [uint64]0
$producerLastAdvancedAt = $null
$hostLastAdvancedAt = $null
$worldProgressStartedAt = $null
$elliottProofState = if ($ElliottCliProof) { "WAIT_UI" } else { "DISABLED" }
$elliottInitialNeutralAck = $false
$elliottMenuPulseCount = 0
$elliottActivePulse = ""
$elliottNextActionAt = $null
$elliottUiDeadline = $null
$elliottMenuDeadline = $null
$elliottInputProofDeadline = $null
$elliottInteractiveMenuSeen = $false
$elliottWorldLoadStarted = $false
$uiReasonSampleSeen = $false
$currentUiReasonFlags = [uint32]0
$worldLoadFullyReady = -not $ElliottCliProof
$worldLoadGameBoundaryIndex = -1
$worldLoadHostBoundaryIndex = -1
$worldLoadProducerBaselineTransaction = [uint64]0
$worldLoadHostBaselineTransaction = [uint64]0
$worldLoadReasonsZeroSince = $null
$worldLoadProducerFrames = 0
$worldLoadHostFrames = 0
$worldLoadSharedFrames = 0
$worldLoadSustainedMilliseconds = [int64]0
$pauseRoundTrip = $false
$pauseTestStarted = $false
$pauseUiObserved = $false
$gameplaySettleProducerBaseline = [uint64]0
$gameplaySettleHostBaseline = [uint64]0
$pauseStartPacketBaseline = [uint64]0
$pauseResumePacketBaseline = [uint64]0
$pauseUiProducerBaselineCount = 0
$pauseUiHostBaselineCount = 0
$pauseWorldProducerBaselineCount = 0
$pauseWorldHostBaselineCount = 0
$pauseWorldProducerBaselineTransaction = [uint64]0
$pauseWorldHostBaselineTransaction = [uint64]0
$elliottStickCommanded = $false
$elliottPoseSweepEnabled = $false
$elliottPoseSweepCompleted = $false
$elliottProofAutomationComplete = $false
$elliottCleanupFailures = @()
$notRespondingSince = $null
$cleanupFailures = @()
$archiveFailures = @()
$restoreFailures = @()
$priorRuntimeOverride = $env:XR_RUNTIME_JSON
$priorSimulatorHeadless = $env:OPENXR_SIMULATOR_HEADLESS
$priorSimulatorExternalFocus =
  $env:OPENXR_SIMULATOR_EXTERNAL_FOCUS_EXE

try {
  Write-Status "SAFETY: $($runtimeInfo.DisplayName) selected; SteamVR absent"
  Write-Status "SAFETY: runtimeKind=$($runtimeInfo.Kind)"
  Write-Status (
    "SAFETY: validated manifest pinned child-only with XR_RUNTIME_JSON; " +
    "system ActiveRuntime unchanged"
  )
  Write-Status (
    "SAFETY: normal Steam authentication only; audited launch-boundary " +
    "checks; no continuous SteamVR process polling"
  )
  $gtaLaunchOptions = @(Get-SteamAppLaunchOptions "12210")
  foreach ($launchOptions in $gtaLaunchOptions) {
    if (-not [string]::IsNullOrWhiteSpace($launchOptions)) {
      throw (
        "GTA IV Steam launch options must be empty for this audited route: " +
        "'$launchOptions'. Clear them in Steam before testing."
      )
    }
  }
  Write-Status "SAFETY: GTA IV Steam launch options are empty"
  Write-Status "ARTIFACT: ASI sha256=$(Get-Sha256 $CandidateAsi)"
  Write-Status "ARTIFACT: host sha256=$(Get-Sha256 $HostExe)"
  if ($ElliottCliProof) {
    New-Item `
      -ItemType Directory `
      -Path $ElliottDataDirectory `
      -Force | Out-Null
    Clear-ElliottCliProofCommands
    Write-Status (
      "ELLIOTT CLI: stale command/ack files cleared; file IPC only; " +
      "simulator-window control forbidden"
    )
  }

  $states = @(
    Save-State $InstalledAsi "gtaiv_dxvk_vr.asi"
    Save-State $BackendFile "gtaiv_dxvk_vr.backend"
    Save-State $StereoFile "gtaiv_dxvk_vr.stereo"
    Save-State $DisableFile "gtaiv_dxvk_vr.disable"
    Save-State $InstalledOpenVr "openvr_api.dll"
    Save-State $DxvkLog "GTAIV_d3d9.log"
  )
  if (Test-Path -LiteralPath $GameLog -PathType Leaf) {
    Copy-Item -LiteralPath $GameLog -Destination (Join-Path $BackupDirectory "previous-game.log") -Force
    Remove-Item -LiteralPath $GameLog -Force
  }
  if (Test-Path -LiteralPath $HostLog -PathType Leaf) {
    Copy-Item -LiteralPath $HostLog -Destination (Join-Path $BackupDirectory "previous-host.log") -Force
    Remove-Item -LiteralPath $HostLog -Force
  }
  if (Test-Path -LiteralPath $DxvkLog -PathType Leaf) {
    Remove-Item -LiteralPath $DxvkLog -Force
  }

  Copy-Item -LiteralPath $CandidateAsi -Destination $InstalledAsi -Force
  Set-Content -LiteralPath $BackendFile -Value "openxr" -Encoding ASCII -NoNewline
  Set-Content -LiteralPath $StereoFile -Value "$StereoMode" -Encoding ASCII -NoNewline
  Set-Content -LiteralPath $DisableFile -Value "1" -Encoding ASCII -NoNewline
  if (Test-Path -LiteralPath $InstalledOpenVr -PathType Leaf) {
    Remove-Item -LiteralPath $InstalledOpenVr -Force
  }
  $stereoDescription = switch ($StereoMode) {
    56 { "DrawScene same-frame L/R stereo candidate"; break }
    57 { "temporal native-frame L/R stereo"; break }
    58 { "Mode204 fused same-tick OpenXR first-person stereo"; break }
    default { "immersive mono baseline" }
  }
  Write-Status "DEPLOYED: backend=openxr stereo=$StereoMode ($stereoDescription)"
  Write-Status "OPENVR ISOLATED: disable sentinel present; openvr_api.dll removed and backed up"

  & $HostExe --self-test | Out-Null
  if ($LASTEXITCODE -ne 0) {
    throw "OpenXR host offline self-test failed."
  }
  $selfTestText = Read-Text $HostLog
  if ($selfTestText -notmatch
      "protocol=v6.*wvpProof=1.*drawSceneProof=1.*immersiveMono=1.*cpuMailbox=1.*cpuMailboxStereo=1.*cpuMailboxWorldUi=1.*stationaryUiQuad=1.*uiAspect=1.*routeSwitch=1.*heldFrameReuse=1.*exactPoseCache=1.*strictSwapchainWait=1.*referenceSpaceReset=1.*srgbDecode=1.*runtimeUntouched=1") {
    throw "OpenXR host baseline gates were not proven by the offline self-test."
  }
  Write-Status "OFFLINE GATES: protocol v6, CPU mono/stereo mailbox, stationary UI, and sRGB decode PASS"

  Assert-And-Stop-SteamVrAtLaunchBoundary
  try {
    $env:XR_RUNTIME_JSON = $runtime
    if ($runtimeInfo.Kind -eq "ElliottSimulator") {
      $env:OPENXR_SIMULATOR_HEADLESS = "1"
      $env:OPENXR_SIMULATOR_EXTERNAL_FOCUS_EXE = "GTAIV.exe"
      Write-Status "SIMULATOR: Elliott CLI/headless mode; no simulator-window control"
    }
    $hostArguments = @(
      Get-OpenXrHostArguments `
        -Mode $StereoMode `
        -TimeoutMs (([uint64]$MaxSeconds + 90) * 1000)
    )
    $hostProcess = Start-Process `
      -FilePath $HostExe `
      -ArgumentList $hostArguments `
      -WorkingDirectory (Split-Path -Parent $HostExe) `
      -WindowStyle Hidden `
      -RedirectStandardOutput (Join-Path $RunDirectory "host.stdout.log") `
      -RedirectStandardError (Join-Path $RunDirectory "host.stderr.log") `
      -PassThru
  }
  finally {
    if ($null -eq $priorRuntimeOverride) {
      Remove-Item Env:XR_RUNTIME_JSON -ErrorAction SilentlyContinue
    }
    else {
      $env:XR_RUNTIME_JSON = $priorRuntimeOverride
    }
    if ($null -eq $priorSimulatorHeadless) {
      Remove-Item Env:OPENXR_SIMULATOR_HEADLESS -ErrorAction SilentlyContinue
    }
    else {
      $env:OPENXR_SIMULATOR_HEADLESS = $priorSimulatorHeadless
    }
    if ($null -eq $priorSimulatorExternalFocus) {
      Remove-Item Env:OPENXR_SIMULATOR_EXTERNAL_FOCUS_EXE -ErrorAction SilentlyContinue
    }
    else {
      $env:OPENXR_SIMULATOR_EXTERNAL_FOCUS_EXE =
        $priorSimulatorExternalFocus
    }
  }
  Write-Status "HOST: started pid=$($hostProcess.Id)"

  $focusDeadline = [DateTime]::UtcNow.AddSeconds(30)
  while ([DateTime]::UtcNow -lt $focusDeadline) {
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
    throw "$($runtimeInfo.DisplayName) session did not reach FOCUSED."
  }
  Write-Status "$inspectionTarget READY: OpenXR session FOCUSED; Touch actions ready"
  if ($ElliottCliProof) {
    [void](Send-ElliottControllerSnapshot -Kind "NeutralBoth")
    $elliottInitialNeutralAck = $true
    Write-Status "ELLIOTT CLI: initial both-controller neutral established"
  }

  Start-AuditedGtaSteamAuthentication "requested GTA IV"

  $launchDeadline = [DateTime]::UtcNow.AddSeconds(120)
  $authRetryAt = [DateTime]::UtcNow.AddSeconds(60)
  $authRetryAttempted = $false
  while ([DateTime]::UtcNow -lt $launchDeadline) {
    $hostProcess.Refresh()
    if ($hostProcess.HasExited) {
      throw "OpenXR host exited while GTA authenticated, exit=$($hostProcess.ExitCode)"
    }
    if (-not $authRetryAttempted -and
        [DateTime]::UtcNow -ge $authRetryAt -and
        @(Get-ByName @("GTAIV")).Count -eq 0) {
      # Rockstar occasionally signs in but never forwards the first Steam
      # launch intent. Retire only this run's stuck prelauncher and submit the
      # same ordinary Steam app request once more. No title arguments are used.
      Stop-ByExactPath `
        @("PlayGTAIV") `
        $PlayGtaExe `
        "stalled Rockstar handoff retry"
      Stop-StaleRockstarLauncherSession "stalled Rockstar handoff retry"
      Start-AuditedGtaSteamAuthentication "Rockstar handoff retry"
      $authRetryAttempted = $true
      Write-Status (
        "AUTH RETRY: Rockstar did not hand off to GTAIV.exe; requested " +
        "app 12210 once more through regular Steam with no custom arguments"
      )
    }

    $gameText = Read-Text $GameLog
    if ($gameText -match "ASI loaded" -and
        $gameText -match "Backend: OpenXR" -and
        $gameText -match "StereoMode: $StereoMode") {
      $gameStarted = $true
      $backendOpenXr = $true
      $stereoModeReady = $true
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
    throw "No fresh GTA OpenXR/Mode $StereoMode ASI session appeared within 120 seconds."
  }
  Write-Status "GTA READY: fresh x86 ASI session backend=OpenXR stereo=$StereoMode"
  switch ($StereoMode) {
    56 {
      Write-Status "${inspectionTarget}: inspect same-frame distinct L/R world stereo, stationary menus, color, and Touch controls"
      break
    }
    57 {
      Write-Status "${inspectionTarget}: inspect temporal distinct L/R world stereo, stationary menus, color, and Touch controls"
      break
    }
    58 {
      Write-Status "${inspectionTarget}: inspect Mode204 fused first-person L/R world stereo, stationary menus, color, and Touch controls"
      break
    }
    default {
      Write-Status "${inspectionTarget}: inspect head-tracked mono world, stationary menus, color, and Touch controls"
    }
  }

  $runDeadline = [DateTime]::UtcNow.AddSeconds($MaxSeconds)
  $gameGoneSince = $null
  if ($ElliottCliProof) {
    $elliottUiDeadline = [DateTime]::UtcNow.AddSeconds(120)
    Write-Status (
      "ELLIOTT CLI: proof state WAIT_UI; " +
      "waiting for stationary producer/host menu route"
    )
  }
  while ([DateTime]::UtcNow -lt $runDeadline) {
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
      $responding = @($gameProcesses | Where-Object { $_.Responding }).Count -gt 0
      if ($responding) {
        $notRespondingSince = $null
      }
      elseif ($null -eq $notRespondingSince) {
        $notRespondingSince = [DateTime]::UtcNow
      }
      elseif (([DateTime]::UtcNow - $notRespondingSince).TotalSeconds -ge 15) {
        throw "GTA stopped responding for 15 seconds."
      }
    }

    $gameText = Read-Text $GameLog
    $hostText = Read-Text $HostLog
    $dxvkText = Read-Text $DxvkLog

    $uiReasonMatches = [regex]::Matches(
      $gameText,
      "UiState: presentation=[^\r\n]* reasons=0x([0-9A-Fa-f]+)"
    )
    if ($uiReasonMatches.Count -gt 0) {
      $uiReasonSampleSeen = $true
      $currentUiReasonFlags = [Convert]::ToUInt32(
        $uiReasonMatches[$uiReasonMatches.Count - 1].Groups[1].Value,
        16
      )
      if (($currentUiReasonFlags -band 0x1) -ne 0) {
        $elliottInteractiveMenuSeen = $true
      }
    }
    $interactiveMenuActive =
      ($currentUiReasonFlags -band 0x1) -ne 0
    $loadingOnlyActive =
      ($currentUiReasonFlags -band 0x2) -ne 0 -and
      -not $interactiveMenuActive
    $loadingAfterInteractiveMenu =
      $elliottInteractiveMenuSeen -and $loadingOnlyActive

    if ($gameText -match "DeviceReset: FAILED") {
      throw "GTA logged DeviceReset: FAILED."
    }
    $lastResetBefore = $gameText.LastIndexOf("DeviceReset: BEFORE Reset")
    $lastResetOk = $gameText.LastIndexOf("DeviceReset: OK")
    if ($lastResetBefore -ge 0) {
      if (-not $deviceResetObserved) {
        Write-Status "MARKER: GTA D3D9 Reset observed; eye resources released"
      }
      $deviceResetObserved = $true
      $resetPending = $lastResetOk -lt $lastResetBefore
      if ($resetPending) {
        if ($deviceResetRecovered -or $null -eq $deviceResetSince) {
          $deviceResetSince = [DateTime]::UtcNow
          $postResetProducerFresh = $false
          $postResetHostFresh = $false
          Write-Status "MARKER: GTA D3D9 Reset awaiting recovery"
        }
        $deviceResetRecovered = $false
      }
      else {
        if (-not $deviceResetRecovered) {
          $gameWorldAtReset = [regex]::Matches(
            $gameText,
            "OpenXRBridge: CPU mailbox transaction=(\d+)[^\r\n]*" +
            "presentation=world-"
          )
          $hostWorldAtReset = [regex]::Matches(
            $hostText,
            "GameBridge: CPU mailbox acquired transaction=(\d+)[^\r\n]*" +
            "presentation=world-"
          )
          $resetRecoveryProducerBaseline =
            if ($gameWorldAtReset.Count -gt 0) {
              [uint64]$gameWorldAtReset[
                $gameWorldAtReset.Count - 1
              ].Groups[1].Value
            }
            else {
              [uint64]0
            }
          $resetRecoveryHostBaseline =
            if ($hostWorldAtReset.Count -gt 0) {
              [uint64]$hostWorldAtReset[
                $hostWorldAtReset.Count - 1
              ].Groups[1].Value
            }
            else {
              [uint64]0
            }
          $postResetProducerFresh = $false
          $postResetHostFresh = $false
          $producerLastAdvancedAt = [DateTime]::UtcNow
          $hostLastAdvancedAt = [DateTime]::UtcNow
          Write-Status "MARKER: GTA D3D9 Reset recovered"
        }
        $deviceResetRecovered = $true
        $deviceResetSince = $null
      }
    }
    if ($deviceResetObserved -and
        -not $deviceResetRecovered -and
        $null -ne $deviceResetSince -and
        ([DateTime]::UtcNow - $deviceResetSince).TotalSeconds -ge 15) {
      throw "GTA D3D9 Reset did not recover within 15 seconds."
    }
    if ($dxvkText -match
        "alive losable resources.*Remaining resources") {
      throw "DXVK reported live default-pool resources blocking Reset."
    }

    if (-not $controllerReady -and
        $gameText -match "OpenXRController: Touch -> XInput ready") {
      $controllerReady = $true
      Write-Status "MARKER: Touch -> XInput controller bridge READY"
    }
    $controllerPattern =
      "OpenXRController: GTA consumed packet=(\d+) " +
      "buttons=0x([0-9A-Fa-f]+) LT=(\d+) RT=(\d+) " +
      "LS=\((-?\d+),(-?\d+)\) RS=\((-?\d+),(-?\d+)\) neutral=([01])"
    foreach ($inputMatch in [regex]::Matches($gameText, $controllerPattern)) {
      $packet = [uint64]$inputMatch.Groups[1].Value
      $buttons = [Convert]::ToUInt32($inputMatch.Groups[2].Value, 16)
      $leftStickX = [int]$inputMatch.Groups[5].Value
      $leftStickY = [int]$inputMatch.Groups[6].Value
      $neutral = $inputMatch.Groups[9].Value -eq "1"

      if (-not $neutral) {
        if (-not $controllerInputConsumed) {
          $controllerInputConsumed = $true
          Write-Status "MARKER: GTA consumed non-neutral OpenXR controller input"
        }
        if (($buttons -band 0x1000) -ne 0) {
          if (-not $controllerAConsumed) {
            $controllerAConsumed = $true
            Write-Status "MARKER: GTA consumed Elliott right-primary as XInput A (0x1000)"
          }
          if ($packet -gt $controllerAConsumedPacket) {
            $controllerAConsumedPacket = $packet
          }
        }
        if (($buttons -band 0x0010) -ne 0) {
          if (-not $controllerStartConsumed) {
            $controllerStartConsumed = $true
            Write-Status "MARKER: GTA consumed Elliott left-menu as XInput Start (0x0010)"
          }
          if ($packet -gt $controllerStartConsumedPacket) {
            $controllerStartConsumedPacket = $packet
          }
        }
        if ([Math]::Abs($leftStickX) -ge 16000 -or
            [Math]::Abs($leftStickY) -ge 16000) {
          if (-not $controllerStickConsumed) {
            $controllerStickConsumed = $true
            Write-Status (
              "MARKER: GTA consumed Elliott left-stick locomotion LS=({0},{1})" -f
              $leftStickX,
              $leftStickY
            )
          }
          if ($packet -gt $controllerStickConsumedPacket) {
            $controllerStickConsumedPacket = $packet
          }
        }
      }
      else {
        if (-not $controllerReleaseConsumed) {
          $controllerReleaseConsumed = $true
          Write-Status "MARKER: GTA consumed controller neutral/release"
        }
        if ($packet -gt $controllerNeutralConsumedPacket) {
          $controllerNeutralConsumedPacket = $packet
        }
        if ($controllerStickConsumedPacket -gt 0 -and
            $packet -gt $controllerStickConsumedPacket -and
            -not $controllerStickNeutralConsumed) {
          $controllerStickNeutralConsumed = $true
          Write-Status "MARKER: GTA consumed neutral after Elliott stick hold"
        }
      }
    }
    if (-not $bridgeReady -and $gameText -match "OpenXRBridge: READY") {
      $bridgeReady = $true
      Write-Status "MARKER: x86 CPU mailbox producer READY"
    }
    if (-not $worldCaptureReady) {
      if ($StereoMode -eq 56 -and
          $gameText -match "StereoOpenXR: DrawScene L/R pair") {
        $worldCaptureReady = $true
        $orderedUiToWorld = $stationaryUiQuad
        Write-Status "MARKER: guarded DrawScene L/R capture READY"
      }
      elseif ($StereoMode -eq 55 -and
          $gameText -match "StereoMono: center-eye capture pair") {
        $worldCaptureReady = $true
        $orderedUiToWorld = $stationaryUiQuad
        Write-Status "MARKER: center-eye mono capture READY"
      }
    }
    if ($StereoMode -eq 56) {
      $stereoPairMatches = [regex]::Matches(
        $gameText,
        "StereoOpenXR: DrawScene L/R pair #(\d+)[^\r\n]*" +
        "camDist=([0-9]+(?:\.[0-9]+)?)cm " +
        "applyGen=(\d+)/(\d+) rawRgbAbsDiff=(-?\d+) proof=draw-scene"
      )
      $priorApplyGenerationLeft = [uint32]0
      $priorApplyGenerationRight = [uint32]0
      foreach ($stereoPairMatch in $stereoPairMatches) {
        $pairOrdinal = [uint32]$stereoPairMatch.Groups[1].Value
        $distanceCm = [double]$stereoPairMatch.Groups[2].Value
        $applyGenerationLeft =
          [uint32]$stereoPairMatch.Groups[3].Value
        $applyGenerationRight =
          [uint32]$stereoPairMatch.Groups[4].Value
        $rawRgbAbsDiff = [int64]$stereoPairMatch.Groups[5].Value
        if ($pairOrdinal -gt $stereoAcceptedPairOrdinal) {
          $stereoAcceptedPairOrdinal = $pairOrdinal
        }
        if ($distanceCm -ge 1.0 -and $distanceCm -le 20.0) {
          $stereoCameraDistanceCm = $distanceCm
          if (-not $stereoCameraDistanceReady) {
            $stereoCameraDistanceReady = $true
            Write-Status (
              "MARKER: stereo camera separation plausible camDist={0:F2}cm" -f
              $distanceCm
            )
          }
        }
        if ($rawRgbAbsDiff -ge 1024 -and
            -not $stereoRawRgbReady) {
          $stereoRawRgbReady = $true
          $stereoRawRgbAbsDiff = $rawRgbAbsDiff
          Write-Status (
            "MARKER: raw stereo RGB proof PASS absDiff={0}" -f
            $rawRgbAbsDiff
          )
        }
        if ($priorApplyGenerationLeft -gt 0 -and
            $priorApplyGenerationRight -gt 0 -and
            $applyGenerationLeft -gt $priorApplyGenerationLeft -and
            $applyGenerationRight -gt $priorApplyGenerationRight -and
            -not $stereoApplyGenerationFresh) {
          $stereoApplyGenerationFresh = $true
          Write-Status (
            "MARKER: fresh stereo camera apply generations PASS " +
            "left={0}->{1} right={2}->{3}" -f
            $priorApplyGenerationLeft,
            $applyGenerationLeft,
            $priorApplyGenerationRight,
            $applyGenerationRight
          )
        }
        if ($applyGenerationLeft -gt $priorApplyGenerationLeft) {
          $priorApplyGenerationLeft = $applyGenerationLeft
        }
        if ($applyGenerationRight -gt $priorApplyGenerationRight) {
          $priorApplyGenerationRight = $applyGenerationRight
        }
        if ($applyGenerationLeft -gt $stereoApplyGenerationLeft) {
          $stereoApplyGenerationLeft = $applyGenerationLeft
        }
        if ($applyGenerationRight -gt $stereoApplyGenerationRight) {
          $stereoApplyGenerationRight = $applyGenerationRight
        }
      }
      if (-not $stereoPixelDistinct -and
          $gameText -match
          "OpenXRBridge: CPU mailbox transaction=[^\r\n]*" +
          "presentation=world-drawscene-stereo eyes=2[^\r\n]*pixelDistinct=1") {
        $stereoPixelDistinct = $true
        Write-Status "MARKER: Mode56 mailbox contains pixel-distinct L/R eyes"
      }
    }
    if ($StereoMode -eq 58) {
      if (-not $firstPersonHardHookReady -and
          $gameText -match
          "CamMatrix: \d+/4 hooked [^\r\n]*HARD FP on ped") {
        $firstPersonHardHookReady = $true
        Write-Status "MARKER: Mode58 hard first-person camera hooks READY"
      }
      if (-not $firstPersonGameplayReady -and
          $gameText -match
          "CamMatrix: gameplay ACTIVE [^\r\n]*FP lock armed") {
        $firstPersonGameplayReady = $true
        Write-Status "MARKER: Mode58 gameplay first-person lock ACTIVE"
      }
      foreach ($pedHideMatch in [regex]::Matches(
          $gameText,
          "PedHide: #\d+ ok=\d+ dead=0 native=1 " +
          "passes=(\d+) mode=58"
        )) {
        $pedHideSuccesses = [uint32]$pedHideMatch.Groups[1].Value
        if ($pedHideSuccesses -gt $firstPersonPedHideSuccesses) {
          $firstPersonPedHideSuccesses = $pedHideSuccesses
        }
      }
      $nativeHideProof = [regex]::Match(
        $gameText,
        "PedHide: native proof pass=(\d+) mode=58 components=5"
      )
      if ($nativeHideProof.Success) {
        $nativeHideProofPass =
          [uint32]$nativeHideProof.Groups[1].Value
        if ($nativeHideProofPass -gt
            $firstPersonPedHideSuccesses) {
          $firstPersonPedHideSuccesses =
            $nativeHideProofPass
        }
      }
      if (-not $firstPersonPedHideReady -and
          $firstPersonPedHideSuccesses -gt 0) {
        $firstPersonPedHideReady = $true
        Write-Status (
          "MARKER: Mode58 native player head hide operational ok={0}" -f
          $firstPersonPedHideSuccesses
        )
      }

      $fusedPairPattern =
        "StereoOpenXRFused: accepted pair #(\d+) " +
        "pose=(\d+) source=(\d+) sameTick=1 " +
        "parentDual=1 firstPerson=1 headHide=1 " +
        "[^\r\n]*camGen=(\d+)/(\d+) " +
        "camDist=([0-9]+(?:\.[0-9]+)?)cm[^\r\n]*" +
        "parent=0x4DE020"
      $fusedPairMatches = [regex]::Matches(
        $gameText,
        $fusedPairPattern
      )
      $priorFusedGenerationLeft = [uint32]0
      $priorFusedGenerationRight = [uint32]0
      foreach ($fusedPairMatch in $fusedPairMatches) {
        $fusedPairOrdinal =
          [uint32]$fusedPairMatch.Groups[1].Value
        $fusedPose = [uint64]$fusedPairMatch.Groups[2].Value
        $fusedSource = [uint64]$fusedPairMatch.Groups[3].Value
        $fusedGenerationLeft =
          [uint32]$fusedPairMatch.Groups[4].Value
        $fusedGenerationRight =
          [uint32]$fusedPairMatch.Groups[5].Value
        $fusedDistanceCm = [double]::Parse(
          $fusedPairMatch.Groups[6].Value,
          [Globalization.CultureInfo]::InvariantCulture
        )
        $fusedPairValid =
          $fusedPairOrdinal -gt 0 -and
          $fusedPose -gt 0 -and
          $fusedSource -gt 0 -and
          $fusedGenerationLeft -gt 0 -and
          $fusedGenerationRight -gt $fusedGenerationLeft -and
          $fusedDistanceCm -ge 1.0 -and
          $fusedDistanceCm -le 20.0
        if ($fusedPairValid) {
          $parentDualFusedPairReady = $true
          $parentDualPairPose = $fusedPose
          $parentDualPairSource = $fusedSource
          $stereoCameraDistanceReady = $true
          $stereoCameraDistanceCm = $fusedDistanceCm
          if ($fusedPairOrdinal -gt $stereoAcceptedPairOrdinal) {
            $stereoAcceptedPairOrdinal = $fusedPairOrdinal
          }
          if ($priorFusedGenerationLeft -gt 0 -and
              $priorFusedGenerationRight -gt 0 -and
              $fusedGenerationLeft -gt
                $priorFusedGenerationLeft -and
              $fusedGenerationRight -gt
                $priorFusedGenerationRight) {
            $stereoApplyGenerationFresh = $true
          }
          if ($fusedGenerationLeft -gt
              $priorFusedGenerationLeft) {
            $priorFusedGenerationLeft = $fusedGenerationLeft
          }
          if ($fusedGenerationRight -gt
              $priorFusedGenerationRight) {
            $priorFusedGenerationRight = $fusedGenerationRight
          }
          if ($fusedGenerationLeft -gt
              $stereoApplyGenerationLeft) {
            $stereoApplyGenerationLeft = $fusedGenerationLeft
          }
          if ($fusedGenerationRight -gt
              $stereoApplyGenerationRight) {
            $stereoApplyGenerationRight =
              $fusedGenerationRight
          }
        }
      }
      if (-not $worldCaptureReady -and
          $parentDualFusedPairReady) {
        $worldCaptureReady = $true
        $orderedUiToWorld = $stationaryUiQuad
        Write-Status (
          "MARKER: Mode58 fused Mode204 parent L/R capture READY"
        )
      }

      $parentDualProducerPattern =
        "OpenXRBridge: CPU mailbox transaction=(\d+)[^\r\n]*" +
        "pair=(\d+) presentation=world-parent-dual-stereo eyes=2 " +
        "sameTick=1 temporal=0 parentDual=1 fp=1 headHide=1" +
        "[^\r\n]*atomic=1[^\r\n]*pixelDistinct=1"
      foreach ($parentDualProducerMatch in [regex]::Matches(
          $gameText,
          $parentDualProducerPattern
        )) {
        $candidateParentTransaction =
          [uint64]$parentDualProducerMatch.Groups[1].Value
        $candidateParentPair =
          [uint64]$parentDualProducerMatch.Groups[2].Value
        if ($candidateParentTransaction -gt 0 -and
            $candidateParentPair -gt 0) {
          $parentDualProducerReady = $true
          $stereoPixelDistinct = $true
          if ($candidateParentTransaction -gt
              $parentDualProducerTransaction) {
            $parentDualProducerTransaction =
              $candidateParentTransaction
          }
        }
      }
      $parentDualProofComplete =
        $parentDualFusedPairReady -and
        $parentDualProducerReady -and
        $parentDualHostExactPoseReady -and
        $stereoCameraDistanceReady -and
        $stereoApplyGenerationFresh -and
        $stereoPixelDistinct
      $firstPersonProofComplete =
        $firstPersonHardHookReady -and
        $firstPersonGameplayReady -and
        $firstPersonPedHideReady -and
        $parentDualFusedPairReady
    }
    if ($StereoMode -eq 57) {
      $temporalBuildReplayProofPattern =
        "StereoOpenXRTemporal: pair #(\d+) " +
        "build=(\d+)/(\d+) pose=(\d+)/(\d+) " +
        "display=(-?\d+)/(-?\d+) rawRgbAbsDiff=(-?\d+) " +
        "changedPixels=(\d+) changedTiles=(\d+) " +
        "camDist=([0-9]+(?:\.[0-9]+)?)cm " +
        "camGen=(\d+)/(\d+) buildTid=(\d+) " +
        "execTid=(\d+) d3dTid=(\d+) " +
        "proof=buildroot-execroot-fifo-beginscene-endscene-entry"
      foreach ($receiptProofMatch in
          [regex]::Matches(
            $gameText,
            $temporalBuildReplayProofPattern
          )) {
        $receiptProofPair =
          [uint64]$receiptProofMatch.Groups[1].Value
        $receiptBuildLeft =
          [uint64]$receiptProofMatch.Groups[2].Value
        $receiptBuildRight =
          [uint64]$receiptProofMatch.Groups[3].Value
        $receiptPoseLeft =
          [uint64]$receiptProofMatch.Groups[4].Value
        $receiptPoseRight =
          [uint64]$receiptProofMatch.Groups[5].Value
        $receiptDisplayLeft =
          [int64]$receiptProofMatch.Groups[6].Value
        $receiptDisplayRight =
          [int64]$receiptProofMatch.Groups[7].Value
        $receiptRawRgb =
          [int64]$receiptProofMatch.Groups[8].Value
        $receiptChangedPixels =
          [uint32]$receiptProofMatch.Groups[9].Value
        $receiptChangedTiles =
          [uint32]$receiptProofMatch.Groups[10].Value
        $receiptDistanceCm =
          [double]$receiptProofMatch.Groups[11].Value
        $receiptCameraGenerationLeft =
          [uint32]$receiptProofMatch.Groups[12].Value
        $receiptCameraGenerationRight =
          [uint32]$receiptProofMatch.Groups[13].Value
        $receiptBuildThread =
          [uint32]$receiptProofMatch.Groups[14].Value
        $receiptExecThread =
          [uint32]$receiptProofMatch.Groups[15].Value
        $receiptD3dThread =
          [uint32]$receiptProofMatch.Groups[16].Value
        $receiptProofValid =
          $receiptBuildLeft -gt 0 -and
          $receiptBuildRight -eq $receiptBuildLeft + 1 -and
          $receiptPoseLeft -gt 0 -and
          $receiptPoseRight -gt $receiptPoseLeft -and
          $receiptDisplayLeft -gt 0 -and
          $receiptDisplayRight -gt $receiptDisplayLeft -and
          $receiptRawRgb -ge 1024 -and
          $receiptChangedPixels -ge 16 -and
          $receiptChangedTiles -ge 4 -and
          $receiptDistanceCm -ge 1.0 -and
          $receiptDistanceCm -le 20.0 -and
          $receiptCameraGenerationLeft -gt 0 -and
          $receiptCameraGenerationRight -gt
            $receiptCameraGenerationLeft -and
          $receiptBuildThread -gt 0 -and
          $receiptExecThread -gt 0 -and
          $receiptD3dThread -gt 0
        if ($receiptProofValid) {
          $stereoCameraDistanceReady = $true
          $stereoCameraDistanceCm = $receiptDistanceCm
          $stereoPixelDistinct = $true
          $stereoRawRgbReady = $true
          $stereoRawRgbAbsDiff = $receiptRawRgb
          $stereoChangedPixels = $receiptChangedPixels
          $stereoChangedTiles = $receiptChangedTiles
          $stereoApplyGenerationFresh = $true
          $stereoApplyGenerationLeft =
            $receiptCameraGenerationLeft
          $stereoApplyGenerationRight =
            $receiptCameraGenerationRight
          if ($receiptProofPair -gt $stereoAcceptedPairOrdinal) {
            $stereoAcceptedPairOrdinal = $receiptProofPair
          }
          if ($receiptProofPair -gt
              $temporalBuildReplayProofPairOrdinal) {
            $temporalBuildReplayProofPairOrdinal =
              $receiptProofPair
          }
          if (-not $temporalBuildReplayReceiptReady -and
              $receiptProofPair -ge 4) {
            $temporalBuildReplayReceiptReady = $true
            Write-Status (
              (
                "MARKER: Mode57 build-to-replay receipt PASS " +
                "pair={0} build={1}/{2} pose={3}/{4} " +
                "display={5}/{6} camGen={7}/{8} " +
                "buildTid={9} execTid={10} d3dTid={11} " +
                "camDist={12:F2}cm rawRgbAbsDiff={13} " +
                "changedPixels={14} changedTiles={15}"
              ) -f
                $receiptProofPair,
                $receiptBuildLeft,
                $receiptBuildRight,
                $receiptPoseLeft,
                $receiptPoseRight,
                $receiptDisplayLeft,
                $receiptDisplayRight,
                $receiptCameraGenerationLeft,
                $receiptCameraGenerationRight,
                $receiptBuildThread,
                $receiptExecThread,
                $receiptD3dThread,
                $receiptDistanceCm,
                $receiptRawRgb,
                $receiptChangedPixels,
                $receiptChangedTiles
            )
          }
        }
      }
      $temporalPairPattern =
        "OpenXRBridge: CPU mailbox transaction=(\d+)[^\r\n]*" +
        "pair=(\d+) presentation=world-temporal-stereo eyes=2 " +
        "sameTick=0 temporal=1 pose=(\d+)/(\d+) source=(\d+)/(\d+)"
      foreach ($temporalMatch in
          [regex]::Matches($gameText, $temporalPairPattern)) {
        $candidateTransaction =
          [uint64]$temporalMatch.Groups[1].Value
        $candidatePair = [uint64]$temporalMatch.Groups[2].Value
        $candidatePoseLeft =
          [uint64]$temporalMatch.Groups[3].Value
        $candidatePoseRight =
          [uint64]$temporalMatch.Groups[4].Value
        $candidateSourceLeft =
          [uint64]$temporalMatch.Groups[5].Value
        $candidateSourceRight =
          [uint64]$temporalMatch.Groups[6].Value
        if ($candidateSourceLeft -gt 0 -and
            $candidateSourceRight -gt $candidateSourceLeft -and
            $candidatePoseLeft -gt 0 -and
            $candidatePoseRight -gt $candidatePoseLeft) {
          if ($candidateTransaction -gt $temporalPairTransaction) {
            $temporalPairTransaction = $candidateTransaction
            $temporalPairOrdinal = $candidatePair
            $temporalSourceFrameLeft = $candidateSourceLeft
            $temporalSourceFrameRight = $candidateSourceRight
            $temporalPoseSequenceLeft = $candidatePoseLeft
            $temporalPoseSequenceRight = $candidatePoseRight
          }
          if (-not $temporalPairReady) {
            $temporalPairReady = $true
            $worldCaptureReady = $true
            $orderedUiToWorld = $stationaryUiQuad
            Write-Status (
              "MARKER: Mode57 ordered temporal L/R pair READY " +
              "transaction={0} pair={1} source={2}/{3} pose={4}/{5}" -f
              $candidateTransaction,
              $candidatePair,
              $candidateSourceLeft,
              $candidateSourceRight,
              $candidatePoseLeft,
              $candidatePoseRight
            )
          }
        }
      }
      $temporalProducerProofPattern =
        "OpenXRBridge: CPU mailbox transaction=\d+" +
        "(?=[^\r\n]*presentation=world-temporal-stereo)" +
        "(?=[^\r\n]*eyes=2)" +
        "[^\r\n]*pixelDistinct=1"
      if (-not $temporalPixelDistinct -and
          $gameText -match $temporalProducerProofPattern) {
        $temporalPixelDistinct = $true
        Write-Status (
          "MARKER: Mode57 mailbox contains temporal, pixel-distinct L/R eyes"
        )
      }
      $temporalSplitStagePattern =
        "OpenXRBridge: Mode57 CPU readback phase=L pair=(\d+) " +
        "poseGate=(\d+) leftQueueMs=[0-9]+(?:\.[0-9]+)? " +
        "transactionHeld=(\d+) replaced=[01] atomic=1"
      if (-not $temporalSplitStageReady -and
          $gameText -match $temporalSplitStagePattern) {
        $stagePair = [uint64]$Matches[1]
        $stagePose = [uint64]$Matches[2]
        $stageTransaction = [uint64]$Matches[3]
        if ($stagePair -gt 0 -and
            $stagePose -gt 0 -and
            $stageTransaction -gt 0) {
          $temporalSplitStageReady = $true
          Write-Status (
            "MARKER: Mode57 deferred-left CPU readback stage PASS " +
            "pair={0} pose={1} transactionHeld={2} atomic=1" -f
            $stagePair,
            $stagePose,
            $stageTransaction
          )
        }
      }
      $temporalSplitReadbackPattern =
        "OpenXRBridge: CPU mailbox transaction=(\d+) slot=\d+ " +
        "pair=(\d+) presentation=world-temporal-stereo eyes=2 " +
        "sameTick=0 temporal=1[^\r\n]*split=1[^\r\n]*" +
        "phaseGapMs=([0-9]+(?:\.[0-9]+)?)[^\r\n]*" +
        "phasePose=(\d+)/(\d+) phaseCall=(\d+)/(\d+) " +
        "phaseEpoch=(\d+) atomic=1[^\r\n]*pixelDistinct=1"
      foreach ($splitMatch in
          [regex]::Matches($gameText, $temporalSplitReadbackPattern)) {
        $candidatePhaseGapMs = [double]::Parse(
          $splitMatch.Groups[3].Value,
          [Globalization.CultureInfo]::InvariantCulture
        )
        $candidateGatePose = [uint64]$splitMatch.Groups[4].Value
        $candidateCurrentPose = [uint64]$splitMatch.Groups[5].Value
        $candidateGateCall = [uint64]$splitMatch.Groups[6].Value
        $candidateCurrentCall = [uint64]$splitMatch.Groups[7].Value
        $candidatePhaseEpoch = [uint64]$splitMatch.Groups[8].Value
        if ($candidatePhaseGapMs -gt 0.0 -and
            $candidatePhaseGapMs -le 250.0 -and
            $candidateGatePose -gt 0 -and
            $candidateCurrentPose -gt $candidateGatePose -and
            $candidateCurrentCall -gt $candidateGateCall -and
            $candidatePhaseEpoch -gt 0) {
          $temporalSplitPhaseGatePose = $candidateGatePose
          $temporalSplitPhaseCurrentPose = $candidateCurrentPose
          $temporalSplitPhaseGateCall = $candidateGateCall
          $temporalSplitPhaseCurrentCall = $candidateCurrentCall
          $temporalSplitPhaseEpoch = $candidatePhaseEpoch
          $temporalSplitPhaseGapMs = $candidatePhaseGapMs
          if (-not $temporalSplitReadbackReady) {
            $temporalSplitReadbackReady = $true
            Write-Status (
              "MARKER: Mode57 two-boundary atomic CPU readback PASS " +
              "phasePose={0}/{1} phaseCall={2}/{3} epoch={4} gapMs={5:F2}" -f
              $candidateGatePose,
              $candidateCurrentPose,
              $candidateGateCall,
              $candidateCurrentCall,
              $candidatePhaseEpoch,
              $candidatePhaseGapMs
            )
          }
        }
      }
      $temporalHostProofPattern =
        "GameBridge: CPU mailbox acquired transaction=\d+" +
        "(?=[^\r\n]*presentation=world-temporal-stereo)" +
        "(?=[^\r\n]*eyes=2)" +
        "(?=[^\r\n]*sameTick=0)" +
        "[^\r\n]*temporal=1"
      if (-not $temporalHostAccepted -and
          $hostText -match $temporalHostProofPattern) {
        $temporalHostAccepted = $true
        Write-Status "MARKER: Mode57 temporal L/R transaction accepted by host"
      }
      $temporalHostPosePattern =
        "XRHost: temporal pair capture poses active transaction=(\d+) " +
        "source=(\d+)/(\d+) pose=(\d+)/(\d+)"
      foreach ($hostPoseMatch in
          [regex]::Matches($hostText, $temporalHostPosePattern)) {
        $hostSourceLeft = [uint64]$hostPoseMatch.Groups[2].Value
        $hostSourceRight = [uint64]$hostPoseMatch.Groups[3].Value
        $hostPoseLeft = [uint64]$hostPoseMatch.Groups[4].Value
        $hostPoseRight = [uint64]$hostPoseMatch.Groups[5].Value
        if ($hostSourceLeft -gt 0 -and
            $hostSourceRight -gt $hostSourceLeft -and
            $hostPoseLeft -gt 0 -and
            $hostPoseRight -gt $hostPoseLeft) {
          if (-not $temporalCapturePoseActive) {
            $temporalCapturePoseActive = $true
            Write-Status (
              "MARKER: Mode57 per-eye capture poses active in OpenXR host"
            )
          }
        }
      }
    }
    $expectedProducerPresentation = switch ($StereoMode) {
      56 { "world-drawscene-stereo"; break }
      57 { "world-temporal-stereo"; break }
      58 { "world-parent-dual-stereo"; break }
      default { "world-immersive-mono" }
    }
    if (-not $producerTransaction -and
        $gameText -match "OpenXRBridge: CPU mailbox transaction=.*presentation=$expectedProducerPresentation") {
      $producerTransaction = $true
      Write-Status "MARKER: GTA published CPU mailbox $expectedProducerPresentation transaction"
    }
    $producerPattern =
      "OpenXRBridge: CPU mailbox transaction=(\d+).*presentation=$expectedProducerPresentation"
    $producerMatches = [regex]::Matches($gameText, $producerPattern)
    if ($producerMatches.Count -gt 0) {
      $producerLatestTransaction =
        [uint64]$producerMatches[$producerMatches.Count - 1].Groups[1].Value
      if ($producerFirstTransaction -eq 0) {
        $producerFirstTransaction =
          [uint64]$producerMatches[0].Groups[1].Value
      }
    }
    if ($producerLatestTransaction -gt $producerStallTransaction) {
      $producerStallTransaction = $producerLatestTransaction
      $producerLastAdvancedAt = [DateTime]::UtcNow
    }
    if (-not $hostConnected -and
        $hostText -match "GameBridge: CONNECTED FNVVR-style CPU mailbox") {
      $hostConnected = $true
      Write-Status "MARKER: host connected to CPU mailbox"
    }
    if (-not $hostAcquired -and
        $hostText -match "GameBridge: CPU mailbox acquired transaction=.*presentation=$expectedProducerPresentation") {
      $hostAcquired = $true
      Write-Status "MARKER: host acquired $expectedProducerPresentation mailbox frame"
    }
    $expectedHostReusePresentation = switch ($StereoMode) {
      56 { "world-stereo"; break }
      57 { "world-temporal-stereo"; break }
      58 { "world-parent-dual-stereo"; break }
      default { "world-headtracked-mono-projection" }
    }
    $hostSwapchainUpdateMatches = [regex]::Matches(
      $hostText,
      "XRHost: game swapchain updated transaction=\d+ " +
      "presentation=[^\s]+ updates=(\d+)"
    )
    if ($hostSwapchainUpdateMatches.Count -gt 0) {
      $hostSwapchainUpdatesLowerBound =
        [uint64]$hostSwapchainUpdateMatches[
        $hostSwapchainUpdateMatches.Count - 1
      ].Groups[1].Value
    }
    $hostSwapchainFreshWorldUpdateMatches = [regex]::Matches(
      $hostText,
      "XRHost: game swapchain updated transaction=(\d+) " +
      "presentation=$expectedHostReusePresentation updates=(\d+)"
    )
    $hostSwapchainFreshWorldUpdateSamples =
      $hostSwapchainFreshWorldUpdateMatches.Count
    if ($StereoMode -eq 58 -and
        -not $hostSwapchainFreshUpdatesReady -and
        $hostSwapchainFreshWorldUpdateSamples -ge 3) {
      $firstFreshWorldUpdateTransaction =
        [uint64]$hostSwapchainFreshWorldUpdateMatches[
        0
      ].Groups[1].Value
      $latestFreshWorldUpdateTransaction =
        [uint64]$hostSwapchainFreshWorldUpdateMatches[
        $hostSwapchainFreshWorldUpdateMatches.Count - 1
      ].Groups[1].Value
      if ($latestFreshWorldUpdateTransaction -gt
          $firstFreshWorldUpdateTransaction -and
          $parentDualHostExactPoseReady) {
        $hostSwapchainFreshUpdatesReady = $true
        Write-Status (
          "MARKER: Mode58 fresh OpenXR world swapchain updates READY " +
          "presentation=$expectedHostReusePresentation samples=" +
          "$hostSwapchainFreshWorldUpdateSamples transaction=" +
          "$firstFreshWorldUpdateTransaction->$latestFreshWorldUpdateTransaction"
        )
      }
    }
    $hostSwapchainReuseMatches = [regex]::Matches(
      $hostText,
      "XRHost: game swapchain reused transaction=(\d+) " +
      "presentation=$expectedHostReusePresentation " +
      "reusedHostFrames=(\d+)"
    )
    if ($hostSwapchainReuseMatches.Count -gt 0) {
      $latestHostSwapchainReuse =
        $hostSwapchainReuseMatches[
        $hostSwapchainReuseMatches.Count - 1
      ]
      $hostSwapchainReuseTransaction =
        [uint64]$latestHostSwapchainReuse.Groups[1].Value
      $hostSwapchainReuseFramesLowerBound =
        [uint64]$latestHostSwapchainReuse.Groups[2].Value
      if (-not $hostSwapchainReuseReady -and
          $hostSwapchainReuseFramesLowerBound -gt 0 -and
          ($StereoMode -ne 57 -or $temporalCapturePoseActive) -and
          ($StereoMode -ne 58 -or $parentDualHostExactPoseReady)) {
        $hostSwapchainReuseReady = $true
        Write-Status (
          "MARKER: unchanged OpenXR world swapchain reuse READY " +
          "presentation=$expectedHostReusePresentation " +
          "transaction=$hostSwapchainReuseTransaction " +
          "globalReuseLowerBound=$hostSwapchainReuseFramesLowerBound"
        )
      }
    }
    $hostPattern =
      "GameBridge: CPU mailbox acquired transaction=(\d+).*presentation=$expectedProducerPresentation"
    $hostMatches = [regex]::Matches($hostText, $hostPattern)
    if ($hostMatches.Count -gt 0) {
      $hostLatestTransaction =
        [uint64]$hostMatches[$hostMatches.Count - 1].Groups[1].Value
      if ($hostFirstTransaction -eq 0) {
        $hostFirstTransaction =
          [uint64]$hostMatches[0].Groups[1].Value
      }
    }
    if ($hostLatestTransaction -gt $hostStallTransaction) {
      $hostStallTransaction = $hostLatestTransaction
      $hostLastAdvancedAt = [DateTime]::UtcNow
    }
    if ($deviceResetObserved -and $deviceResetRecovered) {
      if (-not $postResetProducerFresh -and
          $producerLatestTransaction -gt
          $resetRecoveryProducerBaseline) {
        $postResetProducerFresh = $true
        Write-Status (
          "MARKER: fresh GTA world transaction after Reset OK " +
          "transaction=$producerLatestTransaction"
        )
      }
      if (-not $postResetHostFresh -and
          $hostLatestTransaction -gt $resetRecoveryHostBaseline) {
        $postResetHostFresh = $true
        Write-Status (
          "MARKER: fresh host world acquisition after Reset OK " +
          "transaction=$hostLatestTransaction"
        )
      }
    }
    if ($StereoMode -eq 56 -and
        -not $pauseTestStarted -and
        -not $mode56PrePauseContinuityReady) {
      $producerUiBoundary =
        $gameText.LastIndexOf("presentation=stationary-ui-quad")
      $hostUiBoundary =
        $hostText.LastIndexOf("presentation=stationary-ui-quad")
      $producerPrePause = Get-MonotonicTransactionSequence `
        -Text $gameText `
        -Pattern $producerPattern `
        -AfterIndex $producerUiBoundary
      $hostPrePause = Get-MonotonicTransactionSequence `
        -Text $hostText `
        -Pattern $hostPattern `
        -AfterIndex $hostUiBoundary
      $mode56PrePauseProducerTransactions = $producerPrePause.Count
      $mode56PrePauseHostTransactions = $hostPrePause.Count
      $prePausePairMatches = @($stereoPairMatches | Where-Object {
        $_.Index -gt $producerUiBoundary
      })
      if ($prePausePairMatches.Count -gt 0) {
        $mode56PrePauseFirstPairOrdinal =
          [uint32]$prePausePairMatches[0].Groups[1].Value
        $prePauseLatestPairOrdinal =
          [uint32]$prePausePairMatches[
            $prePausePairMatches.Count - 1
          ].Groups[1].Value
        $mode56PrePauseAcceptedPairSpan =
          $prePauseLatestPairOrdinal -
          $mode56PrePauseFirstPairOrdinal +
          1
      }
      $mode56PrePauseSharedTransaction =
        @($producerPrePause.Ids | Where-Object {
          $hostPrePause.Ids -contains $_
        }).Count -gt 0
      if ($mode56PrePauseAcceptedPairSpan -ge 120 -and
          $producerPrePause.Count -ge 2 -and
          $hostPrePause.Count -ge 2 -and
          $producerPrePause.Monotonic -and
          $hostPrePause.Monotonic -and
          $mode56PrePauseSharedTransaction) {
        $mode56PrePauseContinuityReady = $true
        $continuousWorldFrames = $true
        Write-Status (
          "MARKER: Mode56 uninterrupted pre-pause world continuity PASS " +
          "pairSpan={0} ({1}->{2}) producerIds={3} hostIds={4} shared=1" -f
          $mode56PrePauseAcceptedPairSpan,
          $mode56PrePauseFirstPairOrdinal,
          $stereoAcceptedPairOrdinal,
          $mode56PrePauseProducerTransactions,
          $mode56PrePauseHostTransactions
        )
      }
    }
    if ($StereoMode -eq 58 -and
        -not $pauseTestStarted -and
        -not $mode58PrePauseContinuityReady) {
      $producerUiBoundary =
        $gameText.LastIndexOf("presentation=stationary-ui-quad")
      $hostUiBoundary =
        $hostText.LastIndexOf("presentation=stationary-ui-quad")
      $producerPrePause = Get-MonotonicTransactionSequence `
        -Text $gameText `
        -Pattern $producerPattern `
        -AfterIndex $producerUiBoundary
      $hostPrePause = Get-MonotonicTransactionSequence `
        -Text $hostText `
        -Pattern $hostPattern `
        -AfterIndex $hostUiBoundary
      $mode58PrePauseProducerTransactions = $producerPrePause.Count
      $mode58PrePauseHostTransactions = $hostPrePause.Count
      $prePauseFusedPairMatches = @($fusedPairMatches | Where-Object {
        $_.Index -gt $producerUiBoundary
      })
      if ($prePauseFusedPairMatches.Count -gt 0) {
        $mode58PrePauseFirstPairOrdinal =
          [uint32]$prePauseFusedPairMatches[0].Groups[1].Value
        $prePauseLatestFusedPairOrdinal =
          [uint32]$prePauseFusedPairMatches[
            $prePauseFusedPairMatches.Count - 1
          ].Groups[1].Value
        $mode58PrePauseAcceptedPairSpan =
          $prePauseLatestFusedPairOrdinal -
          $mode58PrePauseFirstPairOrdinal +
          1
      }
      $mode58PrePauseSharedTransaction =
        @($producerPrePause.Ids | Where-Object {
          $hostPrePause.Ids -contains $_
        }).Count -gt 0
      if ($mode58PrePauseAcceptedPairSpan -ge 120 -and
          $producerPrePause.Count -ge 2 -and
          $hostPrePause.Count -ge 2 -and
          $producerPrePause.Monotonic -and
          $hostPrePause.Monotonic -and
          $mode58PrePauseSharedTransaction) {
        $mode58PrePauseContinuityReady = $true
        $continuousWorldFrames = $true
        Write-Status (
          "MARKER: Mode58 uninterrupted pre-pause fused world continuity PASS " +
          "pairSpan={0} ({1}->{2}) producerIds={3} hostIds={4} shared=1" -f
          $mode58PrePauseAcceptedPairSpan,
          $mode58PrePauseFirstPairOrdinal,
          $stereoAcceptedPairOrdinal,
          $mode58PrePauseProducerTransactions,
          $mode58PrePauseHostTransactions
        )
      }
    }
    $intentionalPauseActive = $elliottProofState -in @(
      "PAUSE_PRESS",
      "PAUSE_RELEASE",
      "WAIT_PAUSE_UI",
      "RESUME_PRESS",
      "RESUME_RELEASE",
      "WAIT_PAUSE_WORLD"
    )
    $currentStationaryUiRoute =
      (
        $gameText.LastIndexOf("presentation=stationary-ui-quad") -gt
        $gameText.LastIndexOf(
          "presentation=$expectedProducerPresentation")
      ) -or (
        $hostText.LastIndexOf("presentation=stationary-ui-quad") -gt
        $hostText.LastIndexOf(
          "presentation=$expectedProducerPresentation")
      )
    if ($currentStationaryUiRoute) {
      $producerLastAdvancedAt = [DateTime]::UtcNow
      $hostLastAdvancedAt = [DateTime]::UtcNow
    }
    if ($ElliottCliProof -and
        $worldCaptureReady -and
        ((-not $deviceResetObserved) -or $deviceResetRecovered) -and
        -not $currentStationaryUiRoute -and
        -not $intentionalPauseActive) {
      if ($null -eq $worldProgressStartedAt) {
        $worldProgressStartedAt = [DateTime]::UtcNow
      }
      $producerProgressAnchor = if ($null -ne $producerLastAdvancedAt) {
        $producerLastAdvancedAt
      }
      else {
        $worldProgressStartedAt
      }
      $hostProgressAnchor = if ($null -ne $hostLastAdvancedAt) {
        $hostLastAdvancedAt
      }
      else {
        $worldProgressStartedAt
      }
      if (([DateTime]::UtcNow - $producerProgressAnchor).TotalSeconds -ge 10) {
        throw (
          "FRAME_STALL: GTA producer world transaction did not advance " +
          "for 10 seconds (latest=$producerLatestTransaction)."
        )
      }
      if (([DateTime]::UtcNow - $hostProgressAnchor).TotalSeconds -ge 10) {
        throw (
          "FRAME_STALL: OpenXR host world transaction did not advance " +
          "for 10 seconds (latest=$hostLatestTransaction)."
        )
      }
    }
    if ($StereoMode -in @(55, 57) -and
        -not $continuousWorldFrames -and
        $producerFirstTransaction -gt 0 -and
        $hostFirstTransaction -gt 0 -and
        $producerLatestTransaction -ge $producerFirstTransaction + 120 -and
        $hostLatestTransaction -ge $hostFirstTransaction + 120) {
      $continuousWorldFrames = $true
      Write-Status (
        "MARKER: continuous world frames proven producer={0}->{1} host={2}->{3}" -f
        $producerFirstTransaction,
        $producerLatestTransaction,
        $hostFirstTransaction,
        $hostLatestTransaction
      )
    }
    $expectedHostPresentation = switch ($StereoMode) {
      56 { "world-stereo"; break }
      57 { "world-temporal-stereo"; break }
      58 { "world-parent-dual-stereo"; break }
      default { "world-headtracked-mono-projection" }
    }
    if ($StereoMode -eq 58 -and
        -not $parentDualHostExactPoseReady -and
        $hostText -match
          "XRHost: parent-dual exact capture pose active " +
          "transaction=\d+ sharedPose=(\d+) displayTime=-?\d+ " +
          "parentDual=1 fp=1 headHide=1 pixelDistinct=1") {
      $parentDualHostExactPoseReady = $true
      $gamePoseMatched = $true
      Write-Status (
        "MARKER: Mode58 parent-dual exact capture pose active in OpenXR host"
      )
    }
    elseif ($StereoMode -ne 58 -and
            -not $gamePoseMatched -and
            $hostText -match
              "XRHost: presentation=$expectedHostPresentation") {
      $gamePoseMatched = $true
      Write-Status (
        "MARKER: OpenXR $expectedHostPresentation presentation active"
      )
    }
    if (-not $srgbDecodeReady -and
        $hostText -match "CONNECTED FNVVR-style CPU mailbox.*sRGB decode SRV") {
      $srgbDecodeReady = $true
      Write-Status "MARKER: game texture sRGB decode SRV READY"
    }
    if (-not $swapchainFormatReady) {
      $swapchainFormatMatch = [regex]::Match(
        $hostText,
        "XRHost: swapchainFormat=(29|91)"
      )
      if ($swapchainFormatMatch.Success) {
        $swapchainFormat = [int64]$swapchainFormatMatch.Groups[1].Value
        $swapchainFormatReady = $true
        Write-Status (
          "MARKER: OpenXR sRGB swapchain format READY format={0}" -f
          $swapchainFormat
        )
      }
    }
    if (-not $colorPathReady -and
        $srgbDecodeReady -and
        $swapchainFormatReady) {
      $colorPathReady = $true
      Write-Status "MARKER: end-to-end sRGB color path READY"
    }
    if (-not $stationaryUiQuad -and
        $hostText -match "XRHost: stationary UI latched localPose=") {
      $stationaryUiQuad = $true
      Write-Status "MARKER: stationary menu/loading/phone quad READY"
    }
    if (-not $stationaryUiProducer -and
        $gameText -match
        "OpenXRBridge: CPU mailbox transaction=.*presentation=stationary-ui-quad") {
      $stationaryUiProducer = $true
      Write-Status "MARKER: GTA published stationary UI quad transaction"
    }
    if (-not $stationaryUiHost -and
        $hostText -match
        "GameBridge: CPU mailbox acquired transaction=.*presentation=stationary-ui-quad") {
      $stationaryUiHost = $true
      Write-Status "MARKER: host acquired stationary UI quad transaction"
    }
    if (-not $orderedUiToWorld -and
        $stationaryUiQuad -and
        $worldCaptureReady) {
      $uiIndex = $hostText.IndexOf(
        "XRHost: stationary UI latched localPose=")
      $worldIndex = $hostText.IndexOf(
        "XRHost: presentation=$expectedHostPresentation")
      if ($uiIndex -ge 0 -and
          $worldIndex -gt $uiIndex) {
        $orderedUiToWorld = $true
        Write-Status "MARKER: ordered stationary-UI -> world presentation transition"
      }
    }

    $worldRouteActive =
      $worldLoadFullyReady -and
      $uiReasonSampleSeen -and
      $currentUiReasonFlags -eq 0 -and
      $worldCaptureReady -and
      $producerTransaction -and
      $hostAcquired -and
      $gamePoseMatched
    if ($ElliottCliProof) {
      $proofNow = [DateTime]::UtcNow
      switch ($elliottProofState) {
        "WAIT_UI" {
          if ($proofNow -ge $elliottUiDeadline) {
            throw (
              "UI_ROUTE_TIMEOUT: an interactive GTA menu was not ready " +
              "within 120 seconds."
            )
          }
          if ($controllerReady -and
              $stationaryUiQuad -and
              $stationaryUiProducer -and
              $stationaryUiHost -and
              $interactiveMenuActive) {
            $elliottProofState = "MENU_PRESS"
            $elliottMenuDeadline = $proofNow.AddSeconds(45)
            $elliottNextActionAt = $proofNow
            Write-Status (
              "ELLIOTT CLI: interactive menu ready; proof state MENU_DRIVE; " +
              "Start/A pulses are command-file snapshots"
            )
          }
          break
        }
        "MENU_PRESS" {
          if ($proofNow -ge $elliottMenuDeadline -or
              $elliottMenuPulseCount -ge 10) {
            throw (
              "MENU_BLOCKED: no complete UI-to-world route after " +
              "$elliottMenuPulseCount bounded Elliott CLI pulses."
            )
          }
          if ($proofNow -ge $elliottNextActionAt) {
            # Start first proves the left-menu path before A can leave the
            # startup UI. Every later pulse alternates hands.
            $elliottActivePulse = if (($elliottMenuPulseCount % 2) -eq 0) {
              "MenuStart"
            }
            else {
              "PrimaryA"
            }
            [void](Send-ElliottControllerSnapshot -Kind $elliottActivePulse)
            $elliottNextActionAt = [DateTime]::UtcNow.AddMilliseconds(350)
            $elliottProofState = "MENU_RELEASE"
          }
          break
        }
        "MENU_RELEASE" {
          if ($proofNow -ge $elliottNextActionAt) {
            $releaseKind = if ($elliottActivePulse -eq "MenuStart") {
              "NeutralLeft"
            }
            else {
              "NeutralRight"
            }
            [void](Send-ElliottControllerSnapshot -Kind $releaseKind)
            ++$elliottMenuPulseCount
            $elliottNextActionAt = [DateTime]::UtcNow.AddSeconds(2)
            $elliottProofState = "MENU_SETTLE"
          }
          break
        }
        "MENU_SETTLE" {
          if ($proofNow -ge $elliottNextActionAt) {
            $menuExitObserved =
              $elliottMenuPulseCount -ge 2 -and
              $controllerAConsumed -and
              (
                $loadingAfterInteractiveMenu -or
                (
                  $uiReasonSampleSeen -and
                  $currentUiReasonFlags -eq 0 -and
                  -not $interactiveMenuActive
                )
              )
            if ($menuExitObserved) {
              $elliottWorldLoadStarted = $true
              $worldLoadFullyReady = $false
              $worldLoadGameBoundaryIndex = $gameText.Length - 1
              $worldLoadHostBoundaryIndex = $hostText.Length - 1
              $worldLoadProducerBaselineTransaction =
                $producerLatestTransaction
              $worldLoadHostBaselineTransaction =
                $hostLatestTransaction
              $worldLoadReasonsZeroSince = if (
                $uiReasonSampleSeen -and
                $currentUiReasonFlags -eq 0
              ) {
                $proofNow
              }
              else {
                $null
              }
              $worldLoadProducerFrames = 0
              $worldLoadHostFrames = 0
              $worldLoadSharedFrames = 0
              $worldLoadSustainedMilliseconds = [int64]0
              $elliottProofState = "WAIT_WORLD_LOAD"
              $elliottInputProofDeadline =
                [DateTime]::UtcNow.AddSeconds(120)
              Write-Status (
                (
                  "ELLIOTT CLI: menu exit observed after {0} pulses; " +
                  "WAIT_WORLD_LOAD requires current reasons=0 and at least " +
                  "3 fresh shared producer/host world frames sustained for 2 seconds; " +
                  "button automation paused for up to 120 seconds"
                ) -f $elliottMenuPulseCount
              )
            }
            else {
              $elliottProofState = "MENU_PRESS"
              $elliottNextActionAt = [DateTime]::UtcNow
            }
          }
          break
        }
        "WAIT_WORLD_LOAD" {
          if (-not $uiReasonSampleSeen -or
              $currentUiReasonFlags -ne 0) {
            $worldLoadReasonsZeroSince = $null
            $worldLoadGameBoundaryIndex = $gameText.Length - 1
            $worldLoadHostBoundaryIndex = $hostText.Length - 1
            $worldLoadProducerBaselineTransaction =
              $producerLatestTransaction
            $worldLoadHostBaselineTransaction =
              $hostLatestTransaction
            $worldLoadProducerFrames = 0
            $worldLoadHostFrames = 0
            $worldLoadSharedFrames = 0
            $worldLoadSustainedMilliseconds = [int64]0
          }
          else {
            if ($null -eq $worldLoadReasonsZeroSince) {
              $worldLoadReasonsZeroSince = $proofNow
            }
            $worldLoadEvidence = Get-SustainedWorldRouteEvidence `
              -GameText $gameText `
              -HostText $hostText `
              -ProducerPattern $producerPattern `
              -HostPattern $hostPattern `
              -GameAfterIndex $worldLoadGameBoundaryIndex `
              -HostAfterIndex $worldLoadHostBoundaryIndex
            $worldLoadProducerFrames =
              $worldLoadEvidence.ProducerCount
            $worldLoadHostFrames = $worldLoadEvidence.HostCount
            $worldLoadSharedFrames = $worldLoadEvidence.SharedCount
            $worldLoadSustainedMilliseconds = [int64](
              ($proofNow - $worldLoadReasonsZeroSince).TotalMilliseconds
            )
            $freshAfterMenuBoundary =
              $worldLoadEvidence.ProducerLatest -gt
                $worldLoadProducerBaselineTransaction -and
              $worldLoadEvidence.HostLatest -gt
                $worldLoadHostBaselineTransaction
            if ($worldLoadEvidence.Ready -and
                $freshAfterMenuBoundary -and
                $worldLoadSustainedMilliseconds -ge 2000 -and
                $worldCaptureReady -and
                $producerTransaction -and
                $hostAcquired -and
                $gamePoseMatched -and
                -not $currentStationaryUiRoute -and
                ((-not $deviceResetObserved) -or
                  $deviceResetRecovered)) {
              $worldLoadFullyReady = $true
              $orderedUiToWorld = $true
            }
          }
          if ($worldLoadFullyReady) {
            $elliottProofState = "WAIT_MENU_INPUT_PROOF"
            $elliottInputProofDeadline =
              [DateTime]::UtcNow.AddSeconds(10)
              Write-Status (
                (
                  "ELLIOTT CLI: WORLD LOAD READY reasons=0 sustainedMs={0} " +
                  "producerFrames={1} hostFrames={2} sharedFrames={3}; " +
                  "waiting for exact A/Start GTA consumption"
                ) -f
                  $worldLoadSustainedMilliseconds,
                  $worldLoadProducerFrames,
                  $worldLoadHostFrames,
                  $worldLoadSharedFrames
              )
          }
          elseif ($proofNow -ge $elliottInputProofDeadline) {
            throw (
              (
                "WORLD_LOAD_TIMEOUT: GTA accepted menu input but did not " +
                "produce a sustained fresh world route within 120 seconds " +
                "(reasonsSeen={0} reasons=0x{1} producerFrames={2} " +
                "hostFrames={3} sharedFrames={4})."
              ) -f
                $uiReasonSampleSeen,
                $currentUiReasonFlags.ToString("X"),
                $worldLoadProducerFrames,
                $worldLoadHostFrames,
                $worldLoadSharedFrames
            )
          }
          break
        }
        "WAIT_MENU_INPUT_PROOF" {
          if ($controllerAConsumed -and $controllerStartConsumed) {
            Send-ElliottPoseSweep -Enabled $true
            $elliottPoseSweepEnabled = $true
            $elliottProofState = "WAIT_PRE_PAUSE_WORLD_PROOF"
            $elliottInputProofDeadline =
              [DateTime]::UtcNow.AddSeconds(90)
            Write-Status (
              "ELLIOTT CLI: exact startup A/Start proof PASS; " +
              "controlled pose sweep started for uninterrupted " +
              "pre-pause world proof"
            )
          }
          elseif ($proofNow -ge $elliottInputProofDeadline) {
            throw (
              "INPUT_PROOF_FAILED: GTA did not consume both XInput A " +
              "(0x1000) and Start (0x0010)."
            )
          }
          break
        }
        "WAIT_PRE_PAUSE_WORLD_PROOF" {
          $prePauseStereoReady = switch ($StereoMode) {
            56 {
              $stereoCameraDistanceReady -and
              $stereoPixelDistinct -and
              $stereoRawRgbReady -and
              $stereoApplyGenerationFresh -and
              $mode56PrePauseContinuityReady
              break
            }
            57 {
              $temporalPairReady -and
              $temporalPixelDistinct -and
              $temporalHostAccepted -and
              $temporalCapturePoseActive -and
              $temporalBuildReplayReceiptReady -and
              $temporalSplitStageReady -and
              $temporalSplitReadbackReady -and
              $continuousWorldFrames
              break
            }
            58 {
              $parentDualProofComplete -and
              $firstPersonProofComplete -and
              $mode58PrePauseContinuityReady
              break
            }
            default { $continuousWorldFrames }
          }
          if ($worldRouteActive -and
              $continuousWorldFrames -and
              $prePauseStereoReady -and
              $colorPathReady -and
              $postResetProducerFresh -and
              $postResetHostFresh) {
            if ($elliottPoseSweepEnabled) {
              Send-ElliottPoseSweep -Enabled $false
              $elliottPoseSweepEnabled = $false
            }
            $elliottProofState = "WAIT_GAMEPLAY_SETTLE"
            $gameplaySettleProducerBaseline =
              $producerLatestTransaction
            $gameplaySettleHostBaseline =
              $hostLatestTransaction
            $elliottNextActionAt = [DateTime]::UtcNow.AddSeconds(3)
            $elliottInputProofDeadline =
              [DateTime]::UtcNow.AddSeconds(15)
            Write-Status (
              "ELLIOTT CLI: pre-pause world proof PASS; requiring " +
              "3 continuous, advancing gameplay seconds before in-game Start"
            )
          }
          elseif ($proofNow -ge $elliottInputProofDeadline) {
            throw (
              "WORLD_PROOF_TIMEOUT: uninterrupted pre-pause world/color/" +
              "stereo proof did not complete within 90 seconds."
            )
          }
          break
        }
        "WAIT_GAMEPLAY_SETTLE" {
          if ($proofNow -ge $elliottInputProofDeadline) {
            throw (
              "GAMEPLAY_SETTLE_TIMEOUT: a continuous gameplay route was not " +
              "held with advancing frames for 3 seconds before the pause test."
            )
          }
          if (-not $worldRouteActive) {
            $gameplaySettleProducerBaseline =
              $producerLatestTransaction
            $gameplaySettleHostBaseline =
              $hostLatestTransaction
            $elliottNextActionAt = $proofNow.AddSeconds(3)
          }
          elseif ($proofNow -ge $elliottNextActionAt) {
            $gameplayAdvanced =
              $producerLatestTransaction -gt
                $gameplaySettleProducerBaseline -and
              $hostLatestTransaction -gt
                $gameplaySettleHostBaseline
            if (-not $gameplayAdvanced) {
              $gameplaySettleProducerBaseline =
                $producerLatestTransaction
              $gameplaySettleHostBaseline =
                $hostLatestTransaction
              $elliottNextActionAt = $proofNow.AddSeconds(3)
            }
            else {
              $pauseUiProducerBaselineCount = (
                [regex]::Matches(
                  $gameText,
                  "OpenXRBridge: CPU mailbox transaction=[^\r\n]*" +
                  "presentation=stationary-ui-quad"
                )
              ).Count
              $pauseUiHostBaselineCount = (
                [regex]::Matches(
                  $hostText,
                  "GameBridge: CPU mailbox acquired transaction=[^\r\n]*" +
                  "presentation=stationary-ui-quad"
                )
              ).Count
              $pauseWorldProducerBaselineCount = $producerMatches.Count
              $pauseWorldHostBaselineCount = $hostMatches.Count
              $pauseWorldProducerBaselineTransaction =
                $producerLatestTransaction
              $pauseWorldHostBaselineTransaction =
                $hostLatestTransaction
              $pauseStartPacketBaseline = $controllerStartConsumedPacket
              $pauseTestStarted = $true
              [void](Send-ElliottControllerSnapshot -Kind "MenuStart")
              $elliottProofState = "PAUSE_RELEASE"
              $elliottNextActionAt =
                [DateTime]::UtcNow.AddMilliseconds(350)
              Write-Status (
                "ELLIOTT CLI: gameplay settle PASS with advancing producer/" +
                "host transactions; in-game Start pause pulse sent"
              )
            }
          }
          break
        }
        "PAUSE_RELEASE" {
          if ($proofNow -ge $elliottNextActionAt) {
            [void](Send-ElliottControllerSnapshot -Kind "NeutralLeft")
            $elliottProofState = "WAIT_PAUSE_UI"
            $elliottInputProofDeadline =
              [DateTime]::UtcNow.AddSeconds(15)
          }
          break
        }
        "WAIT_PAUSE_UI" {
          $pauseUiProducerCount = (
            [regex]::Matches(
              $gameText,
              "OpenXRBridge: CPU mailbox transaction=[^\r\n]*" +
              "presentation=stationary-ui-quad"
            )
          ).Count
          $pauseUiHostCount = (
            [regex]::Matches(
              $hostText,
              "GameBridge: CPU mailbox acquired transaction=[^\r\n]*" +
              "presentation=stationary-ui-quad"
            )
          ).Count
          if ($pauseUiProducerCount -gt $pauseUiProducerBaselineCount -and
              $pauseUiHostCount -gt $pauseUiHostBaselineCount -and
              $controllerStartConsumedPacket -gt
              $pauseStartPacketBaseline) {
            $pauseUiObserved = $true
            $pauseResumePacketBaseline = $controllerStartConsumedPacket
            [void](Send-ElliottControllerSnapshot -Kind "MenuStart")
            $elliottProofState = "RESUME_RELEASE"
            $elliottNextActionAt =
              [DateTime]::UtcNow.AddMilliseconds(350)
            Write-Status (
              "ELLIOTT CLI: distinct in-game pause UI observed; " +
              "resume Start pulse sent"
            )
          }
          elseif ($proofNow -ge $elliottInputProofDeadline) {
            throw (
              "PAUSE_ROUND_TRIP_FAILED: in-game Start did not produce " +
              "fresh stationary UI producer/host transactions."
            )
          }
          break
        }
        "RESUME_RELEASE" {
          if ($proofNow -ge $elliottNextActionAt) {
            [void](Send-ElliottControllerSnapshot -Kind "NeutralLeft")
            $elliottProofState = "WAIT_PAUSE_WORLD"
            $elliottInputProofDeadline =
              [DateTime]::UtcNow.AddSeconds(20)
          }
          break
        }
        "WAIT_PAUSE_WORLD" {
          if ($producerMatches.Count -gt
              $pauseWorldProducerBaselineCount -and
              $hostMatches.Count -gt
              $pauseWorldHostBaselineCount -and
              $producerLatestTransaction -gt
              $pauseWorldProducerBaselineTransaction -and
              $hostLatestTransaction -gt
              $pauseWorldHostBaselineTransaction -and
              $controllerStartConsumedPacket -gt
              $pauseResumePacketBaseline) {
            $pauseRoundTrip = $true
            $producerLastAdvancedAt = [DateTime]::UtcNow
            $hostLastAdvancedAt = [DateTime]::UtcNow
            if ($StereoMode -eq 58 -and
                -not $elliottPoseSweepEnabled) {
              Send-ElliottPoseSweep -Enabled $true
              $elliottPoseSweepEnabled = $true
              Write-Status (
                "ELLIOTT CLI: Mode58 pose sweep active for recorded walk"
              )
            }
            [void](Send-ElliottControllerSnapshot -Kind "StickForward")
            $elliottStickCommanded = $true
            $elliottProofState = "STICK_HOLD"
            $elliottNextActionAt =
              [DateTime]::UtcNow.AddSeconds($ElliottWalkSeconds)
            $walkPoseDetail = if ($StereoMode -eq 58) {
              "; Mode58 pose sweep active=$elliottPoseSweepEnabled"
            }
            else {
              ""
            }
            $walkReadyLine = (
              "ELLIOTT CLI: in-game world->pause UI->world round trip PASS; " +
              "holding left stick Y=0.8 for $ElliottWalkSeconds seconds" +
              $walkPoseDetail
            )
            Write-Status $walkReadyLine
            $walkReadyTemporary = "$ElliottWalkReadyMarker.tmp"
            Set-Content `
              -LiteralPath $walkReadyTemporary `
              -Value $walkReadyLine `
              -Encoding UTF8
            Move-Item `
              -LiteralPath $walkReadyTemporary `
              -Destination $ElliottWalkReadyMarker `
              -Force
          }
          elseif ($proofNow -ge $elliottInputProofDeadline) {
            throw (
              "PAUSE_ROUND_TRIP_FAILED: resume did not produce fresh " +
              "world producer/host transactions."
            )
          }
          break
        }
        "STICK_HOLD" {
          if ($proofNow -ge $elliottNextActionAt) {
            [void](Send-ElliottControllerSnapshot -Kind "NeutralLeft")
            $elliottProofState = "WAIT_STICK_INPUT_PROOF"
            $elliottInputProofDeadline =
              [DateTime]::UtcNow.AddSeconds(10)
            Write-Status (
              "ELLIOTT CLI: stick hold released; " +
              "waiting for GTA locomotion and post-stick neutral"
            )
          }
          break
        }
        "WAIT_STICK_INPUT_PROOF" {
          if ($controllerStickConsumed -and
              $controllerStickNeutralConsumed) {
            if ($StereoMode -eq 58) {
              if ($elliottPoseSweepEnabled) {
                Send-ElliottPoseSweep -Enabled $false
                $elliottPoseSweepEnabled = $false
              }
              $elliottPoseSweepCompleted = $true
              $elliottProofAutomationComplete = $true
              $elliottProofState = "COMPLETE"
              Write-Status (
                "ELLIOTT CLI: Mode58 recorded walk pose sweep and " +
                "stick/neutral automation COMPLETE"
              )
            }
            else {
              Send-ElliottPoseSweep -Enabled $true
              $elliottPoseSweepEnabled = $true
              $elliottProofState = "POSE_SWEEP"
              $elliottNextActionAt = [DateTime]::UtcNow.AddSeconds(8)
              Write-Status (
                "ELLIOTT CLI: stick/neutral proof PASS; " +
                "8-second head-pose sweep started"
              )
            }
          }
          elseif ($proofNow -ge $elliottInputProofDeadline) {
            throw (
              "INPUT_PROOF_FAILED: GTA did not consume the commanded " +
              "left-stick locomotion and its neutral release."
            )
          }
          break
        }
        "POSE_SWEEP" {
          if ($proofNow -ge $elliottNextActionAt) {
            Send-ElliottPoseSweep -Enabled $false
            $elliottPoseSweepEnabled = $false
            $elliottPoseSweepCompleted = $true
            $elliottProofAutomationComplete = $true
            $elliottProofState = "COMPLETE"
            Write-Status (
              "ELLIOTT CLI: command-file A/Start/stick/neutral/pose " +
              "automation COMPLETE"
            )
          }
          break
        }
        "COMPLETE" {
          break
        }
        default {
          throw "ELLIOTT CLI INTERNAL ERROR: unknown proof state '$elliottProofState'."
        }
      }
    }

    if ($gameText -match "Backend: OpenVR" -or
        $gameText -match "OpenVR: VR_Init" -or
        $hostText -match "FATAL:") {
      throw "Forbidden OpenVR or fatal OpenXR state was logged."
    }
    if ($StereoMode -eq 56 -and
        $gameText -match
        "StereoOpenXR: DrawScene stereo capture failed") {
      throw "Mode56 DrawScene stereo capture failed."
    }
    if ($StereoMode -eq 58 -and
        $gameText -match
        "Mode58: (?:FAIL|KILL)[^\r\n]*wrote stereo=57") {
      throw "Mode58 fused parent-dual path failed closed to Mode57."
    }
    if ($StereoMode -eq 58 -and
        $hostText -match
        "XRHost: parent-dual exact capture pose missing; projection layer held") {
      throw (
        "Mode58 exact capture pose was missing; the OpenXR host held the " +
        "projection layer instead of presenting a mismatched frame."
      )
    }
    $controllerProofComplete = if ($ElliottCliProof) {
      $controllerAConsumed -and
      $controllerStartConsumed -and
      $controllerStickConsumed -and
      $controllerStickNeutralConsumed -and
      $pauseRoundTrip -and
      $elliottProofAutomationComplete
    }
    else {
      $controllerInputConsumed -and $controllerReleaseConsumed
    }
    $stereoProofComplete = if ($StereoMode -eq 58) {
      $stereoCameraDistanceReady -and
      $stereoPixelDistinct -and
      $stereoApplyGenerationFresh -and
      $parentDualProofComplete -and
      $mode58PrePauseContinuityReady
    }
    else {
      $stereoCameraDistanceReady -and
      $stereoPixelDistinct -and
      $stereoRawRgbReady -and
      $stereoApplyGenerationFresh -and
      $temporalPairReady -and
      $temporalPixelDistinct -and
      $temporalHostAccepted -and
      $temporalCapturePoseActive -and
      $temporalBuildReplayReceiptReady -and
      $temporalSplitStageReady -and
      $temporalSplitReadbackReady
    }
    $hostPresentationContinuityReady = if ($StereoMode -eq 58) {
      $hostSwapchainFreshUpdatesReady
    }
    else {
      $hostSwapchainReuseReady
    }
    if ($StopWhenProofComplete -and
        ((-not $deviceResetObserved) -or $deviceResetRecovered) -and
        $postResetProducerFresh -and
        $postResetHostFresh -and
        $controllerProofComplete -and
        $stereoProofComplete -and
        ($StereoMode -ne 58 -or
          ($parentDualProofComplete -and
           $firstPersonProofComplete)) -and
        $bridgeReady -and
        $worldCaptureReady -and
        $continuousWorldFrames -and
        $orderedUiToWorld -and
        $colorPathReady -and
        $stationaryUiQuad -and
        $producerTransaction -and
        $hostConnected -and
        $hostAcquired -and
        $gamePoseMatched -and
        $hostPresentationContinuityReady) {
      $outcome = "PROOF_COMPLETE"
      Write-Status (
        "PROOF COMPLETE: reset, exact input, UI, continuous world frames, " +
        "color, stereo, first-person, and OpenXR presentation gates"
      )
      break
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
  if ($_.InvocationInfo.PositionMessage) {
    Write-Status (
      "ABORT LOCATION: " +
      $_.InvocationInfo.PositionMessage.Trim()
    )
  }
  if ($_.ScriptStackTrace) {
    Write-Status "ABORT STACK: $($_.ScriptStackTrace)"
  }
}
finally {
  # This nested try/finally is the restoration barrier. No process cleanup,
  # command-file cleanup, status write, or diagnostic archive failure can
  # bypass an attempt to restore every saved game-file state.
  try {
    try {
      if ($ElliottCliProof -and $hostProcess) {
        $cleanupHost = Get-Process `
          -Id $hostProcess.Id `
          -ErrorAction SilentlyContinue
        if ($cleanupHost) {
          try {
            [void](Send-ElliottControllerSnapshot `
              -Kind "NeutralBoth" `
              -Attempts 1)
            Write-StatusBestEffort (
              "ELLIOTT CLI CLEANUP: final both-controller neutral acknowledged"
            )
          }
          catch {
            $elliottCleanupFailures +=
              "final neutral: $($_.Exception.Message)"
            Write-StatusBestEffort (
              "ELLIOTT CLI CLEANUP WARNING: final neutral: " +
              $_.Exception.Message
            )
          }
          try {
            Send-ElliottPoseSweep -Enabled $false -Attempts 1
            $elliottPoseSweepEnabled = $false
            Write-StatusBestEffort "ELLIOTT CLI CLEANUP: pose sweep disabled"
          }
          catch {
            $elliottCleanupFailures +=
              "pose sweep disable: $($_.Exception.Message)"
            Write-StatusBestEffort (
              "ELLIOTT CLI CLEANUP WARNING: pose sweep disable: " +
              $_.Exception.Message
            )
          }
        }
      }
    }
    catch {
      $cleanupFailures +=
        "Elliott cleanup: $($_.Exception.Message)"
      Write-StatusBestEffort (
        "CLEANUP WARNING: Elliott cleanup: " + $_.Exception.Message
      )
    }

    try {
      Stop-ByName @("GTAIV") "end supervised GTA run"
    }
    catch {
      $cleanupFailures += "GTAIV cleanup: $($_.Exception.Message)"
      Write-StatusBestEffort (
        "CLEANUP WARNING: GTAIV cleanup: " + $_.Exception.Message
      )
    }
    try {
      Stop-ByExactPath `
        @("PlayGTAIV") `
        $PlayGtaExe `
        "end supervised GTA run"
    }
    catch {
      $cleanupFailures += "PlayGTAIV cleanup: $($_.Exception.Message)"
      Write-StatusBestEffort (
        "CLEANUP WARNING: PlayGTAIV cleanup: " + $_.Exception.Message
      )
    }
    try {
      Stop-StaleRockstarLauncherSession "end supervised GTA run"
    }
    catch {
      $cleanupFailures +=
        "Rockstar launcher cleanup: $($_.Exception.Message)"
      Write-StatusBestEffort (
        "CLEANUP WARNING: Rockstar launcher cleanup: " +
        $_.Exception.Message
      )
    }
    try {
      if ($hostProcess) {
        $liveHost = Get-Process `
          -Id $hostProcess.Id `
          -ErrorAction SilentlyContinue
        if ($liveHost) {
          Write-StatusBestEffort "STOP: OpenXR host pid=$($hostProcess.Id)"
          Stop-Process `
            -Id $hostProcess.Id `
            -Force `
            -ErrorAction SilentlyContinue
          try {
            [void]$liveHost.WaitForExit(5000)
          }
          catch {
            # Restore-State has its own bounded retry for lingering handles.
          }
        }
      }
    }
    catch {
      $cleanupFailures += "OpenXR host cleanup: $($_.Exception.Message)"
      Write-StatusBestEffort (
        "CLEANUP WARNING: OpenXR host cleanup: " + $_.Exception.Message
      )
    }
    try {
      if ($ElliottCliProof) {
        Clear-ElliottCliProofCommands
        Write-StatusBestEffort (
          "ELLIOTT CLI CLEANUP: command/ack files removed after host stop"
        )
      }
    }
    catch {
      $cleanupFailures +=
        "Elliott command-file cleanup: $($_.Exception.Message)"
      Write-StatusBestEffort (
        "CLEANUP WARNING: Elliott command-file cleanup: " +
        $_.Exception.Message
      )
    }
    try {
      Stop-ByName $SteamVrNames "unexpected SteamVR cleanup"
    }
    catch {
      $cleanupFailures += "SteamVR cleanup: $($_.Exception.Message)"
      Write-StatusBestEffort (
        "CLEANUP WARNING: SteamVR cleanup: " + $_.Exception.Message
      )
    }
    try {
      if (-not $steamWasRunning) {
        Stop-ByName @("steam", "steamwebhelper") "Steam started for this test"
      }
    }
    catch {
      $cleanupFailures += "Steam cleanup: $($_.Exception.Message)"
      Write-StatusBestEffort (
        "CLEANUP WARNING: Steam cleanup: " + $_.Exception.Message
      )
    }

    $diagnostics = @(
      [pscustomobject]@{
        Name = "GTA log"
        Source = $GameLog
        Destination = Join-Path $RunDirectory "gtaiv_dxvk_vr.log"
      }
      [pscustomobject]@{
        Name = "OpenXR host log"
        Source = $HostLog
        Destination = Join-Path $RunDirectory "gtaiv_xr_host.log"
      }
      [pscustomobject]@{
        Name = "DXVK log"
        Source = $DxvkLog
        Destination = Join-Path $RunDirectory "GTAIV_d3d9.log"
      }
    )
    foreach ($diagnostic in $diagnostics) {
      try {
        if (Test-Path -LiteralPath $diagnostic.Source -PathType Leaf) {
          Copy-Item `
            -LiteralPath $diagnostic.Source `
            -Destination $diagnostic.Destination `
            -Force
        }
      }
      catch {
        $archiveFailures +=
          "$($diagnostic.Name): $($_.Exception.Message)"
        Write-StatusBestEffort (
          "ARCHIVE WARNING: $($diagnostic.Name): " +
          $_.Exception.Message
        )
      }
    }
  }
  catch {
    $cleanupFailures +=
      "unexpected cleanup barrier failure: $($_.Exception.Message)"
    Write-StatusBestEffort (
      "CLEANUP WARNING: unexpected cleanup barrier failure: " +
      $_.Exception.Message
    )
  }
  finally {
    foreach ($state in $states) {
      try {
        Restore-State $state
      }
      catch {
        $restoreFailures += "$($state.Name): $($_.Exception.Message)"
        Write-StatusBestEffort (
          "RESTORE WARNING: $($state.Name): $($_.Exception.Message)"
        )
      }
    }
  }

  $controllerProofComplete = if ($ElliottCliProof) {
    $controllerAConsumed -and
    $controllerStartConsumed -and
    $controllerStickConsumed -and
    $controllerStickNeutralConsumed -and
    $pauseRoundTrip -and
    $elliottProofAutomationComplete
  }
  else {
    $controllerInputConsumed -and $controllerReleaseConsumed
  }
  $stereoProofComplete = if ($StereoMode -eq 58) {
    $stereoCameraDistanceReady -and
    $stereoPixelDistinct -and
    $stereoApplyGenerationFresh -and
    $parentDualProofComplete -and
    $mode58PrePauseContinuityReady
  }
  else {
    $stereoCameraDistanceReady -and
    $stereoPixelDistinct -and
    $stereoRawRgbReady -and
    $stereoApplyGenerationFresh -and
    $temporalPairReady -and
    $temporalPixelDistinct -and
    $temporalHostAccepted -and
    $temporalCapturePoseActive -and
    $temporalBuildReplayReceiptReady -and
    $temporalSplitStageReady -and
    $temporalSplitReadbackReady
  }
  $hostPresentationContinuityReady = if ($StereoMode -eq 58) {
    $hostSwapchainFreshUpdatesReady
  }
  else {
    $hostSwapchainReuseReady
  }
  $result = @(
    "outcome=$outcome"
    "failure=$failure"
    "runtime=$runtime"
    "runtimeKind=$($runtimeInfo.Kind)"
    "gameStarted=$gameStarted"
    "hostFocused=$hostFocused"
    "backendOpenXr=$backendOpenXr"
    "stereoMode=$StereoMode"
    "stereoModeReady=$stereoModeReady"
    "controllerReady=$controllerReady"
    "controllerInputConsumed=$controllerInputConsumed"
    "controllerReleaseConsumed=$controllerReleaseConsumed"
    "controllerAConsumed=$controllerAConsumed"
    "controllerAConsumedPacket=$controllerAConsumedPacket"
    "controllerStartConsumed=$controllerStartConsumed"
    "controllerStartConsumedPacket=$controllerStartConsumedPacket"
    "controllerStickConsumed=$controllerStickConsumed"
    "controllerStickConsumedPacket=$controllerStickConsumedPacket"
    "controllerStickNeutralConsumed=$controllerStickNeutralConsumed"
    "controllerNeutralConsumedPacket=$controllerNeutralConsumedPacket"
    "controllerProofComplete=$controllerProofComplete"
    "bridgeReady=$bridgeReady"
    "worldCaptureReady=$worldCaptureReady"
    "colorPathReady=$colorPathReady"
    "srgbDecodeReady=$srgbDecodeReady"
    "swapchainFormatReady=$swapchainFormatReady"
    "swapchainFormat=$swapchainFormat"
    "stationaryUiQuad=$stationaryUiQuad"
    "stationaryUiProducer=$stationaryUiProducer"
    "stationaryUiHost=$stationaryUiHost"
    "producerTransaction=$producerTransaction"
    "hostConnected=$hostConnected"
    "hostAcquired=$hostAcquired"
    "gamePoseMatched=$gamePoseMatched"
    "hostPresentationContinuityReady=$hostPresentationContinuityReady"
    "hostSwapchainReuseReady=$hostSwapchainReuseReady"
    "hostSwapchainFreshUpdatesReady=$hostSwapchainFreshUpdatesReady"
    "hostSwapchainFreshWorldUpdateSamples=$hostSwapchainFreshWorldUpdateSamples"
    "hostSwapchainReuseFramesLowerBound=$hostSwapchainReuseFramesLowerBound"
    "hostSwapchainUpdatesLowerBound=$hostSwapchainUpdatesLowerBound"
    "deviceResetObserved=$deviceResetObserved"
    "deviceResetRecovered=$deviceResetRecovered"
    "deviceResetHealthy=$((-not $deviceResetObserved) -or $deviceResetRecovered)"
    "postResetProducerFresh=$postResetProducerFresh"
    "postResetHostFresh=$postResetHostFresh"
    "orderedUiToWorld=$orderedUiToWorld"
    "continuousWorldFrames=$continuousWorldFrames"
    "producerTransactions=$producerFirstTransaction->$producerLatestTransaction"
    "hostTransactions=$hostFirstTransaction->$hostLatestTransaction"
    "stereoCameraDistanceReady=$stereoCameraDistanceReady"
    "stereoCameraDistanceCm=$stereoCameraDistanceCm"
    "stereoPixelDistinct=$stereoPixelDistinct"
    "stereoRawRgbReady=$stereoRawRgbReady"
    "stereoRawRgbAbsDiff=$stereoRawRgbAbsDiff"
    "stereoChangedPixels=$stereoChangedPixels"
    "stereoChangedTiles=$stereoChangedTiles"
    "stereoApplyGenerationFresh=$stereoApplyGenerationFresh"
    "stereoApplyGenerations=$stereoApplyGenerationLeft/$stereoApplyGenerationRight"
    "stereoAcceptedPairOrdinal=$stereoAcceptedPairOrdinal"
    "mode56PrePauseFirstPairOrdinal=$mode56PrePauseFirstPairOrdinal"
    "mode56PrePauseAcceptedPairSpan=$mode56PrePauseAcceptedPairSpan"
    "mode56PrePauseProducerTransactions=$mode56PrePauseProducerTransactions"
    "mode56PrePauseHostTransactions=$mode56PrePauseHostTransactions"
    "mode56PrePauseSharedTransaction=$mode56PrePauseSharedTransaction"
    "mode56PrePauseContinuityReady=$mode56PrePauseContinuityReady"
    "mode58PrePauseFirstPairOrdinal=$mode58PrePauseFirstPairOrdinal"
    "mode58PrePauseAcceptedPairSpan=$mode58PrePauseAcceptedPairSpan"
    "mode58PrePauseProducerTransactions=$mode58PrePauseProducerTransactions"
    "mode58PrePauseHostTransactions=$mode58PrePauseHostTransactions"
    "mode58PrePauseSharedTransaction=$mode58PrePauseSharedTransaction"
    "mode58PrePauseContinuityReady=$mode58PrePauseContinuityReady"
    "parentDualFusedPairReady=$parentDualFusedPairReady"
    "parentDualProducerReady=$parentDualProducerReady"
    "parentDualPairPose=$parentDualPairPose"
    "parentDualPairSource=$parentDualPairSource"
    "parentDualProducerTransaction=$parentDualProducerTransaction"
    "parentDualHostExactPoseReady=$parentDualHostExactPoseReady"
    "parentDualProofComplete=$parentDualProofComplete"
    "firstPersonHardHookReady=$firstPersonHardHookReady"
    "firstPersonGameplayReady=$firstPersonGameplayReady"
    "firstPersonPedHideReady=$firstPersonPedHideReady"
    "firstPersonPedHideSuccesses=$firstPersonPedHideSuccesses"
    "firstPersonProofComplete=$firstPersonProofComplete"
    "temporalPairReady=$temporalPairReady"
    "temporalPixelDistinct=$temporalPixelDistinct"
    "temporalHostAccepted=$temporalHostAccepted"
    "temporalCapturePoseActive=$temporalCapturePoseActive"
    "temporalBuildReplayReceiptReady=$temporalBuildReplayReceiptReady"
    "temporalSplitStageReady=$temporalSplitStageReady"
    "temporalSplitReadbackReady=$temporalSplitReadbackReady"
    "temporalSplitPhasePoses=$temporalSplitPhaseGatePose/$temporalSplitPhaseCurrentPose"
    "temporalSplitPhaseCalls=$temporalSplitPhaseGateCall/$temporalSplitPhaseCurrentCall"
    "temporalSplitPhaseEpoch=$temporalSplitPhaseEpoch"
    "temporalSplitPhaseGapMs=$($temporalSplitPhaseGapMs.ToString('F2', [Globalization.CultureInfo]::InvariantCulture))"
    "temporalBuildReplayProofPairOrdinal=$temporalBuildReplayProofPairOrdinal"
    "temporalPairTransaction=$temporalPairTransaction"
    "temporalPairOrdinal=$temporalPairOrdinal"
    "temporalSourceFrames=$temporalSourceFrameLeft/$temporalSourceFrameRight"
    "temporalPoseSequences=$temporalPoseSequenceLeft/$temporalPoseSequenceRight"
    "stereoProofComplete=$stereoProofComplete"
    "elliottCliProof=$([bool]$ElliottCliProof)"
    "elliottProofState=$elliottProofState"
    "elliottInitialNeutralAck=$elliottInitialNeutralAck"
    "elliottMenuPulseCount=$elliottMenuPulseCount"
    "elliottInteractiveMenuSeen=$elliottInteractiveMenuSeen"
    "elliottWorldLoadStarted=$elliottWorldLoadStarted"
    "uiReasonSampleSeen=$uiReasonSampleSeen"
    "currentUiReasonFlags=0x$($currentUiReasonFlags.ToString('X'))"
    "worldLoadFullyReady=$worldLoadFullyReady"
    "worldLoadProducerFrames=$worldLoadProducerFrames"
    "worldLoadHostFrames=$worldLoadHostFrames"
    "worldLoadSharedFrames=$worldLoadSharedFrames"
    "worldLoadSustainedMilliseconds=$worldLoadSustainedMilliseconds"
    "pauseTestStarted=$pauseTestStarted"
    "pauseUiObserved=$pauseUiObserved"
    "pauseRoundTrip=$pauseRoundTrip"
    "elliottStickCommanded=$elliottStickCommanded"
    "elliottWalkSeconds=$ElliottWalkSeconds"
    "elliottPoseSweepCompleted=$elliottPoseSweepCompleted"
    "elliottProofAutomationComplete=$elliottProofAutomationComplete"
    "elliottCleanupFailures=$($elliottCleanupFailures -join ' | ')"
    "cleanupFailures=$($cleanupFailures -join ' | ')"
    "archiveFailures=$($archiveFailures -join ' | ')"
    "restoreFailures=$($restoreFailures -join ' | ')"
  )
  Set-Content -LiteralPath $ResultPath -Value $result -Encoding UTF8
  Write-Status "RESULT: $outcome"
  if ($restoreFailures.Count -eq 0) {
    Write-Status "RESTORED: original ASI/backend/stereo/OpenVR state"
  }
  else {
    Write-Status "RESTORE INCOMPLETE: see result.txt"
  }
}

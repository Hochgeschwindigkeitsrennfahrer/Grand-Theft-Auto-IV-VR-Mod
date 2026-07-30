[CmdletBinding()]
param(
  [string]$GameDir =
    "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV",
  [string]$RuntimeManifest = "",
  [ValidateSet(55, 56, 57, 58, 204)]
  [uint32]$StereoMode = 58,
  [ValidateRange(120, 1800)]
  [uint32]$MaxSeconds = 600,
  [ValidateRange(15, 60)]
  [uint32]$WalkSeconds = 20,
  [ValidateRange(10, 12)]
  [uint32]$DurationSeconds = 10,
  [ValidateRange(30, 60)]
  [uint32]$FrameRate = 60,
  [ValidateRange(60, 600)]
  [uint32]$WaitSeconds = 360,
  [string]$FfmpegPath = "",
  [string]$ControlDirectory = "",
  [switch]$Authorized
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ($StereoMode -eq 204 -and
    -not $PSBoundParameters.ContainsKey("DurationSeconds")) {
  # Mode58 keeps its established 10-second default. Literal Mode204 uses the
  # longest capture supported by the existing non-activating recorder seam.
  $DurationSeconds = 12
}

function Read-TextIfPresent([string]$Path) {
  if (Test-Path -LiteralPath $Path -PathType Leaf) {
    try {
      return [IO.File]::ReadAllText($Path)
    }
    catch [IO.IOException] {
      # The launcher may briefly hold the append handle while the watcher fires.
      # The bounded event loop will retry without polling or changing proof state.
      return ""
    }
  }
  return ""
}

function Get-Mode204RetrySignal(
  [string]$StatusText,
  [uint32]$ExpectedWalkSeconds
) {
  $pattern =
    "(?m)^.*ELLIOTT CLI: advancing world recovered; starting clean " +
    [regex]::Escape("$ExpectedWalkSeconds") +
    "-second walk attempt 2/2\s*$"
  $retryMatch = [regex]::Match($StatusText, $pattern)
  if (-not $retryMatch.Success) {
    return ""
  }
  return $retryMatch.Value.Trim()
}

function Wait-Mode204RetrySignal(
  [string]$StatusPath,
  [System.Diagnostics.Process]$LauncherProcess,
  [uint32]$ExpectedWalkSeconds,
  [uint32]$TimeoutSeconds
) {
  $statusDirectory = Split-Path -Parent $StatusPath
  $statusName = Split-Path -Leaf $StatusPath
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  $watcher = New-Object IO.FileSystemWatcher($statusDirectory, $statusName)
  $watcher.IncludeSubdirectories = $false
  $watcher.NotifyFilter =
    [IO.NotifyFilters]::FileName -bor
    [IO.NotifyFilters]::LastWrite -bor
    [IO.NotifyFilters]::Size
  $watcher.EnableRaisingEvents = $true
  try {
    while ($true) {
      $retrySignal = Get-Mode204RetrySignal `
        -StatusText (Read-TextIfPresent $StatusPath) `
        -ExpectedWalkSeconds $ExpectedWalkSeconds
      if (-not [string]::IsNullOrWhiteSpace($retrySignal)) {
        return $retrySignal
      }
      $LauncherProcess.Refresh()
      if ($LauncherProcess.HasExited) {
        return ""
      }
      $remainingMilliseconds = [int][Math]::Ceiling(
        ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
      if ($remainingMilliseconds -le 0) {
        return ""
      }
      $waitMilliseconds = [Math]::Min(1000, $remainingMilliseconds)
      [void]$watcher.WaitForChanged(
        [IO.WatcherChangeTypes]::All,
        $waitMilliseconds)
    }
  }
  finally {
    $watcher.EnableRaisingEvents = $false
    $watcher.Dispose()
  }
}

function Write-TextAtomic(
  [string]$Path,
  [string[]]$Value
) {
  $temporaryPath = "$Path.tmp"
  Set-Content `
    -LiteralPath $temporaryPath `
    -Value $Value `
    -Encoding UTF8
  Move-Item `
    -LiteralPath $temporaryPath `
    -Destination $Path `
    -Force
}

function Start-ElliottCaptureAttempt(
  [string]$RecorderPath,
  [string]$WalkMarkerPath,
  [string]$OutputPath,
  [uint32]$LauncherPid,
  [uint32]$DurationSeconds,
  [uint32]$FrameRate,
  [uint32]$WaitSeconds,
  [string]$FfmpegExecutable,
  [string]$StandardOutputPath,
  [string]$StandardErrorPath,
  [string]$PidPath,
  [uint32]$Attempt
) {
  $recorderArguments = (
    "-NoLogo -NoProfile -ExecutionPolicy Bypass " +
    "-File `"$RecorderPath`" " +
    "-WalkMarkerPath `"$WalkMarkerPath`" " +
    "-OutputPath `"$OutputPath`" " +
    "-LauncherPid $LauncherPid " +
    "-DurationSeconds $DurationSeconds -FrameRate $FrameRate " +
    "-WaitSeconds $WaitSeconds " +
    "-FfmpegPath `"$FfmpegExecutable`""
  )
  $captureProcess = Start-Process `
    -FilePath "powershell.exe" `
    -ArgumentList $recorderArguments `
    -RedirectStandardOutput $StandardOutputPath `
    -RedirectStandardError $StandardErrorPath `
    -WindowStyle Hidden `
    -PassThru
  $captureProcess.Id | Set-Content -LiteralPath $PidPath
  return [pscustomobject]@{
    Attempt = $Attempt
    Process = $captureProcess
    VideoPath = $OutputPath
    ReceiptPath = "$OutputPath.capture.txt"
    StandardOutputPath = $StandardOutputPath
    StandardErrorPath = $StandardErrorPath
  }
}

function Get-UniqueResultValue(
  [string[]]$ResultLines,
  [string]$Key
) {
  $prefix = "$Key="
  $matches = @($ResultLines | Where-Object {
    $_.StartsWith($prefix, [StringComparison]::Ordinal)
  })
  if ($matches.Count -ne 1) {
    throw (
      "Expected exactly one '$Key' field in launcher result.txt; found " +
      "$($matches.Count)."
    )
  }
  return $matches[0].Substring($prefix.Length)
}

function Get-Mode204AcceptedWalkAttempt(
  [string[]]$ResultLines,
  [uint32]$ExpectedWalkSeconds
) {
  $requiredValues = [ordered]@{
    outcome = "PROOF_COMPLETE"
    stereoMode = "204"
    elliottWalkSeconds = "$ExpectedWalkSeconds"
    walkQualityReady = "True"
    sustainedPoseControllerProof = "True"
  }
  foreach ($requiredValue in $requiredValues.GetEnumerator()) {
    $actualValue = Get-UniqueResultValue `
      -ResultLines $ResultLines `
      -Key $requiredValue.Key
    if ($actualValue -ne $requiredValue.Value) {
      throw (
        "Mode204 accepted-attempt selection requires " +
        "$($requiredValue.Key)=$($requiredValue.Value); got " +
        "$($requiredValue.Key)=$actualValue."
      )
    }
  }
  $attemptText = Get-UniqueResultValue `
    -ResultLines $ResultLines `
    -Key "acceptedWalkAttempt"
  $acceptedAttempt = [uint32]0
  if (-not [uint32]::TryParse($attemptText, [ref]$acceptedAttempt) -or
      $acceptedAttempt -notin @(1, 2)) {
    throw (
      "Mode204 launcher result selected invalid acceptedWalkAttempt=" +
      "'$attemptText'; expected 1 or 2."
    )
  }
  return $acceptedAttempt
}

function Publish-Mode204AcceptedCapture(
  [string]$CandidatePath,
  [string]$CandidateReceiptPath,
  [string]$OutputPath,
  [string]$ResultPath,
  [uint32]$AcceptedAttempt
) {
  if (Test-Path -LiteralPath $OutputPath) {
    throw "Refusing to overwrite an existing proof video: $OutputPath"
  }
  $publishingPath = "$OutputPath.publishing"
  Copy-Item `
    -LiteralPath $CandidatePath `
    -Destination $publishingPath
  $candidateHash = (
    Get-FileHash -LiteralPath $CandidatePath -Algorithm SHA256
  ).Hash
  $publishingHash = (
    Get-FileHash -LiteralPath $publishingPath -Algorithm SHA256
  ).Hash
  if ($publishingHash -ne $candidateHash) {
    throw (
      "Accepted-attempt video copy failed hash verification: " +
      "candidate=$candidateHash publishing=$publishingHash"
    )
  }
  Move-Item `
    -LiteralPath $publishingPath `
    -Destination $OutputPath

  $publishedReceiptLines = @(
    Get-Content -LiteralPath $CandidateReceiptPath |
      ForEach-Object {
        if ($_.StartsWith("output=", [StringComparison]::Ordinal)) {
          "output=$OutputPath"
        }
        else {
          $_
        }
      }
  )
  $publishedReceiptLines += @(
    "acceptedWalkAttempt=$AcceptedAttempt",
    "selectedCandidate=$CandidatePath",
    "launcherResult=$ResultPath",
    "candidateSha256=$candidateHash",
    "outputSha256=$publishingHash",
    "publication=ACCEPTED_ATTEMPT_ONLY"
  )
  Write-TextAtomic `
    -Path "$OutputPath.capture.txt" `
    -Value $publishedReceiptLines
}

if (-not $Authorized) {
  throw "Live launch and recording are locked. Pass -Authorized for one run."
}
if ($StereoMode -eq 204 -and $WalkSeconds -ne 60) {
  throw "Literal Mode204 video proof requires WalkSeconds=60."
}
if ($WalkSeconds -lt ($DurationSeconds + 3)) {
  throw (
    "WalkSeconds must leave at least three seconds around the recording: " +
    "walk=$WalkSeconds capture=$DurationSeconds."
  )
}

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$launcherPath =
  Join-Path $PSScriptRoot "run-openxr-gta-steam-safe.ps1"
$recorderPath =
  Join-Path $PSScriptRoot "record-elliott-vr-preview.ps1"
$gameDirectory = (Resolve-Path -LiteralPath $GameDir).Path

if ([string]::IsNullOrWhiteSpace($RuntimeManifest)) {
  $RuntimeManifest = Join-Path (
    Join-Path $root "out-openxr\external\OpenXR-Simulator\bin"
  ) "openxr_simulator.json"
}
$runtimePath = (Resolve-Path -LiteralPath $RuntimeManifest).Path

if ([string]::IsNullOrWhiteSpace($FfmpegPath)) {
  $ffmpegCommand = Get-Command ffmpeg -ErrorAction SilentlyContinue
  if ($ffmpegCommand) {
    $FfmpegPath = $ffmpegCommand.Source
  }
  else {
    $wingetPackages = Join-Path (
      [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::LocalApplicationData
      )
    ) "Microsoft\WinGet\Packages"
    $ffmpegCandidate = Get-ChildItem `
      -LiteralPath $wingetPackages `
      -Recurse `
      -Filter "ffmpeg.exe" `
      -File `
      -ErrorAction SilentlyContinue |
      Where-Object {
        $_.FullName -match "Gyan\.FFmpeg_"
      } |
      Sort-Object LastWriteTime -Descending |
      Select-Object -First 1
    if (-not $ffmpegCandidate) {
      throw "ffmpeg.exe was not found in PATH or the WinGet package cache."
    }
    $FfmpegPath = $ffmpegCandidate.FullName
  }
}
$ffmpegExecutable = (Resolve-Path -LiteralPath $FfmpegPath).Path

if ([string]::IsNullOrWhiteSpace($ControlDirectory)) {
  $ControlDirectory = Join-Path (
    Join-Path $root "out-openxr"
  ) ("video-{0}-vr" -f (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$controlPath = [IO.Path]::GetFullPath($ControlDirectory)
if (Test-Path -LiteralPath $controlPath) {
  throw "Control directory already exists: $controlPath"
}
New-Item -ItemType Directory -Path $controlPath | Out-Null

$runRoot = Join-Path $root "out-openxr\runs"
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
$existingRuns = @(
  Get-ChildItem -LiteralPath $runRoot -Directory |
    ForEach-Object FullName
)

$launcherOutput = Join-Path $controlPath "launcher.stdout.log"
$launcherError = Join-Path $controlPath "launcher.stderr.log"
$videoPath = Join-Path $controlPath (
  "gtaiv-openxr-mode{0}-vr-sbs-{1}s.mp4" -f
    $StereoMode,
    $DurationSeconds
)
$attemptCaptureDirectory = Join-Path $controlPath "attempt-captures"
if ($StereoMode -eq 204) {
  New-Item `
    -ItemType Directory `
    -Path $attemptCaptureDirectory |
    Out-Null
}

$runWatcher = New-Object IO.FileSystemWatcher($runRoot, "*-steam-safe")
$runWatcher.IncludeSubdirectories = $false
$runWatcher.NotifyFilter = [IO.NotifyFilters]::DirectoryName
$runWatcher.EnableRaisingEvents = $true
$launcherProcess = $null
$runDirectory = $null
try {
  $launcherArguments = (
    "-NoLogo -NoProfile -ExecutionPolicy Bypass " +
    "-File `"$launcherPath`" " +
    "-GameDir `"$gameDirectory`" " +
    "-MaxSeconds $MaxSeconds -StereoMode $StereoMode " +
    "-RuntimeManifest `"$runtimePath`" " +
    "-ElliottCliProof -StopWhenProofComplete " +
    "-ElliottWalkSeconds $WalkSeconds -Authorized"
  )
  $launcherProcess = Start-Process `
    -FilePath "powershell.exe" `
    -ArgumentList $launcherArguments `
    -RedirectStandardOutput $launcherOutput `
    -RedirectStandardError $launcherError `
    -WindowStyle Hidden `
    -PassThru
  $launcherProcess.Id |
    Set-Content -LiteralPath (Join-Path $controlPath "launcher.pid")

  $runDeadline = [DateTime]::UtcNow.AddSeconds(15)
  while (-not $runDirectory -and [DateTime]::UtcNow -lt $runDeadline) {
    $runDirectory = Get-ChildItem -LiteralPath $runRoot -Directory |
      Where-Object {
        $existingRuns -notcontains $_.FullName
      } |
      Sort-Object CreationTime -Descending |
      Select-Object -First 1
    if ($runDirectory) {
      break
    }
    if ($launcherProcess.HasExited) {
      throw (
        "The launcher exited before creating a run directory. See " +
        "$launcherError"
      )
    }
    [void]$runWatcher.WaitForChanged(
      [IO.WatcherChangeTypes]::All,
      1000)
  }
}
finally {
  $runWatcher.EnableRaisingEvents = $false
  $runWatcher.Dispose()
}
if (-not $runDirectory) {
  throw "Timed out locating the supervised run directory."
}

$runPath = $runDirectory.FullName
$walkMarkerPath = Join-Path $runPath "elliott-walk-ready.marker"
$runPath | Set-Content -LiteralPath (Join-Path $controlPath "run.path")
$videoPath | Set-Content -LiteralPath (Join-Path $controlPath "video.path")
$controlPath |
  Set-Content -LiteralPath (
    Join-Path $root "out-openxr\latest-vr-video-control.txt"
  )

$captureAttempts = @()
$attemptOneVideoPath = if ($StereoMode -eq 204) {
  Join-Path $attemptCaptureDirectory (
    "gtaiv-openxr-mode204-vr-sbs-{0}s.attempt1.candidate.mp4" -f
      $DurationSeconds
  )
}
else {
  $videoPath
}
$attemptOneOutput = Join-Path $controlPath $(if ($StereoMode -eq 204) {
  "capture-attempt1.stdout.log"
}
else {
  "capture.stdout.log"
})
$attemptOneError = Join-Path $controlPath $(if ($StereoMode -eq 204) {
  "capture-attempt1.stderr.log"
}
else {
  "capture.stderr.log"
})
$attemptOnePid = Join-Path $controlPath $(if ($StereoMode -eq 204) {
  "capture-attempt1.pid"
}
else {
  "capture.pid"
})
$captureAttempts += Start-ElliottCaptureAttempt `
  -RecorderPath $recorderPath `
  -WalkMarkerPath $walkMarkerPath `
  -OutputPath $attemptOneVideoPath `
  -LauncherPid ([uint32]$launcherProcess.Id) `
  -DurationSeconds $DurationSeconds `
  -FrameRate $FrameRate `
  -WaitSeconds $WaitSeconds `
  -FfmpegExecutable $ffmpegExecutable `
  -StandardOutputPath $attemptOneOutput `
  -StandardErrorPath $attemptOneError `
  -PidPath $attemptOnePid `
  -Attempt 1

if ($StereoMode -eq 204) {
  $retrySignal = Wait-Mode204RetrySignal `
    -StatusPath (Join-Path $runPath "status.log") `
    -LauncherProcess $launcherProcess `
    -ExpectedWalkSeconds $WalkSeconds `
    -TimeoutSeconds ([uint32]($MaxSeconds + 120))
  if (-not [string]::IsNullOrWhiteSpace($retrySignal)) {
    $attemptTwoMarkerPath =
      Join-Path $controlPath "attempt2-walk-ready.marker"
    Write-TextAtomic `
      -Path $attemptTwoMarkerPath `
      -Value @($retrySignal)
    $attemptTwoVideoPath = Join-Path $attemptCaptureDirectory (
      "gtaiv-openxr-mode204-vr-sbs-{0}s.attempt2.candidate.mp4" -f
        $DurationSeconds
    )
    $captureAttempts += Start-ElliottCaptureAttempt `
      -RecorderPath $recorderPath `
      -WalkMarkerPath $attemptTwoMarkerPath `
      -OutputPath $attemptTwoVideoPath `
      -LauncherPid ([uint32]$launcherProcess.Id) `
      -DurationSeconds $DurationSeconds `
      -FrameRate $FrameRate `
      -WaitSeconds $WaitSeconds `
      -FfmpegExecutable $ffmpegExecutable `
      -StandardOutputPath (
        Join-Path $controlPath "capture-attempt2.stdout.log"
      ) `
      -StandardErrorPath (
        Join-Path $controlPath "capture-attempt2.stderr.log"
      ) `
      -PidPath (Join-Path $controlPath "capture-attempt2.pid") `
      -Attempt 2
  }
}

$captureWaitMilliseconds =
  [int](($WaitSeconds + $DurationSeconds + 30) * 1000)
$launcherWaitMilliseconds = [int](($MaxSeconds + 120) * 1000)
$captureOutcomes = @()
foreach ($captureAttempt in $captureAttempts) {
  $captureExited =
    $captureAttempt.Process.WaitForExit($captureWaitMilliseconds)
  $captureExitCode = if ($captureExited) {
    $captureAttempt.Process.ExitCode
  }
  else {
    $null
  }
  $captureOutcomes += [pscustomobject]@{
    Attempt = $captureAttempt.Attempt
    Exited = $captureExited
    ExitCode = $captureExitCode
    VideoPath = $captureAttempt.VideoPath
    ReceiptPath = $captureAttempt.ReceiptPath
    StandardOutputPath = $captureAttempt.StandardOutputPath
    StandardErrorPath = $captureAttempt.StandardErrorPath
  }
}
$launcherExited = $launcherProcess.WaitForExit($launcherWaitMilliseconds)
$launcherExitCode = if ($launcherExited) {
  $launcherProcess.ExitCode
}
else {
  $null
}

$failures = @()
if (-not $launcherExited) {
  $failures += "Launcher did not exit within its bounded wait."
}
elseif ($null -ne $launcherExitCode -and $launcherExitCode -ne 0) {
  $failures += (
    "Launcher exited with code $launcherExitCode; see " +
    "$launcherError."
  )
}
if ((Test-Path -LiteralPath $launcherError -PathType Leaf) -and
    (Get-Item -LiteralPath $launcherError).Length -ne 0) {
  $failures += "Launcher stderr is not empty: $launcherError."
}

$resultPath = Join-Path $runPath "result.txt"
$resultLines = @()
$acceptedWalkAttempt = if ($StereoMode -eq 204) {
  [uint32]0
}
else {
  [uint32]1
}
if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
  $failures += "Launcher result.txt is missing."
}
else {
  $resultLines = @(Get-Content -LiteralPath $resultPath)
  $requiredResultValues = [ordered]@{
    outcome = "PROOF_COMPLETE"
    failure = ""
    runtimeKind = "ElliottSimulator"
    gameStarted = "True"
    hostFocused = "True"
    backendOpenXr = "True"
    stereoMode = "$StereoMode"
    stereoModeReady = "True"
    controllerProofComplete = "True"
    bridgeReady = "True"
    worldCaptureReady = "True"
    colorPathReady = "True"
    srgbDecodeReady = "True"
    swapchainFormatReady = "True"
    stationaryUiQuad = "True"
    stationaryUiProducer = "True"
    stationaryUiHost = "True"
    producerTransaction = "True"
    hostConnected = "True"
    hostAcquired = "True"
    gamePoseMatched = "True"
    hostPresentationContinuityReady = "True"
    deviceResetHealthy = "True"
    postResetProducerFresh = "True"
    postResetHostFresh = "True"
    orderedUiToWorld = "True"
    continuousWorldFrames = "True"
    stereoProofComplete = "True"
    pauseRoundTrip = "True"
    elliottPoseSweepCompleted = "True"
    elliottProofAutomationComplete = "True"
  }
  if ($StereoMode -in @(58, 204)) {
    $requiredResultValues["hostSwapchainFreshUpdatesReady"] = "True"
    $requiredResultValues["parentDualFusedPairReady"] = "True"
    $requiredResultValues["parentDualProducerReady"] = "True"
    $requiredResultValues["parentDualHostExactPoseReady"] = "True"
    $requiredResultValues["parentDualProofComplete"] = "True"
    $requiredResultValues["firstPersonHardHookReady"] = "True"
    $requiredResultValues["firstPersonGameplayReady"] = "True"
    $requiredResultValues["firstPersonProofComplete"] = "True"
  }
  if ($StereoMode -eq 58) {
    $requiredResultValues["mode58PrePauseContinuityReady"] = "True"
    $requiredResultValues["firstPersonPedHideReady"] = "True"
  }
  elseif ($StereoMode -eq 204) {
    $requiredResultValues["mode204PrePauseContinuityReady"] = "True"
    $requiredResultValues["mode204AdapterReady"] = "True"
    $requiredResultValues["mode204GpuProducerReady"] = "True"
    $requiredResultValues["mode204GpuQueueReady"] = "True"
    $requiredResultValues["mode204GpuHostReady"] = "True"
    $requiredResultValues["mode204ConfigRestored"] = "True"
    $requiredResultValues["elliottQuestProfileReady"] = "True"
    $requiredResultValues["elliottQuestProfileReset"] = "True"
    $requiredResultValues["elliottWalkSeconds"] = "60"
    $requiredResultValues["walkQualityReady"] = "True"
    $requiredResultValues["sustainedPoseControllerProof"] = "True"
  }
  else {
    $requiredResultValues["hostSwapchainReuseReady"] = "True"
  }
  foreach ($requiredResult in $requiredResultValues.GetEnumerator()) {
    $requiredLine = "{0}={1}" -f
      $requiredResult.Key,
      $requiredResult.Value
    if ($resultLines -notcontains $requiredLine) {
      $failures += (
        "Launcher proof result is missing required field '$requiredLine'."
      )
    }
  }
  $swapchainFormatLine = @($resultLines | Where-Object {
    $_ -match '^swapchainFormat=(29|91)$'
  })
  if ($swapchainFormatLine.Count -ne 1) {
    $failures += (
      "Launcher proof did not report one sRGB swapchain format (29 or 91)."
    )
  }
  if ($StereoMode -eq 204) {
    try {
      $acceptedWalkAttempt = Get-Mode204AcceptedWalkAttempt `
        -ResultLines $resultLines `
        -ExpectedWalkSeconds $WalkSeconds
    }
    catch {
      $failures += $_.Exception.Message
    }
  }
}

foreach ($captureOutcome in $captureOutcomes) {
  if (-not $captureOutcome.Exited) {
    $failures += (
      "Recorder attempt $($captureOutcome.Attempt) did not exit within " +
      "its bounded wait."
    )
  }
}

$selectedCaptures = @($captureOutcomes | Where-Object {
  $_.Attempt -eq $acceptedWalkAttempt
})
$selectedCapture = $null
if ($acceptedWalkAttempt -eq 0) {
  $failures += "Launcher did not identify an accepted walk attempt."
}
elseif ($selectedCaptures.Count -ne 1) {
  $failures += (
    "Expected one captured candidate for launcher-accepted walk attempt " +
    "$acceptedWalkAttempt; found $($selectedCaptures.Count)."
  )
}
else {
  $selectedCapture = $selectedCaptures[0]
}

if ($StereoMode -eq 204 -and $acceptedWalkAttempt -in @(1, 2)) {
  foreach ($captureOutcome in $captureOutcomes) {
    $selection = if (
      $captureOutcome.Attempt -eq $acceptedWalkAttempt
    ) {
      "LAUNCHER_ACCEPTED_CANDIDATE"
    }
    else {
      "REJECTED_NOT_PROOF"
    }
    Write-TextAtomic `
      -Path "$($captureOutcome.VideoPath).selection.txt" `
      -Value @(
        "attempt=$($captureOutcome.Attempt)",
        "launcherAcceptedAttempt=$acceptedWalkAttempt",
        "selection=$selection",
        "proofPublished=False",
        "video=$($captureOutcome.VideoPath)",
        "launcherResult=$resultPath"
      )
  }
}

if ($null -ne $selectedCapture) {
  if ($null -ne $selectedCapture.ExitCode -and
      $selectedCapture.ExitCode -ne 0) {
    $failures += (
      "Recorder for accepted attempt $acceptedWalkAttempt exited with " +
      "code $($selectedCapture.ExitCode); see " +
      "$($selectedCapture.StandardErrorPath)."
    )
  }
  if ((Test-Path `
        -LiteralPath $selectedCapture.StandardErrorPath `
        -PathType Leaf) -and
      (Get-Item `
        -LiteralPath $selectedCapture.StandardErrorPath).Length -ne 0) {
    $failures += (
      "Recorder stderr for accepted attempt $acceptedWalkAttempt is not " +
      "empty: $($selectedCapture.StandardErrorPath)."
    )
  }
  if (-not (Test-Path `
        -LiteralPath $selectedCapture.ReceiptPath `
        -PathType Leaf)) {
    $failures += (
      "Recorder receipt for accepted attempt $acceptedWalkAttempt is " +
      "missing."
    )
  }
  else {
    $candidateReceiptLines =
      @(Get-Content -LiteralPath $selectedCapture.ReceiptPath)
    foreach ($requiredReceiptLine in @(
        "capture=PASS",
        "simulatorInput=CLI/headless",
        "windowActivation=False",
        "durationSeconds=$DurationSeconds",
        "frameRate=$FrameRate",
        "output=$($selectedCapture.VideoPath)"
      )) {
      if ($candidateReceiptLines -notcontains $requiredReceiptLine) {
        $failures += (
          "Accepted-attempt recorder receipt is missing " +
          "'$requiredReceiptLine'."
        )
      }
    }
  }
  if (-not (Test-Path `
        -LiteralPath $selectedCapture.VideoPath `
        -PathType Leaf)) {
    $failures += "Accepted-attempt recorded video is missing."
  }
  elseif ((Get-Item `
        -LiteralPath $selectedCapture.VideoPath).Length -le 1024) {
    $failures += "Accepted-attempt recorded video is unexpectedly small."
  }
}

if ($StereoMode -eq 204 -and
    $failures.Count -eq 0 -and
    $null -ne $selectedCapture) {
  try {
    Publish-Mode204AcceptedCapture `
      -CandidatePath $selectedCapture.VideoPath `
      -CandidateReceiptPath $selectedCapture.ReceiptPath `
      -OutputPath $videoPath `
      -ResultPath $resultPath `
      -AcceptedAttempt $acceptedWalkAttempt
    Write-TextAtomic `
      -Path "$($selectedCapture.VideoPath).selection.txt" `
      -Value @(
        "attempt=$acceptedWalkAttempt",
        "launcherAcceptedAttempt=$acceptedWalkAttempt",
        "selection=LAUNCHER_ACCEPTED_CANDIDATE",
        "proofPublished=True",
        "proofVideo=$videoPath",
        "video=$($selectedCapture.VideoPath)",
        "launcherResult=$resultPath"
      )
  }
  catch {
    $failures += (
      "Accepted-attempt proof publication failed: " +
      $_.Exception.Message
    )
  }
}

$receiptPath = "$videoPath.capture.txt"
if ($failures.Count -eq 0) {
  if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
    $failures += "Published recorder capture receipt is missing."
  }
  else {
    $receiptLines = @(Get-Content -LiteralPath $receiptPath)
    $publishedReceiptRequirements = @(
      "capture=PASS",
      "simulatorInput=CLI/headless",
      "windowActivation=False",
      "durationSeconds=$DurationSeconds",
      "frameRate=$FrameRate",
      "output=$videoPath"
    )
    if ($StereoMode -eq 204) {
      $publishedReceiptRequirements += @(
        "acceptedWalkAttempt=$acceptedWalkAttempt",
        "publication=ACCEPTED_ATTEMPT_ONLY"
      )
    }
    foreach ($requiredReceiptLine in $publishedReceiptRequirements) {
      if ($receiptLines -notcontains $requiredReceiptLine) {
        $failures += (
          "Published recorder receipt is missing '$requiredReceiptLine'."
        )
      }
    }
  }
  if (-not (Test-Path -LiteralPath $videoPath -PathType Leaf)) {
    $failures += "Published recorded video is missing."
  }
  elseif ((Get-Item -LiteralPath $videoPath).Length -le 1024) {
    $failures += "Published recorded video is unexpectedly small."
  }
}

if ($failures.Count -ne 0) {
  throw ($failures -join " ")
}

Write-Output "ELLIOTT FULL-GAME MODE $StereoMode VR RECORDING PASS"
Write-Output "Video: $videoPath"
Write-Output "Run: $runPath"
Write-Output "Receipt: $receiptPath"

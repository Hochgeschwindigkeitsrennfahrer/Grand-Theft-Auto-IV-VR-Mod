[CmdletBinding()]
param(
  [string]$GameDir =
    "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV",
  [string]$RuntimeManifest = "",
  [ValidateSet(55, 56, 57, 58)]
  [uint32]$StereoMode = 58,
  [ValidateRange(120, 1800)]
  [uint32]$MaxSeconds = 600,
  [ValidateRange(15, 30)]
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

if (-not $Authorized) {
  throw "Live launch and recording are locked. Pass -Authorized for one run."
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
$captureOutput = Join-Path $controlPath "capture.stdout.log"
$captureError = Join-Path $controlPath "capture.stderr.log"
$videoPath = Join-Path $controlPath (
  "gtaiv-openxr-mode{0}-vr-sbs-{1}s.mp4" -f
    $StereoMode,
    $DurationSeconds
)

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

$recorderArguments = (
  "-NoLogo -NoProfile -ExecutionPolicy Bypass " +
  "-File `"$recorderPath`" " +
  "-WalkMarkerPath `"$walkMarkerPath`" " +
  "-OutputPath `"$videoPath`" " +
  "-LauncherPid $($launcherProcess.Id) " +
  "-DurationSeconds $DurationSeconds -FrameRate $FrameRate " +
  "-WaitSeconds $WaitSeconds " +
  "-FfmpegPath `"$ffmpegExecutable`""
)
$captureProcess = Start-Process `
  -FilePath "powershell.exe" `
  -ArgumentList $recorderArguments `
  -RedirectStandardOutput $captureOutput `
  -RedirectStandardError $captureError `
  -WindowStyle Hidden `
  -PassThru
$captureProcess.Id |
  Set-Content -LiteralPath (Join-Path $controlPath "capture.pid")

$captureWaitMilliseconds =
  [int](($WaitSeconds + $DurationSeconds + 30) * 1000)
$launcherWaitMilliseconds = [int](($MaxSeconds + 120) * 1000)
$captureExited = $captureProcess.WaitForExit($captureWaitMilliseconds)
$launcherExited = $launcherProcess.WaitForExit($launcherWaitMilliseconds)
$captureExitCode = $captureProcess.ExitCode
$launcherExitCode = $launcherProcess.ExitCode

$failures = @()
if (-not $captureExited) {
  $failures += "Recorder did not exit within its bounded wait."
}
elseif ($null -ne $captureExitCode -and $captureExitCode -ne 0) {
  $failures += (
    "Recorder exited with code $captureExitCode; see $captureError."
  )
}
if (-not $launcherExited) {
  $failures += "Launcher did not exit within its bounded wait."
}
elseif ($null -ne $launcherExitCode -and $launcherExitCode -ne 0) {
  $failures += (
    "Launcher exited with code $launcherExitCode; see " +
    "$launcherError."
  )
}
if ((Test-Path -LiteralPath $captureError -PathType Leaf) -and
    (Get-Item -LiteralPath $captureError).Length -ne 0) {
  $failures += "Recorder stderr is not empty: $captureError."
}
if ((Test-Path -LiteralPath $launcherError -PathType Leaf) -and
    (Get-Item -LiteralPath $launcherError).Length -ne 0) {
  $failures += "Launcher stderr is not empty: $launcherError."
}

$resultPath = Join-Path $runPath "result.txt"
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
  if ($StereoMode -eq 58) {
    $requiredResultValues["hostSwapchainFreshUpdatesReady"] = "True"
    $requiredResultValues["mode58PrePauseContinuityReady"] = "True"
    $requiredResultValues["parentDualFusedPairReady"] = "True"
    $requiredResultValues["parentDualProducerReady"] = "True"
    $requiredResultValues["parentDualHostExactPoseReady"] = "True"
    $requiredResultValues["parentDualProofComplete"] = "True"
    $requiredResultValues["firstPersonHardHookReady"] = "True"
    $requiredResultValues["firstPersonGameplayReady"] = "True"
    $requiredResultValues["firstPersonPedHideReady"] = "True"
    $requiredResultValues["firstPersonProofComplete"] = "True"
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
}

$receiptPath = "$videoPath.capture.txt"
if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
  $failures += "Recorder capture receipt is missing."
}
else {
  $receiptLines = @(Get-Content -LiteralPath $receiptPath)
  foreach ($requiredReceiptLine in @(
      "capture=PASS",
      "simulatorInput=CLI/headless",
      "windowActivation=False",
      "durationSeconds=$DurationSeconds",
      "frameRate=$FrameRate",
      "output=$videoPath"
    )) {
    if ($receiptLines -notcontains $requiredReceiptLine) {
      $failures += (
        "Recorder receipt is missing '$requiredReceiptLine'."
      )
    }
  }
}
if (-not (Test-Path -LiteralPath $videoPath -PathType Leaf)) {
  $failures += "Recorded video is missing."
}
elseif ((Get-Item -LiteralPath $videoPath).Length -le 1024) {
  $failures += "Recorded video is unexpectedly small."
}

if ($failures.Count -ne 0) {
  throw ($failures -join " ")
}

Write-Output "ELLIOTT FULL-GAME MODE $StereoMode VR RECORDING PASS"
Write-Output "Video: $videoPath"
Write-Output "Run: $runPath"
Write-Output "Receipt: $receiptPath"

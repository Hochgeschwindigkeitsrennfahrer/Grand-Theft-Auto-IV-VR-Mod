[CmdletBinding()]
param(
  [string]$GameDir =
    "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV",
  [string]$RuntimeManifest = "",
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
  "gtaiv-openxr-mode57-vr-sbs-{0}s.mp4" -f $DurationSeconds
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
    "-MaxSeconds $MaxSeconds -StereoMode 57 " +
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

$failures = @()
if (-not $captureExited) {
  $failures += "Recorder did not exit within its bounded wait."
}
elseif ($captureProcess.ExitCode -ne 0) {
  $failures += (
    "Recorder exited with code $($captureProcess.ExitCode); see $captureError."
  )
}
if (-not $launcherExited) {
  $failures += "Launcher did not exit within its bounded wait."
}
elseif ($launcherProcess.ExitCode -ne 0) {
  $failures += (
    "Launcher exited with code $($launcherProcess.ExitCode); see " +
    "$launcherError."
  )
}

$resultPath = Join-Path $runPath "result.txt"
if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
  $failures += "Launcher result.txt is missing."
}
else {
  $resultLines = @(Get-Content -LiteralPath $resultPath)
  if ($resultLines -notcontains "outcome=PROOF_COMPLETE") {
    $failures += "Launcher did not report outcome=PROOF_COMPLETE."
  }
}

$receiptPath = "$videoPath.capture.txt"
if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
  $failures += "Recorder capture receipt is missing."
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

Write-Output "ELLIOTT FULL-GAME VR RECORDING PASS"
Write-Output "Video: $videoPath"
Write-Output "Run: $runPath"
Write-Output "Receipt: $receiptPath"

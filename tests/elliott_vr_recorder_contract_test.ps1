$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Assert-NoAutomaticHostVariable(
  [Parameter(Mandatory = $true)]
  [System.Management.Automation.Language.Ast]$Ast,
  [Parameter(Mandatory = $true)]
  [string]$Label
) {
  $hostVariables = @(
    $Ast.FindAll(
      {
        param($node)
        $node -is
          [System.Management.Automation.Language.VariableExpressionAst] -and
        $node.VariablePath.UserPath -ieq "Host"
      },
      $true
    )
  )
  if ($hostVariables.Count -ne 0) {
    $locations = $hostVariables |
      ForEach-Object {
        "{0}:{1}" -f
          $_.Extent.StartLineNumber,
          $_.Extent.StartColumnNumber
      }
    throw (
      "$Label uses reserved automatic variable `$Host at " +
      ($locations -join ", ")
    )
  }
}

$recorderPath = Join-Path `
  $PSScriptRoot "..\scripts\record-elliott-vr-preview.ps1"
$recorder = Get-Content -LiteralPath $recorderPath -Raw

$tokens = $null
$parseErrors = $null
$recorderAst = [System.Management.Automation.Language.Parser]::ParseFile(
  (Resolve-Path -LiteralPath $recorderPath).Path,
  [ref]$tokens,
  [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
  throw (
    "Recorder has PowerShell parser errors: " +
    (($parseErrors | ForEach-Object Message) -join " | ")
  )
}
Assert-NoAutomaticHostVariable -Ast $recorderAst -Label "Recorder"

$requiredPatterns = [ordered]@{
  WalkMarkerParameter = '\[string\]\$WalkMarkerPath'
  TenToTwelveSeconds = '\[ValidateRange\(10, 12\)\]'
  FileSystemWatcher = 'New-Object IO\.FileSystemWatcher'
  EventWait = '\.WaitForChanged\('
  BoundedWait = '\[Math\]::Min\(1000, \$remainingMilliseconds\)'
  WatcherDispose = '\$watcher\.Dispose\(\)'
  ProcessScopedWindow =
    'FindSimulatorPreview\(\s*\[uint32\]\$hostProcess\.Id\)'
  ShowNoActivate = 'SW_SHOWNOACTIVATE'
  WindowNoActivate = 'SWP_NOACTIVATE'
  HideAfterCapture =
    'finally \{\s*\[ElliottPreviewCapture\.NativeMethods\]::Hide\(\$hwnd\)'
  HwndCapture = '"gfxcapture=hwnd=\$\(\$hwnd\.ToInt64\(\)\)'
  NoCursor = 'capture_cursor=0'
  RecordingExtent = 'width=3840:height=1920:resize_mode=scale_aspect'
  Nvenc = '-c:v h264_nvenc'
  SideBySideMetadata = 'stereo_mode=left_right'
  NativeStderrScope = '\$ErrorActionPreference = "Continue"'
  ExitCodeSnapshot = '\$ffmpegExitCode = \$LASTEXITCODE'
  ExitCodeCheck = 'if \(\$ffmpegExitCode -ne 0\)'
  ComposedPreviewReceipt = '"source=Elliott OpenXR composed SBS preview"'
  NoWindowActivationReceipt = '"windowActivation=False"'
}
foreach ($entry in $requiredPatterns.GetEnumerator()) {
  if ($recorder -notmatch $entry.Value) {
    throw "Missing Elliott VR recorder contract: $($entry.Key)"
  }
}

$forbiddenPatterns = [ordered]@{
  StatusLogPolling = 'Get-Content[^\r\n]*StatusPath'
  SimulatorSendKeys = 'SendKeys'
  SimulatorAppActivate = 'AppActivate'
  ForegroundWindow = 'SetForegroundWindow'
  SteamVr = '(?i)(vrserver|vrmonitor|vrcompositor|steamvr)'
  CpuPixelFormatFilter = '-vf\s+"fps=.*format=nv12"'
}
foreach ($entry in $forbiddenPatterns.GetEnumerator()) {
  if ($recorder -match $entry.Value) {
    throw "Forbidden Elliott VR recorder contract found: $($entry.Key)"
  }
}

$wrapperPath = Join-Path `
  $PSScriptRoot "..\scripts\run-and-record-elliott-vr.ps1"
$wrapper = Get-Content -LiteralPath $wrapperPath -Raw
$wrapperTokens = $null
$wrapperParseErrors = $null
$wrapperAst = [System.Management.Automation.Language.Parser]::ParseFile(
  (Resolve-Path -LiteralPath $wrapperPath).Path,
  [ref]$wrapperTokens,
  [ref]$wrapperParseErrors
)
if ($wrapperParseErrors.Count -ne 0) {
  throw (
    "Recording wrapper has PowerShell parser errors: " +
    (($wrapperParseErrors | ForEach-Object Message) -join " | ")
  )
}
Assert-NoAutomaticHostVariable -Ast $wrapperAst -Label "Recording wrapper"

$launcherPath = Join-Path `
  $PSScriptRoot "..\scripts\run-openxr-gta-steam-safe.ps1"
$launcherTokens = $null
$launcherParseErrors = $null
$launcherAst = [System.Management.Automation.Language.Parser]::ParseFile(
  (Resolve-Path -LiteralPath $launcherPath).Path,
  [ref]$launcherTokens,
  [ref]$launcherParseErrors
)
if ($launcherParseErrors.Count -ne 0) {
  throw (
    "Supervised launcher has PowerShell parser errors: " +
    (($launcherParseErrors | ForEach-Object Message) -join " | ")
  )
}
Assert-NoAutomaticHostVariable -Ast $launcherAst -Label "Supervised launcher"

$wrapperRequiredPatterns = [ordered]@{
  AuthorizationGate = 'if \(-not \$Authorized\)'
  StereoModeParameter = '\[ValidateSet\(55, 56, 57, 58\)\]'
  Mode58Default = '\[uint32\]\$StereoMode = 58'
  SelectedMode = '-StereoMode \$StereoMode'
  ModeFilename = 'gtaiv-openxr-mode\{0\}-vr-sbs-\{1\}s\.mp4'
  ElliottProof = '-ElliottCliProof -StopWhenProofComplete'
  ExtendedWalk = '-ElliottWalkSeconds \$WalkSeconds'
  RunDirectoryWatcher = 'New-Object IO\.FileSystemWatcher'
  WalkMarker = '"elliott-walk-ready\.marker"'
  RecorderLaunch = '-File `"\$recorderPath`"'
  HiddenHelpers = '-WindowStyle Hidden'
  EventProcessWait = '\.WaitForExit\('
  NullExitCodeGuard =
    '\$null -ne \$captureExitCode -and \$captureExitCode -ne 0'
  EmptyRecorderStderr =
    '\(Get-Item -LiteralPath \$captureError\)\.Length -ne 0'
  EmptyLauncherStderr =
    '\(Get-Item -LiteralPath \$launcherError\)\.Length -ne 0'
  ProofResult = 'outcome = "PROOF_COMPLETE"'
  RuntimeProof = 'runtimeKind = "ElliottSimulator"'
  BackendProof = 'backendOpenXr = "True"'
  StereoProof = 'stereoProofComplete = "True"'
  PresentationContinuityProof =
    'hostPresentationContinuityReady = "True"'
  Mode58FreshUpdateProof =
    '\$requiredResultValues\["hostSwapchainFreshUpdatesReady"\] = "True"'
  ParentDualProof =
    '\$requiredResultValues\["parentDualProofComplete"\] = "True"'
  ParentDualExactPose =
    '\$requiredResultValues\["parentDualHostExactPoseReady"\] = "True"'
  FirstPersonProof =
    '\$requiredResultValues\["firstPersonProofComplete"\] = "True"'
  ControllerProof = 'controllerProofComplete = "True"'
  MenuProof = 'stationaryUiQuad = "True"'
  ColorProof = 'colorPathReady = "True"'
  SrgbFormatProof = "\^swapchainFormat=\(29\|91\)\$"
  ContinuityProof = 'continuousWorldFrames = "True"'
  PauseProof = 'pauseRoundTrip = "True"'
  PoseProof = 'elliottPoseSweepCompleted = "True"'
  CaptureReceipt = '\$receiptPath = "\$videoPath\.capture\.txt"'
  CaptureReceiptPass = '"capture=PASS"'
  HeadlessReceipt = '"simulatorInput=CLI/headless"'
  NoActivationReceipt = '"windowActivation=False"'
}
foreach ($entry in $wrapperRequiredPatterns.GetEnumerator()) {
  if ($wrapper -notmatch $entry.Value) {
    throw "Missing Elliott recording-wrapper contract: $($entry.Key)"
  }
}
if ($wrapper -match '(?i)(vrserver|vrmonitor|vrcompositor|steamvr)') {
  throw "Recording wrapper contains forbidden SteamVR monitoring/control."
}
if ($wrapper -match 'Get-Content[^\r\n]*status\.log') {
  throw "Recording wrapper reads the live launcher status log."
}

Write-Output (
  "ElliottVrRecorderContractTest: PASS " +
  "required=$($requiredPatterns.Count) forbidden=$($forbiddenPatterns.Count) " +
  "wrapperRequired=$($wrapperRequiredPatterns.Count) automaticHostGuards=3"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$recorderPath = Join-Path `
  $PSScriptRoot "..\scripts\record-elliott-vr-preview.ps1"
$recorder = Get-Content -LiteralPath $recorderPath -Raw

$tokens = $null
$parseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(
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
[void][System.Management.Automation.Language.Parser]::ParseFile(
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

$wrapperRequiredPatterns = [ordered]@{
  AuthorizationGate = 'if \(-not \$Authorized\)'
  Mode57 = '-StereoMode 57'
  ElliottProof = '-ElliottCliProof -StopWhenProofComplete'
  ExtendedWalk = '-ElliottWalkSeconds \$WalkSeconds'
  RunDirectoryWatcher = 'New-Object IO\.FileSystemWatcher'
  WalkMarker = '"elliott-walk-ready\.marker"'
  RecorderLaunch = '-File `"\$recorderPath`"'
  HiddenHelpers = '-WindowStyle Hidden'
  EventProcessWait = '\.WaitForExit\('
  ProofResult = '"outcome=PROOF_COMPLETE"'
  CaptureReceipt = '\$receiptPath = "\$videoPath\.capture\.txt"'
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
  "wrapperRequired=$($wrapperRequiredPatterns.Count)"
)

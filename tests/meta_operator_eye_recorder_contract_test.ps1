$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$recorderPath = Join-Path `
  $PSScriptRoot "..\scripts\record-meta-operator-eye.ps1"
$recorder = Get-Content -LiteralPath $recorderPath -Raw -ErrorAction Stop

$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
  (Resolve-Path -LiteralPath $recorderPath).Path,
  [ref]$tokens,
  [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
  throw (
    "Meta Operator eye recorder has parser errors: " +
    (($parseErrors | ForEach-Object Message) -join " | ")
  )
}

$requiredPatterns = [ordered]@{
  BoundedDuration =
    '\[ValidateRange\(5, 300\)\]\s*\[uint32\]\$DurationSeconds'
  BoundedRate =
    '\[ValidateRange\(1, 60\)\]\s*\[uint32\]\$CaptureRate'
  ExactProxy =
    '"meta-xr-operator-mcp-proxy\.exe"'
  HiddenProxy =
    '\$startInfo\.CreateNoWindow = \$true'
  OperatorCapture =
    'name = "openxr_capture_composited_image"'
  EyeArgument =
    'arguments = \[ordered\]@\{ eye = \$Eye \}'
  PngFrames =
    '"frame-\{0:D6\}\.png"'
  Ffmpeg =
    '& \$ffmpeg'
  Nvenc =
    '-c:v h264_nvenc'
  MonoMetadata =
    'stereo_mode=mono'
  EyeMetadata =
    'source_eye=\$Eye'
  NativeStderrScope =
    '\$ErrorActionPreference = "Continue"'
  NativeStderrRestore =
    '\$ErrorActionPreference = \$priorErrorActionPreference'
  ExitSnapshot =
    '\$ffmpegExitCode = \$LASTEXITCODE'
  ExitGate =
    '\$ffmpegExitCode -ne 0'
  FfmpegLog =
    '"\$output\.ffmpeg\.log"'
  VideoHash =
    'Get-FileHash -LiteralPath \$output -Algorithm SHA256'
  SafeFrameRoot =
    '\$resolvedFrameDirectory\.StartsWith\('
  SafeFrameName =
    '"meta-operator-\*-eye-frames-\*"'
}
foreach ($entry in $requiredPatterns.GetEnumerator()) {
  if ($recorder -notmatch $entry.Value) {
    throw "Missing Meta Operator eye-recorder contract: $($entry.Key)"
  }
}

$ffmpegScope = [regex]::Match(
  $recorder,
  '(?s)\$priorErrorActionPreference = \$ErrorActionPreference.*?' +
  '\$ErrorActionPreference = "Continue".*?' +
  '& \$ffmpeg.*?' +
  '\$ffmpegExitCode = \$LASTEXITCODE.*?' +
  'finally \{\s*\$ErrorActionPreference = \$priorErrorActionPreference'
)
if (-not $ffmpegScope.Success) {
  throw (
    "FFmpeg native stderr is not contained by a Continue/restore scope with " +
    "an explicit exit-code snapshot."
  )
}

$forbiddenPatterns = [ordered]@{
  GtaLaunch =
    '(?i)(?:GTAIV|PlayGTAIV)\.exe'
  SteamLaunch =
    '(?i)(?:steam|vrmonitor|vrserver|vrcompositor)\.exe'
  SimulatorLaunch =
    '(?i)(?:MetaXRSimulator|XrSimulator)\.exe'
  GuiControl =
    '(?i)(?:SendKeys|SetForegroundWindow|SetCursorPos|mouse_event|keybd_event)'
  RuntimeMutation =
    '(?i)(?:XR_RUNTIME_JSON|ActiveRuntime)'
}
foreach ($entry in $forbiddenPatterns.GetEnumerator()) {
  if ($recorder -match $entry.Value) {
    throw "Forbidden Meta Operator eye-recorder surface: $($entry.Key)"
  }
}

$processStarts = @($ast.FindAll(
  {
    param($node)
    $node -is [Management.Automation.Language.InvokeMemberExpressionAst] -and
    $node.Member.Extent.Text -eq "Start"
  },
  $true
))
if ($processStarts.Count -ne 1 -or
    $processStarts[0].Expression.Extent.Text -ne '$proxy') {
  throw (
    "Recorder must start exactly one validated Operator proxy; found " +
    "$($processStarts.Count) process starts."
  )
}

Write-Output (
  "MetaOperatorEyeRecorderContractTest: PASS " +
  "operatorFrames=1 nativeStderrSafe=1 exitGate=1 guiControl=0 " +
  "gameLaunch=0 steamLaunch=0"
)

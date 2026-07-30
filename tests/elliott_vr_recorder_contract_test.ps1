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
  StereoModeParameter = '\[ValidateSet\(55, 56, 57, 58, 204\)\]'
  Mode58Default = '\[uint32\]\$StereoMode = 58'
  SixtySecondWalkRange = '\[ValidateRange\(15, 60\)\]'
  LiteralMode204Walk =
    '\$StereoMode -eq 204 -and \$WalkSeconds -ne 60'
  Mode204TwelveSecondDefault =
    '(?s)\$StereoMode -eq 204 -and\s+-not \$PSBoundParameters\.ContainsKey\("DurationSeconds"\).*?\$DurationSeconds = 12'
  SelectedMode = '-StereoMode \$StereoMode'
  ModeFilename = 'gtaiv-openxr-mode\{0\}-vr-sbs-\{1\}s\.mp4'
  ElliottProof = '-ElliottCliProof -StopWhenProofComplete'
  ExtendedWalk = '-ElliottWalkSeconds \$WalkSeconds'
  RunDirectoryWatcher = 'New-Object IO\.FileSystemWatcher'
  WalkMarker = '"elliott-walk-ready\.marker"'
  RecorderLaunch = 'Start-ElliottCaptureAttempt'
  HiddenHelpers = '-WindowStyle Hidden'
  EventProcessWait = '\.WaitForExit\('
  SafeExitCodeSnapshot = '\$captureExitCode = if \(\$captureExited\)'
  EmptyRecorderStderr =
    '\$selectedCapture\.StandardErrorPath'
  EmptyLauncherStderr =
    '\(Get-Item -LiteralPath \$launcherError\)\.Length -ne 0'
  RetryStatusWatcher =
    'Wait-Mode204RetrySignal'
  RetryStatusFile = 'Join-Path \$runPath "status\.log"'
  RetryStatusExact =
    'advancing world recovered; starting clean "'
  RetryEventWait =
    '\$watcher\.WaitForChanged\('
  AttemptOneCandidate =
    'mode204-vr-sbs-\{0\}s\.attempt1\.candidate\.mp4'
  AttemptTwoCandidate =
    'mode204-vr-sbs-\{0\}s\.attempt2\.candidate\.mp4'
  AttemptTwoMarker = '"attempt2-walk-ready\.marker"'
  Mode58DirectCapture =
    '(?s)\$attemptOneVideoPath = if \(\$StereoMode -eq 204\).*?else \{\s+\$videoPath\s+\}'
  Mode58CaptureLog = '"capture\.stdout\.log"'
  AcceptedAttemptSelector =
    'Get-Mode204AcceptedWalkAttempt'
  AcceptedAttemptField =
    '-Key "acceptedWalkAttempt"'
  CandidateSelection =
    '\$captureOutcome\.Attempt -eq \$acceptedWalkAttempt'
  RejectedAttemptLabel = '"REJECTED_NOT_PROOF"'
  AcceptedOnlyPublication =
    '"publication=ACCEPTED_ATTEMPT_ONLY"'
  PublicationAfterProof =
    '\$failures\.Count -eq 0'
  ProofResult = 'outcome = "PROOF_COMPLETE"'
  RuntimeProof = 'runtimeKind = "ElliottSimulator"'
  BackendProof = 'backendOpenXr = "True"'
  StereoProof = 'stereoProofComplete = "True"'
  PresentationContinuityProof =
    'hostPresentationContinuityReady = "True"'
  Mode58FreshUpdateProof =
    '\$requiredResultValues\["hostSwapchainFreshUpdatesReady"\] = "True"'
  Mode204ContinuityProof =
    '\$requiredResultValues\["mode204PrePauseContinuityReady"\] = "True"'
  Mode204GpuHostProof =
    '\$requiredResultValues\["mode204GpuHostReady"\] = "True"'
  Mode204ConfigRestored =
    '\$requiredResultValues\["mode204ConfigRestored"\] = "True"'
  Mode204WalkQuality =
    '\$requiredResultValues\["walkQualityReady"\] = "True"'
  Mode204SustainedProof =
    '\$requiredResultValues\["sustainedPoseControllerProof"\] = "True"'
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
foreach ($forbiddenWrapperControl in @(
    "Start-Sleep",
    "SendKeys",
    "AppActivate",
    "SetForegroundWindow"
  )) {
  if ($wrapper -match [regex]::Escape($forbiddenWrapperControl)) {
    throw (
      "Recording wrapper contains forbidden polling/GUI control: " +
      $forbiddenWrapperControl
    )
  }
}

$wrapperFunctionNames = @(
  "Write-TextAtomic",
  "Get-UniqueResultValue",
  "Get-Mode204AcceptedWalkAttempt",
  "Get-Mode204RetrySignal",
  "Publish-Mode204AcceptedCapture"
)
foreach ($wrapperFunctionName in $wrapperFunctionNames) {
  $wrapperFunction = $wrapperAst.Find(
    {
      param($node)
      $node -is
        [System.Management.Automation.Language.FunctionDefinitionAst] -and
      $node.Name -eq $wrapperFunctionName
    },
    $true
  )
  if (-not $wrapperFunction) {
    throw "Wrapper function AST was not found: $wrapperFunctionName"
  }
  . ([scriptblock]::Create($wrapperFunction.Extent.Text))
}

$retryStatusFixture =
  "[12:00:00] ELLIOTT CLI: walk quality retry 2/2`n" +
  "[12:00:01] ELLIOTT CLI: advancing world recovered; starting clean " +
  "60-second walk attempt 2/2"
$retrySignal = Get-Mode204RetrySignal `
  -StatusText $retryStatusFixture `
  -ExpectedWalkSeconds 60
if ($retrySignal -notmatch '60-second walk attempt 2/2$') {
  throw "Mode204 retry signal did not select the exact attempt-2 start."
}
$wrongDurationRetrySignal = Get-Mode204RetrySignal `
  -StatusText $retryStatusFixture `
  -ExpectedWalkSeconds 30
if (-not [string]::IsNullOrWhiteSpace($wrongDurationRetrySignal)) {
  throw "Mode204 retry signal accepted a mismatched walk duration."
}

$acceptedAttemptTwoResult = @(
  "outcome=PROOF_COMPLETE",
  "stereoMode=204",
  "elliottWalkSeconds=60",
  "walkQualityReady=True",
  "sustainedPoseControllerProof=True",
  "acceptedWalkAttempt=2"
)
$acceptedAttempt = Get-Mode204AcceptedWalkAttempt `
  -ResultLines $acceptedAttemptTwoResult `
  -ExpectedWalkSeconds 60
if ($acceptedAttempt -ne 2) {
  throw "Mode204 result selector did not select accepted attempt 2."
}
$acceptedAttemptOneResult = @(
  $acceptedAttemptTwoResult | Where-Object {
    $_ -notmatch '^acceptedWalkAttempt='
  }
) + "acceptedWalkAttempt=1"
$acceptedAttempt = Get-Mode204AcceptedWalkAttempt `
  -ResultLines $acceptedAttemptOneResult `
  -ExpectedWalkSeconds 60
if ($acceptedAttempt -ne 1) {
  throw "Mode204 result selector did not preserve accepted attempt 1."
}

$rejectedResultFixtures = [ordered]@{
  FailedQuality = @(
    $acceptedAttemptTwoResult | ForEach-Object {
      if ($_ -eq "walkQualityReady=True") {
        "walkQualityReady=False"
      }
      else {
        $_
      }
    }
  )
  AbortedOutcome = @(
    $acceptedAttemptTwoResult | ForEach-Object {
      if ($_ -eq "outcome=PROOF_COMPLETE") {
        "outcome=ABORTED"
      }
      else {
        $_
      }
    }
  )
  InvalidAttempt = @(
    $acceptedAttemptTwoResult | ForEach-Object {
      if ($_ -eq "acceptedWalkAttempt=2") {
        "acceptedWalkAttempt=0"
      }
      else {
        $_
      }
    }
  )
  WrongDuration = @(
    $acceptedAttemptTwoResult | ForEach-Object {
      if ($_ -eq "elliottWalkSeconds=60") {
        "elliottWalkSeconds=30"
      }
      else {
        $_
      }
    }
  )
  DuplicateAttempt = @(
    $acceptedAttemptTwoResult + "acceptedWalkAttempt=1"
  )
}
foreach ($rejectedResultFixture in $rejectedResultFixtures.GetEnumerator()) {
  $rejected = $false
  try {
    [void](Get-Mode204AcceptedWalkAttempt `
      -ResultLines $rejectedResultFixture.Value `
      -ExpectedWalkSeconds 60)
  }
  catch {
    $rejected = $true
  }
  if (-not $rejected) {
    throw (
      "Mode204 result selector accepted rejected fixture: " +
      $rejectedResultFixture.Key
    )
  }
}

$resultSelectionIndex = $wrapper.LastIndexOf(
  '$acceptedWalkAttempt = Get-Mode204AcceptedWalkAttempt'
)
$publicationCallIndex = $wrapper.LastIndexOf(
  'Publish-Mode204AcceptedCapture `'
)
if ($resultSelectionIndex -lt 0 -or
    $publicationCallIndex -le $resultSelectionIndex) {
  throw (
    "Mode204 proof publication is not ordered after launcher-result " +
    "accepted-attempt selection."
  )
}

$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$publicationFixtureRoot = [IO.Path]::GetFullPath(
  (Join-Path $temporaryBase (
    "gtaiv-mode204-publication-contract-" + [Guid]::NewGuid().ToString("N")
  ))
)
if (-not $publicationFixtureRoot.StartsWith(
    $temporaryBase,
    [StringComparison]::OrdinalIgnoreCase
  )) {
  throw "Publication fixture escaped the system temporary directory."
}
New-Item `
  -ItemType Directory `
  -Path $publicationFixtureRoot |
  Out-Null
try {
  $candidatePath =
    Join-Path $publicationFixtureRoot "attempt2.candidate.mp4"
  $candidateReceiptPath = "$candidatePath.capture.txt"
  $publishedPath = Join-Path $publicationFixtureRoot "proof.mp4"
  $launcherResultPath = Join-Path $publicationFixtureRoot "result.txt"
  Set-Content `
    -LiteralPath $candidatePath `
    -Value ("candidate-attempt-2-" * 128) `
    -Encoding UTF8
  Set-Content `
    -LiteralPath $candidateReceiptPath `
    -Value @(
      "capture=PASS",
      "simulatorInput=CLI/headless",
      "windowActivation=False",
      "durationSeconds=12",
      "frameRate=60",
      "output=$candidatePath"
    ) `
    -Encoding UTF8
  Set-Content `
    -LiteralPath $launcherResultPath `
    -Value $acceptedAttemptTwoResult `
    -Encoding UTF8
  Publish-Mode204AcceptedCapture `
    -CandidatePath $candidatePath `
    -CandidateReceiptPath $candidateReceiptPath `
    -OutputPath $publishedPath `
    -ResultPath $launcherResultPath `
    -AcceptedAttempt 2
  if (-not (Test-Path -LiteralPath $candidatePath -PathType Leaf) -or
      -not (Test-Path -LiteralPath $publishedPath -PathType Leaf)) {
    throw "Accepted publication did not preserve the candidate and proof copy."
  }
  $candidateHash = (
    Get-FileHash -LiteralPath $candidatePath -Algorithm SHA256
  ).Hash
  $publishedHash = (
    Get-FileHash -LiteralPath $publishedPath -Algorithm SHA256
  ).Hash
  if ($candidateHash -ne $publishedHash) {
    throw "Accepted publication changed candidate video bytes."
  }
  $publishedReceipt =
    @(Get-Content -LiteralPath "$publishedPath.capture.txt")
  foreach ($publishedRequirement in @(
      "output=$publishedPath",
      "acceptedWalkAttempt=2",
      "selectedCandidate=$candidatePath",
      "publication=ACCEPTED_ATTEMPT_ONLY"
    )) {
    if ($publishedReceipt -notcontains $publishedRequirement) {
      throw (
        "Accepted publication receipt is missing '$publishedRequirement'."
      )
    }
  }
  $overwriteRejected = $false
  try {
    Publish-Mode204AcceptedCapture `
      -CandidatePath $candidatePath `
      -CandidateReceiptPath $candidateReceiptPath `
      -OutputPath $publishedPath `
      -ResultPath $launcherResultPath `
      -AcceptedAttempt 1
  }
  catch {
    $overwriteRejected = $true
  }
  if (-not $overwriteRejected) {
    throw "Accepted publication overwrote an existing proof video."
  }
}
finally {
  if (Test-Path -LiteralPath $publicationFixtureRoot) {
    Remove-Item `
      -LiteralPath $publicationFixtureRoot `
      -Recurse `
      -Force
  }
}

Write-Output (
  "ElliottVrRecorderContractTest: PASS " +
  "required=$($requiredPatterns.Count) forbidden=$($forbiddenPatterns.Count) " +
  "wrapperRequired=$($wrapperRequiredPatterns.Count) automaticHostGuards=3 " +
  "retryFixtures=2 acceptedAttemptFixtures=7 publicationFixtures=1 " +
  "publicationOrder=1"
)

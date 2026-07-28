$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$launcherPath = Join-Path `
  $PSScriptRoot "..\scripts\run-openxr-gta-steam-safe.ps1"
$launcher = Get-Content -LiteralPath $launcherPath -Raw

$parseErrors = $null
$launcherAst = [System.Management.Automation.Language.Parser]::ParseFile(
  (Resolve-Path -LiteralPath $launcherPath).Path,
  [ref]$null,
  [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
  throw (
    "Launcher has PowerShell parser errors: " +
    (($parseErrors | ForEach-Object Message) -join " | ")
  )
}

$requiredPatterns = [ordered]@{
  OptInSwitch = '\[switch\]\$ElliottCliProof'
  StereoMode57 = '\[ValidateSet\(55, 56, 57\)\]'
  TemporalHostOptIn = '"--allow-temporal-stereo"'
  TemporalModeOnly = 'if \(\$Mode -eq 57\)'
  TemporalProducerRoute = '"world-temporal-stereo"'
  TemporalPairGate = '\$temporalPairReady'
  TemporalPixelGate = '\$temporalPixelDistinct'
  TemporalHostGate = '\$temporalHostAccepted'
  TemporalPoseGate = '\$temporalCapturePoseActive'
  TemporalBuildReplayReceiptGate = '\$temporalBuildReplayReceiptReady'
  HostSwapchainReuseGate = '\$hostSwapchainReuseReady'
  HostSwapchainReuseMarker = 'unchanged OpenXR world swapchain reuse READY'
  HostSwapchainWorldRoute = 'presentation=\$expectedHostReusePresentation'
  HeldFrameSelfTest = 'heldFrameReuse=1.*exactPoseCache=1'
  StrictSwapchainWait = 'strictSwapchainWait=1'
  ReferenceSpaceReset = 'referenceSpaceReset=1'
  TemporalBuildReplayReceiptProof = 'proof=buildroot-execroot-fifo-beginscene-endscene-entry'
  TemporalBuildEpochField = 'build=\(\\d\+\)/\(\\d\+\)'
  TemporalBuildEpochGate = '\$receiptBuildRight -eq \$receiptBuildLeft \+ 1'
  TemporalSameTickProof = '"sameTick=0 temporal=1 pose='
  TemporalSourceProof = 'source=\(\\d\+\)/\(\\d\+\)'
  ElliottOnly = '\$runtimeInfo\.Kind -ne "ElliottSimulator"'
  ControllerMailbox = '"controller_pose_command\.json"'
  PoseSweepMailbox = '"pose_sweep_command\.json"'
  PrePausePoseSweep = 'controlled pose sweep started for uninterrupted'
  AckMailbox = '"command_ack\.json"'
  AtomicMove = 'Move-Item -LiteralPath \$temporaryPath -Destination \$Path -Force'
  SequencedAck = '-Command "controller_state"'
  ControllerLease = 'lease_ms = 750'
  StickLease = 'lease_ms = 6000'
  StickForward = 'stickY = 0\.8'
  PrimaryButton = 'primary = \$true'
  MenuButton = 'menu = \$true'
  BothNeutral = '-Kind "NeutralBoth"'
  APacketGate = '\$buttons -band 0x1000'
  StartPacketGate = '\$buttons -band 0x0010'
  StickGate = '\$controllerStickConsumed'
  NeutralAfterStickGate = '\$controllerStickNeutralConsumed'
  FrameStall = '"FRAME_STALL:'
  TenSecondStall = '\.TotalSeconds -ge 10'
  ImmediateResetAbort = '\$gameText -match "DeviceReset: FAILED"'
  Mode56Camera = 'camDist='
  CameraLowerBound = '\$distanceCm -ge 1\.0'
  CameraUpperBound = '\$distanceCm -le 20\.0'
  PixelDistinct = 'pixelDistinct=1'
  RawRgbProof = 'rawRgbAbsDiff'
  RawRgbThreshold = '\$rawRgbAbsDiff -ge 1024'
  TemporalChangedPixels = 'changedPixels=\(\\d\+\)'
  TemporalChangedTiles = 'changedTiles=\(\\d\+\)'
  TemporalChangedPixelsGate = '\$receiptChangedPixels -ge 16'
  TemporalChangedTilesGate = '\$receiptChangedTiles -ge 4'
  FreshApplyGeneration = '\$stereoApplyGenerationFresh'
  AcceptedPairSpan = '\$mode56PrePauseAcceptedPairSpan -ge 120'
  SharedTransaction = '\$mode56PrePauseSharedTransaction'
  PauseRoundTrip = '\$pauseRoundTrip'
  InteractiveMenuGate = '\$interactiveMenuActive'
  LoadingAfterMenuGate = '\$loadingAfterInteractiveMenu'
  WorldLoadWaitState = '"WAIT_WORLD_LOAD"'
  WorldLoadTimeout = '"WORLD_LOAD_TIMEOUT:'
  LatestUiReasonSample = '\$uiReasonSampleSeen'
  WorldReasonsZero = '\$currentUiReasonFlags -eq 0'
  FreshWorldEvidence = 'Get-SustainedWorldRouteEvidence'
  ThreeProducerFrames = '\$producer\.Count -ge 3'
  ThreeHostFrames = '\$hostFrames\.Count -ge 3'
  ThreeSharedFrames = '\$sharedIds\.Count -ge 3'
  SustainedTwoSeconds = '\$worldLoadSustainedMilliseconds -ge 2000'
  WorldFullyReady = '\$worldLoadFullyReady'
  RockstarHandoffRetry = '"AUTH RETRY: Rockstar did not hand off to GTAIV\.exe'
  OneAuthRetry = '\$authRetryAttempted = \$true'
  RetryNoTitleArguments = '"app 12210 once more through regular Steam with no custom arguments"'
  RockstarPathScope = '\(Join-Path \$env:ProgramFiles "Rockstar Games"\)'
  RockstarTitleGuard = '"Refusing Rockstar launcher cleanup while GTAIV/PlayGTAIV is active\."'
  RockstarPreflight = 'Stop-StaleRockstarLauncherSession "stale preflight Rockstar session"'
  PostResetProducer = '\$postResetProducerFresh'
  PostResetHost = '\$postResetHostFresh'
  SrgbSwapchainFormat = 'XRHost: swapchainFormat=\(29\|91\)'
  SrgbDecode = 'sRGB decode SRV'
  AtomicTextSnapshot = '\[string\]\$snapshot = \$reader\.ReadToEnd\(\)'
  LiveWriterSharing = '\[System\.IO\.FileShare\]::ReadWrite'
  HeadlessMode = 'OPENXR_SIMULATOR_HEADLESS'
  ExternalFocus = 'OPENXR_SIMULATOR_EXTERNAL_FOCUS_EXE'
  CliCleanup = 'command/ack files removed after host stop'
}

foreach ($entry in $requiredPatterns.GetEnumerator()) {
  if ($launcher -notmatch $entry.Value) {
    throw "Missing Elliott CLI proof contract: $($entry.Key)"
  }
}

$forbiddenPatterns = [ordered]@{
  SimulatorGuiProcess = 'Start-Process[^\r\n]*(MetaXRSimulator|OpenXR-Simulator)'
  SendKeys = 'SendKeys'
  AppActivate = 'AppActivate'
  ForegroundWindow = 'SetForegroundWindow'
  PythonDependency = '(?m)^\s*(python|py)(?:\.exe)?\s'
  BuildExecThreadInequality = '\$receiptBuildThread -ne \$receiptExecThread'
  ExecD3dThreadEquality = '\$receiptExecThread -eq \$receiptD3dThread'
}

foreach ($entry in $forbiddenPatterns.GetEnumerator()) {
  if ($launcher -match $entry.Value) {
    throw "Forbidden simulator UI/dependency contract found: $($entry.Key)"
  }
}

$sequenceFunction = $launcherAst.Find(
  {
    param($node)
    $node -is
      [System.Management.Automation.Language.FunctionDefinitionAst] -and
      $node.Name -eq "Get-MonotonicTransactionSequence"
  },
  $true
)
if (-not $sequenceFunction) {
  throw "Get-MonotonicTransactionSequence AST was not found."
}
. ([scriptblock]::Create($sequenceFunction.Extent.Text))

$worldEvidenceFunction = $launcherAst.Find(
  {
    param($node)
    $node -is
      [System.Management.Automation.Language.FunctionDefinitionAst] -and
      $node.Name -eq "Get-SustainedWorldRouteEvidence"
  },
  $true
)
if (-not $worldEvidenceFunction) {
  throw "Get-SustainedWorldRouteEvidence AST was not found."
}
. ([scriptblock]::Create($worldEvidenceFunction.Extent.Text))

$hostArgumentsFunction = $launcherAst.Find(
  {
    param($node)
    $node -is
      [System.Management.Automation.Language.FunctionDefinitionAst] -and
      $node.Name -eq "Get-OpenXrHostArguments"
  },
  $true
)
if (-not $hostArgumentsFunction) {
  throw "Get-OpenXrHostArguments AST was not found."
}
. ([scriptblock]::Create($hostArgumentsFunction.Extent.Text))

$readTextFunction = $launcherAst.Find(
  {
    param($node)
    $node -is
      [System.Management.Automation.Language.FunctionDefinitionAst] -and
      $node.Name -eq "Read-Text"
  },
  $true
)
if (-not $readTextFunction) {
  throw "Read-Text AST was not found."
}
. ([scriptblock]::Create($readTextFunction.Extent.Text))

$snapshotFixturePath = Join-Path (
  [System.IO.Path]::GetTempPath()
) ("gtaiv-openxr-text-snapshot-{0}.log" -f [Guid]::NewGuid())
try {
  $snapshotFixture =
    "presentation=world-temporal-stereo`n" +
    "presentation=stationary-ui-quad"
  [System.IO.File]::WriteAllText(
    $snapshotFixturePath,
    $snapshotFixture
  )
  $fixtureWriter = [System.IO.FileStream]::new(
    $snapshotFixturePath,
    [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Write,
    [System.IO.FileShare]::ReadWrite
  )
  try {
    $snapshot = Read-Text $snapshotFixturePath
  }
  finally {
    $fixtureWriter.Dispose()
  }
  if ($snapshot -isnot [string] -or @($snapshot).Count -ne 1) {
    throw "Read-Text did not return exactly one String snapshot."
  }
  $fixtureStationaryRoute =
    (
      $snapshot.LastIndexOf("presentation=stationary-ui-quad") -gt
      $snapshot.LastIndexOf("presentation=world-temporal-stereo")
    )
  if (-not $fixtureStationaryRoute) {
    throw "Scalar route comparison fixture did not select stationary UI."
  }
  $missingSnapshot = Read-Text "$snapshotFixturePath.missing"
  if ($missingSnapshot -isnot [string] -or
      @($missingSnapshot).Count -ne 1 -or
      $missingSnapshot.Length -ne 0) {
    throw "Missing Read-Text snapshot was not one empty String."
  }
}
finally {
  Remove-Item -LiteralPath $snapshotFixturePath `
    -Force `
    -ErrorAction SilentlyContinue
}

$sequence = Get-MonotonicTransactionSequence `
  -Text "transaction=10`ntransaction=10`ntransaction=12`ntransaction=15" `
  -Pattern "transaction=(\d+)" `
  -AfterIndex -1
if ($sequence.Count -ne 3 -or
    -not $sequence.Monotonic -or
    $sequence.First -ne 10 -or
    $sequence.Latest -ne 15) {
  throw "Monotonic distinct transaction proof returned an invalid result."
}
$outOfOrder = Get-MonotonicTransactionSequence `
  -Text "transaction=10`ntransaction=9" `
  -Pattern "transaction=(\d+)" `
  -AfterIndex -1
if ($outOfOrder.Monotonic) {
  throw "Out-of-order transaction proof was incorrectly accepted."
}

$mode55Arguments = @(Get-OpenXrHostArguments -Mode 55 -TimeoutMs 990000)
$mode56Arguments = @(Get-OpenXrHostArguments -Mode 56 -TimeoutMs 990000)
$mode57Arguments = @(Get-OpenXrHostArguments -Mode 57 -TimeoutMs 990000)
foreach ($nonTemporalArguments in @($mode55Arguments, $mode56Arguments)) {
  if ($nonTemporalArguments -contains "--allow-temporal-stereo") {
    throw "Temporal host opt-in leaked into Mode55 or Mode56."
  }
}
if (@($mode57Arguments | Where-Object {
      $_ -eq "--allow-temporal-stereo"
    }).Count -ne 1) {
  throw "Mode57 did not receive exactly one temporal host opt-in."
}

$producerPattern = "producer=(\d+)"
$hostPattern = "host=(\d+)"
$staleGame = "producer=1`nBOUNDARY`nproducer=10`nproducer=11"
$staleHost = "host=1`nBOUNDARY`nhost=10`nhost=11"
$staleEvidence = Get-SustainedWorldRouteEvidence `
  -GameText $staleGame `
  -HostText $staleHost `
  -ProducerPattern $producerPattern `
  -HostPattern $hostPattern `
  -GameAfterIndex $staleGame.IndexOf("BOUNDARY") `
  -HostAfterIndex $staleHost.IndexOf("BOUNDARY")
if ($staleEvidence.Ready) {
  throw "Two fresh frames plus stale pre-boundary evidence passed world load."
}

$readyGame =
  "producer=1`nBOUNDARY`nproducer=10`nproducer=11`nproducer=12"
$readyHost =
  "host=1`nBOUNDARY`nhost=10`nhost=11`nhost=12"
$readyEvidence = Get-SustainedWorldRouteEvidence `
  -GameText $readyGame `
  -HostText $readyHost `
  -ProducerPattern $producerPattern `
  -HostPattern $hostPattern `
  -GameAfterIndex $readyGame.IndexOf("BOUNDARY") `
  -HostAfterIndex $readyHost.IndexOf("BOUNDARY")
if (-not $readyEvidence.Ready -or
    $readyEvidence.ProducerCount -ne 3 -or
    $readyEvidence.HostCount -ne 3 -or
    $readyEvidence.SharedCount -ne 3) {
  throw "Three fresh monotonic shared world frames did not pass."
}

$mismatchedHost =
  "host=1`nBOUNDARY`nhost=10`nhost=11`nhost=13"
$mismatchedEvidence = Get-SustainedWorldRouteEvidence `
  -GameText $readyGame `
  -HostText $mismatchedHost `
  -ProducerPattern $producerPattern `
  -HostPattern $hostPattern `
  -GameAfterIndex $readyGame.IndexOf("BOUNDARY") `
  -HostAfterIndex $mismatchedHost.IndexOf("BOUNDARY")
if ($mismatchedEvidence.Ready) {
  throw "World load passed without three shared producer/host transactions."
}

$menuSettleStart = $launcher.IndexOf('"MENU_SETTLE" {')
$waitWorldStart = $launcher.IndexOf(
  '"WAIT_WORLD_LOAD" {',
  $menuSettleStart + 1
)
if ($menuSettleStart -lt 0 -or $waitWorldStart -le $menuSettleStart) {
  throw "MENU_SETTLE/WAIT_WORLD_LOAD state ordering was not found."
}
$menuSettleBlock = $launcher.Substring(
  $menuSettleStart,
  $waitWorldStart - $menuSettleStart
)
if ($menuSettleBlock -match '"WAIT_MENU_INPUT_PROOF"') {
  throw "MENU_SETTLE still bypasses WAIT_WORLD_LOAD."
}
if ($menuSettleBlock -notmatch '"WAIT_WORLD_LOAD"') {
  throw "MENU_SETTLE does not enter WAIT_WORLD_LOAD."
}

Write-Output (
  (
    "ElliottCliProofContractTest: PASS required={0} forbidden={1} " +
    "sequence=2 hostModes=3 worldEvidence=3 menuWait=1 textSnapshot=2"
  ) -f $requiredPatterns.Count, $forbiddenPatterns.Count
)

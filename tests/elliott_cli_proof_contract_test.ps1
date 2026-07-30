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
  StereoModes = '\[ValidateSet\(55, 56, 57, 58, 204\)\]'
  SafeLauncherDefault = '\[uint32\]\$StereoMode = 55'
  TemporalHostOptIn = '"--allow-temporal-stereo"'
  TemporalModeOnly = 'if \(\$Mode -eq 57\)'
  TemporalProducerRoute = '"world-temporal-stereo"'
  TemporalPairGate = '\$temporalPairReady'
  TemporalPixelGate = '\$temporalPixelDistinct'
  TemporalHostGate = '\$temporalHostAccepted'
  TemporalPoseGate = '\$temporalCapturePoseActive'
  TemporalBuildReplayReceiptGate = '\$temporalBuildReplayReceiptReady'
  TemporalSplitStageGate = '\$temporalSplitStageReady'
  TemporalSplitReadbackGate = '\$temporalSplitReadbackReady'
  TemporalSplitStageProof = 'Mode57 CPU readback phase=L'
  TemporalSplitFlag = 'split=1'
  TemporalSplitAtomic = 'atomic=1'
  TemporalSplitPhasePose = 'phasePose=\(\\d\+\)/\(\\d\+\)'
  TemporalSplitPhaseCall = 'phaseCall=\(\\d\+\)/\(\\d\+\)'
  TemporalSplitPhaseGap = 'phaseGapMs=\(\[0-9\]\+\(\?:\\\.\[0-9\]\+\)\?\)'
  TemporalSplitFreshPoseGate = '\$candidateCurrentPose -gt \$candidateGatePose'
  TemporalSplitFreshCallGate = '\$candidateCurrentCall -gt \$candidateGateCall'
  TemporalSplitPositiveGapGate = '\$candidatePhaseGapMs -gt 0\.0'
  TemporalSplitBoundedGapGate = '\$candidatePhaseGapMs -le 250\.0'
  TemporalSplitResult = 'temporalSplitReadbackReady=\$temporalSplitReadbackReady'
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
  HeadsetProfileMailbox = '"headset_profile_command\.json"'
  Quest3ProfilePayload = 'Send-ElliottHeadsetProfile -Name "quest3"'
  HeadsetProfileAck = '-Command "headset_profile"'
  HeadsetProfileReset =
    'Send-ElliottHeadsetProfile -Name "default" -Attempts 1'
  PrePausePoseSweep = 'controlled pose sweep started for uninterrupted'
  AckMailbox = '"command_ack\.json"'
  AtomicMove = 'Move-Item -LiteralPath \$temporaryPath -Destination \$Path -Force'
  SequencedAck = '-Command "controller_state"'
  ControllerLease = 'lease_ms = 750'
  WalkSecondsParameter = '\[uint32\]\$ElliottWalkSeconds = 6'
  WalkSecondsRange = '\[ValidateRange\(6, 60\)\]'
  WalkReadyMarker =
    'Join-Path \$RunDirectory "elliott-walk-ready\.marker"'
  WalkReadyTemporary = '\$walkReadyTemporary = "\$ElliottWalkReadyMarker\.tmp"'
  WalkReadyAtomicMove =
    'Move-Item\s+`\s+-LiteralPath \$walkReadyTemporary\s+`\s+-Destination \$ElliottWalkReadyMarker'
  StickLeaseMargin =
    'lease_ms = \[uint32\]\(\(\$ElliottWalkSeconds \+ 3\) \* 1000\)'
  StickHoldDuration =
    'AddSeconds\(\$ElliottWalkSeconds\)'
  StickHoldResult =
    '"elliottWalkSeconds=\$ElliottWalkSeconds"'
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
  Mode204PhaseDeadlineStallPolicy =
    '-not \(\$StereoMode -eq 204\)'
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
  Mode58FusedPair = 'StereoOpenXRFused: accepted pair'
  Mode58SameTick = 'sameTick=1 temporal=0'
  Mode58ParentTarget = 'parent=0x4DE020'
  Mode58ParentProducer = '\$parentDualProducerReady'
  Mode58ParentRoute = 'world-parent-dual-stereo'
  Mode58ExactHostPose = '\$parentDualHostExactPoseReady'
  Mode58ExactHostPoseLog = 'parent-dual exact capture pose active'
  Mode58FreshWorldUpdates =
    '\$hostSwapchainFreshWorldUpdateSamples -ge\s+\$requiredFreshWorldUpdateSamples'
  Mode58FreshUpdateGate = '\$hostSwapchainFreshUpdatesReady'
  Mode58PresentationContinuity =
    '\$hostPresentationContinuityReady = if \(\$parentDualMode\)'
  Mode58FreshUpdateResult =
    '"hostSwapchainFreshUpdatesReady=\$hostSwapchainFreshUpdatesReady"'
  Mode58ExactPoseFailClosed =
    'parent-dual exact capture pose missing; projection layer held'
  Mode58ParentProof = '\$parentDualProofComplete'
  Mode58FirstPersonHardHook = '\$firstPersonHardHookReady'
  Mode58FirstPersonGameplay = '\$firstPersonGameplayReady'
  Mode58NativePedHide = '\$firstPersonPedHideReady'
  Mode58NativePedHideProof =
    'PedHide: native proof pass=\(\\d\+\) mode=58 components=5'
  Mode58FirstPersonProof = '\$firstPersonProofComplete'
  Mode58ContinuitySpan = '\$mode58PrePauseAcceptedPairSpan -ge 120'
  Mode58ContinuityResult =
    '"mode58PrePauseContinuityReady=\$mode58PrePauseContinuityReady"'
  Mode58ParentResult =
    '"parentDualProofComplete=\$parentDualProofComplete"'
  Mode58FirstPersonResult =
    '"firstPersonProofComplete=\$firstPersonProofComplete"'
  Mode58WalkPose = 'Mode\$StereoMode pose sweep active for '
  Mode58FailClosed =
    'Mode58: \(\?:FAIL\|KILL\)\[\^\\r\\n\]\*wrote stereo=57'
  Mode204Adapter =
    'OpenXRSubmitAdapter: Mode204 passive exact textures'
  Mode204GpuReady =
    'OpenXRBridge: READY Vulkan-imported D3D11 NT frame resources'
  Mode204GpuProducer =
    'OpenXRBridge: frame transaction=\(\\d\+\) slot=\\d\+ pair=\(\\d\+\)'
  Mode204UnchangedUpstreamWvp =
    'presentation=world-stereo sameTick=1 wvpProof=0'
  Mode204GpuQueue = 'GameBridge: isolated GPU ingest queue READY'
  Mode204GpuConnect = 'GameBridge: CONNECTED frame source pid='
  Mode204GpuHost =
    'GameBridge: frame acquired transaction=\(\\d\+\) privateSlot=\\d\+'
  Mode204AuthoritativeHostSamples =
    'XRHost: game swapchain \(\?:updated\|reused\)'
  Mode204TwoFreshUpdateSamples =
    '(?s)\$requiredFreshWorldUpdateSamples = if \(\$StereoMode -eq 204\) \{\s+2\s+\}\s+else \{\s+3'
  Mode204FreshUpdateCounterAdvance =
    '\$latestFreshWorldUpdateCounter -gt\s+\$firstFreshWorldUpdateCounter'
  Mode204ProducerRoute = '204 \{ "world-stereo"; break \}'
  Mode204HostRoute = '204 \{ "world-parent-dual-stereo"; break \}'
  Mode204AsymmetricRouteComparison =
    '(?s)\$currentStationaryUiRoute\s*=.*?\$gameText\.LastIndexOf.*?\$expectedProducerPresentation.*?\$hostText\.LastIndexOf.*?\$expectedHostAcquiredPresentation'
  Mode204CpuFailClosed =
    'Mode204 entered the legacy CPU mailbox'
  Mode204ContinuityGate = '\$mode204PrePauseContinuityReady'
  Mode204AdapterAdvance =
    '\$mode204PrePauseAcceptedPairSpan -ge 2'
  Mode204AdapterMonotonic =
    '\$mode204PrePauseAdapterSequence\.Monotonic'
  Mode204ThreeProducer =
    '\$mode204PrePauseEvidence\.ProducerCount'
  Mode204ThreeHost =
    '\$mode204PrePauseEvidence\.HostCount'
  Mode204RangeCoupled =
    '\$mode204PrePauseRangeCoupled'
  Mode204SampledGpuEvidence =
    'Get-SampledGpuWorldRouteEvidence'
  Mode204WorldRouteSelection =
    '(?s)\$worldLoadEvidence = if \(\$StereoMode -eq 204\) \{\s+Get-SampledGpuWorldRouteEvidence'
  Mode204CoupledWorldResult =
    '"worldLoadCoupledFrames=\$worldLoadCoupledFrames"'
  Mode204AdapterResult =
    '"mode204AdapterReady=\$mode204AdapterReady"'
  Mode204GpuProducerResult =
    '"mode204GpuProducerReady=\$mode204GpuProducerReady"'
  Mode204GpuHostResult =
    '"mode204GpuHostReady=\$mode204GpuHostReady"'
  Mode204ConfigResult =
    '"mode204ConfigRestored=\$mode204ConfigRestored"'
  QuestProfileResult =
    '"elliottQuestProfileReady=\$elliottQuestProfileReady"'
  SustainedPoseControllerResult =
    '"sustainedPoseControllerProof=\$sustainedPoseControllerProof"'
  SustainedPoseControllerGate =
    '\$ElliottWalkSeconds -ge 60'
  Mode204WalkMinimumRate =
    '\$walkMinimumTransactionAdvance =\s+\[uint64\]\$ElliottWalkSeconds \* \[uint64\]20'
  Mode204WalkRetryState = '"WAIT_WALK_RECOVERY"'
  Mode204WalkHeartbeatGate = '\$walkHeartbeatPauseFree'
  Mode204WalkFrameGate = '\$walkFrameContinuityReady'
  Mode204WalkPositionParser = 'Get-LatestMode204FirstPersonPosition'
  Mode204WalkDistanceGate =
    '\$walkPlanarDistance -ge \$walkMinimumPlanarDistance'
  Mode204AcceptedStickPacket =
    '\$controllerStickConsumedPacket -gt\s+\$walkStickPacketBaseline'
  Mode204AcceptedNeutralPacket =
    '\$controllerNeutralConsumedPacket -gt\s+\$walkNeutralPacketBaseline'
  Mode204WalkQualityResult =
    '"walkQualityReady=\$walkQualityReady"'
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
  GameplaySettleState = '"WAIT_GAMEPLAY_SETTLE"'
  GameplaySettleDelay = '\[DateTime\]::UtcNow\.AddSeconds\(3\)'
  GameplaySettleContinuousReset =
    '\$elliottNextActionAt = \$proofNow\.AddSeconds\(3\)'
  GameplaySettleProducerAdvance =
    '\$producerLatestTransaction -gt\s+\$gameplaySettleProducerBaseline'
  GameplaySettleHostAdvance =
    '\$hostLatestTransaction -gt\s+\$gameplaySettleHostBaseline'
  GameplaySettleTimeout = '"GAMEPLAY_SETTLE_TIMEOUT:'
  GameplaySettlePass =
    '"ELLIOTT CLI: gameplay settle PASS with advancing producer/"'
  RockstarHandoffRetry = '"AUTH RETRY: Rockstar did not hand off to GTAIV\.exe'
  OneAuthRetry = '\$authRetryAttempted = \$true'
  RetryNoTitleArguments = '"app 12210 once more through regular Steam with no custom arguments"'
  RockstarPathScope = '\(Join-Path \$env:ProgramFiles "Rockstar Games"\)'
  RockstarTitleGuard = '"Refusing Rockstar launcher cleanup while GTAIV/PlayGTAIV is active\."'
  RockstarPreflight = 'Stop-StaleRockstarLauncherSession "stale preflight Rockstar session"'
  RockstarFinalCleanup =
    'Stop-StaleRockstarLauncherSession "end supervised GTA run"'
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

$mode204AdapterScopes = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.IfStatementAst] -and
    $node.Extent.Text -match 'if \(\$StereoMode -eq 204\)' -and
    $node.Extent.Text -match 'OpenXRSubmitAdapter: Mode204' -and
    $node.Extent.Text -match '\$mode204GpuHostReady'
  },
  $true
))
if ($mode204AdapterScopes.Count -ne 1) {
  throw (
    "Expected one isolated literal Mode204 adapter/GPU proof scope; found " +
    "$($mode204AdapterScopes.Count)."
  )
}
$mode204AdapterScopeText = $mode204AdapterScopes[0].Extent.Text
foreach ($legacyEvidence in @(
    "StereoOpenXRFused",
    "OpenXRBridge: CPU mailbox",
    "SquareCommandLine"
  )) {
  if ($mode204AdapterScopeText -match [regex]::Escape($legacyEvidence)) {
    throw (
      "Literal Mode204 proof scope accepts legacy renderer/transport " +
      "evidence: $legacyEvidence"
    )
  }
}

$gameplaySettleTransition = $launcher.IndexOf(
  '$elliottProofState = "WAIT_GAMEPLAY_SETTLE"'
)
$gameplaySettleState = $launcher.IndexOf(
  '"WAIT_GAMEPLAY_SETTLE" {',
  $gameplaySettleTransition + 1
)
$settledPauseStart = $launcher.IndexOf(
  '[void](Send-ElliottControllerSnapshot -Kind "MenuStart")',
  $gameplaySettleState + 1
)
if ($gameplaySettleTransition -lt 0 -or
    $gameplaySettleState -le $gameplaySettleTransition -or
    $settledPauseStart -le $gameplaySettleState) {
  throw (
    "Pause automation must transition through the bounded gameplay-settle " +
    "state before sending the in-game Start pulse."
  )
}

$mode58RecordedWalkPose = $launcher.IndexOf(
  '"ELLIOTT CLI: Mode$StereoMode pose sweep active for "'
)
$mode58RecordedWalkStick = $launcher.IndexOf(
  '[void](Send-ElliottControllerSnapshot -Kind "StickForward")',
  $mode58RecordedWalkPose + 1
)
$mode58RecordedWalkMarker = $launcher.IndexOf(
  '$walkReadyTemporary = "$ElliottWalkReadyMarker.tmp"',
  $mode58RecordedWalkStick + 1
)
if ($mode58RecordedWalkPose -lt 0 -or
    $mode58RecordedWalkStick -le $mode58RecordedWalkPose -or
    $mode58RecordedWalkMarker -le $mode58RecordedWalkStick) {
  throw (
    "Mode58 recorded-walk ordering must enable pose sweep, command " +
    "StickForward, then publish the capture marker."
  )
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

$sampledGpuEvidenceFunction = $launcherAst.Find(
  {
    param($node)
    $node -is
      [System.Management.Automation.Language.FunctionDefinitionAst] -and
      $node.Name -eq "Get-SampledGpuWorldRouteEvidence"
  },
  $true
)
if (-not $sampledGpuEvidenceFunction) {
  throw "Get-SampledGpuWorldRouteEvidence AST was not found."
}
. ([scriptblock]::Create($sampledGpuEvidenceFunction.Extent.Text))

$firstPersonPositionFunction = $launcherAst.Find(
  {
    param($node)
    $node -is
      [System.Management.Automation.Language.FunctionDefinitionAst] -and
      $node.Name -eq "Get-LatestMode204FirstPersonPosition"
  },
  $true
)
if (-not $firstPersonPositionFunction) {
  throw "Get-LatestMode204FirstPersonPosition AST was not found."
}
. ([scriptblock]::Create($firstPersonPositionFunction.Extent.Text))

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

$mode204GameRouteSnapshot =
  "presentation=stationary-ui-quad`n" +
  "presentation=world-stereo"
$mode204HostRouteSnapshot =
  "presentation=stationary-ui-quad`n" +
  "presentation=world-parent-dual-stereo"
$mode204StationaryRoute =
  (
    $mode204GameRouteSnapshot.LastIndexOf(
      "presentation=stationary-ui-quad") -gt
    $mode204GameRouteSnapshot.LastIndexOf(
      "presentation=world-stereo")
  ) -or (
    $mode204HostRouteSnapshot.LastIndexOf(
      "presentation=stationary-ui-quad") -gt
    $mode204HostRouteSnapshot.LastIndexOf(
      "presentation=world-parent-dual-stereo")
  )
if ($mode204StationaryRoute) {
  throw "Mode204 asymmetric producer/host world route remained stuck on UI."
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
$mode58Arguments = @(Get-OpenXrHostArguments -Mode 58 -TimeoutMs 990000)
$mode204Arguments = @(Get-OpenXrHostArguments -Mode 204 -TimeoutMs 990000)
foreach ($nonTemporalArguments in @(
    $mode55Arguments,
    $mode56Arguments,
    $mode58Arguments,
    $mode204Arguments
  )) {
  if ($nonTemporalArguments -contains "--allow-temporal-stereo") {
    throw "Temporal host opt-in leaked into a non-temporal mode."
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

$sampledGpuGame =
  "producer=1`nBOUNDARY`nproducer=2400`nproducer=2520`n" +
  "producer=2640`nproducer=2760`nproducer=2880"
$sampledGpuHost =
  "host=1`nBOUNDARY`nhost=2386`nhost=2511`nhost=2732`nhost=2870"
$sampledGpuEvidence = Get-SampledGpuWorldRouteEvidence `
  -GameText $sampledGpuGame `
  -HostText $sampledGpuHost `
  -ProducerPattern $producerPattern `
  -HostPattern $hostPattern `
  -GameAfterIndex $sampledGpuGame.IndexOf("BOUNDARY") `
  -HostAfterIndex $sampledGpuHost.IndexOf("BOUNDARY")
if (-not $sampledGpuEvidence.Ready -or
    $sampledGpuEvidence.ProducerCount -ne 5 -or
    $sampledGpuEvidence.HostCount -ne 4 -or
    $sampledGpuEvidence.CoupledCount -ne 3 -or
    $sampledGpuEvidence.CoupledProducerCount -ne 4 -or
    $sampledGpuEvidence.CoupledHostCount -ne 3 -or
    $sampledGpuEvidence.OverlapStart -ne 2400 -or
    $sampledGpuEvidence.OverlapEnd -ne 2870) {
  throw (
    "Different-cadence Mode204 GPU samples did not prove coupled " +
    "monotonic endpoint advancement."
  )
}
$sampledGpuExactIds = @(@(
    2400,
    2520,
    2640,
    2760,
    2880
  ) | Where-Object {
    @(2386, 2511, 2732, 2870) -contains $_
  })
if ($sampledGpuExactIds.Count -ne 0) {
  throw "Sampled GPU proof fixture unexpectedly contained exact shared IDs."
}

$uncoupledGpuHost =
  "host=1`nBOUNDARY`nhost=3000`nhost=3120`nhost=3240"
$uncoupledGpuEvidence = Get-SampledGpuWorldRouteEvidence `
  -GameText $sampledGpuGame `
  -HostText $uncoupledGpuHost `
  -ProducerPattern $producerPattern `
  -HostPattern $hostPattern `
  -GameAfterIndex $sampledGpuGame.IndexOf("BOUNDARY") `
  -HostAfterIndex $uncoupledGpuHost.IndexOf("BOUNDARY")
if ($uncoupledGpuEvidence.Ready) {
  throw "Disjoint Mode204 producer/host transaction ranges passed."
}

$thinOverlapGpuHost =
  "host=1`nBOUNDARY`nhost=2760`nhost=2820`nhost=2940"
$thinOverlapGpuEvidence = Get-SampledGpuWorldRouteEvidence `
  -GameText $sampledGpuGame `
  -HostText $thinOverlapGpuHost `
  -ProducerPattern $producerPattern `
  -HostPattern $hostPattern `
  -GameAfterIndex $sampledGpuGame.IndexOf("BOUNDARY") `
  -HostAfterIndex $thinOverlapGpuHost.IndexOf("BOUNDARY")
if (-not $thinOverlapGpuEvidence.Ready -or
    $thinOverlapGpuEvidence.CoupledProducerCount -ne 2 -or
    $thinOverlapGpuEvidence.CoupledHostCount -ne 2) {
  throw (
    "Mode204 phased samples with independent monotonic advancement and a " +
    "nonempty range overlap did not pass."
  )
}

$regressingGpuHost =
  "host=1`nBOUNDARY`nhost=2386`nhost=2732`nhost=2511`nhost=2870"
$regressingGpuEvidence = Get-SampledGpuWorldRouteEvidence `
  -GameText $sampledGpuGame `
  -HostText $regressingGpuHost `
  -ProducerPattern $producerPattern `
  -HostPattern $hostPattern `
  -GameAfterIndex $sampledGpuGame.IndexOf("BOUNDARY") `
  -HostAfterIndex $regressingGpuHost.IndexOf("BOUNDARY")
if ($regressingGpuEvidence.Ready) {
  throw "Regressing Mode204 host transaction samples passed."
}

$archivedMode204Game =
  "OpenXRBridge: frame transaction=2160 presentation=world-stereo`n" +
  "OpenXRBridge: frame transaction=2280 presentation=world-stereo`n" +
  "OpenXRBridge: frame transaction=2400 presentation=world-stereo`n" +
  "OpenXRBridge: frame transaction=2520 presentation=world-stereo`n" +
  "OpenXRBridge: frame transaction=2640 presentation=world-stereo"
$archivedMode204Host =
  "GameBridge: frame acquired transaction=2121 " +
  "presentation=world-parent-dual-stereo`n" +
  "XRHost: game swapchain updated transaction=2200 " +
  "presentation=world-parent-dual-stereo`n" +
  "XRHost: game swapchain reused transaction=2284 " +
  "presentation=world-parent-dual-stereo`n" +
  "GameBridge: frame acquired transaction=2559 " +
  "presentation=world-parent-dual-stereo`n" +
  "XRHost: game swapchain reused transaction=2710 " +
  "presentation=world-parent-dual-stereo"
$archivedMode204Evidence = Get-SampledGpuWorldRouteEvidence `
  -GameText $archivedMode204Game `
  -HostText $archivedMode204Host `
  -ProducerPattern (
    "OpenXRBridge: frame transaction=(\d+)[^\r\n]*" +
    "presentation=world-stereo"
  ) `
  -HostPattern (
    "(?:GameBridge: frame acquired|" +
    "XRHost: game swapchain (?:updated|reused)) " +
    "transaction=(\d+)[^\r\n]*" +
    "presentation=world-parent-dual-stereo"
  ) `
  -GameAfterIndex -1 `
  -HostAfterIndex -1
if (-not $archivedMode204Evidence.Ready -or
    $archivedMode204Evidence.ProducerCount -ne 5 -or
    $archivedMode204Evidence.HostCount -ne 5 -or
    $archivedMode204Evidence.CoupledProducerCount -ne 5 -or
    $archivedMode204Evidence.CoupledHostCount -ne 3) {
  throw "Archived Mode204 mixed acquired/update/reuse host samples did not pass."
}

$positionLog =
  "CamMatrix: FP lock #100 L pos=(10.000,-20.000,5.000) eye=left`n" +
  "CamMatrix: FP lock #101 R pos=(99.000,99.000,99.000) eye=right`n" +
  "CamMatrix: FP lock #200 L pos=(13.000,-16.000,5.500) eye=left"
$firstPosition =
  Get-LatestMode204FirstPersonPosition `
    -Text $positionLog.Substring(
      0,
      $positionLog.LastIndexOf("CamMatrix: FP lock #200")
    )
$latestPosition =
  Get-LatestMode204FirstPersonPosition `
    -Text $positionLog `
    -AfterIndex $firstPosition.Index
if (-not $firstPosition.Found -or
    -not $latestPosition.Found -or
    $firstPosition.X -ne 10.0 -or
    $firstPosition.Y -ne -20.0 -or
    $latestPosition.X -ne 13.0 -or
    $latestPosition.Y -ne -16.0) {
  throw "Mode204 first-person position parser did not isolate fresh left-eye positions."
}
$positionDistance = [Math]::Sqrt(
  [Math]::Pow($latestPosition.X - $firstPosition.X, 2) +
  [Math]::Pow($latestPosition.Y - $firstPosition.Y, 2)
)
if ([Math]::Abs($positionDistance - 5.0) -gt 0.0001) {
  throw "Mode204 planar movement fixture was not 5.0 units."
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
    "sequence=2 hostModes=5 worldEvidence=3 mode204Scope=1 menuWait=1 " +
    "gameplaySettle=1 textSnapshot=2 asymmetricRoute=1"
  ) -f $requiredPatterns.Count, $forbiddenPatterns.Count
)

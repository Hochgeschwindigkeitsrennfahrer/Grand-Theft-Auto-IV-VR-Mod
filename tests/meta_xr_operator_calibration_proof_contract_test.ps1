$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$harnessPath = Join-Path `
  $PSScriptRoot "..\scripts\run-meta-xr-operator-calibration-proof.ps1"
$harness = Get-Content -LiteralPath $harnessPath -Raw

$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
  (Resolve-Path -LiteralPath $harnessPath).Path,
  [ref]$tokens,
  [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
  throw (
    "Operator proof harness has PowerShell parser errors: " +
    (($parseErrors | ForEach-Object Message) -join " | ")
  )
}

$requiredPatterns = [ordered]@{
  MetaSimulatorOnly =
    '\$runtimeInfo\.Kind -ne "MetaSimulator"'
  ExactLayerName =
    '"XR_APILAYER_METAX_operator"'
  SignedLibrary =
    'Test-MetaSignature \$operatorLibraryPath'
  SignedProxy =
    'Test-MetaSignature \$operatorProxyPath'
  X64Library =
    'Get-PeMachine \$operatorLibraryPath\) -ne 0x8664'
  X64Proxy =
    'Get-PeMachine \$operatorProxyPath\) -ne 0x8664'
  X64Host =
    'Get-PeMachine \$hostExe\) -ne 0x8664'
  SteamVrPreflight =
    '\$steamVrPreflight = @\(Get-SteamVrProcesses\)'
  SteamVrReject =
    'SteamVR is running\. Close it before this direct Meta Simulator proof'
  ExistingHostPreflight =
    '\$existingHosts = @\('
  ExistingHostNoKill =
    'this harness never stops a pre-existing host'
  OperatorPortPreflight =
    'if \(Test-PortListener -Port 8720\)'
  ChildRuntime =
    '\$env:XR_RUNTIME_JSON = \$runtimeManifestResolved'
  ChildLayerPath =
    '\$env:XR_API_LAYER_PATH = \$operatorDirectoryResolved'
  ChildLayerEnable =
    '\$env:XR_ENABLE_API_LAYERS = "XR_APILAYER_METAX_operator"'
  RestoreRuntime =
    'Restore-EnvironmentValue\s+`\s+-Name "XR_RUNTIME_JSON"'
  RestoreLayerPath =
    'Restore-EnvironmentValue\s+`\s+-Name "XR_API_LAYER_PATH"'
  RestoreLayerEnable =
    'Restore-EnvironmentValue\s+`\s+-Name "XR_ENABLE_API_LAYERS"'
  HostFrameBound =
    '"--frames"'
  HostTimeBound =
    '"--timeout-ms"'
  ProofDeadline =
    '\$script:proofDeadlineUtc = \[DateTime\]::UtcNow\.AddSeconds\(\$TimeoutSeconds\)'
  InvokeExistingClient =
    '"invoke-meta-xr-operator\.ps1"'
  ReadExistingBridge =
    '"read-openxr-pose-bridge\.ps1"'
  InstanceInfo =
    '"openxr_get_instance_info"'
  SessionInfo =
    '"openxr_get_session_info"'
  SessionFocusedValue =
    '\$stateValue -eq 5'
  SessionFocusedName =
    '\$stateName -eq "XR_SESSION_STATE_FOCUSED"'
  InteractionProfile =
    '"openxr_get_active_interaction_profile"'
  TouchPlus =
    '"/interaction_profiles/meta/touch_controller_plus"'
  GetHead =
    '"openxr_get_head_pose"'
  SetHead =
    '"openxr_set_head_pose"'
  LocalFloor =
    'base_space = "local_floor"'
  BaselineHead =
    'position = @\(0\.0, 1\.65, 0\.0\)'
  MovedHead =
    'position = @\(0\.2, 1\.65, -0\.3\)'
  MovedYaw =
    'orientation = @\(0\.0, 0\.258819, 0\.0, 0\.965926\)'
  GetController =
    '"openxr_get_controller_pose"'
  SetController =
    '"openxr_set_controller_pose"'
  GripEnum =
    '\[ValidateSet\("grip", "aim"\)\]'
  LeftGripTarget =
    'leftGrip = \[ordered\]@\{'
  LeftAimTarget =
    'leftAim = \[ordered\]@\{'
  RightGripTarget =
    'rightGrip = \[ordered\]@\{'
  RightAimTarget =
    'rightAim = \[ordered\]@\{'
  InputTool =
    '"openxr_set_controller_input"'
  LeftStickComponent =
    '-Component "Thumbstick"\s+`\s+-SubComponent "Y"\s+`\s+-Value 0\.8'
  RightAComponent =
    '-Component "A"\s+`\s+-Value 1\.0'
  LeftStickRelease =
    '-Component "Thumbstick"\s+`\s+-SubComponent "Y"\s+`\s+-Value 0\.0'
  RightARelease =
    '-Component "A"\s+`\s+-Value 0\.0'
  NoAutoRelease =
    'auto_release = \$false'
  CaptureTool =
    '"openxr_capture_composited_image"'
  BeforeLeft =
    '"before-left\.png"'
  BeforeRight =
    '"before-right\.png"'
  AfterLeft =
    '"after-left\.png"'
  AfterRight =
    '"after-right\.png"'
  BaselineBridge =
    '"pose-bridge-baseline\.json"'
  MovedBridge =
    '"pose-bridge-moved\.json"'
  ControllerBridge =
    '"pose-bridge-controllers\.json"'
  HeldBridge =
    '"pose-bridge-input-held\.json"'
  NeutralBridge =
    '"pose-bridge-input-neutral\.json"'
  LeftGripAimFlags =
    '\(\$leftActive -band 0x03\) -eq 0x03'
  RightGripAimFlags =
    '\(\$rightActive -band 0x03\) -eq 0x03'
  LeftStickBridgeGate =
    'leftController\.thumbstick\[1\] -ge 0\.70'
  RightABridgeGate =
    '\(\$heldRightButtons -band 0x01\) -eq 0x01'
  NeutralStickGate =
    '\) -le 0\.05'
  NeutralAGate =
    '\(\$neutralRightButtons -band 0x01\) -eq 0'
  HeadBridgeMotionGate =
    'poseBridgeHeadMotionMeters -ge 0\.30'
  ImageChangeGate =
    'bothEyesChangedAfterHeadMotion'
  FrameAdvanceGate =
    'poseBridgeAdvanced'
  ResultArtifact =
    '"result\.txt"'
  SummaryArtifact =
    '"summary\.json"'
  TranscriptArtifact =
    '"operator-transcript\.jsonl"'
  HashManifest =
    '"artifacts\.sha256"'
  BinaryHashes =
    'operatorLibrarySha256=\$\(\$summary\.operator\.librarySha256\)'
  CaptureHashes =
    'beforeLeftSha256=\$\(\$summary\.captures\.beforeLeft\.sha256\)'
  CompleteOutcome =
    '"PROOF_COMPLETE"'
  CleanupDeadline =
    '\$script:proofDeadlineUtc = \[DateTime\]::UtcNow\.AddSeconds\(40\)'
  StopOwnedHost =
    'Stop-Process\s+`\s+-Id \$script:hostProcess\.Id'
  CleanupPort =
    'cleanupPortReleased'
  FinalSteamVr =
    '\$steamVrFinal = @\(Get-SteamVrProcesses\)'
  NoGuiClaim =
    'simulatorGuiControlled = \$false'
  NoSteamStartClaim =
    'steamVrStarted = \$false'
}

foreach ($entry in $requiredPatterns.GetEnumerator()) {
  if ($harness -notmatch $entry.Value) {
    throw "Operator proof contract is missing $($entry.Key)."
  }
}

$forbiddenPatterns = [ordered]@{
  AnimatedPose =
    'duration_seconds'
  GtaLauncher =
    'run-openxr-gta(?:-steam-safe)?\.ps1'
  GtaExecutable =
    '(?i)(?:GTAIV|PlayGTAIV)\.exe'
  SteamExecutable =
    '(?i)steam\.exe'
  SteamVrExecutable =
    '(?i)(?:vrmonitor|vrserver|vrcompositor|vrdashboard)\.exe'
  MetaSimulatorExecutable =
    '(?i)(?:MetaXRSimulator|XrSimulator)\.exe'
  StartUri =
    '(?i)steam://'
  SendKeys =
    '(?i)SendKeys'
  MouseEvent =
    '(?i)mouse_event'
  KeyboardEvent =
    '(?i)keybd_event'
  CursorControl =
    '(?i)SetCursorPos'
  ForegroundControl =
    '(?i)SetForegroundWindow'
  WindowLookup =
    '(?i)FindWindow'
  UiAutomation =
    '(?i)UIAutomation'
  ComAutomation =
    '(?i)WScript\.Shell'
  ComputerVision =
    '(?i)(?:pyautogui|opencv|cv2)'
}
foreach ($entry in $forbiddenPatterns.GetEnumerator()) {
  if ($harness -match $entry.Value) {
    throw "Operator proof contains forbidden surface $($entry.Key)."
  }
}

$startCommands = @($ast.FindAll(
  {
    param($node)
    if ($node -isnot
        [Management.Automation.Language.CommandAst]) {
      return $false
    }
    return $node.GetCommandName() -eq "Start-Process"
  },
  $true
))
if ($startCommands.Count -ne 1) {
  throw (
    "Operator proof must have exactly one process-start primitive for the " +
    "owned x64 host; found $($startCommands.Count)."
  )
}
$startExtent = $startCommands[0].Extent.Text
if ($startExtent -notmatch '-FilePath \$hostExe' -or
    $startExtent -notmatch '-WindowStyle Hidden' -or
    $startExtent -notmatch '-PassThru') {
  throw (
    "The only process start must be the hidden, owned x64 host with a " +
    "returned process handle."
  )
}

$whileLoops = @($ast.FindAll(
  {
    param($node)
    $node -is [Management.Automation.Language.WhileStatementAst]
  },
  $true
))
foreach ($loop in $whileLoops) {
  $text = $loop.Extent.Text
  if ($text -match 'Get-SteamVrProcesses|Get-Process') {
    throw "SteamVR/process enumeration must not occur in a polling loop."
  }
  if ($text -notmatch 'Deadline|deadline') {
    throw "Every Operator proof polling loop must have an explicit deadline."
  }
}

$setControllerCalls = @(
  [regex]::Matches($harness, 'Set-ControllerPose\s+`')
)
if ($setControllerCalls.Count -lt 5) {
  throw (
    "Expected four deterministic controller injections and the looped cleanup " +
    "restore; found only $($setControllerCalls.Count) Set-ControllerPose calls."
  )
}

Write-Host (
  "MetaXrOperatorCalibrationProofContractTest: PASS " +
  "cliOnly=1 guiControl=0 steamVrStart=0 hostStarts=1 " +
  "head=1 controllers=4 inputHeldNeutral=1 captures=4 " +
  "poseBridge=1 hashes=1 boundedCleanup=1"
)

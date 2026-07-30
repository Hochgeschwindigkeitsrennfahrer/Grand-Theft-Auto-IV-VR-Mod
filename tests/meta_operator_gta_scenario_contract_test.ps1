$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$scriptPath =
  Join-Path $root "scripts\run-meta-xr-operator-gta-scenario.ps1"
$scenarioPath =
  Join-Path $root "config\meta-operator-roman-apartment-street-car.json"
$source = Get-Content -LiteralPath $scriptPath -Raw
$scenario = Get-Content -LiteralPath $scenarioPath -Raw |
  ConvertFrom-Json -ErrorAction Stop

$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
  $scriptPath,
  [ref]$tokens,
  [ref]$errors
)
if ($errors.Count -ne 0) {
  throw (
    "Scenario script has PowerShell parse errors: " +
    (($errors | ForEach-Object Message) -join " | ")
  )
}

foreach ($required in ([ordered]@{
    ValidateOnly = 'MetaXrOperatorGtaScenarioPlan: PASS'
    AuthorizationGate = 'Live controller/head automation is locked'
    MetaRuntimeOnly = 'runtimeKind=MetaSimulator'
    Mode204Only = 'literal upstream Mode 204'
    PinnedOperatorBundle = 'pinned official 205\.1'
    SignedOperatorBundle = 'Get-AuthenticodeSignature'
    NoGuiControlReceipt = 'simulatorGuiControl=False'
    NoRendererChangesReceipt = 'rendererChanges=False'
    SteamVrBoundaryOnly = 'Assert-SteamVrAbsent "scenario-preflight"'
    OperatorFocused = 'openxr_get_session_info'
    TouchProfile = 'openxr_get_active_interaction_profile'
    BothGripAim = 'foreach \(\$poseType in @\("grip", "aim"\)\)'
    ControllerPoseReadback = 'openxr_get_controller_pose'
    HeadPoseReadback = 'openxr_get_head_pose'
    StableHeadOnce = 'HEAD LOCK: stable OpenXR head pose set once'
    StableHeadReceipt = 'headPoseSetCount='
    HeadYawTelemetry = 'LookMove: heading='
    ControllerSteering = 'Set-WorldHeadingWithController'
    SteeringPulseReceipt = 'controllerSteeringPulseCount='
    RightStickTurn = '-Hand "right" `\s+-Component "Thumbstick"'
    CoordinateTelemetry = 'CamMatrix: FP lock'
    ProducerContinuity = 'presentation=world-stereo'
    HostContinuity = 'presentation=world-parent-dual-stereo'
    HeartbeatGate = 'GameBridge: GTA frame heartbeat paused'
    InputConsumption = 'OpenXRController: GTA consumed packet='
    LeftYVehicleMapping = '-Hand "left" `\s+-Component "Y"'
    VehicleOccupancy = 'VehCam: INSIDE vehicle\|EnterFp: vehicle ptr set'
    RightTriggerAcceleration = '-Hand "right" `\s+-Component "Trigger"'
    VehicleDriveProof = 'VEHICLE DRIVE PASS'
    VehicleDriveReceipt = 'vehicleDriveDistanceMeters='
    OutsideGate = 'outsideReached='
    StreetGate = 'streetReached='
    NeutralFinally = 'Set-AllInputsNeutral -BestEffort \$true'
    PairedCaptures = 'openxr_capture_composited_image'
    BoundedStall = 'after three bounded pulses'
    StatusOffSuccessPipeline = '\[Console\]::Out\.WriteLine\(\$line\)'
  }).GetEnumerator()) {
  if ($source -notmatch $required.Value) {
    throw "Missing Meta Operator scenario contract: $($required.Key)"
  }
}

$headPoseSetCalls = @(
  [regex]::Matches($source, '-Name "openxr_set_head_pose"')
).Count
if ($headPoseSetCalls -ne 1) {
  throw (
    "Route must set one stable head pose exactly once; found " +
    "$headPoseSetCalls Operator head-pose setters."
  )
}
if ($source -match
    'function (?:Set-WorldHeading|Get-WorldHeadingCalibration)\b') {
  throw "Absolute head-yaw route calibration/steering is still present."
}

foreach ($forbidden in ([ordered]@{
    SteamLaunch = '(?i)steam\.exe|-applaunch'
    GtaLaunch = '(?i)Start-Process[^\r\n]+GTA|PlayGTAIV\.exe'
    SteamVrLaunch = '(?i)vrmonitor\.exe|vrstartup|steam://rungameid/250820'
    SimulatorGui = '(?i)SetForegroundWindow|ShowWindow|SendKeys|mouse_event'
    SaveMutation = '(?i)SGTA4\d+.*(?:Set-Content|WriteAllBytes|Copy-Item)'
    RendererSource = '(?i)stereo_render\.cpp|openxr_bridge\.cpp'
    Warp = '(?i)SET_CHAR_COORDINATES|teleport'
  }).GetEnumerator()) {
  if ($source -match $forbidden.Value) {
    throw "Forbidden Meta Operator scenario behavior: $($forbidden.Key)"
  }
}

if ([int]$scenario.schemaVersion -ne 1 -or
    [int]$scenario.stereoMode -ne 204 -or
    [string]$scenario.runtimeKind -cne "MetaSimulator") {
  throw "Scenario JSON is not locked to schema 1 / Mode204 / MetaSimulator."
}
foreach ($hashName in @(
    "manifestSha256",
    "librarySha256",
    "proxySha256"
  )) {
  if ([string]$scenario.operatorBundle.$hashName -notmatch
      '^[0-9A-F]{64}$') {
    throw "Scenario JSON does not pin operatorBundle.$hashName."
  }
}
if ([bool]$scenario.coordinateNotes.teleportOrWarpUsed -or
    -not [bool]$scenario.coordinateNotes.saveFilesAreReadOnly) {
  throw "Scenario JSON permits a warp or writable save."
}
if (@($scenario.route).Count -lt 3 -or
    @($scenario.vehicleAttempts).Count -lt 1) {
  throw "Scenario JSON does not reach outside/street and attempt a vehicle."
}
$drive = $scenario.vehicleDriveProof
if ($null -eq $drive -or
    [double]$drive.triggerValue -lt 0.5 -or
    [int]$drive.maxPulses -lt 1 -or
    [double]$drive.minPlanarDistanceMeters -lt 0.5) {
  throw "Scenario JSON does not define bounded RT vehicle drive proof."
}

$validationOutput = & $scriptPath `
  -ScenarioPath $scenarioPath `
  -ValidateOnly
if (($validationOutput -join "`n") -notmatch
      'launchesGame=0 simulatorGuiControl=0 rendererChanges=0') {
  throw "Scenario validate-only mode did not prove the offline safety surface."
}

Write-Output (
  "MetaOperatorGtaScenarioContractTest: PASS " +
  "mode204Unchanged=1 cliOnly=1 gripAimRequired=1 " +
  "stableHead=1 rightStickSteering=1 vehicleDrive=1 " +
  "telemetryClosedLoop=1 neutralFinally=1 noLaunch=1"
)

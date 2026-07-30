[CmdletBinding()]
param(
  [string]$RuntimeManifest =
    "C:\Program Files\MetaXRSimulator\v205.0\meta_openxr_simulator.json",

  [string]$OperatorDirectory =
    (
      Join-Path (
        Join-Path (
          Join-Path (
            Join-Path $PSScriptRoot "..\out-openxr\external"
          ) "meta-xr-operator-standalone-205.1"
        ) "extracted\meta-xr-operator-standalone-public"
      ) "windows"
    ),

  [string]$OutputDirectory = "",

  [string]$MappingName = "Local\GTAIV_XR_PoseBridge_v1",

  [ValidateRange(45, 300)]
  [uint32]$TimeoutSeconds = 120,

  [ValidateRange(5, 30)]
  [uint32]$ToolTimeoutSeconds = 15,

  [ValidateRange(3000, 36000)]
  [uint64]$FrameLimit = 18000
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "openxr-runtime-info.ps1")

$invokeOperatorPath =
  (Resolve-Path (
    Join-Path $PSScriptRoot "invoke-meta-xr-operator.ps1"
  )).Path
$readPoseBridgePath =
  (Resolve-Path (
    Join-Path $PSScriptRoot "read-openxr-pose-bridge.ps1"
  )).Path
$hostExe =
  (Resolve-Path (
    Join-Path $root "out-openxr\gtaiv_xr_host.exe"
  )).Path
$runtimeManifestResolved =
  (Resolve-Path -LiteralPath $RuntimeManifest).Path
$operatorDirectoryResolved =
  (Resolve-Path -LiteralPath $OperatorDirectory).Path

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
  $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
  $OutputDirectory = Join-Path (
    Join-Path $root "out-openxr\operator-calibration-proofs"
  ) $stamp
}
$outputDirectoryResolved = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputDirectoryResolved) {
  throw "Proof output directory already exists: $outputDirectoryResolved"
}
[void](New-Item -ItemType Directory -Path $outputDirectoryResolved)

$resultPath = Join-Path $outputDirectoryResolved "result.txt"
$summaryPath = Join-Path $outputDirectoryResolved "summary.json"
$errorPath = Join-Path $outputDirectoryResolved "error.txt"
$transcriptPath =
  Join-Path $outputDirectoryResolved "operator-transcript.jsonl"
$hostLogDeltaPath =
  Join-Path $outputDirectoryResolved "host-log-delta.txt"
$artifactManifestPath =
  Join-Path $outputDirectoryResolved "artifacts.sha256"

function Write-Utf8NoBom {
  param(
    [Parameter(Mandatory)]
    [string]$Path,

    [Parameter(Mandatory)]
    [AllowEmptyString()]
    [string]$Text
  )

  $encoding = [Text.UTF8Encoding]::new($false)
  [IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Append-Utf8NoBom {
  param(
    [Parameter(Mandatory)]
    [string]$Path,

    [Parameter(Mandatory)]
    [AllowEmptyString()]
    [string]$Text
  )

  $encoding = [Text.UTF8Encoding]::new($false)
  [IO.File]::AppendAllText($Path, $Text, $encoding)
}

function Get-Sha256 {
  param(
    [Parameter(Mandatory)]
    [string]$Path
  )

  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Get-PeMachine {
  param(
    [Parameter(Mandatory)]
    [string]$Path
  )

  $stream = [IO.File]::Open(
    $Path,
    [IO.FileMode]::Open,
    [IO.FileAccess]::Read,
    [IO.FileShare]::ReadWrite
  )
  try {
    $reader = [IO.BinaryReader]::new($stream)
    try {
      if ($reader.ReadUInt16() -ne 0x5A4D) {
        throw "Not a PE file: $Path"
      }
      $stream.Position = 0x3C
      $peOffset = $reader.ReadUInt32()
      $stream.Position = $peOffset
      if ($reader.ReadUInt32() -ne 0x00004550) {
        throw "Invalid PE signature: $Path"
      }
      return $reader.ReadUInt16()
    }
    finally {
      $reader.Dispose()
    }
  }
  finally {
    $stream.Dispose()
  }
}

function Test-PortListener {
  param(
    [Parameter(Mandatory)]
    [uint16]$Port
  )

  $listeners = @(
    Get-NetTCPConnection `
      -State Listen `
      -LocalPort $Port `
      -ErrorAction SilentlyContinue
  )
  return $listeners.Count -gt 0
}

function Get-SteamVrProcesses {
  return @(
    Get-Process `
      -Name vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper `
      -ErrorAction SilentlyContinue
  )
}

function Test-MetaSignature {
  param(
    [Parameter(Mandatory)]
    [string]$Path
  )

  $signature = Get-AuthenticodeSignature -LiteralPath $Path
  return (
    [string]$signature.Status -eq "Valid" -and
    $null -ne $signature.SignerCertificate -and
    [string]$signature.SignerCertificate.Subject -match
      '(?:^|,\s*)O="?Meta Platforms, Inc\."?(?:,|$)'
  )
}

function Restore-EnvironmentValue {
  param(
    [Parameter(Mandatory)]
    [string]$Name,

    [AllowNull()]
    [string]$PriorValue
  )

  if ($null -eq $PriorValue) {
    Remove-Item -LiteralPath "Env:$Name" -ErrorAction SilentlyContinue
  }
  else {
    Set-Item -LiteralPath "Env:$Name" -Value $PriorValue
  }
}

function Convert-OperatorPayload {
  param(
    [Parameter(Mandatory)]
    [object]$Result,

    [Parameter(Mandatory)]
    [string]$ToolName
  )

  if ($null -eq $Result.PSObject.Properties["content"]) {
    throw "Operator $ToolName returned no content."
  }
  $textItems = @($Result.content | Where-Object {
    $null -ne $_.PSObject.Properties["type"] -and
    [string]$_.type -eq "text" -and
    $null -ne $_.PSObject.Properties["text"]
  })
  if ($textItems.Count -ne 1) {
    throw (
      "Operator $ToolName returned $($textItems.Count) text payloads; " +
      "expected exactly one."
    )
  }
  $text = [string]$textItems[0].text
  if ($text.TrimStart().StartsWith("Error:", [StringComparison]::OrdinalIgnoreCase)) {
    throw "Operator $ToolName returned: $text"
  }
  try {
    return $text | ConvertFrom-Json -ErrorAction Stop
  }
  catch {
    throw (
      "Operator $ToolName returned non-JSON text: $text " +
      "($($_.Exception.Message))"
    )
  }
}

function Invoke-Operator {
  param(
    [Parameter(Mandatory)]
    [string]$Name,

    [Parameter(Mandatory)]
    [Collections.IDictionary]$Arguments,

    [string]$ImageOutputPath = "",

    [uint32]$CallTimeoutSeconds = $ToolTimeoutSeconds
  )

  if ([DateTime]::UtcNow -ge $script:proofDeadlineUtc) {
    throw "Operator proof exceeded its $TimeoutSeconds-second deadline."
  }
  $remainingSeconds = [Math]::Floor(
    ($script:proofDeadlineUtc - [DateTime]::UtcNow).TotalSeconds
  )
  if ($remainingSeconds -lt 5) {
    throw "Less than five seconds remain in the Operator proof deadline."
  }
  $effectiveTimeout = [uint32][Math]::Min(
    [double]$CallTimeoutSeconds,
    [double]$remainingSeconds
  )
  if ($effectiveTimeout -lt 5) {
    $effectiveTimeout = 5
  }

  $argumentsJson = $Arguments | ConvertTo-Json -Depth 12 -Compress
  $record = [ordered]@{
    timestampUtc = [DateTime]::UtcNow.ToString("o")
    tool = $Name
    arguments = $Arguments
    imageOutputPath = $ImageOutputPath
    status = "started"
  }
  try {
    $invokeArguments = @{
      ToolName = $Name
      ArgumentsJson = $argumentsJson
      ProxyDirectory = $operatorDirectoryResolved
      TimeoutSeconds = $effectiveTimeout
    }
    if (-not [string]::IsNullOrWhiteSpace($ImageOutputPath)) {
      $invokeArguments.ImageOutputPath = $ImageOutputPath
    }
    $rawOutput =
      (& $invokeOperatorPath @invokeArguments | Out-String).Trim()
    $result = $rawOutput | ConvertFrom-Json -ErrorAction Stop
    $record.status = "ok"
    $record.result = $result
    return $result
  }
  catch {
    $record.status = "error"
    $record.error = $_.Exception.Message
    throw
  }
  finally {
    Append-Utf8NoBom `
      -Path $transcriptPath `
      -Text (($record | ConvertTo-Json -Depth 20 -Compress) + "`r`n")
  }
}

function Invoke-OperatorJson {
  param(
    [Parameter(Mandatory)]
    [string]$Name,

    [Parameter(Mandatory)]
    [Collections.IDictionary]$Arguments,

    [uint32]$CallTimeoutSeconds = $ToolTimeoutSeconds
  )

  $result = Invoke-Operator `
    -Name $Name `
    -Arguments $Arguments `
    -CallTimeoutSeconds $CallTimeoutSeconds
  return Convert-OperatorPayload -Result $result -ToolName $Name
}

function Save-JsonArtifact {
  param(
    [Parameter(Mandatory)]
    [string]$Name,

    [Parameter(Mandatory)]
    [object]$Value
  )

  $path = Join-Path $outputDirectoryResolved $Name
  Write-Utf8NoBom `
    -Path $path `
    -Text (($Value | ConvertTo-Json -Depth 20) + "`r`n")
  return $path
}

function Read-PoseBridgeArtifact {
  param(
    [Parameter(Mandatory)]
    [string]$Name
  )

  $raw =
    (& $readPoseBridgePath -MappingName $MappingName -Attempts 40 |
      Out-String).Trim()
  $poseBridge = $raw | ConvertFrom-Json -ErrorAction Stop
  $path = Join-Path $outputDirectoryResolved $Name
  Write-Utf8NoBom -Path $path -Text ($raw + "`r`n")
  return $poseBridge
}

function Get-Pose {
  param(
    [Parameter(Mandatory)]
    [object]$Payload,

    [Parameter(Mandatory)]
    [string]$Label
  )

  if ($null -eq $Payload.PSObject.Properties["pose"] -or
      $null -eq $Payload.pose.PSObject.Properties["position"] -or
      $null -eq $Payload.pose.PSObject.Properties["orientation"] -or
      @($Payload.pose.position).Count -ne 3 -or
      @($Payload.pose.orientation).Count -ne 4) {
    throw "Operator $Label response has no complete pose."
  }
  return [ordered]@{
    position = @($Payload.pose.position | ForEach-Object { [double]$_ })
    orientation =
      @($Payload.pose.orientation | ForEach-Object { [double]$_ })
  }
}

function Get-PositionDistance {
  param(
    [Parameter(Mandatory)]
    [object[]]$A,

    [Parameter(Mandatory)]
    [object[]]$B
  )

  $dx = [double]$A[0] - [double]$B[0]
  $dy = [double]$A[1] - [double]$B[1]
  $dz = [double]$A[2] - [double]$B[2]
  return [Math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz)
}

function Get-QuaternionAgreement {
  param(
    [Parameter(Mandatory)]
    [object[]]$A,

    [Parameter(Mandatory)]
    [object[]]$B
  )

  $dot = 0.0
  for ($index = 0; $index -lt 4; $index++) {
    $dot += [double]$A[$index] * [double]$B[$index]
  }
  return [Math]::Abs($dot)
}

function Test-PoseMatches {
  param(
    [Parameter(Mandatory)]
    [object]$Actual,

    [Parameter(Mandatory)]
    [Collections.IDictionary]$Expected,

    [double]$PositionTolerance = 0.01,

    [double]$QuaternionAgreement = 0.999
  )

  return (
    (Get-PositionDistance `
      -A @($Actual.position) `
      -B @($Expected.position)) -le $PositionTolerance -and
    (Get-QuaternionAgreement `
      -A @($Actual.orientation) `
      -B @($Expected.orientation)) -ge $QuaternionAgreement
  )
}

function Convert-HexUInt32 {
  param(
    [Parameter(Mandatory)]
    [string]$Value
  )

  $trimmed = $Value.Trim()
  if ($trimmed.StartsWith("0x", [StringComparison]::OrdinalIgnoreCase)) {
    $trimmed = $trimmed.Substring(2)
  }
  return [Convert]::ToUInt32($trimmed, 16)
}

function Test-ToolSuccess {
  param(
    [Parameter(Mandatory)]
    [object]$Payload
  )

  return (
    $null -ne $Payload.PSObject.Properties["success"] -and
    [bool]$Payload.success
  )
}

function Wait-OperatorListener {
  param(
    [Parameter(Mandatory)]
    [DateTime]$DeadlineUtc
  )

  while ([DateTime]::UtcNow -lt $DeadlineUtc) {
    if ($null -ne $script:hostProcess) {
      $script:hostProcess.Refresh()
      if ($script:hostProcess.HasExited) {
        throw (
          "OpenXR calibration host exited before Operator became ready " +
          "(exit=$($script:hostProcess.ExitCode))."
        )
      }
    }
    if (Test-PortListener -Port 8720) {
      return
    }
    Start-Sleep -Milliseconds 100
  }
  throw "Meta XR Operator did not listen on port 8720 before the ready deadline."
}

function Wait-SessionFocused {
  param(
    [Parameter(Mandatory)]
    [DateTime]$DeadlineUtc
  )

  $lastSession = $null
  while ([DateTime]::UtcNow -lt $DeadlineUtc) {
    try {
      $lastSession = Invoke-OperatorJson `
        -Name "openxr_get_session_info" `
        -Arguments ([ordered]@{ include_history = $false }) `
        -CallTimeoutSeconds 8
      $stateValue = -1
      $stateName = ""
      if ($null -ne $lastSession.PSObject.Properties["state"]) {
        if ($null -ne $lastSession.state.PSObject.Properties["value"]) {
          $stateValue = [int]$lastSession.state.value
        }
        if ($null -ne $lastSession.state.PSObject.Properties["name"]) {
          $stateName = [string]$lastSession.state.name
        }
      }
      if ($stateValue -eq 5 -and
          $stateName -eq "XR_SESSION_STATE_FOCUSED") {
        return $lastSession
      }
    }
    catch {
      if ([DateTime]::UtcNow.AddSeconds(1) -ge $DeadlineUtc) {
        throw
      }
    }
    Start-Sleep -Milliseconds 250
  }
  throw (
    "OpenXR session did not reach XR_SESSION_STATE_FOCUSED. Last=" +
    ($lastSession | ConvertTo-Json -Depth 8 -Compress)
  )
}

function Set-HeadPose {
  param(
    [Parameter(Mandatory)]
    [Collections.IDictionary]$Pose,

    [uint32]$CallTimeoutSeconds = $ToolTimeoutSeconds
  )

  return Invoke-OperatorJson `
    -Name "openxr_set_head_pose" `
    -Arguments ([ordered]@{
      position = @($Pose.position)
      orientation = @($Pose.orientation)
      base_space = "local_floor"
    }) `
    -CallTimeoutSeconds $CallTimeoutSeconds
}

function Get-HeadPose {
  return Invoke-OperatorJson `
    -Name "openxr_get_head_pose" `
    -Arguments ([ordered]@{ base_space = "local_floor" })
}

function Set-ControllerPose {
  param(
    [Parameter(Mandatory)]
    [ValidateSet("left", "right")]
    [string]$Hand,

    [Parameter(Mandatory)]
    [ValidateSet("grip", "aim")]
    [string]$PoseType,

    [Parameter(Mandatory)]
    [Collections.IDictionary]$Pose,

    [uint32]$CallTimeoutSeconds = $ToolTimeoutSeconds
  )

  # Intentionally instant. Operator cannot animate from a controller pose that
  # has not yet been established in a fresh simulator session.
  return Invoke-OperatorJson `
    -Name "openxr_set_controller_pose" `
    -Arguments ([ordered]@{
      hand = $Hand
      pose_type = $PoseType
      position = @($Pose.position)
      orientation = @($Pose.orientation)
      base_space = "local_floor"
    }) `
    -CallTimeoutSeconds $CallTimeoutSeconds
}

function Get-ControllerPose {
  param(
    [Parameter(Mandatory)]
    [ValidateSet("left", "right")]
    [string]$Hand,

    [Parameter(Mandatory)]
    [ValidateSet("grip", "aim")]
    [string]$PoseType
  )

  return Invoke-OperatorJson `
    -Name "openxr_get_controller_pose" `
    -Arguments ([ordered]@{
      hand = $Hand
      pose_type = $PoseType
      base_space = "local_floor"
    })
}

function Set-ControllerInput {
  param(
    [Parameter(Mandatory)]
    [ValidateSet("left", "right")]
    [string]$Hand,

    [Parameter(Mandatory)]
    [string]$Component,

    [string]$SubComponent = "",

    [Parameter(Mandatory)]
    [double]$Value,

    [uint32]$CallTimeoutSeconds = $ToolTimeoutSeconds
  )

  $arguments = [ordered]@{
    hand = $Hand
    component = $Component
    value = $Value
    auto_release = $false
  }
  if (-not [string]::IsNullOrWhiteSpace($SubComponent)) {
    $arguments.sub_component = $SubComponent
  }
  return Invoke-OperatorJson `
    -Name "openxr_set_controller_input" `
    -Arguments $arguments `
    -CallTimeoutSeconds $CallTimeoutSeconds
}

function Capture-Eye {
  param(
    [Parameter(Mandatory)]
    [ValidateSet("left", "right")]
    [string]$Eye,

    [Parameter(Mandatory)]
    [string]$FileName
  )

  $path = Join-Path $outputDirectoryResolved $FileName
  $result = Invoke-Operator `
    -Name "openxr_capture_composited_image" `
    -Arguments ([ordered]@{ eye = $Eye }) `
    -ImageOutputPath $path
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Operator did not create the $Eye capture: $path"
  }
  $imageItems = @($result.content | Where-Object {
    $null -ne $_.PSObject.Properties["type"] -and
    [string]$_.type -eq "image"
  })
  if ($imageItems.Count -ne 1) {
    throw "Operator returned $($imageItems.Count) images for eye=$Eye."
  }
  $width = 0
  $height = 0
  if ($null -ne $imageItems[0].PSObject.Properties["annotations"]) {
    if ($null -ne $imageItems[0].annotations.PSObject.Properties["width"]) {
      $width = [int]$imageItems[0].annotations.width
    }
    if ($null -ne $imageItems[0].annotations.PSObject.Properties["height"]) {
      $height = [int]$imageItems[0].annotations.height
    }
  }
  return [ordered]@{
    path = $path
    bytes = (Get-Item -LiteralPath $path).Length
    sha256 = Get-Sha256 $path
    width = $width
    height = $height
  }
}

function Copy-HostLogDelta {
  param(
    [Parameter(Mandatory)]
    [string]$HostLogPath,

    [Parameter(Mandatory)]
    [long]$OriginalLength,

    [Parameter(Mandatory)]
    [string]$Destination
  )

  if (-not (Test-Path -LiteralPath $HostLogPath -PathType Leaf)) {
    Write-Utf8NoBom -Path $Destination -Text ""
    return
  }
  $bytes = [IO.File]::ReadAllBytes($HostLogPath)
  $offset = if ($bytes.LongLength -ge $OriginalLength) {
    $OriginalLength
  }
  else {
    0
  }
  $count = [int]($bytes.LongLength - $offset)
  $delta = [byte[]]::new($count)
  if ($count -gt 0) {
    [Array]::Copy($bytes, $offset, $delta, 0, $count)
  }
  [IO.File]::WriteAllBytes($Destination, $delta)
}

$runtimeInfo =
  Get-OpenXrRuntimeInfo -ManifestPath $runtimeManifestResolved
$manifestCandidates = @(
  Get-ChildItem `
    -LiteralPath $operatorDirectoryResolved `
    -Filter "*.json" `
    -File
)
$operatorManifestPath = $null
$operatorManifest = $null
foreach ($candidate in $manifestCandidates) {
  try {
    $candidateManifest =
      Get-Content -LiteralPath $candidate.FullName -Raw |
      ConvertFrom-Json -ErrorAction Stop
    if ([string]$candidateManifest.api_layer.name -eq
        "XR_APILAYER_METAX_operator") {
      if ($null -ne $operatorManifestPath) {
        throw (
          "More than one XR_APILAYER_METAX_operator manifest exists in " +
          "$operatorDirectoryResolved."
        )
      }
      $operatorManifestPath = $candidate.FullName
      $operatorManifest = $candidateManifest
    }
  }
  catch {
    if ($_.Exception.Message -match
        "More than one XR_APILAYER_METAX_operator") {
      throw
    }
  }
}
if ($null -eq $operatorManifestPath) {
  throw (
    "No XR_APILAYER_METAX_operator manifest exists in " +
    "$operatorDirectoryResolved."
  )
}
$operatorLibraryPath = [IO.Path]::GetFullPath(
  (Join-Path `
    $operatorDirectoryResolved `
    ([string]$operatorManifest.api_layer.library_path))
)
$operatorProxyPath =
  Join-Path $operatorDirectoryResolved "meta-xr-operator-mcp-proxy.exe"
foreach ($requiredPath in @($operatorLibraryPath, $operatorProxyPath)) {
  if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
    throw "Required Meta XR Operator binary is missing: $requiredPath"
  }
}
if ((Get-PeMachine $operatorLibraryPath) -ne 0x8664 -or
    (Get-PeMachine $operatorProxyPath) -ne 0x8664 -or
    (Get-PeMachine $hostExe) -ne 0x8664) {
  throw "Meta XR Operator and calibration host must all be x64 PE files."
}
if (-not (Test-MetaSignature $operatorLibraryPath) -or
    -not (Test-MetaSignature $operatorProxyPath)) {
  throw "Meta XR Operator binaries do not have valid Meta Platforms signatures."
}
if ($runtimeInfo.Kind -ne "MetaSimulator") {
  throw (
    "This proof requires Meta XR Simulator so head pose can be overridden. " +
    "Resolved runtime: $($runtimeInfo.Kind) ($runtimeManifestResolved)"
  )
}

$steamVrPreflight = @(Get-SteamVrProcesses)
if ($steamVrPreflight.Count -ne 0) {
  throw (
    "SteamVR is running. Close it before this direct Meta Simulator proof. " +
    "The harness will never stop or start SteamVR."
  )
}
$existingHosts = @(
  Get-Process `
    -Name ([IO.Path]::GetFileNameWithoutExtension($hostExe)) `
    -ErrorAction SilentlyContinue |
    Where-Object {
      try {
        -not $_.HasExited -and
        [string]::Equals(
          [IO.Path]::GetFullPath($_.Path),
          [IO.Path]::GetFullPath($hostExe),
          [StringComparison]::OrdinalIgnoreCase
        )
      }
      catch {
        $false
      }
    }
)
if ($existingHosts.Count -ne 0) {
  throw (
    "The x64 calibration host is already running. Stop that owned session " +
    "first; this harness never stops a pre-existing host."
  )
}
if (Test-PortListener -Port 8720) {
  throw (
    "Port 8720 is already listening. Stop the other Operator session before " +
    "running this isolated proof."
  )
}

$hostLogPath = Join-Path (Split-Path -Parent $hostExe) "gtaiv_xr_host.log"
$hostLogOriginalLength = if (
  Test-Path -LiteralPath $hostLogPath -PathType Leaf
) {
  (Get-Item -LiteralPath $hostLogPath).Length
}
else {
  0
}

$baselineHead = [ordered]@{
  position = @(0.0, 1.65, 0.0)
  orientation = @(0.0, 0.0, 0.0, 1.0)
}
$movedHead = [ordered]@{
  position = @(0.2, 1.65, -0.3)
  orientation = @(0.0, 0.258819, 0.0, 0.965926)
}
$controllerTargets = [ordered]@{
  leftGrip = [ordered]@{
    position = @(-0.25, 1.25, -0.40)
    orientation = @(0.0, 0.0, 0.0, 1.0)
  }
  leftAim = [ordered]@{
    position = @(-0.25, 1.25, -0.50)
    orientation = @(0.0, 0.0, 0.0, 1.0)
  }
  rightGrip = [ordered]@{
    position = @(0.25, 1.25, -0.40)
    orientation = @(0.0, 0.0, 0.0, 1.0)
  }
  rightAim = [ordered]@{
    position = @(0.25, 1.25, -0.50)
    orientation = @(0.0, 0.0, 0.0, 1.0)
  }
}

$script:proofDeadlineUtc = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$script:hostProcess = $null
$originalHeadPose = $null
$originalControllerPoses = [ordered]@{}
$inputWasTouched = $false
$headWasTouched = $false
$controllerPoseWasTouched = $false
$failureMessage = ""
$failureDetail = ""
$cleanupErrors = [Collections.Generic.List[string]]::new()

$summary = [ordered]@{
  schema = "gtaiv-meta-xr-operator-calibration-proof-v1"
  startedUtc = [DateTime]::UtcNow.ToString("o")
  completedUtc = ""
  outcome = "PROOF_FAILED"
  outputDirectory = $outputDirectoryResolved
  runtime = [ordered]@{
    kind = $runtimeInfo.Kind
    manifestPath = $runtimeManifestResolved
    manifestSha256 = Get-Sha256 $runtimeManifestResolved
  }
  operator = [ordered]@{
    layerName = "XR_APILAYER_METAX_operator"
    directory = $operatorDirectoryResolved
    manifestPath = $operatorManifestPath
    manifestSha256 = Get-Sha256 $operatorManifestPath
    libraryPath = $operatorLibraryPath
    librarySha256 = Get-Sha256 $operatorLibraryPath
    libraryMetaSignatureValid = $true
    proxyPath = $operatorProxyPath
    proxySha256 = Get-Sha256 $operatorProxyPath
    proxyMetaSignatureValid = $true
    port = 8720
  }
  host = [ordered]@{
    path = $hostExe
    sha256 = Get-Sha256 $hostExe
    pid = 0
    frameLimit = $FrameLimit
    timeoutSeconds = $TimeoutSeconds
  }
  safety = [ordered]@{
    simulatorGuiControlled = $false
    steamVrStarted = $false
    steamVrPreflightAbsent = $true
    steamVrFinalAbsent = $false
    childOnlyRuntimeOverride = $true
    childOnlyOperatorLayer = $true
  }
  observations = [ordered]@{}
  metrics = [ordered]@{
    operatorHeadMotionMeters = 0.0
    poseBridgeHeadMotionMeters = 0.0
    poseBridgeFlags = ""
    leftControllerActiveFlags = ""
    rightControllerActiveFlags = ""
    heldLeftThumbstickY = 0.0
    heldRightButtons = ""
    neutralLeftThumbstickY = 0.0
    neutralRightButtons = ""
    poseBridgeFrameIds = @()
    poseBridgeFrameAdvance = 0
  }
  captures = [ordered]@{
    beforeLeft = [ordered]@{
      path = ""
      bytes = 0
      sha256 = ""
      width = 0
      height = 0
    }
    beforeRight = [ordered]@{
      path = ""
      bytes = 0
      sha256 = ""
      width = 0
      height = 0
    }
    afterLeft = [ordered]@{
      path = ""
      bytes = 0
      sha256 = ""
      width = 0
      height = 0
    }
    afterRight = [ordered]@{
      path = ""
      bytes = 0
      sha256 = ""
      width = 0
      height = 0
    }
  }
  gates = [ordered]@{
    sessionFocused = $false
    touchProfiles = $false
    headOperatorReadback = $false
    headPoseBridgeMotion = $false
    controllerOperatorReadback = $false
    controllerPoseBridgeReadback = $false
    leftThumbstickHeld = $false
    rightAButtonHeld = $false
    explicitNeutralRelease = $false
    bothEyesCapturedBefore = $false
    bothEyesCapturedAfter = $false
    bothEyesChangedAfterHeadMotion = $false
    poseBridgeAdvanced = $false
    cleanupInputNeutral = $false
    cleanupPosesRestored = $false
    cleanupHostStopped = $false
    cleanupPortReleased = $false
  }
  cleanupErrors = @()
  failure = ""
}

try {
  $priorRuntime = $env:XR_RUNTIME_JSON
  $priorLayerPath = $env:XR_API_LAYER_PATH
  $priorEnabledLayers = $env:XR_ENABLE_API_LAYERS
  try {
    $env:XR_RUNTIME_JSON = $runtimeManifestResolved
    $env:XR_API_LAYER_PATH = $operatorDirectoryResolved
    $env:XR_ENABLE_API_LAYERS = "XR_APILAYER_METAX_operator"
    $hostArguments = @(
      "--frames"
      $FrameLimit.ToString()
      "--timeout-ms"
      ([uint64]$TimeoutSeconds * 1000).ToString()
    )
    $script:hostProcess = Start-Process `
      -FilePath $hostExe `
      -ArgumentList $hostArguments `
      -WorkingDirectory (Split-Path -Parent $hostExe) `
      -WindowStyle Hidden `
      -PassThru
  }
  finally {
    Restore-EnvironmentValue `
      -Name "XR_RUNTIME_JSON" `
      -PriorValue $priorRuntime
    Restore-EnvironmentValue `
      -Name "XR_API_LAYER_PATH" `
      -PriorValue $priorLayerPath
    Restore-EnvironmentValue `
      -Name "XR_ENABLE_API_LAYERS" `
      -PriorValue $priorEnabledLayers
  }
  $summary.host.pid = $script:hostProcess.Id

  $readyDeadline = [DateTime]::UtcNow.AddSeconds(
    [Math]::Min(30, [int]$TimeoutSeconds - 10)
  )
  Wait-OperatorListener -DeadlineUtc $readyDeadline
  $session = Wait-SessionFocused -DeadlineUtc $readyDeadline
  $summary.gates.sessionFocused = $true
  $summary.observations.session =
    Save-JsonArtifact -Name "session.json" -Value $session

  $instance = Invoke-OperatorJson `
    -Name "openxr_get_instance_info" `
    -Arguments ([ordered]@{
      include_api_layers = $true
      include_enabled_extensions = $true
      include_available_extensions = $false
    })
  $summary.observations.instance =
    Save-JsonArtifact -Name "instance.json" -Value $instance

  $leftProfile = Invoke-OperatorJson `
    -Name "openxr_get_active_interaction_profile" `
    -Arguments ([ordered]@{ hand = "left" })
  $rightProfile = Invoke-OperatorJson `
    -Name "openxr_get_active_interaction_profile" `
    -Arguments ([ordered]@{ hand = "right" })
  $summary.observations.leftProfile =
    Save-JsonArtifact -Name "left-profile.json" -Value $leftProfile
  $summary.observations.rightProfile =
    Save-JsonArtifact -Name "right-profile.json" -Value $rightProfile
  $summary.gates.touchProfiles = (
    [string]$leftProfile.left.profile -eq
      "/interaction_profiles/meta/touch_controller_plus" -and
    [string]$rightProfile.right.profile -eq
      "/interaction_profiles/meta/touch_controller_plus"
  )

  $originalHeadPayload = Get-HeadPose
  $originalHeadPose =
    Get-Pose -Payload $originalHeadPayload -Label "original head"
  $summary.observations.originalHead =
    Save-JsonArtifact `
      -Name "original-head.json" `
      -Value $originalHeadPayload

  foreach ($hand in @("left", "right")) {
    foreach ($poseType in @("grip", "aim")) {
      $key = "$hand-$poseType"
      $payload = Get-ControllerPose -Hand $hand -PoseType $poseType
      $originalControllerPoses[$key] =
        Get-Pose -Payload $payload -Label "original $key"
      $summary.observations["original-$key"] =
        Save-JsonArtifact -Name "original-$key.json" -Value $payload
    }
  }

  $headWasTouched = $true
  $baselineSet = Set-HeadPose -Pose $baselineHead
  if (-not (Test-ToolSuccess $baselineSet)) {
    throw "Operator rejected the baseline head pose."
  }
  Start-Sleep -Milliseconds 350
  $baselineHeadPayload = Get-HeadPose
  $baselineHeadReadback =
    Get-Pose -Payload $baselineHeadPayload -Label "baseline head"
  $summary.observations.baselineHead =
    Save-JsonArtifact `
      -Name "baseline-head.json" `
      -Value $baselineHeadPayload
  if (-not (Test-PoseMatches `
      -Actual $baselineHeadReadback `
      -Expected $baselineHead)) {
    throw "Operator baseline head pose did not read back exactly."
  }
  $baselineBridge =
    Read-PoseBridgeArtifact -Name "pose-bridge-baseline.json"

  $summary.captures.beforeLeft =
    Capture-Eye -Eye "left" -FileName "before-left.png"
  $summary.captures.beforeRight =
    Capture-Eye -Eye "right" -FileName "before-right.png"
  $summary.gates.bothEyesCapturedBefore = (
    $summary.captures.beforeLeft.bytes -gt 0 -and
    $summary.captures.beforeRight.bytes -gt 0
  )

  $movedSet = Set-HeadPose -Pose $movedHead
  if (-not (Test-ToolSuccess $movedSet)) {
    throw "Operator rejected the moved head pose."
  }
  Start-Sleep -Milliseconds 350
  $movedHeadPayload = Get-HeadPose
  $movedHeadReadback =
    Get-Pose -Payload $movedHeadPayload -Label "moved head"
  $summary.observations.movedHead =
    Save-JsonArtifact -Name "moved-head.json" -Value $movedHeadPayload
  $summary.gates.headOperatorReadback = (
    (Test-PoseMatches `
      -Actual $baselineHeadReadback `
      -Expected $baselineHead) -and
    (Test-PoseMatches `
      -Actual $movedHeadReadback `
      -Expected $movedHead)
  )
  $movedBridge =
    Read-PoseBridgeArtifact -Name "pose-bridge-moved.json"

  $summary.captures.afterLeft =
    Capture-Eye -Eye "left" -FileName "after-left.png"
  $summary.captures.afterRight =
    Capture-Eye -Eye "right" -FileName "after-right.png"
  $summary.gates.bothEyesCapturedAfter = (
    $summary.captures.afterLeft.bytes -gt 0 -and
    $summary.captures.afterRight.bytes -gt 0
  )
  $summary.gates.bothEyesChangedAfterHeadMotion = (
    $summary.captures.beforeLeft.sha256 -ne
      $summary.captures.afterLeft.sha256 -and
    $summary.captures.beforeRight.sha256 -ne
      $summary.captures.afterRight.sha256
  )

  $summary.metrics.operatorHeadMotionMeters =
    Get-PositionDistance `
      -A @($baselineHeadReadback.position) `
      -B @($movedHeadReadback.position)
  $summary.metrics.poseBridgeHeadMotionMeters =
    Get-PositionDistance `
      -A @($baselineBridge.hmdPose.position) `
      -B @($movedBridge.hmdPose.position)
  $summary.gates.headPoseBridgeMotion = (
    $summary.metrics.poseBridgeHeadMotionMeters -ge 0.30
  )

  $controllerPoseWasTouched = $true
  $controllerSetResults = @(
    Set-ControllerPose `
      -Hand "left" `
      -PoseType "grip" `
      -Pose $controllerTargets.leftGrip
    Set-ControllerPose `
      -Hand "left" `
      -PoseType "aim" `
      -Pose $controllerTargets.leftAim
    Set-ControllerPose `
      -Hand "right" `
      -PoseType "grip" `
      -Pose $controllerTargets.rightGrip
    Set-ControllerPose `
      -Hand "right" `
      -PoseType "aim" `
      -Pose $controllerTargets.rightAim
  )
  if (@($controllerSetResults | Where-Object {
      -not (Test-ToolSuccess $_)
    }).Count -ne 0) {
    throw "Operator rejected at least one controller pose."
  }
  Start-Sleep -Milliseconds 350

  $controllerReadbacks = [ordered]@{}
  foreach ($item in @(
    [ordered]@{
      key = "leftGrip"
      hand = "left"
      type = "grip"
      expected = $controllerTargets.leftGrip
    }
    [ordered]@{
      key = "leftAim"
      hand = "left"
      type = "aim"
      expected = $controllerTargets.leftAim
    }
    [ordered]@{
      key = "rightGrip"
      hand = "right"
      type = "grip"
      expected = $controllerTargets.rightGrip
    }
    [ordered]@{
      key = "rightAim"
      hand = "right"
      type = "aim"
      expected = $controllerTargets.rightAim
    }
  )) {
    $payload =
      Get-ControllerPose -Hand $item.hand -PoseType $item.type
    $pose = Get-Pose -Payload $payload -Label $item.key
    $controllerReadbacks[$item.key] = [ordered]@{
      payload = $payload
      pose = $pose
      matched = Test-PoseMatches `
        -Actual $pose `
        -Expected $item.expected
    }
  }
  $summary.observations.controllerReadbacks =
    Save-JsonArtifact `
      -Name "controller-readbacks.json" `
      -Value $controllerReadbacks
  $summary.gates.controllerOperatorReadback = (
    @($controllerReadbacks.Values | Where-Object {
      -not $_.matched -or
      -not [bool]$_.payload.is_active -or
      -not [bool]$_.payload.simulated
    }).Count -eq 0
  )

  $controllerBridge =
    Read-PoseBridgeArtifact -Name "pose-bridge-controllers.json"
  $leftActive =
    Convert-HexUInt32 ([string]$controllerBridge.leftController.activeFlags)
  $rightActive =
    Convert-HexUInt32 ([string]$controllerBridge.rightController.activeFlags)
  $bridgeFlags = Convert-HexUInt32 ([string]$controllerBridge.flags)
  $summary.metrics.poseBridgeFlags = [string]$controllerBridge.flags
  $summary.metrics.leftControllerActiveFlags =
    [string]$controllerBridge.leftController.activeFlags
  $summary.metrics.rightControllerActiveFlags =
    [string]$controllerBridge.rightController.activeFlags
  $summary.gates.controllerPoseBridgeReadback = (
    ($bridgeFlags -band 0x70) -eq 0x70 -and
    ($leftActive -band 0x03) -eq 0x03 -and
    ($rightActive -band 0x03) -eq 0x03
  )

  $inputWasTouched = $true
  $leftStickHeld = Set-ControllerInput `
    -Hand "left" `
    -Component "Thumbstick" `
    -SubComponent "Y" `
    -Value 0.8
  $rightAHeld = Set-ControllerInput `
    -Hand "right" `
    -Component "A" `
    -Value 1.0
  if (-not (Test-ToolSuccess $leftStickHeld) -or
      -not (Test-ToolSuccess $rightAHeld)) {
    throw "Operator rejected held controller input."
  }
  Start-Sleep -Milliseconds 350
  $heldBridge =
    Read-PoseBridgeArtifact -Name "pose-bridge-input-held.json"
  $heldRightButtons =
    Convert-HexUInt32 ([string]$heldBridge.rightController.buttons)
  $summary.metrics.heldLeftThumbstickY =
    [double]$heldBridge.leftController.thumbstick[1]
  $summary.metrics.heldRightButtons =
    [string]$heldBridge.rightController.buttons
  $summary.gates.leftThumbstickHeld = (
    [double]$heldBridge.leftController.thumbstick[1] -ge 0.70
  )
  $summary.gates.rightAButtonHeld = (
    ($heldRightButtons -band 0x01) -eq 0x01
  )

  $leftStickReleased = Set-ControllerInput `
    -Hand "left" `
    -Component "Thumbstick" `
    -SubComponent "Y" `
    -Value 0.0
  $rightAReleased = Set-ControllerInput `
    -Hand "right" `
    -Component "A" `
    -Value 0.0
  if (-not (Test-ToolSuccess $leftStickReleased) -or
      -not (Test-ToolSuccess $rightAReleased)) {
    throw "Operator rejected explicit controller neutral."
  }
  Start-Sleep -Milliseconds 350
  $neutralBridge =
    Read-PoseBridgeArtifact -Name "pose-bridge-input-neutral.json"
  $neutralRightButtons =
    Convert-HexUInt32 ([string]$neutralBridge.rightController.buttons)
  $summary.metrics.neutralLeftThumbstickY =
    [double]$neutralBridge.leftController.thumbstick[1]
  $summary.metrics.neutralRightButtons =
    [string]$neutralBridge.rightController.buttons
  $summary.gates.explicitNeutralRelease = (
    [Math]::Abs(
      [double]$neutralBridge.leftController.thumbstick[1]
    ) -le 0.05 -and
    ($neutralRightButtons -band 0x01) -eq 0
  )
  $summary.gates.cleanupInputNeutral =
    $summary.gates.explicitNeutralRelease

  $frameIds = @(
    [uint64]$baselineBridge.frameId
    [uint64]$movedBridge.frameId
    [uint64]$controllerBridge.frameId
    [uint64]$heldBridge.frameId
    [uint64]$neutralBridge.frameId
  )
  $frameIdsAdvance = $true
  for ($index = 1; $index -lt $frameIds.Count; $index++) {
    if ($frameIds[$index] -le $frameIds[$index - 1]) {
      $frameIdsAdvance = $false
    }
  }
  $summary.metrics.poseBridgeFrameIds = $frameIds
  $summary.metrics.poseBridgeFrameAdvance =
    $frameIds[$frameIds.Count - 1] - $frameIds[0]
  $summary.gates.poseBridgeAdvanced = $frameIdsAdvance

  $requiredGateNames = @(
    "sessionFocused"
    "touchProfiles"
    "headOperatorReadback"
    "headPoseBridgeMotion"
    "controllerOperatorReadback"
    "controllerPoseBridgeReadback"
    "leftThumbstickHeld"
    "rightAButtonHeld"
    "explicitNeutralRelease"
    "bothEyesCapturedBefore"
    "bothEyesCapturedAfter"
    "bothEyesChangedAfterHeadMotion"
    "poseBridgeAdvanced"
  )
  $failedGates = @($requiredGateNames | Where-Object {
    -not [bool]$summary.gates[$_]
  })
  if ($failedGates.Count -ne 0) {
    throw "Proof gates failed: $($failedGates -join ', ')"
  }
}
catch {
  $failureMessage = $_.Exception.Message
  $failureDetail = ($_ | Out-String)
}
finally {
  # Cleanup has its own strict bound, so explicit neutral and pose restoration
  # are still attempted when the proof deadline itself caused the failure.
  $script:proofDeadlineUtc = [DateTime]::UtcNow.AddSeconds(40)
  if (-not $inputWasTouched) {
    $summary.gates.cleanupInputNeutral = $true
  }
  if ($inputWasTouched -and
      $null -ne $script:hostProcess -and
      -not $script:hostProcess.HasExited) {
    try {
      $releaseStick = Set-ControllerInput `
        -Hand "left" `
        -Component "Thumbstick" `
        -SubComponent "Y" `
        -Value 0.0 `
        -CallTimeoutSeconds 5
      $releaseA = Set-ControllerInput `
        -Hand "right" `
        -Component "A" `
        -Value 0.0 `
        -CallTimeoutSeconds 5
      $summary.gates.cleanupInputNeutral = (
        (Test-ToolSuccess $releaseStick) -and
        (Test-ToolSuccess $releaseA)
      )
    }
    catch {
      $cleanupErrors.Add(
        "input neutral: $($_.Exception.Message)"
      )
      $summary.gates.cleanupInputNeutral = $false
    }
  }

  $poseRestoreResults = [Collections.Generic.List[bool]]::new()
  if ($controllerPoseWasTouched -and
      $null -ne $script:hostProcess -and
      -not $script:hostProcess.HasExited) {
    foreach ($hand in @("left", "right")) {
      foreach ($poseType in @("grip", "aim")) {
        $key = "$hand-$poseType"
        if ($originalControllerPoses.Contains($key)) {
          try {
            $restore = Set-ControllerPose `
              -Hand $hand `
              -PoseType $poseType `
              -Pose $originalControllerPoses[$key] `
              -CallTimeoutSeconds 5
            $poseRestoreResults.Add((Test-ToolSuccess $restore))
          }
          catch {
            $cleanupErrors.Add(
              "restore $key pose: $($_.Exception.Message)"
            )
            $poseRestoreResults.Add($false)
          }
        }
      }
    }
  }
  if ($headWasTouched -and
      $null -ne $originalHeadPose -and
      $null -ne $script:hostProcess -and
      -not $script:hostProcess.HasExited) {
    try {
      $restoreHead = Set-HeadPose `
        -Pose $originalHeadPose `
        -CallTimeoutSeconds 5
      $poseRestoreResults.Add((Test-ToolSuccess $restoreHead))
    }
    catch {
      $cleanupErrors.Add(
        "restore head pose: $($_.Exception.Message)"
      )
      $poseRestoreResults.Add($false)
    }
  }
  if (-not $controllerPoseWasTouched -and -not $headWasTouched) {
    $summary.gates.cleanupPosesRestored = $true
  }
  else {
    $summary.gates.cleanupPosesRestored = (
      $poseRestoreResults.Count -gt 0 -and
      @($poseRestoreResults | Where-Object { -not $_ }).Count -eq 0
    )
  }

  if ($null -ne $script:hostProcess) {
    try {
      $script:hostProcess.Refresh()
      if (-not $script:hostProcess.HasExited) {
        Stop-Process `
          -Id $script:hostProcess.Id `
          -Force `
          -ErrorAction Stop
        [void]$script:hostProcess.WaitForExit(5000)
      }
      $script:hostProcess.Refresh()
      $summary.gates.cleanupHostStopped = $script:hostProcess.HasExited
    }
    catch {
      $cleanupErrors.Add(
        "stop owned calibration host: $($_.Exception.Message)"
      )
    }
  }

  $portReleaseDeadline = [DateTime]::UtcNow.AddSeconds(5)
  while ((Test-PortListener -Port 8720) -and
      [DateTime]::UtcNow -lt $portReleaseDeadline) {
    Start-Sleep -Milliseconds 100
  }
  $summary.gates.cleanupPortReleased =
    -not (Test-PortListener -Port 8720)

  $steamVrFinal = @(Get-SteamVrProcesses)
  $summary.safety.steamVrFinalAbsent = $steamVrFinal.Count -eq 0
  if ($steamVrFinal.Count -ne 0) {
    $cleanupErrors.Add(
      "SteamVR appeared during the direct Meta Simulator proof."
    )
  }

  try {
    Copy-HostLogDelta `
      -HostLogPath $hostLogPath `
      -OriginalLength $hostLogOriginalLength `
      -Destination $hostLogDeltaPath
  }
  catch {
    $cleanupErrors.Add(
      "archive host log delta: $($_.Exception.Message)"
    )
  }

  $summary.completedUtc = [DateTime]::UtcNow.ToString("o")
  $summary.cleanupErrors = @($cleanupErrors)
  if ([string]::IsNullOrWhiteSpace($failureMessage) -and
      $summary.gates.cleanupInputNeutral -and
      $summary.gates.cleanupPosesRestored -and
      $summary.gates.cleanupHostStopped -and
      $summary.gates.cleanupPortReleased -and
      $summary.safety.steamVrFinalAbsent -and
      $cleanupErrors.Count -eq 0) {
    $summary.outcome = "PROOF_COMPLETE"
  }
  else {
    $summary.outcome = "PROOF_FAILED"
    if ([string]::IsNullOrWhiteSpace($failureMessage)) {
      $failureMessage = "One or more cleanup/safety gates failed."
    }
  }
  $summary.failure = $failureMessage

  if (-not [string]::IsNullOrWhiteSpace($failureDetail)) {
    Write-Utf8NoBom -Path $errorPath -Text $failureDetail
  }

  Write-Utf8NoBom `
    -Path $summaryPath `
    -Text (($summary | ConvertTo-Json -Depth 30) + "`r`n")

  $resultLines = @(
    "outcome=$($summary.outcome)"
    "runtimeKind=$($summary.runtime.kind)"
    "runtimeManifestSha256=$($summary.runtime.manifestSha256)"
    "operatorManifestSha256=$($summary.operator.manifestSha256)"
    "operatorLibrarySha256=$($summary.operator.librarySha256)"
    "operatorProxySha256=$($summary.operator.proxySha256)"
    "hostSha256=$($summary.host.sha256)"
    "sessionFocused=$($summary.gates.sessionFocused)"
    "touchProfiles=$($summary.gates.touchProfiles)"
    "headOperatorReadback=$($summary.gates.headOperatorReadback)"
    "headPoseBridgeMotion=$($summary.gates.headPoseBridgeMotion)"
    "operatorHeadMotionMeters=$($summary.metrics.operatorHeadMotionMeters)"
    "poseBridgeHeadMotionMeters=$($summary.metrics.poseBridgeHeadMotionMeters)"
    "controllerOperatorReadback=$($summary.gates.controllerOperatorReadback)"
    "controllerPoseBridgeReadback=$($summary.gates.controllerPoseBridgeReadback)"
    "leftThumbstickHeld=$($summary.gates.leftThumbstickHeld)"
    "rightAButtonHeld=$($summary.gates.rightAButtonHeld)"
    "explicitNeutralRelease=$($summary.gates.explicitNeutralRelease)"
    "poseBridgeAdvanced=$($summary.gates.poseBridgeAdvanced)"
    "poseBridgeFrameAdvance=$($summary.metrics.poseBridgeFrameAdvance)"
    "beforeLeftSha256=$($summary.captures.beforeLeft.sha256)"
    "beforeRightSha256=$($summary.captures.beforeRight.sha256)"
    "afterLeftSha256=$($summary.captures.afterLeft.sha256)"
    "afterRightSha256=$($summary.captures.afterRight.sha256)"
    "bothEyesChangedAfterHeadMotion=$($summary.gates.bothEyesChangedAfterHeadMotion)"
    "steamVrPreflightAbsent=$($summary.safety.steamVrPreflightAbsent)"
    "steamVrFinalAbsent=$($summary.safety.steamVrFinalAbsent)"
    "simulatorGuiControlled=$($summary.safety.simulatorGuiControlled)"
    "cleanupInputNeutral=$($summary.gates.cleanupInputNeutral)"
    "cleanupPosesRestored=$($summary.gates.cleanupPosesRestored)"
    "cleanupHostStopped=$($summary.gates.cleanupHostStopped)"
    "cleanupPortReleased=$($summary.gates.cleanupPortReleased)"
    "failure=$failureMessage"
  )
  Write-Utf8NoBom `
    -Path $resultPath `
    -Text (($resultLines -join "`r`n") + "`r`n")

  $manifestLines = @()
  foreach ($file in @(
    Get-ChildItem -LiteralPath $outputDirectoryResolved -File |
      Where-Object { $_.FullName -ne $artifactManifestPath } |
      Sort-Object Name
  )) {
    $manifestLines += "$(Get-Sha256 $file.FullName) *$($file.Name)"
  }
  Write-Utf8NoBom `
    -Path $artifactManifestPath `
    -Text (($manifestLines -join "`r`n") + "`r`n")
}

Write-Host "Meta XR Operator proof: $($summary.outcome)"
Write-Host "Result: $resultPath"
if ($summary.outcome -ne "PROOF_COMPLETE") {
  throw "Meta XR Operator calibration proof failed: $failureMessage"
}

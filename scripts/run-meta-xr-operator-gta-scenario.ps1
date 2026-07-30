[CmdletBinding()]
param(
  [string]$GameDir =
    "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV",

  [string]$RunDirectory = "",

  [string]$ScenarioPath =
    (
      Join-Path (
        Join-Path $PSScriptRoot "..\config"
      ) "meta-operator-roman-apartment-street-car.json"
    ),

  [string]$ProxyDirectory =
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

  [ValidateRange(15, 180)]
  [uint32]$WorldReadyTimeoutSeconds = 120,

  [switch]$RequireVehicleEntry,

  [switch]$SkipStageCaptures,

  [switch]$Authorized,

  [switch]$ValidateOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ScenarioPath = (Resolve-Path -LiteralPath $ScenarioPath).Path
$scenario = Get-Content `
  -LiteralPath $ScenarioPath `
  -Raw `
  -ErrorAction Stop |
  ConvertFrom-Json -ErrorAction Stop

function Get-RequiredProperty(
  [object]$Value,
  [string]$Name,
  [string]$Context
) {
  if ($null -eq $Value) {
    throw "$Context is null; required property '$Name' is missing."
  }
  $property = $Value.PSObject.Properties[$Name]
  if ($null -eq $property) {
    throw "$Context is missing required property '$Name'."
  }
  return $property.Value
}

function Assert-NumberArray(
  [object]$Value,
  [int]$Count,
  [string]$Context
) {
  $items = @($Value)
  if ($items.Count -ne $Count) {
    throw "$Context must contain exactly $Count numbers."
  }
  foreach ($item in $items) {
    if ($item -isnot [ValueType]) {
      throw "$Context contains a non-numeric value."
    }
  }
}

function Test-ScenarioDefinition([object]$Definition) {
  $schemaVersion = [int](
    Get-RequiredProperty $Definition "schemaVersion" "scenario"
  )
  if ($schemaVersion -ne 1) {
    throw "Unsupported scenario schemaVersion=$schemaVersion; expected 1."
  }
  $name = [string](Get-RequiredProperty $Definition "name" "scenario")
  if ([string]::IsNullOrWhiteSpace($name)) {
    throw "Scenario name is empty."
  }
  $mode = [int](Get-RequiredProperty $Definition "stereoMode" "scenario")
  if ($mode -ne 204) {
    throw "The Meta Operator route is locked to literal upstream Mode 204."
  }
  $runtimeKind =
    [string](Get-RequiredProperty $Definition "runtimeKind" "scenario")
  if ($runtimeKind -cne "MetaSimulator") {
    throw (
      "The automated head-pose route requires runtimeKind=MetaSimulator. " +
      "Physical Quest head pose cannot be overridden."
    )
  }
  $start =
    Get-RequiredProperty $Definition "expectedStart" "scenario"
  Assert-NumberArray `
    -Value (Get-RequiredProperty $start "cameraPosition" "expectedStart") `
    -Count 3 `
    -Context "expectedStart.cameraPosition"
  $startRadius =
    [double](Get-RequiredProperty $start "radiusMeters" "expectedStart")
  if ($startRadius -le 0.0 -or $startRadius -gt 5.0) {
    throw "expectedStart.radiusMeters must be in (0, 5]."
  }

  $route = @(Get-RequiredProperty $Definition "route" "scenario")
  if ($route.Count -lt 3) {
    throw "Scenario route must contain apartment, outside, and street stages."
  }
  $stageIds = @()
  foreach ($stage in $route) {
    $stageId = [string](Get-RequiredProperty $stage "id" "route stage")
    if ([string]::IsNullOrWhiteSpace($stageId) -or
        $stageIds -contains $stageId) {
      throw "Route stage ids must be non-empty and unique: '$stageId'."
    }
    $stageIds += $stageId
    Assert-NumberArray `
      -Value (Get-RequiredProperty $stage "target" "route stage $stageId") `
      -Count 3 `
      -Context "route.$stageId.target"
    $radius =
      [double](Get-RequiredProperty $stage "radiusMeters" "route stage $stageId")
    $pulse =
      [double](Get-RequiredProperty $stage "pulseSeconds" "route stage $stageId")
    $maximum =
      [int](Get-RequiredProperty $stage "maxPulses" "route stage $stageId")
    if ($radius -le 0.0 -or $radius -gt 5.0) {
      throw "route.$stageId.radiusMeters must be in (0, 5]."
    }
    if ($pulse -lt 0.25 -or $pulse -gt 2.0) {
      throw "route.$stageId.pulseSeconds must be in [0.25, 2.0]."
    }
    if ($maximum -lt 1 -or $maximum -gt 30) {
      throw "route.$stageId.maxPulses must be in [1, 30]."
    }
  }

  $attempts =
    @(Get-RequiredProperty $Definition "vehicleAttempts" "scenario")
  if ($attempts.Count -lt 1 -or $attempts.Count -gt 5) {
    throw "vehicleAttempts must contain between one and five attempts."
  }
  foreach ($attempt in $attempts) {
    $attemptId =
      [string](Get-RequiredProperty $attempt "id" "vehicle attempt")
    Assert-NumberArray `
      -Value (
        Get-RequiredProperty $attempt "target" "vehicle attempt $attemptId"
      ) `
      -Count 3 `
      -Context "vehicleAttempts.$attemptId.target"
  }
  $driveProof =
    Get-RequiredProperty $Definition "vehicleDriveProof" "scenario"
  $driveTrigger = [double](
    Get-RequiredProperty $driveProof "triggerValue" "vehicleDriveProof"
  )
  $drivePulse = [double](
    Get-RequiredProperty $driveProof "pulseSeconds" "vehicleDriveProof"
  )
  $drivePulses = [int](
    Get-RequiredProperty $driveProof "maxPulses" "vehicleDriveProof"
  )
  $driveDistance = [double](
    Get-RequiredProperty `
      $driveProof `
      "minPlanarDistanceMeters" `
      "vehicleDriveProof"
  )
  if ($driveTrigger -lt 0.5 -or $driveTrigger -gt 1.0) {
    throw "vehicleDriveProof.triggerValue must be in [0.5, 1.0]."
  }
  if ($drivePulse -lt 0.5 -or $drivePulse -gt 2.0) {
    throw "vehicleDriveProof.pulseSeconds must be in [0.5, 2.0]."
  }
  if ($drivePulses -lt 1 -or $drivePulses -gt 6) {
    throw "vehicleDriveProof.maxPulses must be in [1, 6]."
  }
  if ($driveDistance -lt 0.5 -or $driveDistance -gt 10.0) {
    throw (
      "vehicleDriveProof.minPlanarDistanceMeters must be in [0.5, 10.0]."
    )
  }
  $bundle =
    Get-RequiredProperty $Definition "operatorBundle" "scenario"
  foreach ($hashName in @(
      "manifestSha256",
      "librarySha256",
      "proxySha256"
    )) {
    $hash = [string](
      Get-RequiredProperty $bundle $hashName "operatorBundle"
    )
    if ($hash -notmatch '^[0-9A-Fa-f]{64}$') {
      throw "operatorBundle.$hashName must be an exact SHA-256."
    }
  }
  return [pscustomobject]@{
    Name = $name
    RouteStages = $route.Count
    VehicleAttempts = $attempts.Count
  }
}

$scenarioSummary = Test-ScenarioDefinition $scenario
if ($ValidateOnly) {
  Write-Output ((
    "MetaXrOperatorGtaScenarioPlan: PASS name='{0}' routeStages={1} " +
    "vehicleAttempts={2} launchesGame=0 simulatorGuiControl=0 " +
    "rendererChanges=0"
  ) -f
    $scenarioSummary.Name,
    $scenarioSummary.RouteStages,
    $scenarioSummary.VehicleAttempts
  )
  return
}

if (-not $Authorized) {
  throw (
    "Live controller/head automation is locked. Start the supervised " +
    "Mode 204 Meta Simulator run separately, then pass -Authorized."
  )
}

$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
$GameLog = Join-Path $GameDir "gtaiv_dxvk_vr.log"
$HostLog = Join-Path $Root "out-openxr\gtaiv_xr_host.log"
$RunsRoot = Join-Path $Root "out-openxr\runs"
if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
  $latestRun = Get-ChildItem `
    -LiteralPath $RunsRoot `
    -Directory `
    -Filter "*-steam-safe" `
    -ErrorAction Stop |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
  if ($null -eq $latestRun) {
    throw "No supervised OpenXR run directory exists under $RunsRoot."
  }
  $RunDirectory = $latestRun.FullName
}
else {
  $RunDirectory = (Resolve-Path -LiteralPath $RunDirectory).Path
}
$LauncherStatus = Join-Path $RunDirectory "status.log"
foreach ($path in @($GameLog, $HostLog, $LauncherStatus)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Required live proof log is missing: $path"
  }
}

$ProxyDirectory = (Resolve-Path -LiteralPath $ProxyDirectory).Path
$ProxyPath =
  Join-Path $ProxyDirectory "meta-xr-operator-mcp-proxy.exe"
if (-not (Test-Path -LiteralPath $ProxyPath -PathType Leaf)) {
  throw "Meta XR Operator MCP proxy is missing: $ProxyPath"
}
$OperatorManifest =
  Join-Path $ProxyDirectory "XrApiLayer_METAX_operator.json"
$OperatorLibrary =
  Join-Path $ProxyDirectory "XrApiLayer_METAX_operator.dll"
foreach ($path in @($OperatorManifest, $OperatorLibrary)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Meta XR Operator bundle file is missing: $path"
  }
}
$expectedBundle = $scenario.operatorBundle
$bundleHashes = [ordered]@{
  manifest = (
    Get-FileHash -LiteralPath $OperatorManifest -Algorithm SHA256
  ).Hash
  library = (
    Get-FileHash -LiteralPath $OperatorLibrary -Algorithm SHA256
  ).Hash
  proxy = (
    Get-FileHash -LiteralPath $ProxyPath -Algorithm SHA256
  ).Hash
}
if ($bundleHashes.manifest -cne
      [string]$expectedBundle.manifestSha256 -or
    $bundleHashes.library -cne
      [string]$expectedBundle.librarySha256 -or
    $bundleHashes.proxy -cne
      [string]$expectedBundle.proxySha256) {
  throw (
    "Meta XR Operator bundle does not match the pinned official 205.1 " +
    "artifacts in the scenario definition."
  )
}
foreach ($signedPath in @($ProxyPath, $OperatorLibrary)) {
  $signature = Get-AuthenticodeSignature -LiteralPath $signedPath
  if ([string]$signature.Status -cne "Valid" -or
      $null -eq $signature.SignerCertificate -or
      $signature.SignerCertificate.Subject -notmatch
        'CN="Meta Platforms, Inc\."') {
    throw "Meta XR Operator signature verification failed: $signedPath"
  }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
  $OutputDirectory = Join-Path (
    Join-Path $Root "out-openxr\operator-scenarios"
  ) (Get-Date -Format "yyyyMMdd-HHmmss-'roman-street-car'")
}
else {
  $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$StatusPath = Join-Path $OutputDirectory "status.log"
$ResultPath = Join-Path $OutputDirectory "result.txt"

function Write-Status([string]$Message) {
  $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $Message
  [Console]::Out.WriteLine($line)
  Add-Content -LiteralPath $StatusPath -Value $line -Encoding UTF8
}

function Read-SharedText([string]$Path) {
  try {
    $share =
      [IO.FileShare]::ReadWrite -bor
      [IO.FileShare]::Delete
    $stream = [IO.FileStream]::new(
      $Path,
      [IO.FileMode]::Open,
      [IO.FileAccess]::Read,
      $share
    )
    try {
      $reader = [IO.StreamReader]::new($stream, $true)
      try {
        return [string]$reader.ReadToEnd()
      }
      finally {
        $reader.Dispose()
      }
    }
    finally {
      $stream.Dispose()
    }
  }
  catch {
    return ""
  }
}

function Get-SteamVrProcesses {
  $names = @(
    "vrmonitor",
    "vrserver",
    "vrcompositor",
    "vrdashboard",
    "vrwebhelper"
  )
  $found = @()
  foreach ($name in $names) {
    $found += @(Get-Process -Name $name -ErrorAction SilentlyContinue)
  }
  return @($found | Sort-Object Id -Unique)
}

function Assert-SteamVrAbsent([string]$Boundary) {
  $found = @(Get-SteamVrProcesses)
  if ($found.Count -ne 0) {
    $summary = ($found | ForEach-Object {
      "{0}(pid={1})" -f $_.ProcessName, $_.Id
    }) -join ", "
    throw "STEAMVR SAFETY ABORT at $Boundary`: $summary"
  }
  Write-Status "SAFETY: SteamVR absent at $Boundary boundary"
}

function Wrap-Degrees([double]$Degrees) {
  while ($Degrees -gt 180.0) {
    $Degrees -= 360.0
  }
  while ($Degrees -le -180.0) {
    $Degrees += 360.0
  }
  return $Degrees
}

function Get-LatestPosition([string]$Text, [int]$AfterIndex = -1) {
  $pattern =
    "CamMatrix: FP lock #\d+ L pos=\(" +
    "(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)\)"
  $matches = @([regex]::Matches($Text, $pattern) | Where-Object {
    $_.Index -gt $AfterIndex
  })
  if ($matches.Count -eq 0) {
    return [pscustomobject]@{
      Found = $false
      Index = -1
      X = [double]0
      Y = [double]0
      Z = [double]0
    }
  }
  $match = $matches[$matches.Count - 1]
  return [pscustomobject]@{
    Found = $true
    Index = $match.Index
    X = [double]::Parse(
      $match.Groups[1].Value,
      [Globalization.CultureInfo]::InvariantCulture
    )
    Y = [double]::Parse(
      $match.Groups[2].Value,
      [Globalization.CultureInfo]::InvariantCulture
    )
    Z = [double]::Parse(
      $match.Groups[3].Value,
      [Globalization.CultureInfo]::InvariantCulture
    )
  }
}

function Get-LatestHeading([string]$Text, [int]$AfterIndex = -1) {
  $matches = @(
    [regex]::Matches(
      $Text,
      "LookMove: heading=(-?\d+(?:\.\d+)?) deg"
    ) | Where-Object {
      $_.Index -gt $AfterIndex
    }
  )
  if ($matches.Count -eq 0) {
    return [pscustomobject]@{
      Found = $false
      Index = -1
      Degrees = [double]0
    }
  }
  $match = $matches[$matches.Count - 1]
  return [pscustomobject]@{
    Found = $true
    Index = $match.Index
    Degrees = [double]::Parse(
      $match.Groups[1].Value,
      [Globalization.CultureInfo]::InvariantCulture
    )
  }
}

function Get-LatestTransaction([string]$Text, [string]$Pattern) {
  $matches = @([regex]::Matches($Text, $Pattern))
  if ($matches.Count -eq 0) {
    return [uint64]0
  }
  return [uint64]$matches[$matches.Count - 1].Groups[1].Value
}

function Get-PlanarDistance(
  [double]$X1,
  [double]$Y1,
  [double]$X2,
  [double]$Y2
) {
  $dx = $X2 - $X1
  $dy = $Y2 - $Y1
  return [Math]::Sqrt($dx * $dx + $dy * $dy)
}

function Wait-ForGameEvidence(
  [scriptblock]$Selector,
  [uint32]$TimeoutSeconds,
  [string]$Description
) {
  $directory = Split-Path -Parent $GameLog
  $name = Split-Path -Leaf $GameLog
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  $watcher = [IO.FileSystemWatcher]::new($directory, $name)
  $watcher.IncludeSubdirectories = $false
  $watcher.NotifyFilter =
    [IO.NotifyFilters]::LastWrite -bor
    [IO.NotifyFilters]::Size
  $watcher.EnableRaisingEvents = $true
  try {
    while ([DateTime]::UtcNow -lt $deadline) {
      $selected = & $Selector (Read-SharedText $GameLog)
      if ($null -ne $selected) {
        return $selected
      }
      $remaining = [int][Math]::Ceiling(
        ($deadline - [DateTime]::UtcNow).TotalMilliseconds
      )
      if ($remaining -le 0) {
        break
      }
      [void]$watcher.WaitForChanged(
        [IO.WatcherChangeTypes]::Changed,
        [Math]::Min(1000, $remaining)
      )
    }
  }
  finally {
    $watcher.EnableRaisingEvents = $false
    $watcher.Dispose()
  }
  throw "Timed out waiting for $Description."
}

function Wait-ForWorldReady([uint32]$TimeoutSeconds) {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  $directory = Split-Path -Parent $GameLog
  $name = Split-Path -Leaf $GameLog
  $watcher = [IO.FileSystemWatcher]::new($directory, $name)
  $watcher.IncludeSubdirectories = $false
  $watcher.NotifyFilter =
    [IO.NotifyFilters]::LastWrite -bor
    [IO.NotifyFilters]::Size
  $watcher.EnableRaisingEvents = $true
  try {
    while ([DateTime]::UtcNow -lt $deadline) {
      $statusText = Read-SharedText $LauncherStatus
      $gameText = Read-SharedText $GameLog
      $hostText = Read-SharedText $HostLog
      $uiMatches = @(
        [regex]::Matches(
          $gameText,
          "UiState: presentation=[^\r\n]* reasons=0x([0-9A-Fa-f]+)"
        )
      )
      $latestUiWorld =
        $uiMatches.Count -gt 0 -and
        [Convert]::ToUInt32(
          $uiMatches[$uiMatches.Count - 1].Groups[1].Value,
          16
        ) -eq 0
      $position = Get-LatestPosition $gameText
      $ready =
        $statusText -match "SAFETY: runtimeKind=MetaSimulator" -and
        $statusText -match "META XR OPERATOR: opt-in layer validated x64" -and
        $statusText -match "DEPLOYED: backend=openxr stereo=204" -and
        $statusText -match "GTA READY: fresh x86 ASI session backend=OpenXR stereo=204" -and
        $statusText -match "Mode204 hard first-person camera hooks READY" -and
        $statusText -match "Mode204 gameplay first-person lock ACTIVE" -and
        $statusText -match "Mode204 native player head hide operational" -and
        $statusText -match "literal Mode204 parent L/R adapter capture READY" -and
        $statusText -match "Mode204 parent-dual exact capture pose active" -and
        $gameText -match "OpenXRController: Touch -> XInput ready" -and
        $latestUiWorld -and
        $position.Found -and
        $hostText -match "presentation=world-parent-dual-stereo"
      if ($ready) {
        return $position
      }
      $remaining = [int][Math]::Ceiling(
        ($deadline - [DateTime]::UtcNow).TotalMilliseconds
      )
      if ($remaining -le 0) {
        break
      }
      [void]$watcher.WaitForChanged(
        [IO.WatcherChangeTypes]::Changed,
        [Math]::Min(1000, $remaining)
      )
    }
  }
  finally {
    $watcher.EnableRaisingEvents = $false
    $watcher.Dispose()
  }
  throw (
    "Loaded Mode 204 first-person world did not satisfy the Meta Simulator, " +
    "Operator, stereo, UI-reasons=0, controller, and exact-pose gates."
  )
}

$script:McpProcess = $null
$script:McpReader = $null
$script:McpWriter = $null
$script:McpStderrTask = $null
$script:McpRequestId = 0

function Wait-McpResponse([int]$RequestId, [uint32]$TimeoutSeconds = 30) {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  while ([DateTime]::UtcNow -lt $deadline) {
    $remaining = [int][Math]::Ceiling(
      ($deadline - [DateTime]::UtcNow).TotalMilliseconds
    )
    $readTask = $script:McpReader.ReadLineAsync()
    if (-not $readTask.Wait([Math]::Max(1, $remaining))) {
      throw "Meta XR Operator MCP response timed out for request $RequestId."
    }
    $line = $readTask.Result
    if ($null -eq $line) {
      $stderr = if ($null -ne $script:McpStderrTask -and
          $script:McpStderrTask.IsCompleted) {
        $script:McpStderrTask.Result
      }
      else {
        ""
      }
      throw "Meta XR Operator MCP proxy closed stdout. stderr=$stderr"
    }
    $trimmed = $line.Trim()
    if (-not $trimmed.StartsWith("{")) {
      continue
    }
    try {
      $message = $trimmed | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
      continue
    }
    $idProperty = $message.PSObject.Properties["id"]
    if ($null -ne $idProperty -and [int]$idProperty.Value -eq $RequestId) {
      return $message
    }
  }
  throw "Meta XR Operator MCP response timed out for request $RequestId."
}

function Send-McpRequest(
  [string]$Method,
  [object]$Parameters,
  [uint32]$TimeoutSeconds = 30
) {
  ++$script:McpRequestId
  $requestId = $script:McpRequestId
  $request = [ordered]@{
    jsonrpc = "2.0"
    id = $requestId
    method = $Method
    params = $Parameters
  }
  $json = $request | ConvertTo-Json -Depth 20 -Compress
  $script:McpWriter.WriteLine($json)
  $script:McpWriter.Flush()
  $response = Wait-McpResponse `
    -RequestId $requestId `
    -TimeoutSeconds $TimeoutSeconds
  $errorProperty = $response.PSObject.Properties["error"]
  if ($null -ne $errorProperty) {
    throw (
      "Meta XR Operator MCP error: " +
      ($errorProperty.Value | ConvertTo-Json -Depth 12 -Compress)
    )
  }
  $resultProperty = $response.PSObject.Properties["result"]
  if ($null -eq $resultProperty) {
    throw "Meta XR Operator MCP response has no result."
  }
  return $resultProperty.Value
}

function Start-McpClient {
  $startInfo = [Diagnostics.ProcessStartInfo]::new()
  $startInfo.FileName = $ProxyPath
  $startInfo.WorkingDirectory = $ProxyDirectory
  $startInfo.UseShellExecute = $false
  $startInfo.CreateNoWindow = $true
  $startInfo.RedirectStandardInput = $true
  $startInfo.RedirectStandardOutput = $true
  $startInfo.RedirectStandardError = $true

  $script:McpProcess = [Diagnostics.Process]::new()
  $script:McpProcess.StartInfo = $startInfo
  if (-not $script:McpProcess.Start()) {
    throw "Failed to start the agent-owned Meta XR Operator MCP proxy."
  }
  $script:McpReader = $script:McpProcess.StandardOutput
  $script:McpWriter = $script:McpProcess.StandardInput
  $script:McpStderrTask =
    $script:McpProcess.StandardError.ReadToEndAsync()
  $initialize = Send-McpRequest `
    -Method "initialize" `
    -Parameters ([ordered]@{
      protocolVersion = "2025-03-26"
      capabilities = [ordered]@{}
      clientInfo = [ordered]@{
        name = "gtaiv-mode204-operator-scenario"
        version = "1.0"
      }
    })
  $notification = [ordered]@{
    jsonrpc = "2.0"
    method = "notifications/initialized"
    params = [ordered]@{}
  } | ConvertTo-Json -Depth 8 -Compress
  $script:McpWriter.WriteLine($notification)
  $script:McpWriter.Flush()
  Write-Status "META XR OPERATOR: persistent CLI MCP session initialized"
}

function Stop-McpClient {
  if ($null -eq $script:McpProcess) {
    return
  }
  try {
    $script:McpWriter.Close()
  }
  catch {}
  try {
    if (-not $script:McpProcess.WaitForExit(3000)) {
      $script:McpProcess.Kill()
    }
  }
  catch {}
}

function Invoke-OperatorTool(
  [string]$Name,
  [System.Collections.IDictionary]$Arguments,
  [uint32]$TimeoutSeconds = 30
) {
  $result = Send-McpRequest `
    -Method "tools/call" `
    -Parameters ([ordered]@{
      name = $Name
      arguments = $Arguments
    }) `
    -TimeoutSeconds $TimeoutSeconds
  $isErrorProperty = $result.PSObject.Properties["isError"]
  if ($null -ne $isErrorProperty -and [bool]$isErrorProperty.Value) {
    throw (
      "Meta XR Operator tool '$Name' reported isError: " +
      ($result | ConvertTo-Json -Depth 12 -Compress)
    )
  }
  return $result
}

function Get-OperatorText([object]$Result) {
  $contentProperty = $Result.PSObject.Properties["content"]
  if ($null -eq $contentProperty) {
    return ""
  }
  return (
    @($contentProperty.Value | Where-Object {
      [string]$_.type -eq "text"
    } | ForEach-Object {
      [string]$_.text
    }) -join "`n"
  )
}

function Invoke-OperatorText(
  [string]$Name,
  [System.Collections.IDictionary]$Arguments,
  [uint32]$TimeoutSeconds = 30
) {
  $result = Invoke-OperatorTool `
    -Name $Name `
    -Arguments $Arguments `
    -TimeoutSeconds $TimeoutSeconds
  return Get-OperatorText $result
}

function Set-ControllerInput(
  [ValidateSet("left", "right")]
  [string]$Hand,
  [string]$Component,
  [double]$Value,
  [string]$SubComponent = "",
  [bool]$AutoRelease = $false,
  [double]$HoldDuration = 0.2
) {
  $arguments = [ordered]@{
    hand = $Hand
    component = $Component
    value = $Value
    reason = "Deterministic GTA IV Mode 204 controller scenario"
    sentiment = "neutral"
  }
  if (-not [string]::IsNullOrWhiteSpace($SubComponent)) {
    $arguments.sub_component = $SubComponent
  }
  if ($AutoRelease) {
    $arguments.auto_release = $true
    $arguments.hold_duration = $HoldDuration
  }
  [void](Invoke-OperatorText `
      -Name "openxr_set_controller_input" `
      -Arguments $arguments)
}

function Set-AllInputsNeutral([bool]$BestEffort) {
  $inputs = @(
    @("left", "Thumbstick", "X"),
    @("left", "Thumbstick", "Y"),
    @("left", "X", ""),
    @("left", "Y", ""),
    @("left", "Menu", ""),
    @("left", "ThumbstickClick", ""),
    @("left", "Trigger", ""),
    @("left", "Grip", ""),
    @("right", "Thumbstick", "X"),
    @("right", "Thumbstick", "Y"),
    @("right", "A", ""),
    @("right", "B", ""),
    @("right", "ThumbstickClick", ""),
    @("right", "Trigger", ""),
    @("right", "Grip", "")
  )
  $failures = @()
  foreach ($input in $inputs) {
    try {
      Set-ControllerInput `
        -Hand $input[0] `
        -Component $input[1] `
        -SubComponent $input[2] `
        -Value 0.0
    }
    catch {
      $failures += "$($input[0])/$($input[1])/$($input[2]): $($_.Exception.Message)"
      if (-not $BestEffort) {
        throw
      }
    }
  }
  if ($failures.Count -ne 0) {
    return ($failures -join " | ")
  }
  return ""
}

function Set-StableHeadPose {
  # Head orientation is set exactly once. All subsequent world heading changes
  # use the right Touch stick so the simulated viewer never snaps or oscillates.
  $YawDegrees = 0.0
  $radians = $YawDegrees * [Math]::PI / 180.0
  $baseline = (Read-SharedText $GameLog).Length
  [void](Invoke-OperatorText `
      -Name "openxr_set_head_pose" `
      -Arguments ([ordered]@{
        orientation = @(
          0.0,
          [Math]::Sin($radians / 2.0),
          0.0,
          [Math]::Cos($radians / 2.0)
        )
        base_space = "local_floor"
        duration_seconds = 0.0
        reason = (
          "Hold one deterministic upright viewer pose; GTA route steering " +
          "uses only the right Touch thumbstick"
        )
        sentiment = "neutral"
      }))
  ++$script:headPoseSetCount
  if ($script:headPoseSetCount -ne 1) {
    throw (
      "Stable-head invariant violated: OpenXR head pose was set " +
      "$script:headPoseSetCount times."
    )
  }
  $heading = Wait-ForGameEvidence `
    -TimeoutSeconds 8 `
    -Description "fresh LookMove heading after the one stable head pose" `
    -Selector {
      param($text)
      $heading = Get-LatestHeading -Text $text -AfterIndex $baseline
      if ($heading.Found) {
        return $heading
      }
      return $null
    }
  Write-Status ((
    "HEAD LOCK: stable OpenXR head pose set once; initial GTA heading={0:F1}; " +
    "all route turns use right Touch thumbstick"
  ) -f $heading.Degrees)
  return $heading
}

function Get-ControllerEvidenceAfter(
  [int]$AfterIndex,
  [ValidateSet("Forward", "Turn", "VehicleY", "Accelerate", "Neutral")]
  [string]$Kind,
  [uint32]$TimeoutSeconds
) {
  $pattern =
    "OpenXRController: GTA consumed packet=(\d+) " +
    "buttons=0x([0-9A-Fa-f]+) LT=(\d+) RT=(\d+) " +
    "LS=\((-?\d+),(-?\d+)\) RS=\((-?\d+),(-?\d+)\) neutral=([01])"
  return Wait-ForGameEvidence `
    -TimeoutSeconds $TimeoutSeconds `
    -Description "GTA controller evidence kind=$Kind" `
    -Selector {
      param($text)
      foreach ($match in @(
          [regex]::Matches($text, $pattern) |
          Where-Object { $_.Index -gt $AfterIndex }
        )) {
        $buttons = [Convert]::ToUInt32($match.Groups[2].Value, 16)
        $rightTrigger = [int]$match.Groups[4].Value
        $leftY = [int]$match.Groups[6].Value
        $rightX = [int]$match.Groups[7].Value
        $neutral = $match.Groups[9].Value -eq "1"
        $accepted = switch ($Kind) {
          "Forward" {
            -not $neutral -and [Math]::Abs($leftY) -ge 16000
            break
          }
          "Turn" {
            -not $neutral -and [Math]::Abs($rightX) -ge 8000
            break
          }
          "VehicleY" {
            -not $neutral -and ($buttons -band 0x8000) -ne 0
            break
          }
          "Accelerate" {
            -not $neutral -and $rightTrigger -ge 128
            break
          }
          "Neutral" {
            $neutral
            break
          }
        }
        if ($accepted) {
          return [pscustomobject]@{
            Index = $match.Index
            Packet = [uint64]$match.Groups[1].Value
            Buttons = $buttons
            LeftY = $leftY
            RightX = $rightX
            RightTrigger = $rightTrigger
            Neutral = $neutral
          }
        }
      }
      return $null
    }
}

function Get-VrYawEvidenceAfter(
  [int]$AfterIndex,
  [ValidateSet("Turn", "Neutral")]
  [string]$Kind,
  [uint32]$TimeoutSeconds
) {
  $pattern =
    "OpenXRController: VR yaw event=(\d+) RSX=(-?\d+) " +
    "nativeSuppressed=1 neutral=([01])"
  return Wait-ForGameEvidence `
    -TimeoutSeconds $TimeoutSeconds `
    -Description "VR-owned right-stick evidence kind=$Kind" `
    -Selector {
      param($text)
      foreach ($match in @(
          [regex]::Matches($text, $pattern) |
          Where-Object { $_.Index -gt $AfterIndex }
        )) {
        $rightX = [int]$match.Groups[2].Value
        $neutral = $match.Groups[3].Value -eq "1"
        $accepted = if ($Kind -eq "Turn") {
          -not $neutral -and [Math]::Abs($rightX) -ge 8000
        }
        else {
          $neutral
        }
        if ($accepted) {
          return [pscustomobject]@{
            Index = $match.Index
            Packet = [uint64]$match.Groups[1].Value
            RightX = $rightX
            Neutral = $neutral
          }
        }
      }
      return $null
    }
}

function Invoke-TurnPulse(
  [double]$ControllerX,
  [double]$Seconds
) {
  $baseline = (Read-SharedText $GameLog).Length
  Set-ControllerInput `
    -Hand "right" `
    -Component "Thumbstick" `
    -SubComponent "Y" `
    -Value 0.0
  Set-ControllerInput `
    -Hand "right" `
    -Component "Thumbstick" `
    -SubComponent "X" `
    -Value $ControllerX `
    -AutoRelease $true `
    -HoldDuration $Seconds
  $turn = Get-VrYawEvidenceAfter `
    -AfterIndex $baseline `
    -Kind "Turn" `
    -TimeoutSeconds 5
  try {
    $neutral = Get-VrYawEvidenceAfter `
      -AfterIndex $turn.Index `
      -Kind "Neutral" `
      -TimeoutSeconds ([uint32][Math]::Ceiling($Seconds + 4.0))
  }
  catch {
    Set-ControllerInput `
      -Hand "right" `
      -Component "Thumbstick" `
      -SubComponent "X" `
      -Value 0.0
    $neutral = Get-VrYawEvidenceAfter `
      -AfterIndex $turn.Index `
      -Kind "Neutral" `
      -TimeoutSeconds 5
  }

  # Wait for a post-release heading sample. This avoids steering from an
  # in-flight value and makes every correction a bounded closed-loop step.
  $headingBaseline = (Read-SharedText $GameLog).Length
  $heading = Wait-ForGameEvidence `
    -TimeoutSeconds 8 `
    -Description "post-neutral LookMove heading after right-stick turn" `
    -Selector {
      param($text)
      $sample = Get-LatestHeading -Text $text -AfterIndex $headingBaseline
      if ($sample.Found) {
        return $sample
      }
      return $null
    }
  return [pscustomobject]@{
    TurnPacket = $turn.Packet
    NeutralPacket = $neutral.Packet
    Heading = $heading
  }
}

function Set-WorldHeadingWithController([double]$TargetDegrees) {
  $target = Wrap-Degrees $TargetDegrees
  $current = Get-LatestHeading (Read-SharedText $GameLog)
  if (-not $current.Found) {
    throw "No GTA LookMove heading is available for controller steering."
  }
  $stallCount = 0
  for ($step = 0; $step -lt 8; ++$step) {
    $before = [double]$current.Degrees
    $error = Wrap-Degrees ($target - $before)
    if ([Math]::Abs($error) -le 5.0) {
      return $before
    }

    $magnitude = if ([Math]::Abs($error) -gt 60.0) {
      0.75
    }
    elseif ([Math]::Abs($error) -gt 20.0) {
      0.60
    }
    else {
      0.42
    }
    # vr_move.cpp negates XInput right-stick X before integrating controller
    # yaw. Therefore negative X increases GTA's logged world heading.
    $controllerX = if ($error -gt 0.0) {
      -$magnitude
    }
    else {
      $magnitude
    }
    $effectiveStick =
      ($magnitude - 0.22) / (1.0 - 0.22)
    $estimatedDegreesPerSecond =
      $effectiveStick * 2.8 * 180.0 / [Math]::PI
    $seconds = [Math]::Min(
      0.45,
      [Math]::Max(
        0.10,
        [Math]::Abs($error) / $estimatedDegreesPerSecond * 0.75
      )
    )
    $pulse = Invoke-TurnPulse `
      -ControllerX $controllerX `
      -Seconds $seconds
    ++$script:controllerSteeringPulseCount
    $current = $pulse.Heading
    $actualTurn = Wrap-Degrees ($current.Degrees - $before)
    $remaining = Wrap-Degrees ($target - $current.Degrees)
    if ([Math]::Abs($actualTurn) -lt 1.0) {
      ++$stallCount
    }
    else {
      $stallCount = 0
    }
    Write-Status ((
      "STEER PULSE: step=$($step + 1)/8 target={0:F1} before={1:F1} " +
      "error={2:F1} rightX={3:F2} hold={4:F2}s after={5:F1} " +
      "remaining={6:F1} packets={7}->{8}"
    ) -f
      $target,
      $before,
      $error,
      $controllerX,
      $seconds,
      $current.Degrees,
      $remaining,
      $pulse.TurnPacket,
      $pulse.NeutralPacket
    )
    if ($stallCount -ge 2) {
      throw (
        "Right-stick steering produced no GTA heading change for two " +
        "bounded pulses; target=$target observed=$($current.Degrees)."
      )
    }
  }
  $finalError = Wrap-Degrees ($target - $current.Degrees)
  if ([Math]::Abs($finalError) -gt 5.0) {
    throw (
      "Controller steering could not converge to target=$target; " +
      "observed=$($current.Degrees) error=$finalError after eight pulses."
    )
  }
  return $current.Degrees
}

function Invoke-ForwardPulse([double]$Seconds) {
  $baseline = (Read-SharedText $GameLog).Length
  Set-ControllerInput `
    -Hand "left" `
    -Component "Thumbstick" `
    -SubComponent "X" `
    -Value 0.0
  Set-ControllerInput `
    -Hand "left" `
    -Component "Thumbstick" `
    -SubComponent "Y" `
    -Value 0.80 `
    -AutoRelease $true `
    -HoldDuration $Seconds
  $forward = Get-ControllerEvidenceAfter `
    -AfterIndex $baseline `
    -Kind "Forward" `
    -TimeoutSeconds 5
  try {
    $neutral = Get-ControllerEvidenceAfter `
      -AfterIndex $forward.Index `
      -Kind "Neutral" `
      -TimeoutSeconds ([uint32][Math]::Ceiling($Seconds + 4.0))
  }
  catch {
    Set-ControllerInput `
      -Hand "left" `
      -Component "Thumbstick" `
      -SubComponent "Y" `
      -Value 0.0
    $neutral = Get-ControllerEvidenceAfter `
      -AfterIndex $forward.Index `
      -Kind "Neutral" `
      -TimeoutSeconds 5
  }
  $position = Wait-ForGameEvidence `
    -TimeoutSeconds 5 `
    -Description "fresh first-person camera position after forward pulse" `
    -Selector {
      param($text)
      $sample = Get-LatestPosition -Text $text -AfterIndex $baseline
      if ($sample.Found) {
        return $sample
      }
      return $null
    }
  return [pscustomobject]@{
    ForwardPacket = $forward.Packet
    NeutralPacket = $neutral.Packet
    Position = $position
  }
}

function Save-OperatorCapture([string]$Label) {
  $captures = @()
  foreach ($eye in @("left", "right")) {
    $result = Invoke-OperatorTool `
      -Name "openxr_capture_composited_image" `
      -Arguments ([ordered]@{
        eye = $eye
        reason = "Archive $Label evidence for the GTA Mode 204 scenario"
        sentiment = "neutral"
      }) `
      -TimeoutSeconds 45
    $images = @($result.content | Where-Object {
      [string]$_.type -eq "image" -and
      $null -ne $_.PSObject.Properties["data"]
    })
    if ($images.Count -ne 1) {
      throw "Operator returned $($images.Count) images for $Label/$eye."
    }
    $safeLabel = $Label -replace '[^A-Za-z0-9_.-]', '-'
    $path = Join-Path $OutputDirectory "$safeLabel-$eye.png"
    $bytes = [Convert]::FromBase64String([string]$images[0].data)
    [IO.File]::WriteAllBytes($path, $bytes)
    $captures += [pscustomobject]@{
      Eye = $eye
      Path = $path
      Bytes = $bytes.Length
      Sha256 = (
        Get-FileHash -LiteralPath $path -Algorithm SHA256
      ).Hash
    }
  }
  foreach ($capture in $captures) {
    Write-Status (
      "CAPTURE: label=$Label eye=$($capture.Eye) " +
      "bytes=$($capture.Bytes) sha256=$($capture.Sha256)"
    )
  }
  return @($captures)
}

function Save-ScenarioCapture([string]$Label) {
  if ($SkipStageCaptures) {
    Write-Status (
      "CAPTURE: label=$Label skipped; dedicated serialized eye recorder active"
    )
    return @()
  }
  return @(Save-OperatorCapture $Label)
}

function Test-StagePosition(
  [object]$Position,
  [object]$Stage,
  [object]$InitialPosition
) {
  $target = @($Stage.target)
  $distance = Get-PlanarDistance `
    -X1 $Position.X `
    -Y1 $Position.Y `
    -X2 ([double]$target[0]) `
    -Y2 ([double]$target[1])
  $ready = $distance -le [double]$Stage.radiusMeters
  $maxZProperty = $Stage.PSObject.Properties["maxCameraZ"]
  if ($null -ne $maxZProperty) {
    $ready = $ready -and $Position.Z -le [double]$maxZProperty.Value
  }
  $dropProperty = $Stage.PSObject.Properties["minDropFromStartMeters"]
  if ($null -ne $dropProperty) {
    $ready =
      $ready -and
      ($InitialPosition.Z - $Position.Z) -ge [double]$dropProperty.Value
  }
  $travelProperty = $Stage.PSObject.Properties["minPlanarFromStartMeters"]
  if ($null -ne $travelProperty) {
    $travel = Get-PlanarDistance `
      -X1 $InitialPosition.X `
      -Y1 $InitialPosition.Y `
      -X2 $Position.X `
      -Y2 $Position.Y
    $ready = $ready -and $travel -ge [double]$travelProperty.Value
  }
  return [pscustomobject]@{
    Ready = $ready
    Distance = $distance
  }
}

function Move-ToStage(
  [object]$Stage,
  [object]$InitialPosition
) {
  $stageId = [string]$Stage.id
  $target = @($Stage.target)
  $position = Get-LatestPosition (Read-SharedText $GameLog)
  $stallCount = 0
  for (
    $pulse = 0;
    $pulse -lt [int]$Stage.maxPulses;
    ++$pulse
  ) {
    $gate = Test-StagePosition `
      -Position $position `
      -Stage $Stage `
      -InitialPosition $InitialPosition
    if ($gate.Ready) {
      Write-Status ((
        "ROUTE PASS: stage=$stageId pulses=$pulse " +
        "position=({0:F3},{1:F3},{2:F3}) targetDistance={3:F3}"
      ) -f
        $position.X,
        $position.Y,
        $position.Z,
        $gate.Distance
      )
      [void](Save-ScenarioCapture "stage-$stageId")
      return $position
    }

    $dx = [double]$target[0] - $position.X
    $dy = [double]$target[1] - $position.Y
    $heading = [Math]::Atan2(-$dx, $dy) * 180.0 / [Math]::PI
    $observed = Set-WorldHeadingWithController -TargetDegrees $heading
    $before = $position
    $pulseResult = Invoke-ForwardPulse ([double]$Stage.pulseSeconds)
    $position = $pulseResult.Position
    $movement = Get-PlanarDistance `
      -X1 $before.X `
      -Y1 $before.Y `
      -X2 $position.X `
      -Y2 $position.Y
    $afterDistance = Get-PlanarDistance `
      -X1 $position.X `
      -Y1 $position.Y `
      -X2 ([double]$target[0]) `
      -Y2 ([double]$target[1])
    $improvement = $gate.Distance - $afterDistance
    if ($movement -lt 0.05 -or $improvement -lt -0.10) {
      ++$stallCount
    }
    else {
      $stallCount = 0
    }
    Write-Status ((
      "ROUTE PULSE: stage=$stageId pulse=$($pulse + 1)/" +
      "$($Stage.maxPulses) requestedHeading={0:F1} observedHeading={1:F1} " +
      "movement={2:F3} targetDistance={3:F3} position=" +
      "({4:F3},{5:F3},{6:F3}) packets={7}->{8}"
    ) -f
      $heading,
      $observed,
      $movement,
      $afterDistance,
      $position.X,
      $position.Y,
      $position.Z,
      $pulseResult.ForwardPacket,
      $pulseResult.NeutralPacket
    )
    if ($stallCount -ge 3) {
      [void](Save-ScenarioCapture "blocked-$stageId")
      throw (
        "Route blocked at stage=$stageId after three bounded pulses; " +
        "position=($($position.X),$($position.Y),$($position.Z)). " +
        "Tune this JSON waypoint from the archived Operator captures; " +
        "the script will not thrash against geometry."
      )
    }
  }
  [void](Save-ScenarioCapture "failed-$stageId")
  throw (
    "Route stage=$stageId exhausted $($Stage.maxPulses) pulses without " +
    "satisfying its coordinate/height gate."
  )
}

function Invoke-VehicleEntryAttempt([string]$AttemptId) {
  $baseline = (Read-SharedText $GameLog).Length
  Set-ControllerInput `
    -Hand "left" `
    -Component "Y" `
    -Value 1.0 `
    -AutoRelease $true `
    -HoldDuration 0.50
  $pressed = Get-ControllerEvidenceAfter `
    -AfterIndex $baseline `
    -Kind "VehicleY" `
    -TimeoutSeconds 5
  try {
    $neutral = Get-ControllerEvidenceAfter `
      -AfterIndex $pressed.Index `
      -Kind "Neutral" `
      -TimeoutSeconds 5
  }
  catch {
    Set-ControllerInput -Hand "left" -Component "Y" -Value 0.0
    $neutral = Get-ControllerEvidenceAfter `
      -AfterIndex $pressed.Index `
      -Kind "Neutral" `
      -TimeoutSeconds 5
  }
  $entered = $false
  try {
    [void](Wait-ForGameEvidence `
        -TimeoutSeconds 8 `
        -Description "vehicle occupancy after attempt $AttemptId" `
        -Selector {
          param($text)
          $tail = if ($baseline -lt $text.Length) {
            $text.Substring($baseline)
          }
          else {
            ""
          }
          if ($tail -match
              "VehCam: INSIDE vehicle|EnterFp: vehicle ptr set") {
            return $true
          }
          return $null
        })
    $entered = $true
  }
  catch {
    $entered = $false
  }
  Write-Status (
    "VEHICLE ATTEMPT: id=$AttemptId consumedPacket=$($pressed.Packet) " +
    "neutralPacket=$($neutral.Packet) entered=$entered"
  )
  [void](Save-ScenarioCapture "vehicle-$AttemptId")
  return [pscustomobject]@{
    Consumed = $true
    Entered = $entered
    PressedPacket = $pressed.Packet
    NeutralPacket = $neutral.Packet
  }
}

function Invoke-VehicleDriveProof([string]$AttemptId) {
  $drive = $scenario.vehicleDriveProof
  $start = Get-LatestPosition (Read-SharedText $GameLog)
  if (-not $start.Found) {
    throw "No first-person camera position exists before vehicle drive proof."
  }
  $last = $start
  $lastPressedPacket = [uint64]0
  $lastNeutralPacket = [uint64]0
  for ($pulse = 0; $pulse -lt [int]$drive.maxPulses; ++$pulse) {
    $baseline = (Read-SharedText $GameLog).Length
    Set-ControllerInput `
      -Hand "right" `
      -Component "Trigger" `
      -Value ([double]$drive.triggerValue) `
      -AutoRelease $true `
      -HoldDuration ([double]$drive.pulseSeconds)
    $pressed = Get-ControllerEvidenceAfter `
      -AfterIndex $baseline `
      -Kind "Accelerate" `
      -TimeoutSeconds 5
    $script:vehicleAccelerationConsumed = $true
    try {
      $neutral = Get-ControllerEvidenceAfter `
        -AfterIndex $pressed.Index `
        -Kind "Neutral" `
        -TimeoutSeconds (
          [uint32][Math]::Ceiling([double]$drive.pulseSeconds + 4.0)
        )
    }
    catch {
      Set-ControllerInput `
        -Hand "right" `
        -Component "Trigger" `
        -Value 0.0
      $neutral = Get-ControllerEvidenceAfter `
        -AfterIndex $pressed.Index `
        -Kind "Neutral" `
        -TimeoutSeconds 5
    }
    $lastPressedPacket = $pressed.Packet
    $lastNeutralPacket = $neutral.Packet
    $positionBaseline = (Read-SharedText $GameLog).Length
    $last = Wait-ForGameEvidence `
      -TimeoutSeconds 5 `
      -Description "fresh vehicle camera position after acceleration" `
      -Selector {
        param($text)
        $sample = Get-LatestPosition -Text $text -AfterIndex $positionBaseline
        if ($sample.Found) {
          return $sample
        }
        return $null
      }
    $travel = Get-PlanarDistance `
      -X1 $start.X `
      -Y1 $start.Y `
      -X2 $last.X `
      -Y2 $last.Y
    $script:vehicleDriveDistanceMeters =
      [Math]::Max($script:vehicleDriveDistanceMeters, $travel)
    Write-Status ((
      "VEHICLE DRIVE PULSE: id=$AttemptId pulse=$($pulse + 1)/" +
      "$($drive.maxPulses) RT={0:F2} hold={1:F2}s travel={2:F3}m " +
      "position=({3:F3},{4:F3},{5:F3}) packets={6}->{7}"
    ) -f
      [double]$drive.triggerValue,
      [double]$drive.pulseSeconds,
      $travel,
      $last.X,
      $last.Y,
      $last.Z,
      $pressed.Packet,
      $neutral.Packet
    )
    if ($travel -ge [double]$drive.minPlanarDistanceMeters) {
      [void](Save-ScenarioCapture "vehicle-drive-$AttemptId")
      Write-Status ((
        "VEHICLE DRIVE PASS: id=$AttemptId travel={0:F3}m " +
        "required={1:F3}m"
      ) -f
        $travel,
        [double]$drive.minPlanarDistanceMeters
      )
      return [pscustomobject]@{
        Proved = $true
        Distance = $travel
        PressedPacket = $lastPressedPacket
        NeutralPacket = $lastNeutralPacket
        Position = $last
      }
    }
  }
  [void](Save-ScenarioCapture "vehicle-drive-failed-$AttemptId")
  $finalTravel = Get-PlanarDistance `
    -X1 $start.X `
    -Y1 $start.Y `
    -X2 $last.X `
    -Y2 $last.Y
  throw (
    "Vehicle entry was confirmed and RT acceleration was consumed, but " +
    "camera travel was only $finalTravel m after $($drive.maxPulses) " +
    "bounded pulses; required $($drive.minPlanarDistanceMeters) m."
  )
}

$outcome = "ABORTED"
$failure = ""
$operatorSessionFocused = $false
$interactionProfileReady = $false
$headPoseReady = $false
$controllerPoseReady = $false
$cleanupNeutral = $false
$cleanupFailure = ""
$outsideReached = $false
$streetReached = $false
$vehicleAttemptConsumed = $false
$vehicleEntered = $false
$vehicleAccelerationConsumed = $false
$vehicleDriveProved = $false
$vehicleDriveDistanceMeters = [double]0
$vehicleAttemptCount = 0
$headPoseSetCount = 0
$controllerSteeringPulseCount = 0
$initialPosition = $null
$finalPosition = $null
$producerStart = [uint64]0
$producerEnd = [uint64]0
$hostStart = [uint64]0
$hostEnd = [uint64]0
$heartbeatPauseStart = 0
$heartbeatPauseEnd = 0
$captures = @()
$producerPattern =
  "OpenXRBridge: frame transaction=(\d+)[^\r\n]*presentation=world-stereo"
$hostPattern =
  "(?:GameBridge: frame acquired|" +
  "XRHost: game swapchain (?:updated|reused)) " +
  "transaction=(\d+)[^\r\n]*presentation=world-parent-dual-stereo"

try {
  Assert-SteamVrAbsent "scenario-preflight"
  $initialPosition = Wait-ForWorldReady $WorldReadyTimeoutSeconds
  $expectedStart = @($scenario.expectedStart.cameraPosition)
  $startDistance = Get-PlanarDistance `
    -X1 $initialPosition.X `
    -Y1 $initialPosition.Y `
    -X2 ([double]$expectedStart[0]) `
    -Y2 ([double]$expectedStart[1])
  $startZError =
    [Math]::Abs($initialPosition.Z - [double]$expectedStart[2])
  if ($startDistance -gt [double]$scenario.expectedStart.radiusMeters -or
      $startZError -gt 2.0) {
    throw (
      "Loaded save is not at the expected Roman apartment start: " +
      "actual=($($initialPosition.X),$($initialPosition.Y)," +
      "$($initialPosition.Z)) expected=($($expectedStart -join ',')) " +
      "planarError=$startDistance zError=$startZError."
    )
  }

  $gameAtStart = Read-SharedText $GameLog
  $hostAtStart = Read-SharedText $HostLog
  $producerStart = Get-LatestTransaction $gameAtStart $producerPattern
  $hostStart = Get-LatestTransaction $hostAtStart $hostPattern
  $heartbeatPauseStart = (
    [regex]::Matches(
      $hostAtStart,
      "GameBridge: GTA frame heartbeat paused"
    )
  ).Count
  Write-Status ((
    "WORLD READY: position=({0:F3},{1:F3},{2:F3}) " +
    "producer=$producerStart host=$hostStart"
  ) -f
    $initialPosition.X,
    $initialPosition.Y,
    $initialPosition.Z
  )

  Start-McpClient
  $sessionText = Invoke-OperatorText `
    -Name "openxr_get_session_info" `
    -Arguments ([ordered]@{
      include_history = $false
      reason = "Require a focused Meta Simulator session before GTA input"
      sentiment = "neutral"
    })
  if ($sessionText -notmatch "FOCUSED") {
    throw "Meta XR Operator session is not FOCUSED: $sessionText"
  }
  $operatorSessionFocused = $true

  $frameText = Invoke-OperatorText `
    -Name "openxr_get_frame_info" `
    -Arguments ([ordered]@{
      include_history = $false
      reason = "Verify active OpenXR frame flow before GTA input"
      sentiment = "neutral"
    })
  if ([string]::IsNullOrWhiteSpace($frameText)) {
    throw "Meta XR Operator returned empty frame information."
  }

  $controllerPoses = @(
    [ordered]@{
      hand = "left"
      position = @(-0.25, 1.25, -0.35)
    },
    [ordered]@{
      hand = "right"
      position = @(0.25, 1.25, -0.35)
    }
  )
  foreach ($controller in $controllerPoses) {
    foreach ($poseType in @("grip", "aim")) {
      [void](Invoke-OperatorText `
          -Name "openxr_set_controller_pose" `
          -Arguments ([ordered]@{
            hand = $controller.hand
            pose_type = $poseType
            position = $controller.position
            orientation = @(0.0, 0.0, 0.0, 1.0)
            base_space = "local_floor"
            duration_seconds = 0.0
            reason = "Activate valid Touch grip and aim pose for GTA actions"
            sentiment = "neutral"
          }))
      $poseText = Invoke-OperatorText `
        -Name "openxr_get_controller_pose" `
        -Arguments ([ordered]@{
          hand = $controller.hand
          pose_type = $poseType
          base_space = "local_floor"
          reason = "Verify GTA will receive a valid simulated controller pose"
          sentiment = "neutral"
        })
      if ($poseText -notmatch "(?i)simulated") {
        throw (
          "Operator did not read back simulated $($controller.hand) " +
          "$poseType pose: $poseText"
        )
      }
    }
  }
  $controllerPoseReady = $true

  $profileText = Invoke-OperatorText `
    -Name "openxr_get_active_interaction_profile" `
    -Arguments ([ordered]@{
      hand = "both"
      reason = "Require Touch bindings before GTA controller automation"
      sentiment = "neutral"
    })
  if ($profileText -notmatch "(?i)touch_controller") {
    throw "Touch interaction profile is not active: $profileText"
  }
  $interactionProfileReady = $true

  $cleanupFailure = Set-AllInputsNeutral -BestEffort $false
  if (-not [string]::IsNullOrWhiteSpace($cleanupFailure)) {
    throw "Initial controller neutral failed: $cleanupFailure"
  }
  Write-Status "INPUT: all mapped Touch components neutral"

  $stableHeading = Set-StableHeadPose
  $headText = Invoke-OperatorText `
    -Name "openxr_get_head_pose" `
    -Arguments ([ordered]@{
      base_space = "local_floor"
      reason = "Verify deterministic upright simulated head pose"
      sentiment = "neutral"
    })
  if ($headText -notmatch "(?i)position" -or
      $headText -notmatch "(?i)orientation") {
    throw "Operator head-pose readback is incomplete: $headText"
  }
  $headPoseReady = $true
  $captures += @(Save-ScenarioCapture "stage-apartment-start")

  foreach ($stage in @($scenario.route)) {
    $finalPosition = Move-ToStage `
      -Stage $stage `
      -InitialPosition $initialPosition
    if ([string]$stage.id -eq "outside-sidewalk") {
      $outsideReached = $true
    }
    if ([string]$stage.id -eq "street-lane") {
      $streetReached = $true
      $recordMarker = Join-Path `
        $OutputDirectory "operator-street-record-ready.marker"
      Set-Content `
        -LiteralPath "$recordMarker.tmp" `
        -Value (
          "Mode204 Meta Simulator street gate PASS at " +
          (Get-Date).ToString("o")
        ) `
        -Encoding UTF8
      Move-Item `
        -LiteralPath "$recordMarker.tmp" `
        -Destination $recordMarker `
        -Force
    }
  }

  foreach ($attempt in @($scenario.vehicleAttempts)) {
    ++$vehicleAttemptCount
    $movementStage = [pscustomobject]@{
      id = "vehicle-$([string]$attempt.id)-position"
      target = @($attempt.target)
      radiusMeters = [double]$attempt.radiusMeters
      pulseSeconds = [double]$attempt.pulseSeconds
      maxPulses = [int]$attempt.maxPulses
    }
    $finalPosition = Move-ToStage `
      -Stage $movementStage `
      -InitialPosition $initialPosition
    $vehicle = Invoke-VehicleEntryAttempt ([string]$attempt.id)
    $vehicleAttemptConsumed =
      $vehicleAttemptConsumed -or $vehicle.Consumed
    if ($vehicle.Entered) {
      $vehicleEntered = $true
      $driveProof = Invoke-VehicleDriveProof ([string]$attempt.id)
      $vehicleAccelerationConsumed =
        [uint64]$driveProof.PressedPacket -gt 0
      $vehicleDriveProved = [bool]$driveProof.Proved
      $vehicleDriveDistanceMeters = [double]$driveProof.Distance
      $finalPosition = $driveProof.Position
      break
    }
  }

  if (-not $outsideReached -or -not $streetReached) {
    throw "Outside/street coordinate and height gates were not both reached."
  }
  if (-not $vehicleAttemptConsumed) {
    throw "GTA did not consume the mapped left-Y vehicle-entry attempt."
  }
  if (($RequireVehicleEntry -or [bool]$scenario.requireVehicleEntry) -and
      -not $vehicleEntered) {
    throw (
      "Vehicle entry was required, but no attempt produced " +
      "VehCam/vehicle-pointer occupancy telemetry."
    )
  }
  if ($vehicleEntered -and
      (-not $vehicleAccelerationConsumed -or -not $vehicleDriveProved)) {
    throw (
      "Vehicle occupancy was confirmed, but RT acceleration consumption and " +
      "world-space drive movement were not both proved."
    )
  }

  $gameAtEnd = Read-SharedText $GameLog
  $hostAtEnd = Read-SharedText $HostLog
  $producerEnd = Get-LatestTransaction $gameAtEnd $producerPattern
  $hostEnd = Get-LatestTransaction $hostAtEnd $hostPattern
  $heartbeatPauseEnd = (
    [regex]::Matches(
      $hostAtEnd,
      "GameBridge: GTA frame heartbeat paused"
    )
  ).Count
  if ($producerEnd -le $producerStart -or
      $hostEnd -le $hostStart -or
      $heartbeatPauseEnd -ne $heartbeatPauseStart) {
    throw (
      "World-frame continuity failed during the scenario: " +
      "producer=$producerStart->$producerEnd host=$hostStart->$hostEnd " +
      "heartbeatPauses=$heartbeatPauseStart->$heartbeatPauseEnd."
    )
  }
  Assert-SteamVrAbsent "scenario-complete"
  $outcome = if ($vehicleEntered) {
    "SCENARIO_COMPLETE_VEHICLE_DRIVEN"
  }
  else {
    "SCENARIO_COMPLETE_VEHICLE_ATTEMPTED"
  }
}
catch {
  $failure = $_.Exception.Message
  Write-Status "SCENARIO FAILED: $failure"
}
finally {
  if ($null -ne $script:McpProcess -and
      -not $script:McpProcess.HasExited) {
    try {
      $cleanupFailure = Set-AllInputsNeutral -BestEffort $true
      $cleanupNeutral =
        [string]::IsNullOrWhiteSpace($cleanupFailure)
      if ($cleanupNeutral) {
        Write-Status "CLEANUP: final all-component controller neutral PASS"
      }
      else {
        Write-Status "CLEANUP WARNING: $cleanupFailure"
      }
    }
    catch {
      $cleanupFailure = $_.Exception.Message
      $cleanupNeutral = $false
      Write-Status "CLEANUP WARNING: $cleanupFailure"
    }
  }
  Stop-McpClient
  if ($null -eq $finalPosition) {
    $candidate = Get-LatestPosition (Read-SharedText $GameLog)
    if ($candidate.Found) {
      $finalPosition = $candidate
    }
  }
  if ($producerEnd -eq 0) {
    $producerEnd = Get-LatestTransaction `
      (Read-SharedText $GameLog) `
      $producerPattern
  }
  if ($hostEnd -eq 0) {
    $hostEnd = Get-LatestTransaction `
      (Read-SharedText $HostLog) `
      $hostPattern
  }
  # Always report the true final pause count, including fail-fast paths.
  $heartbeatPauseEnd = (
    [regex]::Matches(
      (Read-SharedText $HostLog),
      "GameBridge: GTA frame heartbeat paused"
    )
  ).Count
  $result = @(
    "outcome=$outcome"
    "failure=$failure"
    "scenario=$($scenarioSummary.Name)"
    "scenarioPath=$ScenarioPath"
    "scenarioSha256=$((Get-FileHash -LiteralPath $ScenarioPath -Algorithm SHA256).Hash)"
    "operatorManifestSha256=$($bundleHashes.manifest)"
    "operatorLibrarySha256=$($bundleHashes.library)"
    "operatorProxySha256=$($bundleHashes.proxy)"
    "runDirectory=$RunDirectory"
    "runtimeKind=MetaSimulator"
    "stereoMode=204"
    "launchesGame=False"
    "simulatorGuiControl=False"
    "rendererChanges=False"
    "operatorSessionFocused=$operatorSessionFocused"
    "interactionProfileReady=$interactionProfileReady"
    "headPoseReady=$headPoseReady"
    "headPoseSetCount=$headPoseSetCount"
    "controllerSteeringPulseCount=$controllerSteeringPulseCount"
    "controllerGripAimPoseReady=$controllerPoseReady"
    "outsideReached=$outsideReached"
    "streetReached=$streetReached"
    "vehicleAttemptCount=$vehicleAttemptCount"
    "vehicleAttemptConsumed=$vehicleAttemptConsumed"
    "vehicleEntered=$vehicleEntered"
    "vehicleAccelerationConsumed=$vehicleAccelerationConsumed"
    "vehicleDriveProved=$vehicleDriveProved"
    "vehicleDriveDistanceMeters=$vehicleDriveDistanceMeters"
    "vehicleEntryRequired=$(($RequireVehicleEntry -or [bool]$scenario.requireVehicleEntry))"
    "initialPosition=$(if ($null -ne $initialPosition) { "$($initialPosition.X),$($initialPosition.Y),$($initialPosition.Z)" } else { "UNAVAILABLE" })"
    "finalPosition=$(if ($null -ne $finalPosition) { "$($finalPosition.X),$($finalPosition.Y),$($finalPosition.Z)" } else { "UNAVAILABLE" })"
    "producerTransactions=$producerStart->$producerEnd"
    "hostTransactions=$hostStart->$hostEnd"
    "heartbeatPauseCount=$heartbeatPauseStart->$heartbeatPauseEnd"
    "cleanupNeutral=$cleanupNeutral"
    "cleanupFailure=$cleanupFailure"
    "captureDirectory=$OutputDirectory"
  )
  Set-Content -LiteralPath $ResultPath -Value $result -Encoding UTF8
  Write-Status "RESULT: $outcome"
}

if ($outcome -notlike "SCENARIO_COMPLETE*") {
  throw "Meta XR Operator GTA scenario failed: $failure"
}
if (-not $cleanupNeutral) {
  throw "Scenario passed, but final controller neutral cleanup failed."
}
Write-Output "META XR OPERATOR GTA SCENARIO PASS: $outcome"

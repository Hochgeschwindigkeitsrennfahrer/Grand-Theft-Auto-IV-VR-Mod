$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$launcherPath = Join-Path `
  $PSScriptRoot "..\scripts\run-openxr-gta-steam-safe.ps1"
$launcher = Get-Content -LiteralPath $launcherPath -Raw

$tokens = $null
$parseErrors = $null
$launcherAst = [System.Management.Automation.Language.Parser]::ParseFile(
  (Resolve-Path -LiteralPath $launcherPath).Path,
  [ref]$tokens,
  [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
  throw (
    "Launcher has PowerShell parser errors: " +
    (($parseErrors | ForEach-Object Message) -join " | ")
  )
}

function Get-LauncherFunctionDefinition([string]$Name) {
  $matches = @($launcherAst.FindAll(
    {
      param($node)
      $node -is
        [System.Management.Automation.Language.FunctionDefinitionAst] -and
      $node.Name -eq $Name
    },
    $true
  ))
  if ($matches.Count -ne 1) {
    throw "Expected exactly one launcher function named $Name."
  }
  return [scriptblock]::Create($matches[0].Extent.Text)
}

. (Get-LauncherFunctionDefinition "Test-CanonicalPathEqual")
. (Get-LauncherFunctionDefinition "Test-PathEqualOrWithinRoot")
. (Get-LauncherFunctionDefinition "Get-PeMachine")
. (Get-LauncherFunctionDefinition "Test-MetaXrOperatorRuntimeKind")
. (Get-LauncherFunctionDefinition "Get-MetaXrOperatorInfo")
. (Get-LauncherFunctionDefinition "Get-Sha256")
. (Get-LauncherFunctionDefinition "Assert-SquareCommandLineTemplate")
. (Get-LauncherFunctionDefinition "Install-SquareCommandLine")
. (Get-LauncherFunctionDefinition "Save-State")
. (Get-LauncherFunctionDefinition "Test-StateRestored")
. (Get-LauncherFunctionDefinition "Restore-State")
. (Get-LauncherFunctionDefinition "Test-SquareCommandLineRequired")
. (Get-LauncherFunctionDefinition "Test-Mode204ConfigRequired")
. (Get-LauncherFunctionDefinition "Get-Mode204ConfigPreset")
. (Get-LauncherFunctionDefinition "Enter-Mode204ConfigScope")
. (Get-LauncherFunctionDefinition "Enter-SquareCommandLineScope")
. (Get-LauncherFunctionDefinition "Get-OpenXrHostExitClassification")

$rockstarRoot = "C:\Program Files\Rockstar Games"
$rootCases = @(
  [pscustomobject]@{
    Name = "equal root"
    Path = "C:\Program Files\Rockstar Games"
    Expected = $true
  }
  [pscustomobject]@{
    Name = "direct child"
    Path = "C:\Program Files\Rockstar Games\Launcher\Launcher.exe"
    Expected = $true
  }
  [pscustomobject]@{
    Name = "case-insensitive child"
    Path = "c:\program files\ROCKSTAR GAMES\Social Club\Helper.exe"
    Expected = $true
  }
  [pscustomobject]@{
    Name = "sibling prefix"
    Path = "C:\Program Files\Rockstar Games Backup\Launcher.exe"
    Expected = $false
  }
  [pscustomobject]@{
    Name = "numeric prefix"
    Path = "C:\Program Files\Rockstar Games2\Launcher.exe"
    Expected = $false
  }
  [pscustomobject]@{
    Name = "normalized sibling"
    Path = "C:\Program Files\Rockstar Games\..\Rockstar Games Backup\Launcher.exe"
    Expected = $false
  }
)
foreach ($case in $rootCases) {
  $actual = Test-PathEqualOrWithinRoot $case.Path $rockstarRoot
  if ($actual -ne $case.Expected) {
    throw (
      "Rockstar root case '$($case.Name)' expected " +
      "$($case.Expected), got $actual."
    )
  }
}

$expectedPlayGta =
  "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV\PlayGTAIV.exe"
if (-not (Test-CanonicalPathEqual `
    $expectedPlayGta `
    "d:\steamlibrary\steamapps\common\Grand Theft Auto IV\GTAIV\PlayGTAIV.exe")) {
  throw "Exact PlayGTAIV path comparison is not case-insensitive."
}
if (Test-CanonicalPathEqual `
    $expectedPlayGta `
    "D:\Other Game\PlayGTAIV.exe") {
  throw "Exact PlayGTAIV path comparison accepted an unrelated executable."
}

$transactionRoot = Join-Path (
  [System.IO.Path]::GetTempPath()
) ("gtaiv-openxr-launcher-" + [Guid]::NewGuid().ToString("N"))
$trackedSquarePreset = (
  Resolve-Path (
    Join-Path $PSScriptRoot "..\config\commandline.vr-square.txt"
  )
).Path
New-Item -ItemType Directory -Path $transactionRoot | Out-Null
try {
  $existingCase = Join-Path $transactionRoot "existing"
  $script:BackupDirectory = Join-Path $existingCase "backup"
  New-Item -ItemType Directory -Path $script:BackupDirectory -Force |
    Out-Null
  $activePath = Join-Path $existingCase "commandline.txt"
  Set-Content `
    -LiteralPath $activePath `
    -Value "-existing-user-option" `
    -Encoding ASCII `
    -NoNewline
  $originalHash = Get-Sha256 $activePath
  $mode58Scope = Enter-SquareCommandLineScope `
    -Mode 58 `
    -TemplatePath $trackedSquarePreset `
    -ActivePath $activePath
  if (-not $mode58Scope.Required -or
      $null -eq $mode58Scope.State -or
      $mode58Scope.TemplateHash -ne $mode58Scope.InstalledHash -or
      (Get-Sha256 $activePath) -ne $mode58Scope.TemplateHash) {
    throw "Square command line was not installed byte-for-byte."
  }
  Restore-State $mode58Scope.State
  if (-not (Test-StateRestored $mode58Scope.State) -or
      (Get-Sha256 $activePath) -ne $originalHash) {
    throw "Existing commandline.txt was not restored byte-for-byte."
  }

  $missingCase = Join-Path $transactionRoot "missing"
  $script:BackupDirectory = Join-Path $missingCase "backup"
  New-Item -ItemType Directory -Path $script:BackupDirectory -Force |
    Out-Null
  $missingActivePath = Join-Path $missingCase "commandline.txt"
  $missingMode204Scope = Enter-SquareCommandLineScope `
    -Mode 204 `
    -TemplatePath $trackedSquarePreset `
    -ActivePath $missingActivePath
  if (-not $missingMode204Scope.Required -or
      $null -eq $missingMode204Scope.State) {
    throw "Mode204 did not enter the square command-line scope."
  }
  Restore-State $missingMode204Scope.State
  if (-not (Test-StateRestored $missingMode204Scope.State) -or
      (Test-Path -LiteralPath $missingActivePath)) {
    throw "Originally absent commandline.txt was not restored to absence."
  }

  $nonSquareCase = Join-Path $transactionRoot "non-square"
  $script:BackupDirectory = Join-Path $nonSquareCase "backup"
  New-Item -ItemType Directory -Path $script:BackupDirectory -Force |
    Out-Null
  $nonSquareActive = Join-Path $nonSquareCase "commandline.txt"
  $missingPreset = Join-Path $nonSquareCase "does-not-exist.txt"
  Set-Content `
    -LiteralPath $nonSquareActive `
    -Value "-existing-user-option" `
    -Encoding ASCII `
    -NoNewline
  $nonSquareOriginalHash = Get-Sha256 $nonSquareActive
  foreach ($mode in @(55, 56, 57)) {
    $scope = Enter-SquareCommandLineScope `
      -Mode $mode `
      -TemplatePath $missingPreset `
      -ActivePath $nonSquareActive
    if ($scope.Required -or
        $null -ne $scope.State -or
        (Get-Sha256 $nonSquareActive) -ne $nonSquareOriginalHash -or
        (Test-Path -LiteralPath $missingPreset)) {
      throw (
        "Mode $mode required the square preset or mutated commandline.txt."
      )
    }
  }

  $mode204Case = Join-Path $transactionRoot "mode204-config"
  $script:BackupDirectory = Join-Path $mode204Case "backup"
  New-Item -ItemType Directory -Path $script:BackupDirectory -Force |
    Out-Null
  $expectedMode204Preset = [ordered]@{
    "gtaiv_dxvk_vr.stereo" = "204"
    "gtaiv_dxvk_vr.vres" = "2048"
    "gtaiv_dxvk_vr.eyefwd" = "0"
    "gtaiv_dxvk_vr.fovadd" = "0"
    "gtaiv_dxvk_vr.fpfov" = "110 110 110"
    "gtaiv_dxvk_vr.camoff" = "0 0 0"
    "gtaiv_dxvk_vr.vehcamoff" = "0 -12 0"
    "gtaiv_dxvk_vr.mousesens" = "50"
    "gtaiv_dxvk_vr.joysens" = "20 30"
    "gtaiv_dxvk_vr.dualn" = "2"
    "gtaiv_dxvk_vr.ipd" = "2"
    "gtaiv_dxvk_vr.pedhide" = "0"
    "gtaiv_dxvk_vr.stereoscale" = "130"
    "gtaiv_dxvk_vr.scale" = "100"
    "gtaiv_dxvk_vr.hudoff" = "center 1.0 -0.10 0.10"
  }
  $actualMode204Preset = Get-Mode204ConfigPreset
  if ($actualMode204Preset.Count -ne $expectedMode204Preset.Count) {
    throw "Mode204 preset file count differs from pack-mode204-zip."
  }
  foreach ($entry in $expectedMode204Preset.GetEnumerator()) {
    if (-not $actualMode204Preset.Contains($entry.Key) -or
        $actualMode204Preset[$entry.Key] -cne $entry.Value) {
      throw (
        "Mode204 preset mismatch for $($entry.Key): " +
        "expected='$($entry.Value)' actual=" +
        "'$($actualMode204Preset[$entry.Key])'"
      )
    }
  }
  $mode204OriginalFiles = [ordered]@{
    "gtaiv_dxvk_vr.stereo" = "user-stereo"
    "gtaiv_dxvk_vr.fovadd" = "user-fov"
  }
  foreach ($entry in $mode204OriginalFiles.GetEnumerator()) {
    Set-Content `
      -LiteralPath (Join-Path $mode204Case $entry.Key) `
      -Value $entry.Value `
      -Encoding ASCII `
      -NoNewline
  }
  $mode204Scope = Enter-Mode204ConfigScope `
    -Mode 204 `
    -Directory $mode204Case
  if (-not $mode204Scope.Required -or
      $mode204Scope.States.Count -ne $expectedMode204Preset.Count) {
    throw "Mode204 did not transactionally stage every exact preset file."
  }
  foreach ($entry in $expectedMode204Preset.GetEnumerator()) {
    $installedValue = [System.IO.File]::ReadAllText(
      (Join-Path $mode204Case $entry.Key)
    )
    if ($installedValue -cne $entry.Value) {
      throw "Mode204 staged value mismatch for $($entry.Key)."
    }
  }
  foreach ($state in $mode204Scope.States) {
    Restore-State $state
  }
  foreach ($entry in $expectedMode204Preset.GetEnumerator()) {
    $restoredPath = Join-Path $mode204Case $entry.Key
    if ($mode204OriginalFiles.Contains($entry.Key)) {
      if (-not (Test-Path -LiteralPath $restoredPath -PathType Leaf) -or
          [System.IO.File]::ReadAllText($restoredPath) -cne
          $mode204OriginalFiles[$entry.Key]) {
        throw "Mode204 did not restore existing $($entry.Key) exactly."
      }
    }
    elseif (Test-Path -LiteralPath $restoredPath) {
      throw "Mode204 did not restore absent $($entry.Key) to absence."
    }
  }

  $invalidCase = Join-Path $transactionRoot "invalid"
  $script:BackupDirectory = Join-Path $invalidCase "backup"
  New-Item -ItemType Directory -Path $script:BackupDirectory -Force |
    Out-Null
  $invalidTemplate = Join-Path $invalidCase "bad-preset.txt"
  $invalidActive = Join-Path $invalidCase "commandline.txt"
  Set-Content `
    -LiteralPath $invalidTemplate `
    -Value "-windowed -width 1920 -height 1080 -norestrictions" `
    -Encoding ASCII `
    -NoNewline
  Set-Content `
    -LiteralPath $invalidActive `
    -Value "-existing-user-option" `
    -Encoding ASCII `
    -NoNewline
  $invalidOriginalHash = Get-Sha256 $invalidActive
  $invalidRejected = $false
  try {
    [void](Enter-SquareCommandLineScope `
      -Mode 58 `
      -TemplatePath $invalidTemplate `
      -ActivePath $invalidActive)
  }
  catch {
    $invalidRejected = $true
  }
  if (-not $invalidRejected -or
      (Get-Sha256 $invalidActive) -ne $invalidOriginalHash) {
    throw "Invalid square preset was not rejected before active-file mutation."
  }
}
finally {
  if (Test-Path -LiteralPath $transactionRoot -PathType Container) {
    [System.IO.Directory]::Delete($transactionRoot, $true)
  }
}

$operatorRuntimeCases = @(
  [pscustomobject]@{
    Kind = "MetaSimulator"
    Expected = $true
  }
  [pscustomobject]@{
    Kind = "MetaQuestLink"
    Expected = $true
  }
  [pscustomobject]@{
    Kind = "ElliottSimulator"
    Expected = $false
  }
  [pscustomobject]@{
    Kind = "SteamVr"
    Expected = $false
  }
  [pscustomobject]@{
    Kind = "Other"
    Expected = $false
  }
)
foreach ($case in $operatorRuntimeCases) {
  $actual = Test-MetaXrOperatorRuntimeKind $case.Kind
  if ($actual -ne $case.Expected) {
    throw (
      "Meta XR Operator runtime '$($case.Kind)' expected " +
      "$($case.Expected), got $actual."
    )
  }
}

function New-SyntheticPe([string]$Path, [uint16]$Machine) {
  $bytes = [byte[]]::new(512)
  $bytes[0] = 0x4d
  $bytes[1] = 0x5a
  [BitConverter]::GetBytes([int32]0x80).CopyTo($bytes, 0x3c)
  $bytes[0x80] = 0x50
  $bytes[0x81] = 0x45
  [BitConverter]::GetBytes($Machine).CopyTo($bytes, 0x84)
  [IO.File]::WriteAllBytes($Path, $bytes)
}

function Write-SyntheticOperatorManifest(
  [string]$Path,
  [string]$LayerName,
  [string]$LibraryPath
) {
  $manifest = [ordered]@{
    file_format_version = "1.0.0"
    api_layer = [ordered]@{
      name = $LayerName
      library_path = $LibraryPath
      api_version = "1.1"
      implementation_version = "1"
      description = "offline contract fixture"
    }
  }
  Set-Content `
    -LiteralPath $Path `
    -Value ($manifest | ConvertTo-Json -Depth 4) `
    -Encoding UTF8
}

function Assert-OperatorFixtureRejected(
  [scriptblock]$Action,
  [string]$CaseName
) {
  $rejected = $false
  try {
    [void](& $Action)
  }
  catch {
    $rejected = $true
  }
  if (-not $rejected) {
    throw "Meta XR Operator fixture was not rejected: $CaseName"
  }
}

$operatorRoot = Join-Path (
  [System.IO.Path]::GetTempPath()
) ("gtaiv-meta-xr-operator-" + [Guid]::NewGuid().ToString("N"))
$operatorDirectory = Join-Path $operatorRoot "windows"
$operatorLayer = Join-Path $operatorDirectory "operator-layer.dll"
$operatorProxy =
  Join-Path $operatorDirectory "meta-xr-operator-mcp-proxy.exe"
$operatorManifest =
  Join-Path $operatorDirectory "arbitrary-layer-manifest.json"
$operatorDecoy = Join-Path $operatorDirectory "name-is-not-authority.json"
$operatorRejectedCases = 0
New-Item -ItemType Directory -Path $operatorDirectory -Force | Out-Null
try {
  New-SyntheticPe -Path $operatorLayer -Machine 0x8664
  New-SyntheticPe -Path $operatorProxy -Machine 0x8664
  Write-SyntheticOperatorManifest `
    -Path $operatorManifest `
    -LayerName "XR_APILAYER_METAX_operator" `
    -LibraryPath ".\operator-layer.dll"
  Write-SyntheticOperatorManifest `
    -Path $operatorDecoy `
    -LayerName "XR_APILAYER_NOT_operator" `
    -LibraryPath ".\missing-decoy.dll"

  $operatorInfo =
    Get-MetaXrOperatorInfo -Directory $operatorDirectory
  if ($operatorInfo.LayerName -cne "XR_APILAYER_METAX_operator" -or
      -not (Test-CanonicalPathEqual `
        $operatorInfo.ManifestPath `
        $operatorManifest) -or
      -not (Test-CanonicalPathEqual `
        $operatorInfo.LibraryPath `
        $operatorLayer) -or
      -not (Test-CanonicalPathEqual `
        $operatorInfo.ProxyPath `
        $operatorProxy)) {
    throw (
      "Meta XR Operator valid fixture was not resolved by declared layer name."
    )
  }

  [IO.File]::Delete($operatorProxy)
  Assert-OperatorFixtureRejected `
    -CaseName "missing proxy" `
    -Action { Get-MetaXrOperatorInfo -Directory $operatorDirectory }
  ++$operatorRejectedCases
  New-SyntheticPe -Path $operatorProxy -Machine 0x8664

  New-SyntheticPe -Path $operatorProxy -Machine 0x014c
  Assert-OperatorFixtureRejected `
    -CaseName "x86 proxy" `
    -Action { Get-MetaXrOperatorInfo -Directory $operatorDirectory }
  ++$operatorRejectedCases
  New-SyntheticPe -Path $operatorProxy -Machine 0x8664

  New-SyntheticPe -Path $operatorLayer -Machine 0x014c
  Assert-OperatorFixtureRejected `
    -CaseName "x86 layer" `
    -Action { Get-MetaXrOperatorInfo -Directory $operatorDirectory }
  ++$operatorRejectedCases
  New-SyntheticPe -Path $operatorLayer -Machine 0x8664

  [IO.File]::Delete($operatorLayer)
  Assert-OperatorFixtureRejected `
    -CaseName "missing layer" `
    -Action { Get-MetaXrOperatorInfo -Directory $operatorDirectory }
  ++$operatorRejectedCases
  New-SyntheticPe -Path $operatorLayer -Machine 0x8664

  Write-SyntheticOperatorManifest `
    -Path $operatorManifest `
    -LayerName "XR_APILAYER_WRONG_operator" `
    -LibraryPath ".\operator-layer.dll"
  Assert-OperatorFixtureRejected `
    -CaseName "wrong declared layer name" `
    -Action { Get-MetaXrOperatorInfo -Directory $operatorDirectory }
  ++$operatorRejectedCases
  Write-SyntheticOperatorManifest `
    -Path $operatorManifest `
    -LayerName "XR_APILAYER_METAX_operator" `
    -LibraryPath ".\operator-layer.dll"

  $duplicateManifest = Join-Path $operatorDirectory "duplicate.json"
  Write-SyntheticOperatorManifest `
    -Path $duplicateManifest `
    -LayerName "XR_APILAYER_METAX_operator" `
    -LibraryPath ".\operator-layer.dll"
  Assert-OperatorFixtureRejected `
    -CaseName "ambiguous matching manifests" `
    -Action { Get-MetaXrOperatorInfo -Directory $operatorDirectory }
  ++$operatorRejectedCases
  [IO.File]::Delete($duplicateManifest)

  $outsideLayer = Join-Path $operatorRoot "outside-layer.dll"
  New-SyntheticPe -Path $outsideLayer -Machine 0x8664
  Write-SyntheticOperatorManifest `
    -Path $operatorManifest `
    -LayerName "XR_APILAYER_METAX_operator" `
    -LibraryPath "..\outside-layer.dll"
  Assert-OperatorFixtureRejected `
    -CaseName "layer outside manifest directory" `
    -Action { Get-MetaXrOperatorInfo -Directory $operatorDirectory }
  ++$operatorRejectedCases
}
finally {
  if (Test-Path -LiteralPath $operatorRoot -PathType Container) {
    [System.IO.Directory]::Delete($operatorRoot, $true)
  }
}

$hostExitCases = @(
  [pscustomobject]@{
    Name = "ordered runtime exit"
    Text = @"
XRHost: session STOPPING
XRHost: session EXITING
XRHost: clean shutdown requested
"@
    Expected = "ORDERLY_RUNTIME_EXITING"
  }
  [pscustomobject]@{
    Name = "fatal takes precedence"
    Text = @"
FATAL: xrEndFrame failed
XRHost: session EXITING
XRHost: clean shutdown requested
"@
    Expected = "FATAL"
  }
  [pscustomobject]@{
    Name = "reversed shutdown evidence"
    Text = @"
XRHost: clean shutdown requested
XRHost: session EXITING
"@
    Expected = "INCOMPLETE_RUNTIME_EXIT"
  }
  [pscustomobject]@{
    Name = "exit without clean marker"
    Text = "XRHost: session EXITING"
    Expected = "INCOMPLETE_RUNTIME_EXIT"
  }
  [pscustomobject]@{
    Name = "unexpected process exit"
    Text = "XRHost: projection submit ok frame=120"
    Expected = "UNEXPECTED"
  }
  [pscustomobject]@{
    Name = "no evidence"
    Text = ""
    Expected = "NO_EVIDENCE"
  }
)
foreach ($case in $hostExitCases) {
  $classification = Get-OpenXrHostExitClassification $case.Text
  if ($classification -ne $case.Expected) {
    throw (
      "Host exit case '$($case.Name)' expected $($case.Expected), " +
      "got $classification."
    )
  }
}

$requiredPatterns = [ordered]@{
  PlayGtaPath =
    '\$PlayGtaExe\s*=\s*Join-Path \$GameDir "PlayGTAIV\.exe"'
  RetryExactPath =
    '(?s)Stop-ByExactPath.{0,180}"stalled Rockstar handoff retry"'
  ExactStopPredicate =
    'Test-CanonicalPathEqual \$processPath \$ExpectedPath'
  RockstarBoundaryHelper =
    'Test-PathEqualOrWithinRoot \$processPath \$rockstarRoot'
  RockstarFinalCleanup =
    'Stop-StaleRockstarLauncherSession "end supervised GTA run"'
  CleanupFailureResult =
    '"cleanupFailures=\$\(\$cleanupFailures -join '' \| ''\)"'
  ArchiveFailureResult =
    '"archiveFailures=\$\(\$archiveFailures -join '' \| ''\)"'
  BestEffortRestoreStatus =
    'Write-StatusBestEffort \(\s*"RESTORE WARNING:'
  OpenXrBackendDeploy =
    'Set-Content -LiteralPath \$BackendFile -Value "openxr"'
  OpenVrDisableSentinel =
    'Set-Content -LiteralPath \$DisableFile -Value "1"'
  OpenVrDllRemoval =
    'Remove-Item -LiteralPath \$InstalledOpenVr -Force'
  SteamLaunchOptionsEmpty =
    '\-not \[string\]::IsNullOrWhiteSpace\(\$launchOptions\)'
  ValidatedRuntimePinned =
    '\$env:XR_RUNTIME_JSON\s*=\s*\$runtime'
  MetaOperatorDisabledByDefault =
    '\[string\]\$MetaXrOperatorDirectory\s*=\s*""'
  MetaOperatorDeclaredLayerName =
    '\$layerName\s*=\s*"XR_APILAYER_METAX_operator"'
  MetaOperatorMetaRuntimeOnly =
    'return \$Kind -in @\("MetaSimulator", "MetaQuestLink"\)'
  MetaOperatorPortPreflight =
    'Assert-MetaXrOperatorPortAvailable -Port 8720'
  MetaOperatorChildApiLayerPath =
    '\$env:XR_API_LAYER_PATH\s*=\s*\$metaXrOperatorInfo\.Directory'
  MetaOperatorChildEnabledLayer =
    '\$env:XR_ENABLE_API_LAYERS\s*=\s*\$metaXrOperatorInfo\.LayerName'
  MetaOperatorApiLayerPathRestore =
    '\$env:XR_API_LAYER_PATH\s*=\s*\$priorMetaXrApiLayerPath'
  MetaOperatorEnabledLayerRestore =
    '\$env:XR_ENABLE_API_LAYERS\s*=\s*\$priorMetaXrEnabledApiLayers'
  MetaOperatorProxyAgentOwned =
    'proxy remains agent-owned and is never launched by this script'
  NoContinuousSteamVrStatus =
    'no continuous SteamVR process polling'
  TrackedSquarePreset =
    'Join-Path \$Root "config\\commandline\.vr-square\.txt"'
  ConditionalSquarePreflight =
    '(?s)if \(\$squareCommandLineRequired\)\s*\{\s*\$requiredFiles \+= \$SquareCommandLineTemplate'
  ModeScopedSquareEntry =
    '(?s)Enter-SquareCommandLineScope.{0,100}-Mode \$StereoMode'
  ActiveCommandLineSnapshot =
    'Save-State \$ActivePath "commandline\.txt"'
  ActiveSquareInstall =
    '(?s)Install-SquareCommandLine.{0,160}-ActivePath \$ActivePath'
  ActiveSquareArchive =
    'Join-Path \$RunDirectory "active-commandline\.txt"'
  ActiveCommandLineRestoreResult =
    '"activeCommandLineRestored=\$activeCommandLineRestored"'
  SquareCommandLineRequiredResult =
    '"squareCommandLineRequired=\$squareCommandLineRequired"'
  NonMode58CommandLineUntouched =
    'commandline\.txt untouched'
  Mode204RequiresSquare =
    'function Test-SquareCommandLineRequired\(\[uint32\]\$Mode\)\s*\{\s*return \$Mode -in @\(58, 204\)'
  Mode204TransactionalScope =
    'Enter-Mode204ConfigScope\s+`\s+-Mode \$StereoMode'
  Mode204InstalledResult =
    '"mode204ConfigInstalled=\$mode204ConfigInstalled"'
  Mode204RestoredResult =
    '"mode204ConfigRestored=\$mode204ConfigRestored"'
  Mode204BuildIdUntouched =
    'buildid/user extras untouched'
  OrderedRuntimeExitOutcome =
    '\$outcome = "OPENXR_RUNTIME_EXITING"'
  OrderedRuntimeExitResult =
    '"hostExitClassification=\$hostExitClassification"'
}
foreach ($entry in $requiredPatterns.GetEnumerator()) {
  if ($launcher -notmatch $entry.Value) {
    throw "Missing launcher safety contract: $($entry.Key)"
  }
}

$forbiddenPatterns = [ordered]@{
  NameOnlyPlayGtaStop =
    '(?s)Stop-ByName\s+@\([^\)]*"PlayGTAIV"'
  RockstarSiblingPrefix =
    '(?s)GetFullPath\(\$processPath\)\.StartsWith\(\s*\$rockstarRoot'
  OldContinuousSteamVrStatus =
    'SteamVR polled every 100 ms'
  DashboardLaunchUri =
    'vrmonitor://'
  RestartScriptLaunch =
    'restart-gtaiv'
  OperatorProxyProcessLaunch =
    '(?s)Start-Process.{0,300}meta-xr-operator-mcp-proxy'
}
foreach ($entry in $forbiddenPatterns.GetEnumerator()) {
  if ($launcher -match $entry.Value) {
    throw "Forbidden launcher safety contract found: $($entry.Key)"
  }
}

$squareScopeGuards = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.IfStatementAst] -and
    $node.Extent.Text -match 'if \(\$squareScope\.Required\)' -and
    $node.Extent.Text -match 'active-commandline\.txt' -and
    $node.Extent.Text -match '\$states \+= \$activeCommandLineState'
  },
  $true
))
if ($squareScopeGuards.Count -ne 1) {
  throw (
    "Expected one square-scope guard around state registration and " +
    "active-commandline archival; found $($squareScopeGuards.Count)."
  )
}

$tryStatements = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.TryStatementAst]
  },
  $true
))
$restorationBarriers = @($tryStatements | Where-Object {
  $_.Finally -and
  $_.Body.Extent.Text -match '\$diagnostics\s*=\s*@\(' -and
  $_.Body.Extent.Text -match 'Copy-Item' -and
  $_.Finally.Extent.Text -match 'foreach \(\$state in \$states\)' -and
  $_.Finally.Extent.Text -match 'Restore-State \$state'
})
if ($restorationBarriers.Count -ne 1) {
  throw (
    "Expected one diagnostic-cleanup try/finally restoration barrier; " +
    "found $($restorationBarriers.Count)."
  )
}
$barrier = $restorationBarriers[0]
if ($barrier.CatchClauses.Count -eq 0) {
  throw "Restoration barrier does not absorb unexpected cleanup failures."
}
if ($barrier.Finally.Extent.Text -notmatch 'Write-StatusBestEffort') {
  throw "Restoration loop can still be interrupted by a status-log failure."
}

$steamVrBoundaryName = "Assert-And-Stop-SteamVrAtLaunchBoundary"
$steamVrBoundaryCalls = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and
    $node.GetCommandName() -eq $steamVrBoundaryName
  },
  $true
))
if ($steamVrBoundaryCalls.Count -ne 3) {
  throw (
    "Expected exactly three one-time SteamVR launch-boundary checks; found " +
    "$($steamVrBoundaryCalls.Count)."
  )
}

$steamVrPollingLoops = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.WhileStatementAst] -and
    $node.Extent.Text -match [regex]::Escape($steamVrBoundaryName)
  },
  $true
))
if ($steamVrPollingLoops.Count -ne 0) {
  throw (
    "SteamVR process checks must not run inside a watchdog/polling loop; found " +
    "$($steamVrPollingLoops.Count)."
  )
}

$operatorPortCheckName = "Assert-MetaXrOperatorPortAvailable"
$operatorPortChecks = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and
    $node.GetCommandName() -eq $operatorPortCheckName
  },
  $true
))
if ($operatorPortChecks.Count -ne 1 -or
    $operatorPortChecks[0].Extent.Text -notmatch '\-Port\s+8720') {
  throw (
    "Expected exactly one Meta XR Operator port-8720 preflight check; " +
    "found $($operatorPortChecks.Count)."
  )
}
$operatorPortPollingLoops = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.WhileStatementAst] -and
    $node.Extent.Text -match [regex]::Escape($operatorPortCheckName)
  },
  $true
))
if ($operatorPortPollingLoops.Count -ne 0) {
  throw "Meta XR Operator port checks must not run in a polling loop."
}

$startProcessCommands = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and
    $node.GetCommandName() -eq "Start-Process"
  },
  $true
))
$hostStartCommands = @($startProcessCommands | Where-Object {
  $_.Extent.Text -match '-FilePath\s+\$HostExe\b'
})
$steamStartCommands = @($startProcessCommands | Where-Object {
  $_.Extent.Text -match '-FilePath\s+\$SteamExe\b'
})
if ($startProcessCommands.Count -ne 2 -or
    $hostStartCommands.Count -ne 1 -or
    $steamStartCommands.Count -ne 1) {
  throw (
    "Launcher process-start surface changed: total=$($startProcessCommands.Count) " +
    "host=$($hostStartCommands.Count) steam=$($steamStartCommands.Count)."
  )
}
foreach ($processStart in $startProcessCommands) {
  if ($processStart.Extent.Text -match
      '(?i)(meta-xr-operator|operator-mcp-proxy)') {
    throw "The launcher attempted to start the Meta XR Operator MCP proxy."
  }
}

$hostEnvironmentScopes = @($tryStatements | Where-Object {
  $_.Body.Extent.Text -match
    '\$env:XR_RUNTIME_JSON\s*=\s*\$runtime' -and
  $_.Body.Extent.Text -match
    '\$env:XR_API_LAYER_PATH\s*=\s*\$metaXrOperatorInfo\.Directory' -and
  $_.Body.Extent.Text -match
    '\$env:XR_ENABLE_API_LAYERS\s*=\s*\$metaXrOperatorInfo\.LayerName' -and
  $_.Body.Extent.Text -match
    '(?s)Start-Process.{0,300}-FilePath\s+\$HostExe\b' -and
  $_.Finally.Extent.Text -match
    '\$priorMetaXrApiLayerPath' -and
  $_.Finally.Extent.Text -match
    '\$priorMetaXrEnabledApiLayers'
})
if ($hostEnvironmentScopes.Count -ne 1) {
  throw (
    "Meta XR Operator variables are not scoped to the one existing x64 " +
    "host launch and its immediate restoration barrier."
  )
}

$apiLayerPathAssignments = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left.Extent.Text -eq '$env:XR_API_LAYER_PATH'
  },
  $true
))
$enabledLayerAssignments = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left.Extent.Text -eq '$env:XR_ENABLE_API_LAYERS'
  },
  $true
))
if ($apiLayerPathAssignments.Count -ne 2 -or
    $enabledLayerAssignments.Count -ne 2) {
  throw (
    "Meta XR Operator environment assignment surface changed: path=" +
    "$($apiLayerPathAssignments.Count) enabled=" +
    "$($enabledLayerAssignments.Count)."
  )
}
foreach ($steamStart in $steamStartCommands) {
  if ($steamStart.Extent.Text -notmatch
      '(?s)-ArgumentList\s+@\(\s*"-applaunch"\s*,\s*"12210"\s*\)') {
    throw "Steam launch is not the exact argument-free GTA IV app 12210 route."
  }
  if ($steamStart.Extent.Text -match
      '(?i)(vrmonitor|steamvr|openvr|vrmode|\-vr\b)') {
    throw "Steam launch contains a forbidden VR argument or target."
  }
}

$steamAuthFunctionName = "Start-AuditedGtaSteamAuthentication"
$steamAuthFunctions = @($launcherAst.FindAll(
  {
    param($node)
    $node -is
      [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq $steamAuthFunctionName
  },
  $true
))
if ($steamAuthFunctions.Count -ne 1 -or
    $steamAuthFunctions[0].Extent.Text -notmatch
      '(?s)Assert-And-Stop-SteamVrAtLaunchBoundary.*Start-Process') {
  throw "Steam authentication is not isolated behind one audited launch helper."
}
$steamAuthCalls = @($launcherAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and
    $node.GetCommandName() -eq $steamAuthFunctionName
  },
  $true
))
if ($steamAuthCalls.Count -ne 2) {
  throw (
    "Expected initial authentication plus one audited retry call site; found " +
    "$($steamAuthCalls.Count)."
  )
}

Write-Output (
  "OpenXrLauncherSafetyContractTest: PASS " +
  "rootCases=$($rootCases.Count) exactPath=2 " +
  "squareCommandLineCases=3 nonSquareCases=3 mode204ConfigCases=15 " +
  "hostExitCases=$($hostExitCases.Count) " +
  "required=$($requiredPatterns.Count) forbidden=$($forbiddenPatterns.Count) " +
  "squareScopeGuard=1 restorationBarrier=1 " +
  "boundaryChecks=$($steamVrBoundaryCalls.Count) " +
  "pollingLoops=0 operatorRuntimeCases=$($operatorRuntimeCases.Count) " +
  "operatorRejectedCases=$operatorRejectedCases operatorPortChecks=1 " +
  "operatorHostScope=1 processStarts=$($startProcessCommands.Count) " +
  "steamAuthCalls=$($steamAuthCalls.Count)"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$clientPath =
  Join-Path $PSScriptRoot "..\scripts\invoke-meta-xr-operator.ps1"
$client = Get-Content -LiteralPath $clientPath -Raw -ErrorAction Stop
$tokens = $null
$parseErrors = $null
$clientAst = [System.Management.Automation.Language.Parser]::ParseFile(
  $clientPath,
  [ref]$tokens,
  [ref]$parseErrors
)
if ($parseErrors.Count -ne 0) {
  throw (
    "Meta XR Operator client has PowerShell parse errors: " +
    (($parseErrors | ForEach-Object Message) -join " | ")
  )
}

$requiredPatterns = [ordered]@{
  MandatoryToolName =
    '(?s)\[Parameter\(Mandatory\)\].{0,120}\[string\]\$ToolName'
  JsonArguments =
    '\[string\]\$ArgumentsJson\s*=\s*"\{\}"'
  BoundedTimeout =
    '(?s)\[ValidateRange\(5,\s*120\)\].{0,80}\$TimeoutSeconds'
  ExactProxyBinary =
    'Join-Path \$proxyDirectoryResolved "meta-xr-operator-mcp-proxy\.exe"'
  MpcInitialize =
    'method\s*=\s*"initialize"'
  InitializedNotification =
    'method\s*=\s*"notifications/initialized"'
  ToolCall =
    'method\s*=\s*"tools/call"'
  RequestedToolForwarded =
    'name\s*=\s*\$ToolName'
  RequestedArgumentsForwarded =
    'arguments\s*=\s*\$arguments'
  NoShellExecute =
    '\$startInfo\.UseShellExecute\s*=\s*\$false'
  HiddenProcess =
    '\$startInfo\.CreateNoWindow\s*=\s*\$true'
  TimeoutKill =
    '(?s)WaitForExit\(\[int\]\$TimeoutSeconds \* 1000\).{0,180}\$process\.Kill\(\)'
  ToolErrorRejected =
    'Meta XR Operator tool error'
  ImageIsBase64 =
    '\[Convert\]::FromBase64String'
  ImageHashReceipt =
    'Get-FileHash -LiteralPath \$imagePath -Algorithm SHA256'
}
foreach ($entry in $requiredPatterns.GetEnumerator()) {
  if ($client -notmatch $entry.Value) {
    throw "Missing Meta XR Operator client contract: $($entry.Key)"
  }
}

$processStarts = @($clientAst.FindAll(
  {
    param($node)
    $node -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and
    $node.Member.Extent.Text -eq "Start"
  },
  $true
))
if ($processStarts.Count -ne 1 -or
    $processStarts[0].Expression.Extent.Text -ne '$process') {
  throw (
    "Meta XR Operator client process-start surface changed: expected one " +
    "start of the validated proxy process, found $($processStarts.Count)."
  )
}

$forbiddenPatterns = [ordered]@{
  Steam = '(?i)(steam\.exe|steamvr|vrmonitor|vrserver|vrcompositor)'
  GtaLaunch = '(?i)(GTAIV\.exe|PlayGTAIV\.exe|\-applaunch)'
  SimulatorLaunch =
    '(?i)(MetaXRSimulator|SIMULATOR\.dll|openxr_simulator\.exe)'
  RuntimeMutation = '(?i)XR_RUNTIME_JSON|ActiveRuntime'
  ApiLayerMutation = '(?i)XR_API_LAYER_PATH|XR_ENABLE_API_LAYERS'
}
foreach ($entry in $forbiddenPatterns.GetEnumerator()) {
  if ($client -match $entry.Value) {
    throw "Forbidden Meta XR Operator client contract found: $($entry.Key)"
  }
}

Write-Output (
  "MetaXrOperatorClientContractTest: PASS " +
  "required=$($requiredPatterns.Count) processStarts=1 forbidden=5"
)

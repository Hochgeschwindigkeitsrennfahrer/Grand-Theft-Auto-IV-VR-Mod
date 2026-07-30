[CmdletBinding()]
param(
  [Parameter(Mandatory)]
  [ValidateNotNullOrEmpty()]
  [string]$ToolName,

  [string]$ArgumentsJson = "{}",

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

  [ValidateRange(5, 120)]
  [uint32]$TimeoutSeconds = 45,

  [string]$ImageOutputPath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$proxyDirectoryResolved =
  (Resolve-Path -LiteralPath $ProxyDirectory).Path
$proxyPath =
  Join-Path $proxyDirectoryResolved "meta-xr-operator-mcp-proxy.exe"
if (-not (Test-Path -LiteralPath $proxyPath -PathType Leaf)) {
  throw "Meta XR Operator MCP proxy is missing: $proxyPath"
}

try {
  $arguments = $ArgumentsJson | ConvertFrom-Json -ErrorAction Stop
}
catch {
  throw "ArgumentsJson is not valid JSON: $($_.Exception.Message)"
}
if ($null -eq $arguments) {
  $arguments = [pscustomobject]@{}
}

$initialize = [ordered]@{
  jsonrpc = "2.0"
  id = 1
  method = "initialize"
  params = [ordered]@{
    protocolVersion = "2025-03-26"
    capabilities = [ordered]@{}
    clientInfo = [ordered]@{
      name = "gtaiv-meta-xr-operator-client"
      version = "1.0"
    }
  }
}
$initialized = [ordered]@{
  jsonrpc = "2.0"
  method = "notifications/initialized"
  params = [ordered]@{}
}
$call = [ordered]@{
  jsonrpc = "2.0"
  id = 2
  method = "tools/call"
  params = [ordered]@{
    name = $ToolName
    arguments = $arguments
  }
}
$messages = @(
  ($initialize | ConvertTo-Json -Depth 12 -Compress)
  ($initialized | ConvertTo-Json -Depth 12 -Compress)
  ($call | ConvertTo-Json -Depth 12 -Compress)
)

$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $proxyPath
$startInfo.WorkingDirectory = $proxyDirectoryResolved
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true

$process = [Diagnostics.Process]::new()
$process.StartInfo = $startInfo
if (-not $process.Start()) {
  throw "Failed to start Meta XR Operator MCP proxy."
}

$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
try {
  foreach ($message in $messages) {
    $process.StandardInput.WriteLine($message)
  }
  $process.StandardInput.Close()

  if (-not $process.WaitForExit([int]$TimeoutSeconds * 1000)) {
    try {
      $process.Kill()
    }
    catch {}
    throw (
      "Meta XR Operator call timed out after $TimeoutSeconds seconds: " +
      $ToolName
    )
  }
  [void]$stdoutTask.Wait(5000)
  [void]$stderrTask.Wait(5000)
}
finally {
  if (-not $process.HasExited) {
    try {
      $process.Kill()
    }
    catch {}
  }
}

$stdout = $stdoutTask.Result
$stderr = $stderrTask.Result
$records = @()
foreach ($line in @($stdout -split "\r?\n")) {
  $trimmed = $line.Trim()
  if (-not $trimmed.StartsWith("{")) {
    continue
  }
  try {
    $record = $trimmed | ConvertFrom-Json -ErrorAction Stop
  }
  catch {
    continue
  }
  if ($null -ne $record.PSObject.Properties["jsonrpc"]) {
    $records += $record
  }
}

$response = @($records | Where-Object {
  $null -ne $_.PSObject.Properties["id"] -and
  [int]$_.id -eq 2
})
if ($response.Count -ne 1) {
  throw (
    "Meta XR Operator returned $($response.Count) tool responses for " +
    "$ToolName. stdout=$stdout stderr=$stderr"
  )
}
$response = $response[0]
if ($null -ne $response.PSObject.Properties["error"]) {
  throw (
    "Meta XR Operator tool error for ${ToolName}: " +
    ($response.error | ConvertTo-Json -Depth 12 -Compress)
  )
}
if ($null -eq $response.PSObject.Properties["result"]) {
  throw "Meta XR Operator returned no result for $ToolName."
}

$result = $response.result
$imageItems = @()
if ($null -ne $result.PSObject.Properties["content"]) {
  $imageItems = @($result.content | Where-Object {
    $null -ne $_.PSObject.Properties["type"] -and
    [string]$_.type -eq "image" -and
    $null -ne $_.PSObject.Properties["data"]
  })
}
if (-not [string]::IsNullOrWhiteSpace($ImageOutputPath)) {
  if ($imageItems.Count -ne 1) {
    throw (
      "Expected exactly one image from $ToolName, got " +
      "$($imageItems.Count)."
    )
  }
  $imagePath = [IO.Path]::GetFullPath($ImageOutputPath)
  $imageDirectory = [IO.Path]::GetDirectoryName($imagePath)
  if (-not (Test-Path -LiteralPath $imageDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $imageDirectory -Force | Out-Null
  }
  $imageBytes = [Convert]::FromBase64String([string]$imageItems[0].data)
  [IO.File]::WriteAllBytes($imagePath, $imageBytes)
  $imageItems[0].data = "<saved:$imagePath>"
  $imageItems[0] |
    Add-Member -NotePropertyName bytes -NotePropertyValue $imageBytes.Length
  $imageItems[0] |
    Add-Member `
      -NotePropertyName sha256 `
      -NotePropertyValue (
        (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash
      )
}
elseif ($imageItems.Count -gt 0) {
  foreach ($item in $imageItems) {
    $imageBytes = [Convert]::FromBase64String([string]$item.data)
    $item.data = "<image omitted; pass -ImageOutputPath to save>"
    $item |
      Add-Member -NotePropertyName bytes -NotePropertyValue $imageBytes.Length
  }
}

$result | ConvertTo-Json -Depth 20

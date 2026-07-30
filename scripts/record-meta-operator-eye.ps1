[CmdletBinding()]
param(
  [Parameter(Mandatory)]
  [string]$OutputPath,

  [ValidateSet("left", "right")]
  [string]$Eye = "right",

  [ValidateRange(5, 300)]
  [uint32]$DurationSeconds = 120,

  [ValidateRange(1, 60)]
  [uint32]$CaptureRate = 15,

  [ValidateRange(24, 60)]
  [uint32]$OutputFrameRate = 30,

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

  [string]$FfmpegPath = "",

  [switch]$KeepFrames
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-Response(
  [Diagnostics.Process]$Process,
  [int]$ExpectedId,
  [uint32]$TimeoutSeconds
) {
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
  while ([DateTime]::UtcNow -lt $deadline) {
    $remaining = [int][Math]::Ceiling(
      ($deadline - [DateTime]::UtcNow).TotalMilliseconds
    )
    $readTask = $Process.StandardOutput.ReadLineAsync()
    if (-not $readTask.Wait([Math]::Max(1, $remaining))) {
      throw "Timed out waiting for Meta XR Operator response id=$ExpectedId."
    }
    $line = $readTask.Result
    if ($null -eq $line) {
      throw "Meta XR Operator proxy closed before response id=$ExpectedId."
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
    if ($null -ne $message.PSObject.Properties["id"] -and
        [int]$message.id -eq $ExpectedId) {
      return $message
    }
  }
  throw "Timed out waiting for Meta XR Operator response id=$ExpectedId."
}

function Send-Message(
  [Diagnostics.Process]$Process,
  [object]$Message
) {
  $json = $Message | ConvertTo-Json -Depth 12 -Compress
  $Process.StandardInput.WriteLine($json)
  $Process.StandardInput.Flush()
}

$proxyDirectoryResolved = (Resolve-Path -LiteralPath $ProxyDirectory).Path
$proxyPath =
  Join-Path $proxyDirectoryResolved "meta-xr-operator-mcp-proxy.exe"
if (-not (Test-Path -LiteralPath $proxyPath -PathType Leaf)) {
  throw "Meta XR Operator MCP proxy is missing: $proxyPath"
}

if ([string]::IsNullOrWhiteSpace($FfmpegPath)) {
  $ffmpegCommand = Get-Command ffmpeg -ErrorAction SilentlyContinue
  if ($ffmpegCommand) {
    $FfmpegPath = $ffmpegCommand.Source
  }
  else {
    $wingetRoot = Join-Path (
      [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::LocalApplicationData
      )
    ) "Microsoft\WinGet\Packages"
    $candidate = Get-ChildItem `
      -LiteralPath $wingetRoot `
      -Recurse `
      -File `
      -Filter "ffmpeg.exe" `
      -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -match "Gyan\.FFmpeg_" } |
      Sort-Object LastWriteTime -Descending |
      Select-Object -First 1
    if (-not $candidate) {
      throw "ffmpeg.exe was not found."
    }
    $FfmpegPath = $candidate.FullName
  }
}
$ffmpeg = (Resolve-Path -LiteralPath $FfmpegPath).Path
$output = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Parent $output
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
  New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}
if (Test-Path -LiteralPath $output) {
  throw "Refusing to overwrite existing video: $output"
}
$frameDirectory = Join-Path $outputDirectory (
  "meta-operator-{0}-eye-frames-{1}" -f
    $Eye,
    (Get-Date -Format "yyyyMMdd-HHmmss")
)
New-Item -ItemType Directory -Path $frameDirectory | Out-Null

$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $proxyPath
$startInfo.WorkingDirectory = $proxyDirectoryResolved
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$proxy = [Diagnostics.Process]::new()
$proxy.StartInfo = $startInfo
if (-not $proxy.Start()) {
  throw "Failed to start Meta XR Operator MCP proxy."
}
$stderrTask = $proxy.StandardError.ReadToEndAsync()

$frameCount = 0
$captureStarted = [DateTime]::UtcNow
$captureElapsed = [TimeSpan]::Zero
$captureFailure = ""
try {
  Send-Message $proxy ([ordered]@{
    jsonrpc = "2.0"
    id = 1
    method = "initialize"
    params = [ordered]@{
      protocolVersion = "2025-03-26"
      capabilities = [ordered]@{}
      clientInfo = [ordered]@{
        name = "gtaiv-meta-xr-eye-recorder"
        version = "1.0"
      }
    }
  })
  $initializeResponse = Get-Response $proxy 1 20
  if ($null -ne $initializeResponse.PSObject.Properties["error"]) {
    throw "Meta XR Operator initialization failed."
  }
  Send-Message $proxy ([ordered]@{
    jsonrpc = "2.0"
    method = "notifications/initialized"
    params = [ordered]@{}
  })

  $captureStarted = [DateTime]::UtcNow
  $captureDeadline = $captureStarted.AddSeconds($DurationSeconds)
  $nextFrameAt = $captureStarted
  $requestId = 10
  while ([DateTime]::UtcNow -lt $captureDeadline) {
    $now = [DateTime]::UtcNow
    if ($now -lt $nextFrameAt) {
      $wait = [int][Math]::Floor(($nextFrameAt - $now).TotalMilliseconds)
      if ($wait -gt 0) {
        Start-Sleep -Milliseconds $wait
      }
    }

    Send-Message $proxy ([ordered]@{
      jsonrpc = "2.0"
      id = $requestId
      method = "tools/call"
      params = [ordered]@{
        name = "openxr_capture_composited_image"
        arguments = [ordered]@{ eye = $Eye }
      }
    })
    $response = Get-Response $proxy $requestId 20
    if ($null -ne $response.PSObject.Properties["error"]) {
      throw (
        "Meta XR Operator capture failed: " +
        ($response.error | ConvertTo-Json -Depth 8 -Compress)
      )
    }
    $images = @($response.result.content | Where-Object {
      $_.type -eq "image" -and
      $null -ne $_.PSObject.Properties["data"]
    })
    if ($images.Count -ne 1) {
      throw "Expected exactly one $Eye-eye image; got $($images.Count)."
    }
    ++$frameCount
    $framePath = Join-Path $frameDirectory (
      "frame-{0:D6}.png" -f $frameCount
    )
    [IO.File]::WriteAllBytes(
      $framePath,
      [Convert]::FromBase64String([string]$images[0].data)
    )
    ++$requestId
    $nextFrameAt =
      $nextFrameAt.AddSeconds(1.0 / [double]$CaptureRate)
  }
  $captureElapsed = [DateTime]::UtcNow - $captureStarted
}
catch {
  $captureElapsed = [DateTime]::UtcNow - $captureStarted
  $captureFailure = $_.Exception.Message
}
finally {
  try {
    $proxy.StandardInput.Close()
  }
  catch {}
  if (-not $proxy.WaitForExit(3000)) {
    try {
      $proxy.Kill()
    }
    catch {}
  }
  [void]$stderrTask.Wait(3000)
}

if ($frameCount -lt 2 -or $captureElapsed.TotalSeconds -le 0) {
  throw "Too few compositor frames were captured: $frameCount."
}
$measuredRate = $frameCount / $captureElapsed.TotalSeconds
$rateText = $measuredRate.ToString(
  "0.######",
  [Globalization.CultureInfo]::InvariantCulture
)
$pattern = Join-Path $frameDirectory "frame-%06d.png"
$ffmpegOutput = @()
$ffmpegExitCode = -1
$priorErrorActionPreference = $ErrorActionPreference
try {
  # FFmpeg reports normal progress and stream metadata on stderr. Windows
  # PowerShell turns redirected native stderr into ErrorRecord objects, which
  # must not terminate a successful encode under the script-wide Stop policy.
  $ErrorActionPreference = "Continue"
  $ffmpegOutput = @(
    & $ffmpeg `
      -hide_banner `
      -loglevel info `
      -y `
      -framerate $rateText `
      -i $pattern `
      -an `
      -vf "fps=$OutputFrameRate" `
      -c:v h264_nvenc `
      -preset p5 `
      -tune hq `
      -rc vbr `
      -cq 18 `
      -b:v 15M `
      -maxrate 25M `
      -bufsize 50M `
      -pix_fmt yuv420p `
      -metadata:s:v:0 stereo_mode=mono `
      -metadata:s:v:0 source_eye=$Eye `
      -movflags +faststart `
      $output 2>&1
  )
  $ffmpegExitCode = $LASTEXITCODE
}
finally {
  $ErrorActionPreference = $priorErrorActionPreference
}
$ffmpegOutput |
  Set-Content -LiteralPath "$output.ffmpeg.log" -Encoding UTF8
if ($ffmpegExitCode -ne 0 -or
    -not (Test-Path -LiteralPath $output -PathType Leaf) -or
    (Get-Item -LiteralPath $output).Length -le 1024) {
  throw "ffmpeg failed to create the Meta Operator eye video."
}

$videoHash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
$captureStatus = if ([string]::IsNullOrWhiteSpace($captureFailure)) {
  "PASS"
}
else {
  "PARTIAL"
}
@(
  "capture=$captureStatus"
  "source=Meta XR Operator OpenXR compositor"
  "eye=$Eye"
  "stereoMode=mono-view-of-stereo"
  "frameCount=$frameCount"
  "captureElapsedSeconds=$($captureElapsed.TotalSeconds)"
  "measuredCaptureRate=$rateText"
  "outputFrameRate=$OutputFrameRate"
  "output=$output"
  "bytes=$((Get-Item -LiteralPath $output).Length)"
  "sha256=$videoHash"
  "captureFailure=$captureFailure"
) | Set-Content -LiteralPath "$output.capture.txt" -Encoding UTF8

if (-not $KeepFrames) {
  $resolvedFrameDirectory = [IO.Path]::GetFullPath($frameDirectory)
  $resolvedOutputDirectory = [IO.Path]::GetFullPath($outputDirectory)
  $expectedPrefix = $resolvedOutputDirectory.TrimEnd(
    [IO.Path]::DirectorySeparatorChar
  ) + [IO.Path]::DirectorySeparatorChar
  if (-not $resolvedFrameDirectory.StartsWith(
      $expectedPrefix,
      [StringComparison]::OrdinalIgnoreCase
    ) -or
    (Split-Path -Leaf $resolvedFrameDirectory) -notlike
      "meta-operator-*-eye-frames-*") {
    throw "Refusing unsafe temporary-frame cleanup: $resolvedFrameDirectory"
  }
  Remove-Item -LiteralPath $resolvedFrameDirectory -Recurse -Force
}

Write-Output "META XR OPERATOR EYE VIDEO CAPTURE PASS"
Write-Output "Output: $output"
Write-Output "Receipt: $output.capture.txt"

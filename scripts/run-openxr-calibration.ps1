param(
  [switch]$Build,
  [uint64]$FrameLimit = 0,
  [uint32]$TimeoutSeconds = 0,
  [switch]$AllowSteamVR
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$HostExe = Join-Path $Root "out-openxr\gtaiv_xr_host.exe"

if ($Build -or -not (Test-Path $HostExe)) {
  & "$PSScriptRoot\build-openxr-host.ps1"
}
if (-not (Test-Path $HostExe)) {
  throw "Missing x64 host: $HostExe"
}

$runtimeKey = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1"
$runtime = (Get-ItemProperty -Path $runtimeKey -Name ActiveRuntime -ErrorAction Stop).ActiveRuntime
if (-not $runtime -or -not (Test-Path $runtime)) {
  throw "The active x64 OpenXR runtime manifest is missing: $runtime"
}

Write-Host "Active x64 OpenXR runtime: $runtime"
$isMetaRuntime = $runtime -match "Oculus|Meta"
if ($isMetaRuntime) {
  Write-Host "Quest route: Meta OpenXR is active. SteamVR will not be launched."

  $ovrService = Get-Service -Name OVRService -ErrorAction SilentlyContinue
  if (-not $ovrService -or $ovrService.Status -ne "Running") {
    throw "Meta OVRService is not running. Start the Meta Quest Link app, connect Quest 3, then rerun."
  }

  if (-not (Get-Process -Name OculusClient -ErrorAction SilentlyContinue)) {
    Write-Warning "The Meta Quest Link desktop window is not open. Open it, then enter Quest Link inside the headset if the host remains IDLE."
  }
}
else {
  Write-Warning "The active runtime is not Meta OpenXR. The host is standard OpenXR and will attempt this runtime for compatibility."
}

$steamVrProcesses = Get-Process -Name vrmonitor, vrserver -ErrorAction SilentlyContinue
if ($isMetaRuntime -and $steamVrProcesses -and -not $AllowSteamVR) {
  throw "SteamVR is running. Close SteamVR for the direct Quest 3 / Meta OpenXR test, or pass -AllowSteamVR only for compatibility testing."
}

$arguments = @()
if ($FrameLimit -gt 0) {
  $arguments += "--frames"
  $arguments += $FrameLimit.ToString()
}
if ($TimeoutSeconds -gt 0) {
  $arguments += "--timeout-ms"
  $arguments += ([uint64]$TimeoutSeconds * 1000).ToString()
}

Push-Location (Split-Path -Parent $HostExe)
try {
  & $HostExe @arguments
  if ($LASTEXITCODE -ne 0) {
    throw "OpenXR calibration host failed with exit code $LASTEXITCODE. See out-openxr\gtaiv_xr_host.log"
  }
}
finally {
  Pop-Location
}

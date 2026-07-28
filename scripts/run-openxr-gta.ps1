param(
  [string]$GameDir = "",
  [switch]$Build,
  [string]$RuntimeManifest = ""
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
. "$PSScriptRoot\openxr-runtime-info.ps1"
$HostExe = Join-Path $Root "out-openxr\gtaiv_xr_host.exe"
$Asi = Join-Path $Root "out-asi\gtaiv_dxvk_vr.asi"

if (-not $GameDir) {
  $candidates = @(
    "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV",
    "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
  )
  $GameDir = $candidates |
    Where-Object { Test-Path (Join-Path $_ "GTAIV.exe") } |
    Select-Object -First 1
}
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir "GTAIV.exe"))) {
  throw "GTAIV.exe was not found. Pass -GameDir '...\Grand Theft Auto IV\GTAIV'."
}
$GameDir = (Resolve-Path $GameDir).Path

if ($Build) {
  & "$PSScriptRoot\build-asi.ps1"
  & "$PSScriptRoot\build-openxr-host.ps1"
}
if (-not (Test-Path $Asi)) {
  throw "Missing x86 ASI. Run .\scripts\build-asi.ps1."
}
if (-not (Test-Path $HostExe)) {
  throw "Missing x64 OpenXR host. Run .\scripts\build-openxr-host.ps1."
}
if (-not (Test-Path (Join-Path $GameDir "d3d9.dll"))) {
  throw "d3d9.dll is missing from the GTA folder. Stock DXVK 3.0.2 must be installed."
}

function Get-PeMachine([string]$Path) {
  $bytes = [IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -lt 256) { throw "Invalid PE file: $Path" }
  $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
  return [BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

$asiMachine = Get-PeMachine $Asi
$hostMachine = Get-PeMachine $HostExe
if ($asiMachine -ne 0x014c) {
  throw ("ASI machine is 0x{0:X4}; expected x86 0x014C." -f $asiMachine)
}
if ($hostMachine -ne 0x8664) {
  throw ("Host machine is 0x{0:X4}; expected x64 0x8664." -f $hostMachine)
}

$runtimeInfo = if ($RuntimeManifest) {
  $resolvedManifest = (Resolve-Path -LiteralPath $RuntimeManifest).Path
  Get-OpenXrRuntimeInfo -ManifestPath $resolvedManifest
}
else {
  Get-ActiveOpenXrRuntimeInfo
}
$runtime = $runtimeInfo.ManifestPath
if (-not $runtimeInfo.IsDirectTestRuntime) {
  throw "Selected x64 OpenXR runtime is not a supported direct route: $runtime. Refusing the no-SteamVR route."
}
if ($runtimeInfo.RequiresOvrService) {
  $ovrService = Get-Service -Name OVRService -ErrorAction SilentlyContinue
  if (-not $ovrService -or $ovrService.Status -ne "Running") {
    throw "Meta OVRService is not running. Start Meta Quest Link first."
  }
}

$steamVr = Get-Process `
  -Name vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper `
  -ErrorAction SilentlyContinue
if ($steamVr) {
  throw "SteamVR is running. Close it before the direct OpenXR test; this script will not launch GTA while it is running."
}
Write-Host "OPENXR GTA PREFLIGHT PASS"
Write-Host "Route:   $($runtimeInfo.DisplayName)"
Write-Host "Runtime: $runtime"
if ($RuntimeManifest) {
  Write-Host "Scope:   child-only runtime override; system ActiveRuntime will not change"
}
Write-Host "GTA:     $(Join-Path $GameDir 'GTAIV.exe') (x86)"
Write-Host "ASI:     $Asi (x86)"
Write-Host "Host:    $HostExe (x64)"
Write-Host "Launch:  disabled; this script cannot start Steam, SteamVR, GTA, or the OpenXR host"

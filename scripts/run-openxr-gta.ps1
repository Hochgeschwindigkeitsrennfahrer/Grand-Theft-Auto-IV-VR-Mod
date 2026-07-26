param(
  [string]$GameDir = "",
  [switch]$Build,
  [switch]$DirectExe,
  [switch]$AllowSteamVR
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$HostExe = Join-Path $Root "out-openxr\gtaiv_xr_host.exe"
$Asi = Join-Path $Root "out-asi\gtaiv_dxvk_vr.asi"
$OpenVr = Join-Path $Root "out-asi\openvr_api.dll"

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

$runtimeKey = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1"
$runtime = (Get-ItemProperty -Path $runtimeKey -Name ActiveRuntime -ErrorAction Stop).ActiveRuntime
if (-not $runtime -or -not (Test-Path $runtime)) {
  throw "The active x64 OpenXR runtime manifest is missing: $runtime"
}
$isMetaRuntime = $runtime -match "Oculus|Meta"
if ($isMetaRuntime) {
  $ovrService = Get-Service -Name OVRService -ErrorAction SilentlyContinue
  if (-not $ovrService -or $ovrService.Status -ne "Running") {
    throw "Meta OVRService is not running. Start Meta Quest Link first."
  }
}

$steamVr = Get-Process -Name vrmonitor,vrserver -ErrorAction SilentlyContinue
if ($isMetaRuntime -and $steamVr -and -not $AllowSteamVR) {
  throw "SteamVR is running. Close it for direct Meta OpenXR, or pass -AllowSteamVR."
}

Get-Process -Name GTAIV,gtaiv_xr_host -ErrorAction SilentlyContinue |
  Stop-Process -Force
Start-Sleep -Milliseconds 750

Copy-Item -LiteralPath $Asi -Destination (Join-Path $GameDir "gtaiv_dxvk_vr.asi") -Force
if (Test-Path $OpenVr) {
  # The ASI keeps the existing OpenVR fallback import, even in OpenXR mode.
  Copy-Item -LiteralPath $OpenVr -Destination (Join-Path $GameDir "openvr_api.dll") -Force
}
[IO.File]::WriteAllText(
  (Join-Path $GameDir "gtaiv_dxvk_vr.backend"),
  "openxr`r`n",
  [Text.Encoding]::ASCII)
[IO.File]::WriteAllText(
  (Join-Path $GameDir "gtaiv_dxvk_vr.stereo"),
  "0`r`n",
  [Text.Encoding]::ASCII)

# Meta OpenXR owns a foreground VR session. GTA's exclusive-fullscreen focus
# loss path makes stock DXVK deliberately lose the D3D9 device, so the game
# transport test must use a normal window.
$commandLinePath = Join-Path (Split-Path -Parent $GameDir) "commandline.txt"
$commandLineBackup = "$commandLinePath.before-openxr"
if (Test-Path $commandLinePath) {
  if (-not (Test-Path $commandLineBackup)) {
    Copy-Item -LiteralPath $commandLinePath -Destination $commandLineBackup
  }
  $commandLine = [IO.File]::ReadAllText($commandLinePath)
}
else {
  $commandLine = ""
}
if ($commandLine -notmatch "(?im)(^|\s)-windowed(\s|$)") {
  $commandLine = $commandLine.TrimEnd() + "`r`n-windowed`r`n"
  [IO.File]::WriteAllText(
    $commandLinePath,
    $commandLine,
    [Text.Encoding]::ASCII)
}

$xrHostProcess = Start-Process `
  -FilePath $HostExe `
  -ArgumentList @("--game") `
  -WorkingDirectory (Split-Path -Parent $HostExe) `
  -WindowStyle Hidden `
  -PassThru
Start-Sleep -Seconds 2
if ($xrHostProcess.HasExited) {
  throw "The OpenXR host exited immediately. See $Root\out-openxr\gtaiv_xr_host.log"
}

$gtaExe = Join-Path $GameDir "GTAIV.exe"
if ($DirectExe) {
  Start-Process -FilePath $gtaExe -WorkingDirectory $GameDir
}
else {
  $steamExe = "C:\Program Files (x86)\Steam\steam.exe"
  if (-not (Test-Path $steamExe)) {
    throw "steam.exe was not found. Rerun with -DirectExe if offline launch is configured."
  }
  Start-Process -FilePath $steamExe -ArgumentList @("-applaunch", "12210")
}

Write-Host "OPENXR GTA TEST STARTED"
Write-Host "Runtime: $runtime"
Write-Host ("Host:    pid {0}, x64, --game" -f $xrHostProcess.Id)
Write-Host "GTA:     $gtaExe (x86)"
Write-Host "ASI log: $(Join-Path $GameDir 'gtaiv_dxvk_vr.log')"
Write-Host "XR log:  $(Join-Path $Root 'out-openxr\gtaiv_xr_host.log')"
Write-Host "In Quest Link, resume the GTA IV OpenXR app. Stay in singleplayer."

# Fetch OpenVR (pinned for SteamVR 2.12 / IVRSystem_023)
# Do NOT use tip/master — newer OpenVR (e.g. 2.15) can hit SteamVR init error 105.
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$dest = Join-Path $Root "thirdparty\openvr"
$Tag = "v2.12.14"

if ((Test-Path (Join-Path $dest "headers\openvr.h")) -and
    (Test-Path (Join-Path $dest "bin\win32\openvr_api.dll")) -and
    (Test-Path (Join-Path $dest "lib\win32\openvr_api.lib"))) {
  Push-Location $dest
  try {
    $cur = (git describe --tags --exact-match HEAD 2>$null)
    if ($cur -eq $Tag) {
      Write-Host "OK: OpenVR $Tag already at $dest"
      exit 0
    }
  } finally {
    Pop-Location
  }
  Write-Host "OpenVR present but not $Tag — re-cloning..."
  Remove-Item -Recurse -Force $dest
}

Write-Host "Cloning OpenVR $Tag into $dest ..."
git clone --depth 1 --branch $Tag https://github.com/ValveSoftware/openvr.git $dest
if (-not (Test-Path (Join-Path $dest "headers\openvr.h"))) {
  throw "OpenVR clone failed — headers\openvr.h missing"
}
Write-Host "OK: OpenVR $Tag ready"

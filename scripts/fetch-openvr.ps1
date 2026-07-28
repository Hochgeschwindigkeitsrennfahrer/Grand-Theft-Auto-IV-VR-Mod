# Fetch OpenVR (pinned for SteamVR 2.12 / IVRSystem_023)
# Do NOT use tip/master - newer OpenVR (e.g. 2.15) can hit SteamVR init error 105.
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$dest = Join-Path $Root "thirdparty\openvr"
$Tag = "v2.12.14"

if ((Test-Path (Join-Path $dest "headers\openvr.h")) -and
    (Test-Path (Join-Path $dest "bin\win32\openvr_api.dll")) -and
    (Test-Path (Join-Path $dest "lib\win32\openvr_api.lib"))) {
  # Scope safe.directory to this exact vendored checkout. Do not mutate the
  # user's global Git configuration just because the workspace was restored or
  # copied under a different Windows account.
  $cur = (& git -c "safe.directory=$dest" -C $dest describe --tags --exact-match HEAD 2>$null)
  if ($LASTEXITCODE -eq 0 -and $cur -eq $Tag) {
    Write-Host "OK: OpenVR $Tag already at $dest"
    exit 0
  }

  # Never recursively delete an existing dependency checkout. If it is clean,
  # move the same checkout to the pinned tag; if it has local work, stop and
  # make the owner decide how to preserve it.
  if (-not (Test-Path (Join-Path $dest ".git"))) {
    throw "OpenVR exists at '$dest' but is not a Git checkout at $Tag. Move it aside, then rerun."
  }
  $dirty = (& git -c "safe.directory=$dest" -C $dest status --porcelain)
  if ($LASTEXITCODE -ne 0) {
    throw "OpenVR checkout is not readable: $dest"
  }
  if ($dirty) {
    throw "OpenVR checkout has local changes and is not $Tag. Preserve or commit them before rerunning."
  }

  Write-Host "Switching clean OpenVR checkout from '$cur' to pinned $Tag..."
  & git -c "safe.directory=$dest" -C $dest fetch --depth 1 origin "tag" $Tag
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to fetch OpenVR $Tag."
  }
  & git -c "safe.directory=$dest" -C $dest checkout --detach $Tag
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to check out OpenVR $Tag."
  }

  $cur = (& git -c "safe.directory=$dest" -C $dest describe --tags --exact-match HEAD 2>$null)
  if ($LASTEXITCODE -ne 0 -or $cur -ne $Tag) {
    throw "OpenVR tag verification failed after checkout. Expected $Tag, found '$cur'."
  }
  Write-Host "OK: OpenVR $Tag ready at $dest"
  exit 0
}

Write-Host "Cloning OpenVR $Tag into $dest ..."
git clone --depth 1 --branch $Tag https://github.com/ValveSoftware/openvr.git $dest
if (-not (Test-Path (Join-Path $dest "headers\openvr.h"))) {
  throw "OpenVR clone failed - headers\openvr.h missing"
}
Write-Host "OK: OpenVR $Tag ready"

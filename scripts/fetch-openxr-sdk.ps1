param(
  [string]$Destination
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $Destination) {
  $Destination = Join-Path $Root "thirdparty\openxr-sdk"
}

$PinnedTag = "release-1.1.61"
$PinnedTagObject = "57e11bb6729e75e494b67360c0795c0b8f5a9cb6"
$PinnedCommit = "5267613edf3d937e3d77556a106a65c2f82b25c6"
$Repository = "https://github.com/KhronosGroup/OpenXR-SDK.git"

function Get-ExactCommit([string]$Path) {
  $commit = & git -C $Path rev-parse HEAD 2>$null
  if ($LASTEXITCODE -ne 0) {
    throw "OpenXR SDK directory is not a readable Git checkout: $Path"
  }
  return $commit.Trim().ToLowerInvariant()
}

if (Test-Path $Destination) {
  $actual = Get-ExactCommit $Destination
  if ($actual -ne $PinnedCommit) {
    throw "OpenXR SDK mismatch at '$Destination'. Expected $PinnedTag ($PinnedCommit), found $actual. Remove or move that dependency directory, then rerun."
  }

  [string]$actualTagObject = & git -C $Destination rev-parse $PinnedTag
  if ($LASTEXITCODE -ne 0) {
    throw "OpenXR SDK tag '$PinnedTag' is missing from $Destination."
  }
  $actualTagObject = $actualTagObject.Trim().ToLowerInvariant()
  if ($actualTagObject -ne $PinnedTagObject) {
    throw "OpenXR SDK tag-object verification failed. Expected $PinnedTagObject, got $actualTagObject."
  }

  Write-Host "OK: OpenXR SDK $PinnedTag already present ($actual)"
  exit 0
}

$parent = Split-Path -Parent $Destination
New-Item -ItemType Directory -Force -Path $parent | Out-Null

Write-Host "Fetching Khronos OpenXR SDK $PinnedTag..."
& git clone --depth 1 --branch $PinnedTag --single-branch $Repository $Destination
if ($LASTEXITCODE -ne 0) {
  throw "Failed to clone Khronos OpenXR SDK."
}

$actual = Get-ExactCommit $Destination
if ($actual -ne $PinnedCommit) {
  throw "OpenXR SDK tag verification failed. Expected $PinnedCommit, got $actual."
}

[string]$actualTagObject = & git -C $Destination rev-parse $PinnedTag
if ($LASTEXITCODE -ne 0) {
  throw "OpenXR SDK clone is missing tag '$PinnedTag'."
}
$actualTagObject = $actualTagObject.Trim().ToLowerInvariant()
if ($actualTagObject -ne $PinnedTagObject) {
  throw "OpenXR SDK tag-object verification failed. Expected $PinnedTagObject, got $actualTagObject."
}

Write-Host "OK: pinned OpenXR SDK verified ($actual)"

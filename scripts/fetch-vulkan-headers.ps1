param(
  [string]$Destination
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $Destination) {
  $Destination = Join-Path $Root "thirdparty\vulkan-headers"
}

# Headers only. The ASI resolves Vulkan entry points from the loader already
# used by DXVK; it does not ship or replace a Vulkan runtime.
$PinnedTag = "vulkan-sdk-1.4.350.1"
$PinnedCommit = "8864cdc896bbc2a9b6eb36b3218fc9ef57908d77"
$Repository = "https://github.com/KhronosGroup/Vulkan-Headers.git"

function Get-ExactCommit([string]$Path) {
  # Keep the trust exception local to this exact vendored checkout. This also
  # works in restored/copied workspaces without changing the user's global Git
  # configuration.
  $commit = & git -c "safe.directory=$Path" -C $Path rev-parse HEAD 2>$null
  if ($LASTEXITCODE -ne 0) {
    throw "Vulkan-Headers directory is not a readable Git checkout: $Path"
  }
  return $commit.Trim().ToLowerInvariant()
}

if (Test-Path $Destination) {
  $actual = Get-ExactCommit $Destination
  if ($actual -ne $PinnedCommit) {
    throw "Vulkan-Headers mismatch at '$Destination'. Expected $PinnedTag ($PinnedCommit), found $actual. Move that dependency directory, then rerun."
  }
  Write-Host "OK: Vulkan-Headers $PinnedTag already present ($actual)"
  exit 0
}

$parent = Split-Path -Parent $Destination
New-Item -ItemType Directory -Force -Path $parent | Out-Null

Write-Host "Fetching Khronos Vulkan-Headers $PinnedTag..."
& git clone --depth 1 --branch $PinnedTag --single-branch $Repository $Destination
if ($LASTEXITCODE -ne 0) {
  throw "Failed to clone Khronos Vulkan-Headers."
}

$actual = Get-ExactCommit $Destination
if ($actual -ne $PinnedCommit) {
  throw "Vulkan-Headers tag verification failed. Expected $PinnedCommit, got $actual."
}

Write-Host "OK: pinned Vulkan-Headers verified ($actual)"

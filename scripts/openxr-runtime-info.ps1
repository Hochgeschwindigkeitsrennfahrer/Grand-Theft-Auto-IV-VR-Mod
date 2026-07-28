function Get-OpenXrRuntimeKind {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory = $true)]
    [string]$ManifestPath,
    [string]$RuntimeLibraryPath = ""
  )

  $normalized = $ManifestPath.Replace("/", "\").ToLowerInvariant()
  $leaf = [IO.Path]::GetFileName($normalized)
  $libraryNormalized = $RuntimeLibraryPath.Replace("/", "\").ToLowerInvariant()
  $libraryLeaf = [IO.Path]::GetFileName($libraryNormalized)

  if ($leaf -eq "meta_openxr_simulator.json") {
    return "MetaSimulator"
  }
  if ($leaf -eq "openxr_simulator.json" -or
      $libraryLeaf -eq "openxr_simulator.dll") {
    return "ElliottSimulator"
  }
  if ($leaf -eq "oculus_openxr_64.json" -or
      $normalized -match "\\oculus\\") {
    return "MetaQuestLink"
  }
  if ($leaf -match "steam.*openxr|steamxr" -or
      $normalized -match "\\steamvr\\") {
    return "SteamVr"
  }
  return "Other"
}

function Get-OpenXrRuntimeInfo {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory = $true)]
    [string]$ManifestPath
  )

  if (-not $manifestPath -or
      -not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "The x64 OpenXR runtime manifest is missing: $manifestPath"
  }

  $runtimeLibraryPath = ""
  try {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -ErrorAction Stop |
      ConvertFrom-Json -ErrorAction Stop
    $runtimeLibraryPath = [string]$manifest.runtime.library_path
  }
  catch {
    throw "The OpenXR runtime manifest is invalid: $manifestPath ($($_.Exception.Message))"
  }

  $kind = Get-OpenXrRuntimeKind `
    -ManifestPath $manifestPath `
    -RuntimeLibraryPath $runtimeLibraryPath
  $displayName = switch ($kind) {
    "MetaSimulator" { "Meta XR Simulator" }
    "ElliottSimulator" { "Elliott OpenXR Simulator" }
    "MetaQuestLink" { "Meta Quest Link" }
    "SteamVr" { "SteamVR OpenXR" }
    default { "Other OpenXR runtime" }
  }

  return [pscustomobject]@{
    ManifestPath = $manifestPath
    RuntimeLibraryPath = $runtimeLibraryPath
    Kind = $kind
    DisplayName = $displayName
    IsMetaDirect = $kind -in @("MetaSimulator", "MetaQuestLink")
    IsDirectTestRuntime = $kind -in @(
      "MetaSimulator",
      "ElliottSimulator",
      "MetaQuestLink"
    )
    IsSimulator = $kind -in @("MetaSimulator", "ElliottSimulator")
    RequiresOvrService = $kind -eq "MetaQuestLink"
    IsSteamVr = $kind -eq "SteamVr"
  }
}

function Get-ActiveOpenXrRuntimeInfo {
  [CmdletBinding()]
  param()

  $runtimeKey = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1"
  $manifestPath = (Get-ItemProperty `
    -Path $runtimeKey `
    -Name ActiveRuntime `
    -ErrorAction Stop).ActiveRuntime
  return Get-OpenXrRuntimeInfo -ManifestPath $manifestPath
}

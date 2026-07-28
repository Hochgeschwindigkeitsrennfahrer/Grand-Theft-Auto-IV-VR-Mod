$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot "..\scripts\openxr-runtime-info.ps1")

$cases = @(
  @{
    Path = "C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json"
    Expected = "MetaQuestLink"
  },
  @{
    Path = "C:\Program Files\Meta XR Simulator\meta_openxr_simulator.json"
    Expected = "MetaSimulator"
  },
  @{
    Path = "C:\PROGRAM FILES\META XR SIMULATOR\META_OPENXR_SIMULATOR.JSON"
    Expected = "MetaSimulator"
  },
  @{
    Path = "D:\tools\OpenXR-Simulator\bin\openxr_simulator.json"
    Expected = "ElliottSimulator"
  },
  @{
    Path = "C:\Users\tester\AppData\Local\openxr\1\active_runtime.json"
    Library = "D:\tools\OpenXR-Simulator\bin\openxr_simulator.dll"
    Expected = "ElliottSimulator"
  },
  @{
    Path = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json"
    Expected = "SteamVr"
  },
  @{
    Path = "C:\Meta\SteamVR\steamxr_win64.json"
    Expected = "SteamVr"
  },
  @{
    Path = "C:\OpenXR\vendor_runtime.json"
    Expected = "Other"
  }
)

foreach ($case in $cases) {
  $library = if ($case.ContainsKey("Library")) {
    $case.Library
  }
  else {
    ""
  }
  $actual = Get-OpenXrRuntimeKind `
    -ManifestPath $case.Path `
    -RuntimeLibraryPath $library
  if ($actual -ne $case.Expected) {
    throw "Runtime classification failed for '$($case.Path)': expected $($case.Expected), got $actual"
  }
}

Write-Output "OpenXrRuntimeInfoTest: PASS cases=$($cases.Count)"

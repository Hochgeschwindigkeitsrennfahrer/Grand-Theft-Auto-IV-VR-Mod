param(
  [switch]$Build,
  [uint64]$FrameLimit = 0,
  [uint32]$TimeoutSeconds = 0,
  [switch]$AllowSteamVR,
  [string]$RuntimeManifest = ""
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
. "$PSScriptRoot\openxr-runtime-info.ps1"
$HostExe = Join-Path $Root "out-openxr\gtaiv_xr_host.exe"

if ($Build -or -not (Test-Path $HostExe)) {
  & "$PSScriptRoot\build-openxr-host.ps1"
}
if (-not (Test-Path $HostExe)) {
  throw "Missing x64 host: $HostExe"
}

$runtimeInfo = if ($RuntimeManifest) {
  $resolvedManifest = (Resolve-Path -LiteralPath $RuntimeManifest).Path
  Get-OpenXrRuntimeInfo -ManifestPath $resolvedManifest
}
else {
  Get-ActiveOpenXrRuntimeInfo
}
$runtime = $runtimeInfo.ManifestPath

if ($RuntimeManifest) {
  Write-Host "Child-only x64 OpenXR runtime override: $runtime"
}
else {
  Write-Host "Active x64 OpenXR runtime: $runtime"
}
if ($runtimeInfo.Kind -eq "MetaSimulator") {
  Write-Host "Simulator route: Meta XR Simulator is selected. No headset or OVRService is required."
}
elseif ($runtimeInfo.Kind -eq "ElliottSimulator") {
  Write-Host "Diagnostic route: Elliott OpenXR Simulator is selected. The system runtime remains unchanged."
  Write-Warning "This desktop runtime does not prove real Quest controls, haptics, optics, color, or performance."
}
elseif ($runtimeInfo.Kind -eq "MetaQuestLink") {
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
  Write-Warning "The active runtime is $($runtimeInfo.DisplayName). The host is standard OpenXR and will attempt it for compatibility."
}

$steamVrProcesses = Get-Process `
  -Name vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper `
  -ErrorAction SilentlyContinue
if ($runtimeInfo.IsSteamVr -and -not $AllowSteamVR) {
  throw "SteamVR is the active OpenXR runtime. Pass -AllowSteamVR only for a deliberate compatibility test."
}
if ($runtimeInfo.IsDirectTestRuntime -and $steamVrProcesses) {
  throw "SteamVR is running. Close it before using the selected direct OpenXR route."
}
if ($steamVrProcesses -and -not $AllowSteamVR) {
  throw "SteamVR is running. Close it, or pass -AllowSteamVR only for a deliberate SteamVR compatibility test."
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
$priorRuntimeOverride = $env:XR_RUNTIME_JSON
$calibrationProcess = $null
try {
  if ($RuntimeManifest) {
    $env:XR_RUNTIME_JSON = $runtime
  }

  if ($runtimeInfo.Kind -eq "ElliottSimulator") {
    if ($FrameLimit -eq 0 -or $TimeoutSeconds -eq 0) {
      throw "Elliott diagnostics must be bounded. Pass both -FrameLimit and -TimeoutSeconds."
    }

    $calibrationProcess = Start-Process `
      -FilePath $HostExe `
      -ArgumentList $arguments `
      -WorkingDirectory (Split-Path -Parent $HostExe) `
      -WindowStyle Hidden `
      -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds + 10)
    $cleanShutdownSince = $null
    $forcedKnownTeardown = $false
    $requestedFrameLimitReached = $false
    $hostLog = Join-Path (Split-Path -Parent $HostExe) "gtaiv_xr_host.log"

    while (-not $calibrationProcess.HasExited -and
        [DateTime]::UtcNow -lt $deadline) {
      $currentRunLog = ""
      try {
        $allHostLog = Get-Content -LiteralPath $hostLog -Raw -ErrorAction Stop
        $modeIndex = $allHostLog.LastIndexOf("Mode: CALIBRATION")
        if ($modeIndex -ge 0) {
          $currentRunLog = $allHostLog.Substring($modeIndex)
        }
      }
      catch {
        $currentRunLog = ""
      }

      if ($currentRunLog -match "requested frame limit reached") {
        $requestedFrameLimitReached = $true
      }
      if ($requestedFrameLimitReached -and
          $currentRunLog -match "clean shutdown requested") {
        if ($null -eq $cleanShutdownSince) {
          $cleanShutdownSince = [DateTime]::UtcNow
        }
        elseif (([DateTime]::UtcNow - $cleanShutdownSince).TotalSeconds -ge 2) {
          Stop-Process -Id $calibrationProcess.Id -Force -ErrorAction SilentlyContinue
          try {
            [void]$calibrationProcess.WaitForExit(5000)
          }
          catch {}
          $forcedKnownTeardown = $true
          break
        }
      }
      Start-Sleep -Milliseconds 100
      $calibrationProcess.Refresh()
    }

    if (-not $calibrationProcess.HasExited) {
      Stop-Process -Id $calibrationProcess.Id -Force -ErrorAction SilentlyContinue
      throw "Elliott calibration exceeded its supervised timeout."
    }
    if ($forcedKnownTeardown) {
      Write-Warning "Elliott runtime completed the requested frames but hung after clean teardown; the supervised wrapper stopped the remaining process."
    }
    elseif (-not $requestedFrameLimitReached) {
      throw "Elliott calibration exited without reaching the requested frame limit."
    }
    elseif ($calibrationProcess.ExitCode -ne 0) {
      throw "OpenXR calibration host failed with exit code $($calibrationProcess.ExitCode). See out-openxr\gtaiv_xr_host.log"
    }
  }
  else {
    & $HostExe @arguments
    if ($LASTEXITCODE -ne 0) {
      throw "OpenXR calibration host failed with exit code $LASTEXITCODE. See out-openxr\gtaiv_xr_host.log"
    }
  }
}
finally {
  if ($calibrationProcess -and -not $calibrationProcess.HasExited) {
    Stop-Process -Id $calibrationProcess.Id -Force -ErrorAction SilentlyContinue
  }
  if ($null -eq $priorRuntimeOverride) {
    Remove-Item Env:XR_RUNTIME_JSON -ErrorAction SilentlyContinue
  }
  else {
    $env:XR_RUNTIME_JSON = $priorRuntimeOverride
  }
  Pop-Location
}

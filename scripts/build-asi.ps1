# Build gtaiv_dxvk_vr.asi (Win32 / x86) + OpenVR
$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

& "$PSScriptRoot\fetch-minhook.ps1"
& "$PSScriptRoot\fetch-vulkan-headers.ps1"
& "$PSScriptRoot\fetch-openvr.ps1"

if (-not (Test-Path "$Root\thirdparty\openvr\headers\openvr.h")) {
  throw "OpenVR missing - run: .\scripts\fetch-openvr.ps1"
}

$openXrSource = Get-Content -LiteralPath "$Root\src\asi\openxr_bridge.cpp" -Raw
foreach ($forbidden in @(
  "LogVrDisplayInfo(",
  "GetCoverFovTangents(",
  "GetEyeRawProjection(",
  "GetNativeFovInsetBounds(",
  "vr::",
  "VR_Init("
)) {
  if ($openXrSource.Contains($forbidden)) {
    throw "OpenXR isolation check failed: openxr_bridge.cpp contains '$forbidden'"
  }
}
Write-Host "OpenXR isolation source check: PASS (no OpenVR API/display calls)"

$stereoConfigSource = Get-Content -LiteralPath "$Root\src\asi\stereo_config.cpp" -Raw
if (-not $stereoConfigSource.Contains(
    "if (!directOpenXr)")) {
  throw "OpenXR isolation check failed: direct mode defaults may query OpenVR IPD"
}

$stereoRenderSource = Get-Content -LiteralPath "$Root\src\asi\stereo_render.cpp" -Raw
if (-not $stereoRenderSource.Contains(
    "ComputeOpenXrEyeRawTangents(")) {
  throw "OpenXR frame isolation check failed: direct modes have no OpenXR per-eye FOV path"
}
if ($stereoRenderSource.Contains(
    "if (!GetEyeRawProjection(eye, &l, &r, &t, &b)")) {
  throw "OpenXR frame isolation check failed: canvas still calls OpenVR projection directly"
}
foreach ($requiredMonoSource in @(
  "CaptureOpenXrWorldMonoPair(",
  "StereoMode::OpenXrImmersiveMono",
  "StereoMono: center-eye capture pair",
  "RunOpenXrDrawSceneStereoGuarded(",
  "StereoMode::OpenXrDrawSceneStereo",
  "StereoOpenXR: DrawScene L/R pair",
  "StereoMode::OpenXrTemporalStereo",
  "StereoOpenXRTemporal: pair #%u"
)) {
  if (-not $stereoRenderSource.Contains($requiredMonoSource)) {
    throw "OpenXR direct-render source check failed: missing '$requiredMonoSource'"
  }
}
foreach ($requiredBridgeSource in @(
  "pair.verifiedDrawSceneStereo",
  "pair.verifiedTemporalStereo",
  "StereoMode::OpenXrDrawSceneStereo",
  "StereoMode::OpenXrTemporalStereo",
  "CpuFrameEyeCount",
  "sampleLockedBgraHash(",
  "rejected exact-mono L/R world readback",
  "pixelDistinct=%d"
)) {
  if (-not $openXrSource.Contains($requiredBridgeSource)) {
    throw "OpenXR stereo-mailbox source check failed: missing '$requiredBridgeSource'"
  }
}
foreach ($requiredStereoProofSource in @(
  "cameraBaselineValid",
  "GetStereoCamApplyGeneration()",
  "RawStereoMinRgbAbsDiff",
  "rawRgbAbsDiff=%lld proof=draw-scene",
  "g_dualDoneThisFrame = true;"
)) {
  if (-not $stereoRenderSource.Contains($requiredStereoProofSource)) {
    throw "OpenXR stereo proof source check failed: missing '$requiredStereoProofSource'"
  }
}
foreach ($requiredTemporalProofSource in @(
  "TemporalStereoMinRgbAbsDiff",
  "TemporalStereoMinChangedPixels",
  "TemporalStereoMinChangedTiles",
  "changedPixels=%u changedTiles=%u",
  "BeginPinnedOpenXrBuildPose(",
  "BeginStereoCamBuildReceipt(",
  "InstallRootProbeHooks(2)",
  "StereoMode57BeginScene(",
  "g_openXrTemporalBuildTickets",
  "g_openXrTemporalSceneTickets",
  "g_openXrTemporalStructuralFailure",
  "g_openXrTemporalExecPrimed",
  "g_openXrTemporalBuildTickets.resetQuiescent()",
  "g_openXrTemporalSceneTickets.resetQuiescent()",
  "BuildRootA waiting for first ExecRoot",
  "first ExecRoot publishing invalid alignment token",
  "ExecRoot -> later successful BeginScene underflow",
  "rightTicket.buildEpoch == leftTicket.buildEpoch + 1u",
  "rightStamp.poseSequence > leftStamp.poseSequence",
  "rightStamp.renderedDisplayTime >",
  "cameraReceipt.eyeOffsetApplied",
  "offsetMagnitude(rightTicket.cameraEyeOffset)",
  "rightTicket.cameraEyeOffsetLocal[0]",
  "appliedOffsetsMatchSelected",
  "leftTicket.cameraEyeOffsetApplied",
  "PromoteOpenXrTemporalReceiptPair(",
  "output->verifiedTemporalStereo = g_openXrPairVerifiedTemporal",
  "proof=buildroot-execroot-fifo-beginscene-endscene-entry"
)) {
  if (-not $stereoRenderSource.Contains($requiredTemporalProofSource)) {
    throw "OpenXR temporal-stereo proof source check failed: missing '$requiredTemporalProofSource'"
  }
}
foreach ($forbiddenTemporalProofSource in @(
  "ticket.execThreadId == d3dThreadId",
  "ticket.buildThreadId != ticket.execThreadId",
  "leftTicket.execThreadId == g_holdOpenXrD3dThreadId[0]"
)) {
  if ($stereoRenderSource.Contains($forbiddenTemporalProofSource)) {
    throw "OpenXR temporal-stereo proof assumes a false cross-lane thread relationship: '$forbiddenTemporalProofSource'"
  }
}
$temporalExecStart = $stereoRenderSource.IndexOf(
  "void RunOpenXrTemporalExecRoot(")
$temporalExecEnd = $stereoRenderSource.IndexOf(
  "void __fastcall HookRoot0(",
  $temporalExecStart + 1)
if ($temporalExecStart -lt 0 -or
    $temporalExecEnd -le $temporalExecStart) {
  throw "OpenXR temporal-stereo ExecRoot function boundary was not found"
}
$temporalExecSource = $stereoRenderSource.Substring(
  $temporalExecStart,
  $temporalExecEnd - $temporalExecStart)
if ([regex]::Matches(
      $temporalExecSource,
      'g_origRoot\[0\]\(self, edx\);').Count -ne 1 -or
    -not $temporalExecSource.Contains(
      "g_openXrTemporalSceneTickets.push(")) {
  throw "OpenXR temporal-stereo owned ExecRoot lane is not one native dispatch after one scene-token publish path"
}
if ($stereoRenderSource -notmatch
    '(?s)mode == StereoMode::OpenXrDrawSceneStereo.*?' +
    'g_dualDoneThisFrame.*?' +
    'RunOpenXrDrawSceneStereoGuarded\(self, edx\).*?' +
    'g_dualDoneThisFrame = true;') {
  throw "OpenXR stereo proof source check failed: Mode56 is not guarded once per EndScene"
}
Write-Host "OpenXR frame isolation source check: PASS (Mode55 mono + Mode56 diagnostic + Mode57 ordered temporal L/R mailbox)"

$hooksSource = Get-Content -LiteralPath "$Root\src\asi\hooks.cpp" -Raw
foreach ($requiredResetSource in @(
  "HookReset(",
  "vt[16]",
  "StereoNotifyDeviceLost();"
)) {
  if (-not $hooksSource.Contains($requiredResetSource)) {
    throw "D3D9 reset-safety source check failed: missing '$requiredResetSource'"
  }
}
if (([regex]::Matches(
      $hooksSource,
      [regex]::Escape("StereoNotifyDeviceLost();"))).Count -lt 2) {
  throw "D3D9 reset-safety source check failed: eye resources must clear before and after Reset"
}
foreach ($requiredResetRenderSource in @(
  "TestCooperativeLevel()",
  "D3DERR_DEVICELOST",
  "D3DERR_DEVICENOTRESET"
)) {
  if (-not $stereoRenderSource.Contains($requiredResetRenderSource)) {
    throw "D3D9 reset-safety source check failed: missing '$requiredResetRenderSource'"
  }
}
Write-Host "D3D9 reset-safety source check: PASS (slot 16 + before/after release + cooperative gate)"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found" }

$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VS with C++ x86 tools not found" }

$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvarsall.bat"
$outDir = Join-Path $Root "out-asi"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$src = @(
  "src\asi\dllmain.cpp",
  "src\asi\log.cpp",
  "src\asi\perf_debug.cpp",
  "src\asi\hooks.cpp",
  "src\asi\openxr_bridge.cpp",
  "src\asi\openxr_controller.cpp",
  "src\asi\openxr_pose_client.cpp",
  "src\asi\openvr_mono.cpp",
  "src\asi\hmd_look.cpp",
  "src\asi\hmd_pose.cpp",
  "src\asi\aob.cpp",
  "src\asi\cam_matrix.cpp",
  "src\asi\ped_hide.cpp",
  "src\asi\vr_move.cpp",
  "src\asi\look_move.cpp",
  "src\asi\vr_display.cpp",
  "src\asi\stereo_eye.cpp",
  "src\asi\stereo_config.cpp",
  "src\asi\stereo_draw_patch.cpp",
  "src\asi\stereo_dual.cpp",
  "src\asi\stereo_render.cpp",
  "src\asi\stereo_proj.cpp",
  "src\asi\ui_state.cpp",
  "thirdparty\minhook\src\buffer.c",
  "thirdparty\minhook\src\hook.c",
  "thirdparty\minhook\src\trampoline.c",
  "thirdparty\minhook\src\hde\hde32.c"
)

$includes = @(
  "/I$Root\src\asi",
  "/I$Root\thirdparty\dxvk",
  "/I$Root\thirdparty\vulkan-headers\include",
  "/I$Root\thirdparty\minhook\include",
  "/I$Root\thirdparty\minhook\src",
  "/I$Root\thirdparty\openvr\headers"
)

$libpath = "/LIBPATH:$Root\thirdparty\openvr\lib\win32"

$cmd = @"
call "$vcvars" x86
cd /d "$Root"
cl /nologo /EHsc /O2 /MD /W4 /WX /std:c++17 tests\gtaiv_stereo_wvp_test.cpp /Fo:"$outDir\gtaiv_stereo_wvp_test.obj" /Fe:"$outDir\gtaiv_stereo_wvp_test.exe"
if errorlevel 1 exit /b 1
"$outDir\gtaiv_stereo_wvp_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /O2 /MD /W4 /WX /std:c++17 tests\gtaiv_openxr_controller_test.cpp /Fo:"$outDir\gtaiv_openxr_controller_test.obj" /Fe:"$outDir\gtaiv_openxr_controller_test.exe"
if errorlevel 1 exit /b 1
"$outDir\gtaiv_openxr_controller_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /O2 /MD /W3 /std:c++17 /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE $($includes -join ' ') $($src -join ' ') /link /DLL /OUT:"$outDir\gtaiv_dxvk_vr.dll" $libpath /DELAYLOAD:openvr_api.dll openvr_api.lib delayimp.lib d3d9.lib d3d11.lib dxgi.lib user32.lib shell32.lib psapi.lib xinput.lib
if errorlevel 1 exit /b 1
copy /Y "$outDir\gtaiv_dxvk_vr.dll" "$outDir\gtaiv_dxvk_vr.asi"
copy /Y "$Root\thirdparty\openvr\bin\win32\openvr_api.dll" "$outDir\openvr_api.dll"
echo Built: $outDir\gtaiv_dxvk_vr.asi
"@

$bat = Join-Path $env:TEMP "build-gtaiv-asi.bat"
Set-Content -Path $bat -Value $cmd -Encoding ASCII
cmd /c $bat
if ($LASTEXITCODE -ne 0) { throw "ASI build failed" }

$AsiPath = Join-Path $outDir "gtaiv_dxvk_vr.asi"
$bytes = [IO.File]::ReadAllBytes($AsiPath)
if ($bytes.Length -lt 256) {
  throw "Built ASI is not a valid PE file: $AsiPath"
}
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
$machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
if ($machine -ne 0x014c) {
  throw ("Built ASI machine is 0x{0:X4}; expected x86 0x014C." -f $machine)
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $AsiPath).Hash

Write-Host "OK: $AsiPath + openvr_api.dll"
Write-Host ("PE:  0x{0:X4} (x86)" -f $machine)
Write-Host "SHA256: $hash"

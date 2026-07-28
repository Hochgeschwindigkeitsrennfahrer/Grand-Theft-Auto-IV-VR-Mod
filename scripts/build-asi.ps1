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

# Extract one C/C++ brace-delimited block while ignoring braces in comments,
# string literals and character literals. Mixed OpenXR/OpenVR translation units
# intentionally retain the fallback; isolation checks below must therefore be
# scoped to the exact functions/branches used by the direct OpenXR route.
function Get-CppBraceBlock {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$StartToken,
    [Parameter(Mandatory = $true)][string]$Label
  )

  $start = $Source.IndexOf($StartToken, [StringComparison]::Ordinal)
  if ($start -lt 0) {
    throw "Source contract failed: $Label start token '$StartToken' was not found"
  }
  $open = $Source.IndexOf("{", $start, [StringComparison]::Ordinal)
  if ($open -lt 0) {
    throw "Source contract failed: $Label opening brace was not found"
  }

  $depth = 0
  $inLineComment = $false
  $inBlockComment = $false
  $inString = $false
  $inCharacter = $false
  $escaped = $false
  for ($i = $open; $i -lt $Source.Length; ++$i) {
    $current = $Source[$i]
    $next = if ($i + 1 -lt $Source.Length) { $Source[$i + 1] } else { [char]0 }

    if ($inLineComment) {
      if ($current -eq [char]10) {
        $inLineComment = $false
      }
      continue
    }
    if ($inBlockComment) {
      if ($current -eq "*" -and $next -eq "/") {
        $inBlockComment = $false
        ++$i
      }
      continue
    }
    if ($inString -or $inCharacter) {
      if ($escaped) {
        $escaped = $false
        continue
      }
      if ($current -eq "\") {
        $escaped = $true
        continue
      }
      if ($inString -and $current -eq '"') {
        $inString = $false
      } elseif ($inCharacter -and $current -eq [char]39) {
        $inCharacter = $false
      }
      continue
    }

    if ($current -eq "/" -and $next -eq "/") {
      $inLineComment = $true
      ++$i
      continue
    }
    if ($current -eq "/" -and $next -eq "*") {
      $inBlockComment = $true
      ++$i
      continue
    }
    if ($current -eq '"') {
      $inString = $true
      continue
    }
    if ($current -eq [char]39) {
      $inCharacter = $true
      continue
    }
    if ($current -eq "{") {
      ++$depth
      continue
    }
    if ($current -eq "}") {
      --$depth
      if ($depth -eq 0) {
        return $Source.Substring($start, $i - $start + 1)
      }
    }
  }
  throw "Source contract failed: $Label closing brace was not found"
}

# Return only executable C/C++ tokens. Comments and literal contents become
# whitespace so a comment such as "must never call VRSystem()" cannot either
# satisfy or fail an API-call isolation assertion.
function Get-CppExecutableText {
  param(
    [Parameter(Mandatory = $true)][string]$Source
  )

  $output = New-Object System.Text.StringBuilder
  $inLineComment = $false
  $inBlockComment = $false
  $inString = $false
  $inCharacter = $false
  $escaped = $false
  for ($i = 0; $i -lt $Source.Length; ++$i) {
    $current = $Source[$i]
    $next = if ($i + 1 -lt $Source.Length) { $Source[$i + 1] } else { [char]0 }

    if ($inLineComment) {
      if ($current -eq [char]10) {
        $inLineComment = $false
        [void]$output.Append($current)
      } else {
        [void]$output.Append(" ")
      }
      continue
    }
    if ($inBlockComment) {
      if ($current -eq "*" -and $next -eq "/") {
        $inBlockComment = $false
        [void]$output.Append("  ")
        ++$i
      } elseif ($current -eq [char]10) {
        [void]$output.Append($current)
      } else {
        [void]$output.Append(" ")
      }
      continue
    }
    if ($inString -or $inCharacter) {
      if ($current -eq [char]10) {
        [void]$output.Append($current)
      } else {
        [void]$output.Append(" ")
      }
      if ($escaped) {
        $escaped = $false
        continue
      }
      if ($current -eq "\") {
        $escaped = $true
        continue
      }
      if ($inString -and $current -eq '"') {
        $inString = $false
      } elseif ($inCharacter -and $current -eq [char]39) {
        $inCharacter = $false
      }
      continue
    }

    if ($current -eq "/" -and $next -eq "/") {
      $inLineComment = $true
      [void]$output.Append("  ")
      ++$i
      continue
    }
    if ($current -eq "/" -and $next -eq "*") {
      $inBlockComment = $true
      [void]$output.Append("  ")
      ++$i
      continue
    }
    if ($current -eq '"') {
      $inString = $true
      [void]$output.Append(" ")
      continue
    }
    if ($current -eq [char]39) {
      $inCharacter = $true
      [void]$output.Append(" ")
      continue
    }
    [void]$output.Append($current)
  }
  return $output.ToString()
}

function Assert-SourceTokens {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string[]]$Required,
    [Parameter(Mandatory = $true)][string]$Label
  )
  foreach ($token in $Required) {
    if (-not $Source.Contains($token)) {
      throw "Source contract failed: $Label is missing '$token'"
    }
  }
}

function Assert-SourceExcludes {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string[]]$Forbidden,
    [Parameter(Mandatory = $true)][string]$Label
  )
  foreach ($token in $Forbidden) {
    if ($Source.Contains($token)) {
      throw "Source contract failed: $Label contains forbidden '$token'"
    }
  }
}

function Assert-SourceTokenOrder {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string[]]$Tokens,
    [Parameter(Mandatory = $true)][string]$Label
  )
  $offset = 0
  foreach ($token in $Tokens) {
    $index = $Source.IndexOf(
      $token,
      $offset,
      [StringComparison]::Ordinal)
    if ($index -lt 0) {
      throw "Source contract failed: $Label does not contain '$token' in the required order"
    }
    $offset = $index + $token.Length
  }
}

$legacyOpenVrApiTokens = @(
  "VRSystem(",
  "VRCompositor(",
  "VR_Init("
)

$openXrSource = Get-Content -LiteralPath "$Root\src\asi\openxr_bridge.cpp" -Raw
$stereoConfigHeaderSource =
    Get-Content -LiteralPath "$Root\src\asi\stereo_config.h" -Raw
$stereoConfigSource =
    Get-Content -LiteralPath "$Root\src\asi\stereo_config.cpp" -Raw
$stereoRenderHeaderSource =
    Get-Content -LiteralPath "$Root\src\asi\stereo_render.h" -Raw
$stereoRenderSource =
    Get-Content -LiteralPath "$Root\src\asi\stereo_render.cpp" -Raw
$hmdPoseSource =
    Get-Content -LiteralPath "$Root\src\asi\hmd_pose.cpp" -Raw
$camMatrixHeaderSource =
    Get-Content -LiteralPath "$Root\src\asi\cam_matrix.h" -Raw
$camMatrixSource =
    Get-Content -LiteralPath "$Root\src\asi\cam_matrix.cpp" -Raw
$pedHideSource =
    Get-Content -LiteralPath "$Root\src\asi\ped_hide.cpp" -Raw
$frameBridgeSource =
    Get-Content -LiteralPath "$Root\src\bridge\gtaiv_xr_frame_bridge.h" -Raw
$hostGameBridgeSource =
    Get-Content -LiteralPath "$Root\src\openxr_host\game_bridge.cpp" -Raw
$hooksSource =
    Get-Content -LiteralPath "$Root\src\asi\hooks.cpp" -Raw

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

if (-not $stereoConfigSource.Contains(
    "if (!directOpenXr)")) {
  throw "OpenXR isolation check failed: direct mode defaults may query OpenVR IPD"
}

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
  "cpu_readback_batch::queueAllBeforeLock(",
  "cpu_temporal_readback::ScheduleDecision::StageLeft",
  "sampleLockedBgraHash(",
  "rejected exact-mono L/R world readback",
  "pixelDistinct=%d",
  "enqueueMs=%.2f waitMs=%.2f",
  "split=%d leftQueueMs=%.2f phaseGapMs=%.2f",
  "phasePose=%llu/%llu phaseCall=%llu/%llu",
  "phaseEpoch=%llu atomic=1",
  "TemporalReadbackMaxPendingAgeMs",
  "cpuMailbox_->slotTransactionId[slot] = 0u;"
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

# ---- Mode 58: Mode 204 parent-dual renderer -> direct OpenXR CPU mailbox ----

Assert-SourceTokens `
    -Source $stereoConfigHeaderSource `
    -Label "Mode58 enum" `
    -Required @(
      "OpenXrFusedFirstPerson = 58,",
      "bool IsOpenXrDirectMode(StereoMode mode);",
      "bool IsOpenXrFusedFirstPerson(StereoMode mode);",
      "bool UsesMode204ParentDualPath(StereoMode mode);"
    )

$directModePredicate = Get-CppBraceBlock `
    -Source $stereoConfigSource `
    -StartToken "bool IsOpenXrDirectMode(StereoMode mode) {" `
    -Label "IsOpenXrDirectMode"
$mode58Predicate = Get-CppBraceBlock `
    -Source $stereoConfigSource `
    -StartToken "bool IsOpenXrFusedFirstPerson(StereoMode mode) {" `
    -Label "IsOpenXrFusedFirstPerson"
$mode204PathPredicate = Get-CppBraceBlock `
    -Source $stereoConfigSource `
    -StartToken "bool UsesMode204ParentDualPath(StereoMode mode) {" `
    -Label "UsesMode204ParentDualPath"
$temporalModePredicate = Get-CppBraceBlock `
    -Source $stereoConfigSource `
    -StartToken "bool IsTemporalStereoMode(StereoMode mode) {" `
    -Label "IsTemporalStereoMode"
Assert-SourceTokens `
    -Source $directModePredicate `
    -Label "Mode58 direct-OpenXR predicate" `
    -Required @("StereoMode::OpenXrFusedFirstPerson")
Assert-SourceTokens `
    -Source $mode58Predicate `
    -Label "Mode58 exact predicate" `
    -Required @(
      "return mode == StereoMode::OpenXrFusedFirstPerson;"
    )
Assert-SourceTokens `
    -Source $mode204PathPredicate `
    -Label "Mode58 Mode204 parent-path predicate" `
    -Required @(
      "IsOpenXrFusedFirstPerson(mode)",
      "StereoMode::OursFpSameTickParentDualTry2"
    )
Assert-SourceExcludes `
    -Source $temporalModePredicate `
    -Label "Mode58 temporal isolation" `
    -Forbidden @(
      "OpenXrFusedFirstPerson",
      "OpenXrTemporalStereo"
    )

$mode58ConfigEnterBlock = Get-CppBraceBlock `
    -Source $stereoConfigSource `
    -StartToken "IsOpenXrFusedFirstPerson(static_cast<StereoMode>(v))) {" `
    -Label "Mode58 config-enter branch"
Assert-SourceTokens `
    -Source $mode58ConfigEnterBlock `
    -Label "Mode58 config-enter branch" `
    -Required @(
      "ReloadIpdScale();",
      "ReloadStereoScale();",
      "ApplyMode162SquareWideBase();",
      "ForceFpFovDegrees(110, 110, 110);",
      "EnsureVehicleCamOffDefaults();",
      "EnsureHudOffDefaults();",
      "SetWorldScalePercent(100);",
      "fallback=57"
    )
Assert-SourceExcludes `
    -Source (Get-CppExecutableText $mode58ConfigEnterBlock) `
    -Label "Mode58 config-enter branch" `
    -Forbidden @(
      "ApplyGeometryCanvasDefaults(",
      "ReadHmdIpdMeters(",
      "PollVrResHotkey("
    )
Assert-SourceExcludes `
    -Source (Get-CppExecutableText $mode58ConfigEnterBlock) `
    -Label "Mode58 config-enter OpenVR API isolation" `
    -Forbidden $legacyOpenVrApiTokens

$parentTargetBlock = Get-CppBraceBlock `
    -Source $stereoRenderSource `
    -StartToken "uint32_t ParentDualTargetRva() {" `
    -Label "Mode204 parent target selector"
Assert-SourceTokens `
    -Source $stereoRenderSource `
    -Label "Mode204 exact parent RVA" `
    -Required @(
      "constexpr uint32_t kMode204TargetRva = 0x4DE020;"
    )
if (([regex]::Matches(
      $stereoRenderSource,
      'kMode204TargetRva\s*=\s*0x4DE020\s*;')).Count -ne 1) {
  throw "Source contract failed: Mode204 parent RVA must have one exact 0x4DE020 definition"
}
Assert-SourceTokens `
    -Source $parentTargetBlock `
    -Label "Mode58 exact Mode204 parent target" `
    -Required @(
      "UsesMode204ParentDualPath(GetStereoMode())",
      "? kMode204TargetRva",
      ": kMode200TargetRva"
    )

$canvasSizeBlock = Get-CppBraceBlock `
    -Source $stereoRenderSource `
    -StartToken "void ComputeCanvasSize(" `
    -Label "ComputeCanvasSize"
Assert-SourceTokens `
    -Source $frameBridgeSource `
    -Label "fixed OpenXR CPU canvas constants" `
    -Required @(
      "CpuFrameMaxWidth = 1280u;",
      "CpuFrameMaxHeight = 720u;"
    )
Assert-SourceTokens `
    -Source $canvasSizeBlock `
    -Label "Mode58 fixed CPU canvas" `
    -Required @(
      "StereoMode::OpenXrFusedFirstPerson",
      "*outW = gtaiv_xr_bridge::CpuFrameMaxWidth;",
      "*outH = gtaiv_xr_bridge::CpuFrameMaxHeight;",
      "OpenXR CPU mailbox=%ux%u"
    )
Assert-SourceTokenOrder `
    -Source $canvasSizeBlock `
    -Label "Mode58 fixed CPU canvas early return" `
    -Tokens @(
      "StereoMode::OpenXrFusedFirstPerson",
      "*outW = gtaiv_xr_bridge::CpuFrameMaxWidth;",
      "*outH = gtaiv_xr_bridge::CpuFrameMaxHeight;",
      "return;",
      "GetCanvasCoverFovTangents("
    )

$canvasCoverBlock = Get-CppBraceBlock `
    -Source $stereoRenderSource `
    -StartToken "bool GetCanvasCoverFovTangents(" `
    -Label "GetCanvasCoverFovTangents"
$canvasEyeBlock = Get-CppBraceBlock `
    -Source $stereoRenderSource `
    -StartToken "bool GetCanvasEyeRawProjection(" `
    -Label "GetCanvasEyeRawProjection"
Assert-SourceTokens `
    -Source $canvasCoverBlock `
    -Label "direct OpenXR cover FOV" `
    -Required @(
      "if (!IsOpenXrDirectMode(GetStereoMode()))",
      "return GetCoverFovTangents(",
      "g_openXrCanvasPoseOverride",
      "ComputeOpenXrCoverTangents("
    )
Assert-SourceTokens `
    -Source $canvasEyeBlock `
    -Label "direct OpenXR per-eye FOV" `
    -Required @(
      "if (!IsOpenXrDirectMode(GetStereoMode()))",
      "return GetEyeRawProjection(",
      "g_openXrCanvasPoseOverride",
      "ComputeOpenXrEyeRawTangents("
    )

$hookDrawWalkBlock = Get-CppBraceBlock `
    -Source $stereoRenderSource `
    -StartToken "void __fastcall HookDrawWalk(void* self, void* edx) {" `
    -Label "HookDrawWalk"
Assert-SourceTokens `
    -Source $hookDrawWalkBlock `
    -Label "Mode58 pinned parent-dual pose" `
    -Required @(
      "IsOpenXrFusedFirstPerson(parentMode)",
      "GetLatestOpenXrPoseBridge(&mode58Pose)",
      "BeginPinnedOpenXrBuildPose(mode58Pose)",
      "CaptureOpenXrStampFromPose(mode58Pose)",
      "g_openXrCanvasPoseOverride = &mode58Pose;",
      "RunMode200ParentDualGuarded(",
      "g_openXrCanvasPoseOverride = nullptr;",
      "EndPinnedOpenXrBuildPose();",
      "if (poseLatch && !openXrFirstPerson)"
    )
Assert-SourceTokenOrder `
    -Source $hookDrawWalkBlock `
    -Label "Mode58 pinned pose/canvas lifetime" `
    -Tokens @(
      "GetLatestOpenXrPoseBridge(&mode58Pose)",
      "BeginPinnedOpenXrBuildPose(mode58Pose)",
      "CaptureOpenXrStampFromPose(mode58Pose)",
      "g_openXrCanvasPoseOverride = &mode58Pose;",
      "RunMode200ParentDualGuarded(",
      "g_openXrCanvasPoseOverride = nullptr;",
      "EndPinnedOpenXrBuildPose();"
    )

$decodeOpenXrPoseBlock = Get-CppBraceBlock `
    -Source $hmdPoseSource `
    -StartToken "bool DecodeOpenXrPose(" `
    -Label "DecodeOpenXrPose"
$beginPinnedPoseBlock = Get-CppBraceBlock `
    -Source $hmdPoseSource `
    -StartToken "bool BeginPinnedOpenXrBuildPose(" `
    -Label "BeginPinnedOpenXrBuildPose"
$cachedEyeOffsetBlock = Get-CppBraceBlock `
    -Source $hmdPoseSource `
    -StartToken "bool GetCachedEyeOffset(" `
    -Label "GetCachedEyeOffset"
$cachedHmdPoseBlock = Get-CppBraceBlock `
    -Source $hmdPoseSource `
    -StartToken "bool GetHmdPoseMatrix(" `
    -Label "GetHmdPoseMatrix"
Assert-SourceTokens `
    -Source $beginPinnedPoseBlock `
    -Label "pinned OpenXR build pose" `
    -Required @(
      "DecodeOpenXrPose(",
      "g_pinnedBuildMatrix = matrix;",
      "g_pinnedBuildEyeL = offsets[0];",
      "g_pinnedBuildEyeR = offsets[1];",
      "g_buildPosePinned = true;"
    )
Assert-SourceTokenOrder `
    -Source $cachedEyeOffsetBlock `
    -Label "pinned OpenXR eye offset priority" `
    -Tokens @(
      "if (g_buildPosePinned)",
      "g_pinnedBuildEyeR",
      "if (!g_eyeOk.load())",
      "g_eyeR"
    )
Assert-SourceTokenOrder `
    -Source $cachedHmdPoseBlock `
    -Label "pinned OpenXR HMD pose priority" `
    -Tokens @(
      "if (g_buildPosePinned)",
      "g_pinnedBuildMatrix",
      "g_valid.load()"
    )

$applyHmdToCamBlock = Get-CppBraceBlock `
    -Source $camMatrixSource `
    -StartToken "void ApplyHmdToCam(Matrix44* mat) {" `
    -Label "ApplyHmdToCam"
Assert-SourceTokens `
    -Source $camMatrixHeaderSource `
    -Label "StereoCamBuildReceipt first-person evidence fields" `
    -Required @(
      "struct StereoCamBuildReceipt",
      "uint32_t applyGeneration = 0u;",
      "uint32_t writerThreadId = 0u;",
      "bool rightEye = false;",
      "bool eyeOffsetApplied = false;",
      "bool firstPersonAnchor = false;",
      "float preEyePosition[3] = {};",
      "float basis[9] = {};"
    )
Assert-SourceTokens `
    -Source $applyHmdToCamBlock `
    -Label "Mode58 camera receipt writer" `
    -Required @(
      "GetCachedEyeOffset(rightEye, &eyeOffset)",
      "StereoMode::OpenXrFusedFirstPerson",
      "g_camBuildReceiptScope.applyGeneration = n;",
      "g_camBuildReceiptScope.lastPreEyePosition",
      "eyeCenter && IsOpenXrFusedFirstPerson(sm)",
      "g_camBuildReceiptScope.lastBasis"
    )

$parentDualBlock = Get-CppBraceBlock `
    -Source $stereoRenderSource `
    -StartToken "const OpenXrCaptureStamp* mode58Stamp) {" `
    -Label "RunMode200ParentDualGuarded"
Assert-SourceTokens `
    -Source $parentDualBlock `
    -Label "Mode58 fresh first-person parent-dual gate" `
    -Required @(
      "StereoCamBuildReceipt leftReceipt{};",
      "StereoCamBuildReceipt rightReceipt{};",
      "GetStereoCamApplyGeneration();",
      "BeginStereoCamBuildReceipt(false);",
      "BeginStereoCamBuildReceipt(true);",
      "EndStereoCamBuildReceipt(&leftReceipt)",
      "EndStereoCamBuildReceipt(&rightReceipt)",
      "leftReceipt.applyGeneration >",
      "rightReceipt.applyGeneration >",
      "rightReceipt.applyGeneration >",
      "leftReceipt.writerThreadId == currentThread",
      "rightReceipt.writerThreadId == currentThread",
      "!leftReceipt.rightEye",
      "rightReceipt.rightEye",
      "leftReceipt.eyeOffsetApplied",
      "rightReceipt.eyeOffsetApplied",
      "leftReceipt.firstPersonAnchor",
      "rightReceipt.firstPersonAnchor",
      "distanceMeters >= 0.01f",
      "distanceMeters <= 0.20f",
      "centerDistanceMeters <= 0.005f",
      "maxBasisDelta <= 0.002f",
      "IsPedHeadHideOperational();",
      "firstPersonCameraPair &&",
      "headHideReady &&",
      "okL && okR",
      "CompleteOpenXrPair(",
      "StereoOpenXRFused: accepted pair #%u",
      "sameTick=1 ",
      "parentDual=1 firstPerson=1 headHide=1 ",
      "parent=0x%X"
    )
$headHideOperationalBlock = Get-CppBraceBlock `
    -Source $pedHideSource `
    -StartToken "bool IsPedHeadHideOperational() {" `
    -Label "IsPedHeadHideOperational"
Assert-SourceTokens `
    -Source $headHideOperationalBlock `
    -Label "Mode58 native head-hide operational gate" `
    -Required @(
      "!g_dead.load()",
      "g_nativeHidePasses.load() > 0u",
      "g_nativeSuccessMode.load() ==",
      "WantsNativePedHide(mode)"
    )

$completePairBlock = Get-CppBraceBlock `
    -Source $stereoRenderSource `
    -StartToken "void CompleteOpenXrPair(" `
    -Label "CompleteOpenXrPair"
$acquirePairBlock = Get-CppBraceBlock `
    -Source $stereoRenderSource `
    -StartToken "bool StereoAcquireOpenXrPair(" `
    -Label "StereoAcquireOpenXrPair"
Assert-SourceTokens `
    -Source $stereoRenderHeaderSource `
    -Label "Mode58 producer pair proof fields" `
    -Required @(
      "bool verifiedParentDualStereo = false;",
      "bool firstPersonCamera = false;",
      "bool nativeHeadHidden = false;"
    )
Assert-SourceTokens `
    -Source $completePairBlock `
    -Label "Mode58 pair-proof dependency chain" `
    -Required @(
      "g_openXrPairVerifiedParentDual =",
      "g_openXrPairVerifiedDrawScene &&",
      "g_openXrPairFirstPersonCamera =",
      "g_openXrPairVerifiedParentDual &&",
      "g_openXrPairNativeHeadHidden =",
      "g_openXrPairFirstPersonCamera &&",
      "!g_openXrPairSameTick &&",
      "verifiedTemporalStereo"
    )
Assert-SourceTokens `
    -Source $acquirePairBlock `
    -Label "Mode58 OpenXR pair output" `
    -Required @(
      "output->verifiedParentDualStereo =",
      "g_openXrPairVerifiedParentDual;",
      "output->firstPersonCamera =",
      "g_openXrPairFirstPersonCamera;",
      "output->nativeHeadHidden =",
      "g_openXrPairNativeHeadHidden;"
    )
Assert-SourceTokens `
    -Source $frameBridgeSource `
    -Label "Mode58 shared protocol proof flags" `
    -Required @(
      "VerifiedParentDualStereo = 1u << 11u,",
      "FirstPersonCamera = 1u << 12u,",
      "NativeHeadHidden = 1u << 13u,"
    )

$producerPublishBlock = Get-CppBraceBlock `
    -Source $openXrSource `
    -StartToken "void publish(" `
    -Label "OpenXrFrameProducer::publish"
$cpuPublishBlock = Get-CppBraceBlock `
    -Source $openXrSource `
    -StartToken "bool publishCpuMailboxFrame(" `
    -Label "OpenXrFrameProducer::publishCpuMailboxFrame"
$producerDescriptorBlock = Get-CppBraceBlock `
    -Source $openXrSource `
    -StartToken "FrameBridge descriptor() const" `
    -Label "OpenXrFrameProducer::descriptor"
Assert-SourceTokens `
    -Source $producerPublishBlock `
    -Label "Mode58 CPU mailbox route" `
    -Required @(
      "stereoMode == StereoMode::OpenXrTemporalStereo;",
      "stereoMode",
      "== StereoMode::OpenXrFusedFirstPerson;",
      "fusedFirstPersonMode",
      "lease.pair.verifiedParentDualStereo",
      "lease.pair.firstPersonCamera",
      "lease.pair.nativeHeadHidden",
      "lease.pair.sameSimulationTick",
      "lease.pair.verifiedTemporalStereo",
      "stereoMode == StereoMode::OpenXrFusedFirstPerson;",
      "if (cpuMailboxRoute)",
      "publishCpuMailboxFrame("
    )
Assert-SourceTokens `
    -Source $cpuPublishBlock `
    -Label "Mode58 immediate atomic CPU mailbox" `
    -Required @(
      "pair.verifiedParentDualStereo",
      "const bool splitTemporalReadback =",
      "temporalStereoMode",
      "&& pair.verifiedTemporalStereo;",
      "immediate CPU mailbox route selected",
      "world-parent-dual-stereo",
      "parentDual=%d fp=%d headHide=%d",
      "atomic=1 ",
      "pixelDistinct=%d"
    )
Assert-SourceTokens `
    -Source $producerDescriptorBlock `
    -Label "Mode58 shared descriptor flags" `
    -Required @(
      "lastPair_.verifiedParentDualStereo",
      "gtaiv_xr_bridge::VerifiedParentDualStereo",
      "lastPair_.firstPersonCamera",
      "gtaiv_xr_bridge::FirstPersonCamera",
      "lastPair_.nativeHeadHidden",
      "gtaiv_xr_bridge::NativeHeadHidden"
    )

$hostQualityBlock = Get-CppBraceBlock `
    -Source $hostGameBridgeSource `
    -StartToken "TransactionQuality assessTransactionQuality(" `
    -Label "OpenXR host transaction quality"
Assert-SourceTokens `
    -Source $hostQualityBlock `
    -Label "Mode58 host proof and temporal exclusion" `
    -Required @(
      "gtaiv_xr_bridge::VerifiedParentDualStereo",
      "gtaiv_xr_bridge::FirstPersonCamera",
      "gtaiv_xr_bridge::NativeHeadHidden",
      "verifiedParentDualStereo",
      "&& firstPersonCamera",
      "&& nativeHeadHidden",
      "&& result.sameTick",
      "&& !temporalStereoFlag",
      "verifiedParentDualStereo != firstPersonCamera",
      "verifiedParentDualStereo != nativeHeadHidden"
    )

# Direct OpenXR runtime isolation is checked only on reachable mixed-source
# functions. Other functions in these files deliberately retain the OpenVR/G2
# fallback and are not globally banned.
$installStereoBlock = Get-CppBraceBlock `
    -Source $stereoRenderSource `
    -StartToken "bool InstallStereoRenderHooks() {" `
    -Label "InstallStereoRenderHooks"
$stereoOnDeviceBlock = Get-CppBraceBlock `
    -Source $stereoRenderSource `
    -StartToken "void StereoRenderOnDevice(" `
    -Label "StereoRenderOnDevice"
$hookEndSceneBlock = Get-CppBraceBlock `
    -Source $hooksSource `
    -StartToken "HRESULT STDMETHODCALLTYPE HookEndScene(" `
    -Label "HookEndScene"
$hookThreadBlock = Get-CppBraceBlock `
    -Source $hooksSource `
    -StartToken "DWORD WINAPI HookThread(" `
    -Label "HookThread"
Assert-SourceTokenOrder `
    -Source $hookEndSceneBlock `
    -Label "exclusive OpenXR/OpenVR EndScene dispatch" `
    -Tokens @(
      "if (asi::IsOpenXrBridgeRequested())",
      "asi::PollOpenXrPoseBridge();",
      "asi::PublishOpenXrFrame(self);",
      "else if (asi::IsOpenVrBridgeRequested())",
      "asi::TryMonoSubmit(self);"
    )
Assert-SourceTokenOrder `
    -Source $hookThreadBlock `
    -Label "exclusive OpenXR/OpenVR startup dispatch" `
    -Tokens @(
      "if (asi::IsOpenXrBridgeRequested())",
      "else if (asi::IsOpenVrBridgeRequested())",
      "asi::InitOpenVrEarly();"
    )
if (([regex]::Matches($hooksSource, '\bTryMonoSubmit\(')).Count -ne 1 -or
    ([regex]::Matches($hooksSource, '\bInitOpenVrEarly\(')).Count -ne 1) {
  throw "Source contract failed: OpenVR startup/submit calls escaped their exclusive backend branches"
}

foreach ($boundedDirectOpenXrPath in @(
  @{ Label = "Mode58 config enter"; Source = $mode58ConfigEnterBlock },
  @{ Label = "DecodeOpenXrPose"; Source = $decodeOpenXrPoseBlock },
  @{ Label = "BeginPinnedOpenXrBuildPose"; Source = $beginPinnedPoseBlock },
  @{ Label = "GetCachedEyeOffset"; Source = $cachedEyeOffsetBlock },
  @{ Label = "GetHmdPoseMatrix"; Source = $cachedHmdPoseBlock },
  @{ Label = "ApplyHmdToCam"; Source = $applyHmdToCamBlock },
  @{ Label = "GetCanvasCoverFovTangents"; Source = $canvasCoverBlock },
  @{ Label = "GetCanvasEyeRawProjection"; Source = $canvasEyeBlock },
  @{ Label = "ComputeCanvasSize"; Source = $canvasSizeBlock },
  @{ Label = "HookDrawWalk"; Source = $hookDrawWalkBlock },
  @{ Label = "RunMode200ParentDualGuarded"; Source = $parentDualBlock },
  @{ Label = "InstallStereoRenderHooks"; Source = $installStereoBlock },
  @{ Label = "StereoRenderOnDevice"; Source = $stereoOnDeviceBlock },
  @{ Label = "OpenXrFrameProducer::publish"; Source = $producerPublishBlock },
  @{ Label = "OpenXrFrameProducer::publishCpuMailboxFrame"; Source = $cpuPublishBlock },
  @{ Label = "HookEndScene"; Source = $hookEndSceneBlock },
  @{ Label = "HookThread"; Source = $hookThreadBlock }
)) {
  Assert-SourceExcludes `
      -Source (Get-CppExecutableText $boundedDirectOpenXrPath.Source) `
      -Label "$($boundedDirectOpenXrPath.Label) direct OpenXR API isolation" `
      -Forbidden $legacyOpenVrApiTokens
}

Write-Host (
  "OpenXR frame isolation source check: PASS " +
  "(Mode55 mono + Mode56 diagnostic + Mode57 ordered temporal L/R + " +
  "Mode58 Mode204 parent-dual first-person CPU mailbox; no OpenVR/temporal opt-in)")

foreach ($requiredResetSource in @(
  "HookReset(",
  "vt[16]",
  "NotifyOpenXrBridgeDeviceLost();",
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
  "src\asi\native_invoke.cpp",
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
  "src\asi\hud_layout.cpp",
  "src\asi\game_timer.cpp",
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
cl /nologo /EHsc /O2 /MD /W4 /WX /std:c++17 tests\gtaiv_cpu_readback_batch_test.cpp /Fo:"$outDir\gtaiv_cpu_readback_batch_test.obj" /Fe:"$outDir\gtaiv_cpu_readback_batch_test.exe"
if errorlevel 1 exit /b 1
"$outDir\gtaiv_cpu_readback_batch_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /O2 /MD /W4 /WX /std:c++17 tests\gtaiv_cpu_temporal_readback_test.cpp /Fo:"$outDir\gtaiv_cpu_temporal_readback_test.obj" /Fe:"$outDir\gtaiv_cpu_temporal_readback_test.exe"
if errorlevel 1 exit /b 1
"$outDir\gtaiv_cpu_temporal_readback_test.exe"
if errorlevel 1 exit /b 1
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

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$vrMovePath = Join-Path $root "src\asi\vr_move.cpp"
$lookMovePath = Join-Path $root "src\asi\look_move.cpp"
$controllerMapPath = Join-Path $root "src\asi\openxr_controller_map.h"

$vrMove = Get-Content -LiteralPath $vrMovePath -Raw
$lookMove = Get-Content -LiteralPath $lookMovePath -Raw
$controllerMap = Get-Content -LiteralPath $controllerMapPath -Raw

function Get-CppFunction {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Signature
  )

  $start = $Source.IndexOf($Signature, [StringComparison]::Ordinal)
  if ($start -lt 0) {
    throw "Missing C++ function signature: $Signature"
  }
  $open = $Source.IndexOf("{", $start, [StringComparison]::Ordinal)
  if ($open -lt 0) {
    throw "Missing opening brace for: $Signature"
  }

  $depth = 0
  for ($i = $open; $i -lt $Source.Length; $i++) {
    if ($Source[$i] -eq "{") {
      $depth++
    }
    elseif ($Source[$i] -eq "}") {
      $depth--
      if ($depth -eq 0) {
        return $Source.Substring($start, $i - $start + 1)
      }
    }
  }
  throw "Missing closing brace for: $Signature"
}

function Assert-OrderedTokens {
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
      [StringComparison]::Ordinal
    )
    if ($index -lt 0) {
      throw "$Label is missing ordered token: $token"
    }
    $offset = $index + $token.Length
  }
}

$updateVrMove = Get-CppFunction $vrMove "void UpdateVrMoveAndStick()"
$updateLookMove = Get-CppFunction $lookMove "void UpdateLookMove()"

foreach ($forbidden in @(
    "ConsumeYawDeltaSeconds",
    "QueryPerformanceCounter",
    "g_yawClockFrequency",
    "kMaxYawDeltaSeconds"
  )) {
  if ($vrMove.Contains($forbidden)) {
    throw "Mode204 movement no longer matches upstream: found $forbidden"
  }
}

Assert-OrderedTokens `
  -Source $updateVrMove `
  -Label "upstream Mode204 UpdateVrMoveAndStick" `
  -Tokens @(
    "EnsureFindPlayerPed();",
    "const float stick = ReadRightStickX();",
    "constexpr float kDt = 1.f / 60.f;",
    "GetFpJoySensScale() * kDt",
    "SendMouseDx(dx);",
    "if (!g_FindPlayerPed)",
    "WantsExternalFpLookMove(GetStereoMode())",
    "if (IsFreeMoveEnabled())",
    "ApplyPedHeading(ped, heading);"
  )

Assert-OrderedTokens `
  -Source $updateLookMove `
  -Label "upstream Mode204 UpdateLookMove" `
  -Tokens @(
    "UpdateVrMoveAndStick();",
    "if (!IsLookMoveEnabled())",
    "EnsureFindPlayerPed()",
    "ApplyPedHeadingSoft(ped, heading);",
    "ApplyPedHeading(ped, heading);"
  )

foreach ($forbidden in @(
    "IsOpenXrFusedFirstPerson",
    "Mode58",
    "game-owned ped"
  )) {
  if ($updateVrMove.Contains($forbidden) -or
      $updateLookMove.Contains($forbidden)) {
    throw "OpenXR renderer policy leaked into upstream movement: $forbidden"
  }
}

foreach ($required in @(
    "gamepad.sThumbLX = stickValue(left.thumbstickX);",
    "gamepad.sThumbLY = stickValue(left.thumbstickY);",
    "gamepad.sThumbRX = stickValue(right.thumbstickX);",
    "gamepad.sThumbRY = stickValue(right.thumbstickY);"
  )) {
  if (-not $controllerMap.Contains($required)) {
    throw "Touch-to-XInput stick mapping changed or is missing: $required"
  }
}

if ($controllerMap.Contains("applyMenuAssist") -or
    $controllerMap.Contains("elapsedMs < 250u")) {
  throw "Synthetic keyboard/menu input leaked into the OpenXR controller map."
}

Write-Output (
  "Mode204ProviderMovementContractTest: PASS " +
  "upstreamYaw=1 upstreamPedOwnership=1 " +
  "poseBridgeControllerOnly=1 touchXInputPreserved=1"
)

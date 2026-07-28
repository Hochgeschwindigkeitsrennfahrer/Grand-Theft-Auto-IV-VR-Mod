#include "hmd_look.h"
#include "cam_matrix.h"
#include "log.h"
#include "stereo_config.h"
#include "vr_move.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <openvr.h>

#include <algorithm>
#include <cmath>

namespace asi {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRad2Deg = 180.0f / kPi;

constexpr float kYawSensLegacy = 380.0f;
constexpr float kPitchSensLegacy = 380.0f;
constexpr float kMaxDeltaRadLegacy = 0.20f;
constexpr int kMaxMouseStepLegacy = 18;

constexpr float kYawSensFp = 720.0f;
constexpr float kPitchSensFp = 980.0f;
constexpr float kMaxDeltaRadFp = 0.55f;
constexpr int kMaxMouseStepFp = 96;

constexpr float kPitchSensFpUnlock = 1400.0f;
constexpr float kMaxDeltaRadFpUnlock = 0.85f;
constexpr int kMaxMouseStepFpUnlock = 120;

constexpr float kYawSensFpLookMove = 360.0f;
constexpr float kPitchSensFpLookMove = 520.0f;
constexpr float kPitchDeadzoneRad = 0.004f;
constexpr float kMaxDeltaRadLookMove = 0.70f;
constexpr int kMaxMouseStepLookMove = 72;

// Mode 147/148/150: full mouse cam (144 signs) + slow pitch; soft ped owns walk-dir.
constexpr float kYawSensSoft = 720.0f;
constexpr float kPitchSensSoft = 480.0f;

bool g_enabled = true;
bool g_pitchEnabled = true;
bool g_haveLast = false;
float g_lastYaw = 0.f;
float g_lastPitch = 0.f;
uint32_t g_logCounter = 0;
uint32_t g_frameSkip = 0;

bool KeyPressedEdge(int vk, bool* wasDown) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool edge = down && !*wasDown;
  *wasDown = down;
  return edge;
}

void SendMouseDelta(int dx, int dy) {
  if (dx == 0 && dy == 0)
    return;
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dx = dx;
  in.mi.dy = dy;
  in.mi.dwFlags = MOUSEEVENTF_MOVE;
  SendInput(1, &in, sizeof(INPUT));
}

void DoRecenter() {
  g_haveLast = false;
  CamMatrixOnRecenter();
  ResetControllerYawOffset();

  bool ok = false;
  if (vr::VRChaperone()) {
    vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseSeated);
    vr::VRChaperone()->ResetZeroPose(vr::TrackingUniverseStanding);
    ok = true;
  }
  if (vr::VRCompositor()) {
    vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseSeated);
    ok = true;
  }
  Log(ok ? "Recenter: F9 — SteamVR zero + ped/veh heading baseline (6DoF unchanged)"
         : "Recenter: F9 — ped/veh heading baseline (6DoF unchanged)");
}

void YawPitchFromMatrix(const vr::HmdMatrix34_t& m, float* yaw, float* pitch) {
  const float fx = m.m[0][2];
  const float fy = m.m[1][2];
  const float fz = m.m[2][2];
  *yaw = std::atan2(fx, fz);
  const float horiz = std::sqrt(fx * fx + fz * fz);
  *pitch = std::atan2(-fy, (std::max)(horiz, 1e-5f));
}

float AngleDelta(float cur, float last) {
  float d = cur - last;
  while (d > kPi)
    d -= 2.f * kPi;
  while (d < -kPi)
    d += 2.f * kPi;
  return d;
}

bool IsGameForeground() {
  HWND fg = GetForegroundWindow();
  if (!fg)
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

struct LookTune {
  float yawSens;
  float pitchSens;
  float maxDeltaRad;
  int maxStep;
  float yawSign;
  float pitchSign;
  bool everyFrame;
  bool perAxisClamp;
  float pitchDeadzone;
  bool mouseYaw;
  const char* tag;
};

LookTune TuneForMode(StereoMode sm) {
  if (IsExternalFpHostSoftMoveStick(sm) || IsExternalFpHostSoftMove(sm) ||
      IsHeadHideNativeWithFp(sm)) {
    return {kYawSensSoft, kPitchSensSoft, kMaxDeltaRadLookMove, kMaxMouseStepLookMove, -1.f,
            -1.f, true, true, kPitchDeadzoneRad, true, "147-soft"};
  }
  if (IsExternalFpHostLookMoveYaw(sm)) {
    return {kYawSensFpLookMove, kPitchSensFpLookMove, kMaxDeltaRadLookMove,
            kMaxMouseStepLookMove, -1.f, -1.f, true, true, kPitchDeadzoneRad, true,
            "146-lookmove+yaw"};
  }
  if (IsExternalFpHostLookMove(sm)) {
    return {0.f, kPitchSensFpLookMove, kMaxDeltaRadLookMove, kMaxMouseStepLookMove, -1.f, -1.f,
            true, true, kPitchDeadzoneRad, false, "145-lookmove"};
  }
  if (IsExternalFpHostLookPitchAlt(sm)) {
    return {kYawSensFp, kPitchSensFpUnlock, kMaxDeltaRadFpUnlock, kMaxMouseStepFpUnlock, -1.f,
            -1.f, true, true, 0.f, true, "144-pitchAlt"};
  }
  if (IsExternalFpHostLookPitch(sm)) {
    return {kYawSensFp, kPitchSensFpUnlock, kMaxDeltaRadFpUnlock, kMaxMouseStepFpUnlock, -1.f,
            +1.f, true, true, 0.f, true, "143-pitch"};
  }
  if (IsExternalFpHostLookAlt(sm)) {
    return {kYawSensFp, kPitchSensFp, kMaxDeltaRadFp, kMaxMouseStepFp, -1.f, -1.f, true, true,
            0.f, true, "142-altYaw"};
  }
  if (IsExternalFpHostLookFix(sm)) {
    return {kYawSensFp, kPitchSensFp, kMaxDeltaRadFp, kMaxMouseStepFp, +1.f, -1.f, true, true,
            0.f, true, "141-fix"};
  }
  return {kYawSensLegacy, kPitchSensLegacy, kMaxDeltaRadLegacy, kMaxMouseStepLegacy, -1.f, -1.f,
          false, false, 0.f, true, "legacy"};
}

}  // namespace

void PollRecenterHotkey() {
  static bool wasF9 = false;
  if (KeyPressedEdge(VK_F9, &wasF9))
    DoRecenter();
}

void ApplyHmdMouseLook(const vr::TrackedDevicePose_t* poses, uint32_t poseCount) {
  if (IsCamMatrixOverrideEnabled())
    return;

  if (!g_enabled)
    return;
  if (!poses || poseCount == 0 || !IsGameForeground())
    return;

  const LookTune tune = TuneForMode(GetStereoMode());
  if (!tune.everyFrame) {
    if ((++g_frameSkip & 1u) != 0)
      return;
  }

  const uint32_t hmd = vr::k_unTrackedDeviceIndex_Hmd;
  if (hmd >= poseCount || !poses[hmd].bPoseIsValid || !poses[hmd].bDeviceIsConnected)
    return;

  float yaw = 0.f, pitch = 0.f;
  YawPitchFromMatrix(poses[hmd].mDeviceToAbsoluteTracking, &yaw, &pitch);

  if (!g_haveLast) {
    g_lastYaw = yaw;
    g_lastPitch = pitch;
    g_haveLast = true;
    return;
  }

  float dyaw = AngleDelta(yaw, g_lastYaw);
  float dpitch = AngleDelta(pitch, g_lastPitch);
  g_lastYaw = yaw;
  g_lastPitch = pitch;

  if (tune.perAxisClamp) {
    if (std::fabs(dyaw) > tune.maxDeltaRad)
      dyaw = 0.f;
    if (std::fabs(dpitch) > tune.maxDeltaRad)
      dpitch = 0.f;
  } else if (std::fabs(dyaw) > tune.maxDeltaRad || std::fabs(dpitch) > tune.maxDeltaRad) {
    return;
  }

  if (tune.pitchDeadzone > 0.f && std::fabs(dpitch) < tune.pitchDeadzone)
    dpitch = 0.f;

  int dx = 0;
  if (tune.mouseYaw) {
    const float yawMul = tune.yawSens * GetFpMouseSensScale();
    dx = static_cast<int>(std::lround(tune.yawSign * dyaw * yawMul));
  }
  int dy = 0;
  if (g_pitchEnabled) {
    const float pitchMul = tune.pitchSens * GetFpMouseSensScale();
    dy = static_cast<int>(std::lround(tune.pitchSign * dpitch * pitchMul));
  }

  dx = (std::max)(-tune.maxStep, (std::min)(tune.maxStep, dx));
  dy = (std::max)(-tune.maxStep, (std::min)(tune.maxStep, dy));
  SendMouseDelta(dx, dy);

  if ((++g_logCounter % 300) == 1) {
    Log("HmdLook[%s]: yaw=%.1f pitch=%.1f dx=%d dy=%d soft=%d stickInv=%d", tune.tag,
        yaw * kRad2Deg, pitch * kRad2Deg, dx, dy,
        WantsSoftPedHeading(GetStereoMode()) ? 1 : 0,
        WantsFpStickInvert(GetStereoMode()) ? 1 : 0);
  }
}

}  // namespace asi

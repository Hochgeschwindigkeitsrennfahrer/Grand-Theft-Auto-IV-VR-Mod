#include "hmd_look.h"
#include "cam_matrix.h"
#include "log.h"
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
constexpr float kYawSens = 380.0f;
constexpr float kPitchSens = 380.0f;
constexpr float kMaxDeltaRad = 0.20f;
constexpr int kMaxMouseStep = 18;

// Defaults on — no toggles (F9 recenter only).
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
  Log(ok ? "Recenter: SteamVR zero + look baseline (F9)" : "Recenter: look baseline (F9)");
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

}  // namespace

void PollRecenterHotkey() {
  static bool wasF9 = false;
  if (KeyPressedEdge(VK_F9, &wasF9))
    DoRecenter();
}

void ApplyHmdMouseLook(const vr::TrackedDevicePose_t* poses, uint32_t poseCount) {
  // Engine cam owns look when active.
  if (IsCamMatrixOverrideEnabled())
    return;

  if (!g_enabled)
    return;
  if (!poses || poseCount == 0 || !IsGameForeground())
    return;

  if ((++g_frameSkip & 1u) != 0)
    return;

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

  if (std::fabs(dyaw) > kMaxDeltaRad || std::fabs(dpitch) > kMaxDeltaRad)
    return;

  int dx = static_cast<int>(std::lround(-dyaw * kYawSens));
  int dy = 0;
  if (g_pitchEnabled)
    dy = static_cast<int>(std::lround(-dpitch * kPitchSens));

  dx = (std::max)(-kMaxMouseStep, (std::min)(kMaxMouseStep, dx));
  dy = (std::max)(-kMaxMouseStep, (std::min)(kMaxMouseStep, dy));
  SendMouseDelta(dx, dy);

  if ((++g_logCounter % 300) == 1) {
    Log("HmdLook: yaw=%.1f pitch=%.1f dx=%d dy=%d", yaw * kRad2Deg, pitch * kRad2Deg, dx, dy);
  }
}

}  // namespace asi

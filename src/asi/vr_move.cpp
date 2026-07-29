#include "vr_move.h"
#include "aob.h"
#include "cam_matrix.h"
#include "hmd_pose.h"
#include "log.h"
#include "stereo_config.h"
#include "vr_pad.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <cstdint>

namespace asi {
namespace {

struct Vec4 {
  float x, y, z, w;
};

struct Matrix44 {
  Vec4 right;
  Vec4 up;
  Vec4 at;
  Vec4 pos;
};

using FindPlayerPed_t = void*(__cdecl*)(int32_t);
FindPlayerPed_t g_FindPlayerPed = nullptr;
std::atomic<bool> g_ready{false};

// Controller yaw baked into look (cam matrix) + ped facing
float g_controllerYaw = 0.f;

constexpr float kStickDead = 0.22f;
constexpr float kStickYawSpeed = 2.8f;   // rad/s at full deflection
constexpr float kMouseStickScale = 28.f; // mouse dx per frame at full stick
constexpr uint32_t kHeadingOff = 0xAA0;  // CPed::m_fCurrentHeading (CE)
constexpr uint32_t kDesiredOff = 0xAA4;  // CPed::m_fDesiredHeading
constexpr uint32_t kMatrixOff = 0x20;    // CEntity::m_pMatrix
constexpr uint32_t kVehicleOff = 0xB30;  // CPed::m_pVehicle

uint32_t g_logCounter = 0;

void OvrToGta(float ox, float oy, float oz, float* gx, float* gy, float* gz) {
  *gx = ox;
  *gy = -oz;
  *gz = oy;
}

void Normalize2(float* x, float* y) {
  const float len = std::sqrt((*x) * (*x) + (*y) * (*y));
  if (len < 1e-5f) {
    *x = 0.f;
    *y = 1.f;
    return;
  }
  *x /= len;
  *y /= len;
}

// GTA-style heading from horizontal forward (Y-north, X-east)
float HeadingFromForwardXY(float fx, float fy) {
  return std::atan2(-fx, fy);
}

bool EnsureFindPlayerPed() {
  if (g_FindPlayerPed)
    return true;
  const uintptr_t p = FindPattern(nullptr, "8B 44 24 04 85 C0 75 18 A1");
  if (!p)
    return false;
  g_FindPlayerPed = reinterpret_cast<FindPlayerPed_t>(p);
  Log("VrMove: FindPlayerPed @ %p", reinterpret_cast<void*>(p));
  return true;
}

bool GetHmdForwardXY(float* fx, float* fy) {
  vr::HmdMatrix34_t h{};
  if (!GetHmdPoseMatrix(&h))
    return false;
  float x, y, z;
  OvrToGta(-h.m[0][2], -h.m[1][2], -h.m[2][2], &x, &y, &z);
  Normalize2(&x, &y);
  *fx = x;
  *fy = y;
  return true;
}

void SendMouseDx(int dx) {
  if (dx == 0)
    return;
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dx = dx;
  in.mi.dy = 0;
  in.mi.dwFlags = MOUSEEVENTF_MOVE;
  SendInput(1, &in, sizeof(INPUT));
}

float ReadRightStickX() {
  if (IsVrPadEnabled()) {
    const float vx = GetVrPadRightStickX();
    if (vx != 0.f)
      return -vx;
  }
  XINPUT_STATE st{};
  if (XInputGetState(0, &st) != ERROR_SUCCESS)
    return 0.f;
  float x = st.Gamepad.sThumbRX / 32767.f;
  if (x > 1.f)
    x = 1.f;
  if (x < -1.f)
    x = -1.f;
  if (std::fabs(x) < kStickDead)
    return 0.f;
  const float s = (std::fabs(x) - kStickDead) / (1.f - kStickDead);
  return -(x < 0.f ? -s : s);
}

void ApplyPedHeading(void* ped, float heading) {
  *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ped) + kHeadingOff) = heading;
  *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ped) + kDesiredOff) = heading;

  auto* mat = *reinterpret_cast<Matrix44**>(reinterpret_cast<uint8_t*>(ped) + kMatrixOff);
  if (!mat)
    return;

  // forward (RAGE "up") from heading; keep world up on "at"
  const float s = std::sin(heading);
  const float c = std::cos(heading);
  mat->up.x = s;
  mat->up.y = c;
  mat->up.z = 0.f;
  mat->up.w = 0.f;
  mat->right.x = c;
  mat->right.y = -s;
  mat->right.z = 0.f;
  mat->right.w = 0.f;
  mat->at.x = 0.f;
  mat->at.y = 0.f;
  mat->at.z = 1.f;
  mat->at.w = 0.f;
}

}  // namespace

float GetControllerYawOffset() {
  return g_controllerYaw;
}

void ResetControllerYawOffset() {
  g_controllerYaw = 0.f;
}

void UpdateVrMoveAndStick() {
  if (!IsCamMatrixOverrideEnabled() && !EnsureFindPlayerPed()) {
    // Still allow stick→mouse before FindPlayerPed resolves
  } else {
    EnsureFindPlayerPed();
  }

  const float stick = ReadRightStickX();
  // ~60fps assumed for yaw integrate; fine for cinema feel
  constexpr float kDt = 1.f / 60.f;
  if (stick != 0.f) {
    g_controllerYaw += stick * kStickYawSpeed * GetFpJoySensScale() * kDt;
    // Keep in [-pi, pi]
    while (g_controllerYaw > 3.14159265f)
      g_controllerYaw -= 6.2831853f;
    while (g_controllerYaw < -3.14159265f)
      g_controllerYaw += 6.2831853f;

    // Mouse path: stick turns camera. Scale by FirstPerson JoySens when configured.
    if (!IsCamMatrixOverrideEnabled()) {
      float m = stick * kMouseStickScale * GetFpJoySensScale();
      if (WantsFpStickInvert(GetStereoMode()))
        m = -m;
      const int dx = static_cast<int>(std::lround(m));
      SendMouseDx(dx);
    }
  }

  if (!g_FindPlayerPed)
    return;

  void* ped = g_FindPlayerPed(0);
  if (!ped)
    return;

  // In vehicle: don't force ped heading (car faces road); stick already moves cam via mouse
  void* veh = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ped) + kVehicleOff);
  if (veh)
    return;

  // look_move owns ped heading when enabled (avoid double-apply / matrix fight with FP).
  if (IsCleanDualLookMove(GetStereoMode()) || WantsExternalFpLookMove(GetStereoMode()))
    return;

  // Free move (gtaiv_dxvk_vr.movemode=1): the game maps stick input relative to the
  // camera we override with the HMD — leave ped heading alone so strafe/backpedal work.
  if (IsFreeMoveEnabled())
    return;

  float fx = 0.f, fy = 1.f;
  if (!GetHmdForwardXY(&fx, &fy))
    return;

  // Look heading = HMD + controller yaw offset
  float heading = HeadingFromForwardXY(fx, fy) + g_controllerYaw;
  ApplyPedHeading(ped, heading);

  if (!g_ready.exchange(true))
    Log("VrMove: head-directed move + right-stick yaw ON");

  if ((++g_logCounter % 300) == 1) {
    Log("VrMove: heading=%.1f stick=%.2f ctrlYaw=%.1f", heading * 57.2957795f, stick,
        g_controllerYaw * 57.2957795f);
  }
}

}  // namespace asi

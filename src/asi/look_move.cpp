#include "look_move.h"
#include "aob.h"
#include "cam_matrix.h"
#include "hmd_pose.h"
#include "log.h"
#include "stereo_config.h"
#include "vr_move.h"

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
std::atomic<int> g_lookMoveCached{-1};
uint32_t g_logCounter = 0;

constexpr uint32_t kHeadingOff = 0xAA0;
constexpr uint32_t kDesiredOff = 0xAA4;
constexpr uint32_t kMatrixOff = 0x20;
constexpr uint32_t kVehicleOff = 0xB30;

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

// GTA ped heading from horizontal forward (Y-north, X-east). Wrong sign = right skew.
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
  Log("LookMove: FindPlayerPed @ %p", reinterpret_cast<void*>(p));
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

void ApplyPedHeading(void* ped, float heading) {
  *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ped) + kHeadingOff) = heading;
  *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ped) + kDesiredOff) = heading;

  auto* mat = *reinterpret_cast<Matrix44**>(reinterpret_cast<uint8_t*>(ped) + kMatrixOff);
  if (!mat)
    return;

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

// Mode 147+: walk look-dir without yanking FirstPerson attach cam (no matrix rewrite).
void ApplyPedHeadingSoft(void* ped, float heading) {
  *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ped) + kHeadingOff) = heading;
  *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ped) + kDesiredOff) = heading;
}

bool ReadLookMoveFile() {
  char path[MAX_PATH]{};
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&ReadLookMoveFile), &self))
    return false;
  if (!GetModuleFileNameA(self, path, MAX_PATH))
    return false;
  char* slash = strrchr(path, '\\');
  if (!slash)
    slash = strrchr(path, '/');
  if (!slash)
    return false;
  slash[1] = 0;
  strcat_s(path, "gtaiv_dxvk_vr.lookmove");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return false;
  char buf[8]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return false;
  int v = 0;
  if (sscanf_s(buf, "%d", &v) != 1)
    return false;
  return v == 1;
}

}  // namespace

bool IsLookMoveEnabled() {
  if (IsCleanDualLookMove(GetStereoMode()) || WantsExternalFpLookMove(GetStereoMode()))
    return true;
  int cached = g_lookMoveCached.load();
  if (cached < 0) {
    const bool on = ReadLookMoveFile();
    cached = on ? 1 : 0;
    g_lookMoveCached.store(cached);
    Log("LookMove: %s (gtaiv_dxvk_vr.lookmove; Mode120/145+/151 forces ON; pedCoupled OFF)",
        on ? "ON" : "OFF");
  }
  return cached == 1;
}

void UpdateLookMove() {
  // Stick yaw offset always (cam_matrix reads GetControllerYawOffset).
  UpdateVrMoveAndStick();

  if (!IsLookMoveEnabled())
    return;

  if (!EnsureFindPlayerPed() || !g_FindPlayerPed)
    return;

  void* ped = g_FindPlayerPed(0);
  if (!ped)
    return;

  void* veh = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ped) + kVehicleOff);
  if (veh)
    return;

  float fx = 0.f, fy = 1.f;
  if (!GetHmdForwardXY(&fx, &fy))
    return;

  const float heading = HeadingFromForwardXY(fx, fy) + GetControllerYawOffset();
  if (WantsSoftPedHeading(GetStereoMode())) {
    ApplyPedHeadingSoft(ped, heading);
    if (!g_ready.exchange(true))
      Log("LookMove: SOFT HMD→ped heading (floats only; FP cam uncoupled from body matrix)");
  } else {
    ApplyPedHeading(ped, heading);
    if (!g_ready.exchange(true))
      Log("LookMove: HMD yaw → ped heading ON (atan2(-fx,fy); pure HMD cam)");
  }

  if ((++g_logCounter % 600) == 1) {
    Log("LookMove: heading=%.1f deg soft=%d", heading * 57.2957795f,
        WantsSoftPedHeading(GetStereoMode()) ? 1 : 0);
  }
}

}  // namespace asi

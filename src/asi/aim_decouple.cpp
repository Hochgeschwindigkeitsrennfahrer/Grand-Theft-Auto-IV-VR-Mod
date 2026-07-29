#include "aim_decouple.h"
#include "cam_matrix.h"
#include "hmd_pose.h"
#include "log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace asi {
namespace {

// Same OpenVR→GTA axis mapping as cam_matrix.cpp:100 (x, -z, y).
void OvrToGta(float ox, float oy, float oz, float* gx, float* gy, float* gz) {
  *gx = ox;
  *gy = -oz;
  *gz = oy;
}

bool Normalize3(float* x, float* y, float* z) {
  const float len = std::sqrt((*x) * (*x) + (*y) * (*y) + (*z) * (*z));
  if (len < 1e-6f)
    return false;
  *x /= len;
  *y /= len;
  *z /= len;
  return true;
}

// Small config file next to the ASI (same pattern as look_move.cpp ReadLookMoveFile).
int ReadIntFileOnce(const char* fileName, int defValue, int minV, int maxV) {
  char path[MAX_PATH]{};
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&ReadIntFileOnce), &self))
    return defValue;
  if (!GetModuleFileNameA(self, path, MAX_PATH))
    return defValue;
  char* slash = strrchr(path, '\\');
  if (!slash)
    slash = strrchr(path, '/');
  if (!slash)
    return defValue;
  slash[1] = 0;
  strcat_s(path, fileName);
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return defValue;
  char buf[16]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return defValue;
  int v = 0;
  if (sscanf_s(buf, "%d", &v) != 1)
    return defValue;
  if (v < minV || v > maxV)
    return defValue;
  return v;
}

std::mutex g_mu;
vr::HmdMatrix34_t g_ctrlMat{};
std::atomic<bool> g_ctrlValid{false};
std::atomic<uint32_t> g_probeN{0};
DWORD g_lastProbeTick = 0;

}  // namespace

int GetAimMode() {
  static std::atomic<int> s_mode{-1};
  int m = s_mode.load();
  if (m < 0) {
    m = ReadIntFileOnce("gtaiv_dxvk_vr.aimmode", 0, 0, 2);
    s_mode.store(m);
    if (m != 0)
      Log("AimDecouple: aimmode=%d (%s) — kill: write 0 to gtaiv_dxvk_vr.aimmode", m,
          m == 1 ? "probe/log only" : "override RESERVED (no verified addresses yet)");
  }
  return m;
}

int GetAimHand() {
  static std::atomic<int> s_hand{-1};
  int h = s_hand.load();
  if (h < 0) {
    h = ReadIntFileOnce("gtaiv_dxvk_vr.aimhand", 0, 0, 1);
    s_hand.store(h);
    if (GetAimMode() != 0)
      Log("AimDecouple: aimhand=%d (%s)", h, h == 0 ? "right" : "left");
  }
  return h;
}

void AimDecoupleOnPoses(const vr::TrackedDevicePose_t* poses, uint32_t poseCount) {
  if (GetAimMode() == 0)
    return;
  if (!poses || poseCount == 0) {
    g_ctrlValid.store(false);
    return;
  }
  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys) {
    g_ctrlValid.store(false);
    return;
  }
  const vr::ETrackedControllerRole role = (GetAimHand() == 1)
                                              ? vr::TrackedControllerRole_LeftHand
                                              : vr::TrackedControllerRole_RightHand;
  const vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(role);
  if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= poseCount ||
      !poses[idx].bPoseIsValid || !poses[idx].bDeviceIsConnected) {
    g_ctrlValid.store(false);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_ctrlMat = poses[idx].mDeviceToAbsoluteTracking;
  }
  g_ctrlValid.store(true);
}

bool GetControllerAimPose(float* posXyz, float* fwdXyz, float* upXyz) {
  if (!g_ctrlValid.load())
    return false;
  vr::HmdMatrix34_t m{};
  {
    std::lock_guard<std::mutex> lock(g_mu);
    m = g_ctrlMat;
  }
  // NOTE: position is in OpenVR tracking space (meters, relative to play area
  // zero), NOT GTA world coords. World anchoring needs the ped eye + HMD delta —
  // done in the override step at home. Direction vectors transfer directly.
  if (posXyz) {
    OvrToGta(m.m[0][3], m.m[1][3], m.m[2][3], &posXyz[0], &posXyz[1], &posXyz[2]);
  }
  if (fwdXyz) {
    OvrToGta(-m.m[0][2], -m.m[1][2], -m.m[2][2], &fwdXyz[0], &fwdXyz[1], &fwdXyz[2]);
    if (!Normalize3(&fwdXyz[0], &fwdXyz[1], &fwdXyz[2]))
      return false;
  }
  if (upXyz) {
    OvrToGta(m.m[0][1], m.m[1][1], m.m[2][1], &upXyz[0], &upXyz[1], &upXyz[2]);
    Normalize3(&upXyz[0], &upXyz[1], &upXyz[2]);
  }
  return true;
}

bool ResolveWeaponOrHandMatrix(float* outMat16RowMajor) {
  // STUB — needs home RE: hand bone tag via PedGetBonePos (cam_matrix.cpp:371)
  // or weapon entity matrix (DECOUPLED_AIM_PLAN.md checklist #6/#7).
  (void)outMat16RowMajor;
  static std::atomic<bool> s_logged{false};
  if (GetAimMode() != 0 && !s_logged.exchange(true))
    Log("AimDecouple: ResolveWeaponOrHandMatrix STUB — needs home RE (bone/weapon matrix)");
  return false;
}

bool ComputeAimForward(float* fwdXyz) {
  if (!fwdXyz)
    return false;
  if (GetControllerAimPose(nullptr, fwdXyz, nullptr))
    return true;
  // Fallback: HMD forward (same as look direction) so callers always have a ray.
  vr::HmdMatrix34_t h{};
  if (!GetHmdPoseMatrix(&h))
    return false;
  OvrToGta(-h.m[0][2], -h.m[1][2], -h.m[2][2], &fwdXyz[0], &fwdXyz[1], &fwdXyz[2]);
  return Normalize3(&fwdXyz[0], &fwdXyz[1], &fwdXyz[2]);
}

bool ApplyAimOverride() {
  // STUB: no verified aim address / native handler exists offline. Refuse.
  static std::atomic<bool> s_logged{false};
  if (GetAimMode() == 2 && !s_logged.exchange(true))
    Log("AimDecouple: aimmode=2 requested but NO verified aim target yet — "
        "no-op (see docs/DECOUPLED_AIM_PLAN.md checklist)");
  return false;
}

void UpdateAimDecouple() {
  if (GetAimMode() == 0)
    return;

  // aimmode=1: ~1 Hz probe log — controller ray vs current cam pos for the
  // home CE session (DECOUPLED_AIM_PLAN.md §5.2). No game memory access.
  const DWORD now = GetTickCount();
  if (now - g_lastProbeTick < 1000)
    return;
  g_lastProbeTick = now;

  float cPos[3]{}, cFwd[3]{}, cUp[3]{};
  const bool haveCtrl = GetControllerAimPose(cPos, cFwd, cUp);
  float camX = 0.f, camY = 0.f, camZ = 0.f;
  const bool haveCam = GetLastStereoCamPos(&camX, &camY, &camZ);
  float hFwd[3]{};
  const bool haveHmdFwd = ComputeAimForward(hFwd);

  const uint32_t n = ++g_probeN;
  Log("AimProbe #%u: ctrl=%d fwd=(%.3f,%.3f,%.3f) posTrk=(%.2f,%.2f,%.2f) | "
      "cam=%d pos=(%.2f,%.2f,%.2f) | ray=%d (%.3f,%.3f,%.3f)",
      n, haveCtrl ? 1 : 0, cFwd[0], cFwd[1], cFwd[2], cPos[0], cPos[1], cPos[2],
      haveCam ? 1 : 0, camX, camY, camZ, haveHmdFwd ? 1 : 0, hFwd[0], hFwd[1], hFwd[2]);

  if (GetAimMode() == 2)
    ApplyAimOverride();
}

}  // namespace asi

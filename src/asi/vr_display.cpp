#include "vr_display.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <openvr.h>

namespace asi {
namespace {

std::atomic<bool> g_loggedInfo{false};
std::atomic<bool> g_boundsReady{false};
vr::VRTextureBounds_t g_boundsL{};
vr::VRTextureBounds_t g_boundsR{};
float g_tanHalfH = 0.f;
float g_tanHalfV = 0.f;

float FovFromProjectionRaw(float left, float right) {
  const float fovRad = std::atan(right) - std::atan(left);
  return fovRad * (180.f / 3.14159265f);
}

void EnsureBoundsCache() {
  if (g_boundsReady.load())
    return;
  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys)
    return;

  float lL = 0.f, lR = 0.f, lT = 0.f, lB = 0.f;
  float rL = 0.f, rR = 0.f, rT = 0.f, rB = 0.f;
  sys->GetProjectionRaw(vr::Eye_Left, &lL, &lR, &lT, &lB);
  sys->GetProjectionRaw(vr::Eye_Right, &rL, &rR, &rT, &rB);

  // Exact L4D2VR / OpenVR sample pattern: one shared cover FOV, per-eye UV crop.
  const float tanH = std::max({-lL, lR, -rL, rR});
  const float tanV = std::max({-lT, lB, -rT, rB});
  if (!(tanH > 0.05f) || !(tanV > 0.05f))
    return;

  g_tanHalfH = tanH;
  g_tanHalfV = tanV;

  g_boundsL.uMin = 0.5f + 0.5f * lL / tanH;
  g_boundsL.uMax = 0.5f + 0.5f * lR / tanH;
  g_boundsL.vMin = 0.5f - 0.5f * lB / tanV;
  g_boundsL.vMax = 0.5f - 0.5f * lT / tanV;

  g_boundsR.uMin = 0.5f + 0.5f * rL / tanH;
  g_boundsR.uMax = 0.5f + 0.5f * rR / tanH;
  g_boundsR.vMin = 0.5f - 0.5f * rB / tanV;
  g_boundsR.vMax = 0.5f - 0.5f * rT / tanV;

  g_boundsReady.store(true);
  Log("VrDisplay: TextureBounds L4D2-style tanH=%.3f tanV=%.3f "
      "L[u=%.3f..%.3f v=%.3f..%.3f] R[u=%.3f..%.3f v=%.3f..%.3f]",
      tanH, tanV, g_boundsL.uMin, g_boundsL.uMax, g_boundsL.vMin, g_boundsL.vMax, g_boundsR.uMin,
      g_boundsR.uMax, g_boundsR.vMin, g_boundsR.vMax);
}

}  // namespace

float GetVrHorizontalFovDegrees() {
  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys)
    return 0.f;

  float lL = 0.f, lR = 0.f, lT = 0.f, lB = 0.f;
  float rL = 0.f, rR = 0.f, rT = 0.f, rB = 0.f;
  sys->GetProjectionRaw(vr::Eye_Left, &lL, &lR, &lT, &lB);
  sys->GetProjectionRaw(vr::Eye_Right, &rL, &rR, &rT, &rB);

  const float fovL = FovFromProjectionRaw(lL, lR);
  const float fovR = FovFromProjectionRaw(rL, rR);
  if (!(fovL > 1.f) || !(fovR > 1.f))
    return 0.f;
  return 0.5f * (fovL + fovR);
}

bool GetEyeSubmitBounds(vr::EVREye eye, vr::VRTextureBounds_t* out) {
  if (!out)
    return false;
  EnsureBoundsCache();
  if (!g_boundsReady.load())
    return false;
  *out = (eye == vr::Eye_Right) ? g_boundsR : g_boundsL;
  return true;
}

bool GetCoverFovTangents(float* tanHalfHoriz, float* tanHalfVert) {
  if (!tanHalfHoriz || !tanHalfVert)
    return false;
  EnsureBoundsCache();
  if (!g_boundsReady.load())
    return false;
  *tanHalfHoriz = g_tanHalfH;
  *tanHalfVert = g_tanHalfV;
  return g_tanHalfH > 0.05f && g_tanHalfV > 0.05f;
}

void LogVrDisplayInfo() {
  if (g_loggedInfo.exchange(true))
    return;

  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys) {
    Log("VrDisplay: VRSystem null");
    return;
  }

  uint32_t rw = 0, rh = 0;
  sys->GetRecommendedRenderTargetSize(&rw, &rh);

  float lL = 0.f, lR = 0.f, lT = 0.f, lB = 0.f;
  sys->GetProjectionRaw(vr::Eye_Left, &lL, &lR, &lT, &lB);
  const float fovH = FovFromProjectionRaw(lL, lR);
  const float fovV = std::fabs(std::atan(lB) - std::atan(lT)) * (180.f / 3.14159265f);

  vr::HmdMatrix34_t e2h = sys->GetEyeToHeadTransform(vr::Eye_Right);
  const float ipd = std::fabs(e2h.m[0][3]) * 2.f;

  char model[256]{};
  sys->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                      vr::Prop_ModelNumber_String, model, sizeof(model), nullptr);

  Log("VrDisplay: HMD=\"%s\" recommended=%ux%u FOVh~%.1f FOVv~%.1f IPD~%.1fmm", model, rw, rh,
      fovH, fovV, ipd * 1000.f);
  EnsureBoundsCache();
}

bool InstallVrDisplayHooks() {
  Log("VrDisplay: FOV hook SKIPPED (CCam write froze) — using CoverFOV+TextureBounds instead");
  LogVrDisplayInfo();
  return false;
}

}  // namespace asi

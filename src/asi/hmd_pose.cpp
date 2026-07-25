#include "hmd_pose.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace asi {
namespace {

std::mutex g_mu;
vr::HmdMatrix34_t g_mat{};
std::atomic<bool> g_valid{false};

// Once-per-frame / dual snapshot (written on BeginFrameHmdPoseSample).
vr::HmdMatrix34_t g_frameMat{};
std::atomic<bool> g_frameValid{false};

EyeOffset g_eyeL{};
EyeOffset g_eyeR{};
vr::HmdMatrix34_t g_e2hL{};
vr::HmdMatrix34_t g_e2hR{};
std::atomic<bool> g_eyeOk{false};

bool g_haveSeatedBase = false;
float g_basePx = 0.f, g_basePy = 0.f, g_basePz = 0.f;

void OvrToGta(float ox, float oy, float oz, float* gx, float* gy, float* gz) {
  *gx = ox;
  *gy = -oz;
  *gz = oy;
}

// Light EMA on orientation columns only (keeps translation raw for seated).
// Alpha high enough to track head, low enough to kill single-sample spikes.
constexpr float kPoseEmaAlpha = 0.65f;
bool g_haveEma = false;
vr::HmdMatrix34_t g_emaMat{};

void EmaPoseOrient(const vr::HmdMatrix34_t& in, vr::HmdMatrix34_t* out) {
  if (!g_haveEma) {
    g_emaMat = in;
    g_haveEma = true;
    *out = in;
    return;
  }
  const float a = kPoseEmaAlpha;
  const float b = 1.f - a;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c)
      g_emaMat.m[r][c] = a * in.m[r][c] + b * g_emaMat.m[r][c];
    // Translation: light smooth (jumpy seated) but keep responsive.
    g_emaMat.m[r][3] = a * in.m[r][3] + b * g_emaMat.m[r][3];
  }
  *out = g_emaMat;
}

}  // namespace

void UpdateHmdPose(const vr::TrackedDevicePose_t* poses, uint32_t poseCount) {
  const uint32_t hmd = vr::k_unTrackedDeviceIndex_Hmd;
  if (!poses || hmd >= poseCount || !poses[hmd].bPoseIsValid || !poses[hmd].bDeviceIsConnected) {
    g_valid = false;
    return;
  }
  vr::HmdMatrix34_t filtered{};
  EmaPoseOrient(poses[hmd].mDeviceToAbsoluteTracking, &filtered);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_mat = filtered;
  }
  g_valid = true;
  // EyeToHead is static for a given IPD — cache once (or after F9 recenter).
  if (!g_eyeOk.load())
    CacheEyeToHeadFromHmd();
}

void BeginFrameHmdPoseSample() {
  if (!g_valid.load()) {
    g_frameValid = false;
    return;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  g_frameMat = g_mat;
  g_frameValid = true;
}

bool GetFrameHmdPoseMatrix(vr::HmdMatrix34_t* out) {
  if (!out || !g_frameValid.load())
    return GetHmdPoseMatrix(out);
  std::lock_guard<std::mutex> lock(g_mu);
  *out = g_frameMat;
  return true;
}

void CacheEyeToHeadFromHmd() {
  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys) {
    g_eyeOk = false;
    return;
  }
  const vr::HmdMatrix34_t L = sys->GetEyeToHeadTransform(vr::Eye_Left);
  const vr::HmdMatrix34_t R = sys->GetEyeToHeadTransform(vr::Eye_Right);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_e2hL = L;
    g_e2hR = R;
    g_eyeL = {L.m[0][3], L.m[1][3], L.m[2][3]};
    g_eyeR = {R.m[0][3], R.m[1][3], R.m[2][3]};
  }
  g_eyeOk = true;
}

bool GetCachedEyeOffset(bool rightEye, EyeOffset* out) {
  if (!out || !g_eyeOk.load())
    return false;
  std::lock_guard<std::mutex> lock(g_mu);
  *out = rightEye ? g_eyeR : g_eyeL;
  return true;
}

bool GetCachedEyeToHead(bool rightEye, vr::HmdMatrix34_t* out) {
  if (!out || !g_eyeOk.load())
    return false;
  std::lock_guard<std::mutex> lock(g_mu);
  *out = rightEye ? g_e2hR : g_e2hL;
  return true;
}

bool GetHmdPoseMatrix(vr::HmdMatrix34_t* out) {
  if (!out || !g_valid.load())
    return false;
  std::lock_guard<std::mutex> lock(g_mu);
  *out = g_mat;
  return true;
}

bool IsHmdPoseValid() {
  return g_valid.load();
}

void HmdPoseOnRecenter() {
  g_haveSeatedBase = false;
  g_haveEma = false;
  g_eyeOk = false;  // refresh EyeToHead after recenter / IPD change
  CacheEyeToHeadFromHmd();
}

bool GetHmdSeatedDeltaGta(float* dx, float* dy, float* dz) {
  if (!dx || !dy || !dz || !g_valid.load())
    return false;
  vr::HmdMatrix34_t h{};
  {
    std::lock_guard<std::mutex> lock(g_mu);
    h = g_mat;
  }
  float px, py, pz;
  OvrToGta(h.m[0][3], h.m[1][3], h.m[2][3], &px, &py, &pz);
  if (!g_haveSeatedBase) {
    g_basePx = px;
    g_basePy = py;
    g_basePz = pz;
    g_haveSeatedBase = true;
    *dx = *dy = *dz = 0.f;
    return true;
  }
  *dx = px - g_basePx;
  *dy = py - g_basePy;
  *dz = pz - g_basePz;
  return true;
}

}  // namespace asi

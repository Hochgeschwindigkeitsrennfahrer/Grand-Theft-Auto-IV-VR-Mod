#include "hmd_pose.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace asi {
namespace {

std::mutex g_mu;
vr::HmdMatrix34_t g_mat{};
std::atomic<bool> g_valid{false};

vr::HmdMatrix34_t g_latchMat{};
std::atomic<bool> g_latchActive{false};

// Mode 271 AER: pose the eye textures now awaiting Submit were rendered with.
vr::HmdMatrix34_t g_renderPose{};
std::atomic<bool> g_renderPoseValid{false};

EyeOffset g_eyeL{};
EyeOffset g_eyeR{};
std::atomic<bool> g_eyeOk{false};

}  // namespace

void UpdateHmdPose(const vr::TrackedDevicePose_t* poses, uint32_t poseCount) {
  const uint32_t hmd = vr::k_unTrackedDeviceIndex_Hmd;
  if (!poses || hmd >= poseCount || !poses[hmd].bPoseIsValid || !poses[hmd].bDeviceIsConnected) {
    g_valid = false;
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_mat = poses[hmd].mDeviceToAbsoluteTracking;
  }
  g_valid = true;
  CacheEyeToHeadFromHmd();
}

void BeginDualHmdPoseLatch() {
  vr::HmdMatrix34_t m{};
  // Prefer live pose; latch fails soft (dual continues without freeze).
  if (!g_valid.load()) {
    g_latchActive.store(false);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_latchMat = g_mat;
  }
  g_latchActive.store(true);
}

void EndDualHmdPoseLatch() {
  g_latchActive.store(false);
}

bool IsDualHmdPoseLatchActive() {
  return g_latchActive.load();
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

bool GetHmdPoseMatrix(vr::HmdMatrix34_t* out) {
  if (!out || !g_valid.load())
    return false;
  std::lock_guard<std::mutex> lock(g_mu);
  *out = g_latchActive.load() ? g_latchMat : g_mat;
  return true;
}

bool IsHmdPoseValid() {
  return g_valid.load();
}

bool SampleLateLatchHmdPose(vr::HmdMatrix34_t* out) {
  if (!out)
    return false;
  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys)
    return false;
  // Prefer a short photon prediction when compositor is up; else "now" (0).
  float predicted = 0.f;
  if (vr::VRCompositor()) {
    predicted = vr::VRCompositor()->GetFrameTimeRemaining();
    if (predicted < 0.f)
      predicted = 0.f;
    if (predicted > 0.05f)
      predicted = 0.05f;
  }
  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
  sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, predicted, poses,
                                       vr::k_unMaxTrackedDeviceCount);
  const uint32_t hmd = vr::k_unTrackedDeviceIndex_Hmd;
  if (!poses[hmd].bPoseIsValid || !poses[hmd].bDeviceIsConnected)
    return false;
  *out = poses[hmd].mDeviceToAbsoluteTracking;
  return true;
}

void PublishRenderPoseForSubmit(const vr::HmdMatrix34_t& m) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_renderPose = m;
  }
  g_renderPoseValid.store(true);
}

bool GetRenderPoseForSubmit(vr::HmdMatrix34_t* out) {
  if (!out || !g_renderPoseValid.load())
    return false;
  std::lock_guard<std::mutex> lock(g_mu);
  *out = g_renderPose;
  return true;
}

}  // namespace asi

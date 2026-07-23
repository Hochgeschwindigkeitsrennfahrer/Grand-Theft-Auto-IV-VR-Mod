#include "hmd_pose.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace asi {
namespace {

std::mutex g_mu;
vr::HmdMatrix34_t g_mat{};
std::atomic<bool> g_valid{false};

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
  *out = g_mat;
  return true;
}

bool IsHmdPoseValid() {
  return g_valid.load();
}

}  // namespace asi

#include "hmd_pose.h"

#include "../bridge/gtaiv_xr_pose_bridge.h"

#include <atomic>
#include <cmath>
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

void UpdateHmdPoseFromOpenXr(const gtaiv_xr_bridge::PoseBridge& pose) {
  const uint32_t required =
      gtaiv_xr_bridge::PoseBridgeHostRunning |
      gtaiv_xr_bridge::PoseBridgeSessionRunning |
      gtaiv_xr_bridge::PoseBridgeViewsValid |
      gtaiv_xr_bridge::PoseBridgeHmdValid;
  if ((pose.flags & required) != required) {
    g_valid = false;
    g_eyeOk = false;
    return;
  }

  const float x = pose.hmdPose.orientation[0];
  const float y = pose.hmdPose.orientation[1];
  const float z = pose.hmdPose.orientation[2];
  const float w = pose.hmdPose.orientation[3];
  const float norm = std::sqrt(x * x + y * y + z * z + w * w);
  if (!std::isfinite(norm) || norm < 0.5f || norm > 1.5f) {
    g_valid = false;
    g_eyeOk = false;
    return;
  }
  const float s = 1.0f / norm;
  const float qx = x * s;
  const float qy = y * s;
  const float qz = z * s;
  const float qw = w * s;

  vr::HmdMatrix34_t matrix {};
  matrix.m[0][0] = 1.f - 2.f * (qy * qy + qz * qz);
  matrix.m[0][1] = 2.f * (qx * qy - qz * qw);
  matrix.m[0][2] = 2.f * (qx * qz + qy * qw);
  matrix.m[1][0] = 2.f * (qx * qy + qz * qw);
  matrix.m[1][1] = 1.f - 2.f * (qx * qx + qz * qz);
  matrix.m[1][2] = 2.f * (qy * qz - qx * qw);
  matrix.m[2][0] = 2.f * (qx * qz - qy * qw);
  matrix.m[2][1] = 2.f * (qy * qz + qx * qw);
  matrix.m[2][2] = 1.f - 2.f * (qx * qx + qy * qy);
  matrix.m[0][3] = pose.hmdPose.position[0];
  matrix.m[1][3] = pose.hmdPose.position[1];
  matrix.m[2][3] = pose.hmdPose.position[2];

  EyeOffset offsets[2] {};
  for (uint32_t eye = 0; eye < 2; ++eye) {
    const float dx = pose.eyePoses[eye].position[0] - matrix.m[0][3];
    const float dy = pose.eyePoses[eye].position[1] - matrix.m[1][3];
    const float dz = pose.eyePoses[eye].position[2] - matrix.m[2][3];
    // R^T converts LOCAL-space eye displacement into head space.
    offsets[eye].x =
        matrix.m[0][0] * dx + matrix.m[1][0] * dy + matrix.m[2][0] * dz;
    offsets[eye].y =
        matrix.m[0][1] * dx + matrix.m[1][1] * dy + matrix.m[2][1] * dz;
    offsets[eye].z =
        matrix.m[0][2] * dx + matrix.m[1][2] * dy + matrix.m[2][2] * dz;
  }

  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_mat = matrix;
    g_eyeL = offsets[0];
    g_eyeR = offsets[1];
  }
  g_eyeOk = true;
  g_valid = true;
}

void InvalidateHmdPose() {
  g_valid = false;
  g_eyeOk = false;
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

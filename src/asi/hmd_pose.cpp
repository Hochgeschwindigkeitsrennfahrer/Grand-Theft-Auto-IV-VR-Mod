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

vr::HmdMatrix34_t g_latchMat{};
std::atomic<bool> g_latchActive{false};

EyeOffset g_eyeL{};
EyeOffset g_eyeR{};
std::atomic<bool> g_eyeOk{false};

thread_local bool g_buildPosePinned = false;
thread_local vr::HmdMatrix34_t g_pinnedBuildMatrix{};
thread_local EyeOffset g_pinnedBuildEyeL{};
thread_local EyeOffset g_pinnedBuildEyeR{};

bool DecodeOpenXrPose(
    const gtaiv_xr_bridge::PoseBridge& pose,
    vr::HmdMatrix34_t* matrix,
    EyeOffset offsets[2]) {
  if (!matrix || !offsets)
    return false;
  const uint32_t required =
      gtaiv_xr_bridge::PoseBridgeHostRunning |
      gtaiv_xr_bridge::PoseBridgeSessionRunning |
      gtaiv_xr_bridge::PoseBridgeViewsValid |
      gtaiv_xr_bridge::PoseBridgeHmdValid;
  if ((pose.flags & required) != required)
    return false;

  for (uint32_t axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(pose.hmdPose.position[axis]) ||
        !std::isfinite(pose.eyePoses[0].position[axis]) ||
        !std::isfinite(pose.eyePoses[1].position[axis])) {
      return false;
    }
  }
  const float eyeDx =
      pose.eyePoses[1].position[0] -
      pose.eyePoses[0].position[0];
  const float eyeDy =
      pose.eyePoses[1].position[1] -
      pose.eyePoses[0].position[1];
  const float eyeDz =
      pose.eyePoses[1].position[2] -
      pose.eyePoses[0].position[2];
  const float eyeSeparation =
      std::sqrt(
          eyeDx * eyeDx +
          eyeDy * eyeDy +
          eyeDz * eyeDz);
  if (!std::isfinite(eyeSeparation) ||
      eyeSeparation < 0.01f ||
      eyeSeparation > 0.20f) {
    return false;
  }

  const float x = pose.hmdPose.orientation[0];
  const float y = pose.hmdPose.orientation[1];
  const float z = pose.hmdPose.orientation[2];
  const float w = pose.hmdPose.orientation[3];
  const float norm = std::sqrt(
      x * x + y * y + z * z + w * w);
  if (!std::isfinite(norm) ||
      norm < 0.5f || norm > 1.5f) {
    return false;
  }
  const float s = 1.0f / norm;
  const float qx = x * s;
  const float qy = y * s;
  const float qz = z * s;
  const float qw = w * s;

  vr::HmdMatrix34_t decoded{};
  decoded.m[0][0] =
      1.f - 2.f * (qy * qy + qz * qz);
  decoded.m[0][1] =
      2.f * (qx * qy - qz * qw);
  decoded.m[0][2] =
      2.f * (qx * qz + qy * qw);
  decoded.m[1][0] =
      2.f * (qx * qy + qz * qw);
  decoded.m[1][1] =
      1.f - 2.f * (qx * qx + qz * qz);
  decoded.m[1][2] =
      2.f * (qy * qz - qx * qw);
  decoded.m[2][0] =
      2.f * (qx * qz - qy * qw);
  decoded.m[2][1] =
      2.f * (qy * qz + qx * qw);
  decoded.m[2][2] =
      1.f - 2.f * (qx * qx + qy * qy);
  decoded.m[0][3] = pose.hmdPose.position[0];
  decoded.m[1][3] = pose.hmdPose.position[1];
  decoded.m[2][3] = pose.hmdPose.position[2];

  EyeOffset decodedOffsets[2]{};
  for (uint32_t eye = 0; eye < 2; ++eye) {
    const float dx =
        pose.eyePoses[eye].position[0] -
        decoded.m[0][3];
    const float dy =
        pose.eyePoses[eye].position[1] -
        decoded.m[1][3];
    const float dz =
        pose.eyePoses[eye].position[2] -
        decoded.m[2][3];
    const float eyeToHmdDistance =
        std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(eyeToHmdDistance) ||
        eyeToHmdDistance > 0.20f) {
      return false;
    }
    // R^T converts LOCAL-space eye displacement into head space.
    decodedOffsets[eye].x =
        decoded.m[0][0] * dx +
        decoded.m[1][0] * dy +
        decoded.m[2][0] * dz;
    decodedOffsets[eye].y =
        decoded.m[0][1] * dx +
        decoded.m[1][1] * dy +
        decoded.m[2][1] * dz;
    decodedOffsets[eye].z =
        decoded.m[0][2] * dx +
        decoded.m[1][2] * dy +
        decoded.m[2][2] * dz;
    if (!std::isfinite(decodedOffsets[eye].x) ||
        !std::isfinite(decodedOffsets[eye].y) ||
        !std::isfinite(decodedOffsets[eye].z)) {
      return false;
    }
  }
  *matrix = decoded;
  offsets[0] = decodedOffsets[0];
  offsets[1] = decodedOffsets[1];
  return true;
}

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

bool IsOpenXrPoseRenderable(
    const gtaiv_xr_bridge::PoseBridge& pose) {
  vr::HmdMatrix34_t matrix{};
  EyeOffset offsets[2]{};
  return DecodeOpenXrPose(pose, &matrix, offsets);
}

void UpdateHmdPoseFromOpenXr(const gtaiv_xr_bridge::PoseBridge& pose) {
  vr::HmdMatrix34_t matrix{};
  EyeOffset offsets[2]{};
  if (!DecodeOpenXrPose(pose, &matrix, offsets)) {
    g_valid = false;
    g_eyeOk = false;
    return;
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

bool BeginPinnedOpenXrBuildPose(
    const gtaiv_xr_bridge::PoseBridge& pose) {
  g_buildPosePinned = false;
  vr::HmdMatrix34_t matrix{};
  EyeOffset offsets[2]{};
  if (!DecodeOpenXrPose(
          pose, &matrix, offsets)) {
    return false;
  }
  g_pinnedBuildMatrix = matrix;
  g_pinnedBuildEyeL = offsets[0];
  g_pinnedBuildEyeR = offsets[1];
  g_buildPosePinned = true;
  return true;
}

void EndPinnedOpenXrBuildPose() {
  g_buildPosePinned = false;
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
  if (!out)
    return false;
  if (g_buildPosePinned) {
    *out = rightEye ? g_pinnedBuildEyeR : g_pinnedBuildEyeL;
    return true;
  }
  if (!g_eyeOk.load())
    return false;
  std::lock_guard<std::mutex> lock(g_mu);
  *out = rightEye ? g_eyeR : g_eyeL;
  return true;
}

bool GetHmdPoseMatrix(vr::HmdMatrix34_t* out) {
  if (!out)
    return false;
  if (g_buildPosePinned) {
    *out = g_pinnedBuildMatrix;
    return true;
  }
  if (!g_valid.load())
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

}  // namespace asi

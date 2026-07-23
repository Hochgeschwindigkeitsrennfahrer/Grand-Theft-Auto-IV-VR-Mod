#pragma once

#include <openvr.h>

namespace asi {

struct EyeOffset {
  float x = 0.f, y = 0.f, z = 0.f;  // OpenVR head space: +X right, +Y up, +Z back
};

// Latest HMD pose from WaitGetPoses (game/render thread writes, cam hooks read).
void UpdateHmdPose(const vr::TrackedDevicePose_t* poses, uint32_t poseCount);
bool GetHmdPoseMatrix(vr::HmdMatrix34_t* out);
bool IsHmdPoseValid();

// Cached eye-to-head translations (updated with poses — safe for CopyMat).
void CacheEyeToHeadFromHmd();
bool GetCachedEyeOffset(bool rightEye, EyeOffset* out);

}  // namespace asi

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

// Mode 136: sample HMD pose immediately before Submit (late latch). Uses
// GetDeviceToAbsoluteTrackingPose with a short photon prediction — not the
// capture-time WaitGetPoses snapshot. Call from the Submit thread only.
bool SampleLateLatchHmdPose(vr::HmdMatrix34_t* out);

// Cached eye-to-head translations (updated with poses — safe for CopyMat).
void CacheEyeToHeadFromHmd();
bool GetCachedEyeOffset(bool rightEye, EyeOffset* out);

}  // namespace asi

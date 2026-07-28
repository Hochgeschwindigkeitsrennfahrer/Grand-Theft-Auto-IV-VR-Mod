#pragma once

#include <openvr.h>

namespace gtaiv_xr_bridge {
struct PoseBridge;
}

namespace asi {

struct EyeOffset {
  float x = 0.f, y = 0.f, z = 0.f;  // OpenVR head space: +X right, +Y up, +Z back
};

// Latest HMD pose from WaitGetPoses (game/render thread writes, cam hooks read).
void UpdateHmdPose(const vr::TrackedDevicePose_t* poses, uint32_t poseCount);

// Converts the standard OpenXR LOCAL-space quaternion/position sample into the
// same tracking matrix cache used by the established GTA camera code.
void UpdateHmdPoseFromOpenXr(const gtaiv_xr_bridge::PoseBridge& pose);
void InvalidateHmdPose();

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

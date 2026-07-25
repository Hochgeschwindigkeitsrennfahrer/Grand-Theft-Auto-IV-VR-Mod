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

// Freeze the latest pose into a once-per-dual / once-per-frame snapshot so L and R
// injects see the same sample (no mid-dual jump). Call at dual start / EndScene.
void BeginFrameHmdPoseSample();
bool GetFrameHmdPoseMatrix(vr::HmdMatrix34_t* out);

// Cached eye-to-head (static for a given IPD — refresh on recenter / first pose).
void CacheEyeToHeadFromHmd();
bool GetCachedEyeOffset(bool rightEye, EyeOffset* out);
bool GetCachedEyeToHead(bool rightEye, vr::HmdMatrix34_t* out);

// F9: clear seated 6DoF baseline used by GetHmdSeatedDeltaGta.
void HmdPoseOnRecenter();

// Seated head translation delta in GTA space (OVR→GTA), relative to F9 baseline.
// Caller applies LeanGain / world scale. Returns false if pose invalid.
bool GetHmdSeatedDeltaGta(float* dx, float* dy, float* dz);

}  // namespace asi

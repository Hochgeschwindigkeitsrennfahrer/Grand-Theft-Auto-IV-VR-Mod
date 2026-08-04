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

// Mode 203: latch ONE HMD pose for a parent-dual L/R pair (same orientation;
// only ±IPD differs via RefreshLiveCam). Not the Mode201 ped/basis freeze.
void BeginDualHmdPoseLatch();
void EndDualHmdPoseLatch();
bool IsDualHmdPoseLatchActive();

// Mode 136: sample HMD pose immediately before Submit (late latch). Uses
// GetDeviceToAbsoluteTrackingPose with a short photon prediction — not the
// capture-time WaitGetPoses snapshot. Call from the Submit thread only.
bool SampleLateLatchHmdPose(vr::HmdMatrix34_t* out);

// Mode 271 AER: the pair is submitted one walk after it was rendered, at
// which point WaitGetPoses has already moved on to a fresher sample and a
// late-latch "now" pose would be the exact lie the honest capture-time stamp
// is meant to avoid. Publish at the eye-pair promote (still inside the dual
// latch), read at Submit. Same idea as UEVR's frame-indexed pose_queue /
// get_pose_for_submit().
void PublishRenderPoseForSubmit(const vr::HmdMatrix34_t& m);
bool GetRenderPoseForSubmit(vr::HmdMatrix34_t* out);

// Cached eye-to-head translations (updated with poses — safe for CopyMat).
void CacheEyeToHeadFromHmd();
bool GetCachedEyeOffset(bool rightEye, EyeOffset* out);

}  // namespace asi

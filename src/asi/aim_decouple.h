#pragma once

#include <openvr.h>

namespace asi {

// Decoupled aiming (docs/DECOUPLED_AIM_PLAN.md). OFF by default.
// gtaiv_dxvk_vr.aimmode:
//   0 = off (default)
//   1 = probe log + one-time AimNative resolve
//   2 = grip LT + trigger FIRE_PED_WEAPON (on foot); TASK_AIM off (no VR hands)
// Kill: write 0 or delete file. Stereo baseline: Mode 243 (do not change here).
int GetAimMode();

// 0 = right hand (default), 1 = left (gtaiv_dxvk_vr.aimhand).
int GetAimHand();

// Cache controller pose + grip from the SAME WaitGetPoses array as the HMD.
void AimDecoupleOnPoses(const vr::TrackedDevicePose_t* poses, uint32_t poseCount);

// EndScene-safe: probe log only (no native calls).
void UpdateAimDecouple();

// Game-thread only (CopyMat / PedHide sites): Plan A native aim when aimmode=2.
void TickAimFromGameThread();

bool GetControllerAimPose(float* posXyz, float* fwdXyz, float* upXyz);
bool ResolveWeaponOrHandMatrix(float* outMat16RowMajor);
bool ComputeAimForward(float* fwdXyz);
bool ApplyAimOverride();

}  // namespace asi
#pragma once

#include <openvr.h>

namespace asi {

// Decoupled-aiming scaffolding (docs/DECOUPLED_AIM_PLAN.md). OFF by default.
// gtaiv_dxvk_vr.aimmode: 0=off (default), 1=probe/log only, 2=override
// (RESERVED — refuses until aim addresses/natives are verified at home).
// Kill switch: delete the file or write 0. Never writes game memory in stub form.
int GetAimMode();

// 0 = right hand (default), 1 = left (gtaiv_dxvk_vr.aimhand).
int GetAimHand();

// Cache controller pose from the SAME WaitGetPoses array as the HMD.
// Call right after UpdateHmdPose on the EndScene/submit thread.
void AimDecoupleOnPoses(const vr::TrackedDevicePose_t* poses, uint32_t poseCount);

// Per-frame update (EndScene thread). aimmode=1 logs ~1 Hz probe lines
// (controller fwd vs cam fwd) for the home CE session. No game writes.
void UpdateAimDecouple();

// Controller aim pose in GTA world axes (OvrToGta: x, -z, y).
// Returns false when no valid controller pose is cached.
bool GetControllerAimPose(float* posXyz, float* fwdXyz, float* upXyz);

// STUB: weapon/hand matrix resolve — needs home RE (bone tags / weapon matrix).
// Returns false until implemented (see DECOUPLED_AIM_PLAN.md checklist #6/#7).
bool ResolveWeaponOrHandMatrix(float* outMat16RowMajor);

// Aim forward = controller forward; fallback HMD forward. False if neither.
bool ComputeAimForward(float* fwdXyz);

// STUB: applies the aim override (Plan A natives / B IK write / C cam hook).
// Logs once and returns false unless aimmode=2 AND a verified target exists
// (none exist in the offline stub — always false today).
bool ApplyAimOverride();

}  // namespace asi
